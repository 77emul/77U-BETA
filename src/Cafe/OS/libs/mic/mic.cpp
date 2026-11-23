#include "Cafe/OS/common/OSCommon.h"
#include "input/InputManager.h"
#include "audio/IAudioInputAPI.h"
#include "config/CemuConfig.h"
#include <atomic>

#define MIC_SAMPLERATE 32000
const int MIC_SAMPLES_PER_3MS_32KHZ = 96;

#if BOOST_OS_IOS
extern "C" bool CemuBridge_IsMicrophoneCaptureReady(void);
#endif

static std::atomic<bool> s_inputDeviceRefreshRequested{false};

void mic_requestInputDeviceRefresh()
{
	s_inputDeviceRefreshRequested.store(true, std::memory_order_release);
}

static void mic_initializeInputDevice()
{
	std::unique_lock lock(g_audioInputMutex);
#if BOOST_OS_IOS
	// Never create an iOS input stream before explicit microphone permission.
	// A permission/route change requests a refresh; it does not touch MICStatus
	// or the guest ring buffer from UIKit's completion thread.
	if (!CemuBridge_IsMicrophoneCaptureReady())
	{
		g_inputAudio.reset();
		return;
	}
#endif
	if (g_inputAudio)
		return;

	auto& config = GetConfig();
	const auto audio_api = IAudioInputAPI::Cubeb;
	if (!IAudioInputAPI::IsAudioInputAPIAvailable(audio_api))
		return;
	auto devices = IAudioInputAPI::GetDevices(audio_api);
	const auto it = std::find_if(devices.begin(), devices.end(), [&config](const auto& d) {
		return d->GetIdentifier() == config.input_device;
	});
	if (it == devices.end())
		return;
	try
	{
		g_inputAudio = IAudioInputAPI::CreateDevice(audio_api, *it, MIC_SAMPLERATE, 1, MIC_SAMPLES_PER_3MS_32KHZ, 16);
		if (g_inputAudio)
			g_inputAudio->SetVolume(config.input_volume);
	}
	catch (std::runtime_error& ex)
	{
		cemuLog_log(LogType::Force, "can't initialize audio input; using silent microphone: {}", ex.what());
		g_inputAudio.reset();
	}
}

enum class MIC_RESULT
{
	SUCCESS			=  0,
	BAD_PARAM		= -2,
	ALREADY_OPEN	= -5,
	NOT_OPEN		= -6,
	NOT_INITIALIZED	= -7,
	NOT_CONNECTED	= -8
};

enum class MIC_STATUS_FLAGS : uint32
{
	FORMAT_PCM16	= (1 << 0),
	IS_OPEN			= (1 << 1),
	IS_CONNECTED	= (1 << 2)
};
DEFINE_ENUM_FLAG_OPERATORS(MIC_STATUS_FLAGS);

#define MIC_HANDLE_DRC0				0
#define MIC_HANDLE_DRC1				1

enum class MIC_STATEID
{
	SAMPLERATE			= 0,
	GAIN_DB				= 1,
	GAIN_MIN			= 2,
	GAIN_MAX			= 3,
	GAIN_STEP			= 4,
	MUTE				= 5,
	ECHO_CANCELLATION	= 7,
	AUTO_SELECTION		= 8
};

struct  
{
	struct  
	{
		bool isInited;
		bool isOpen;
		// ringbuffer information
		void* ringbufferSampleData;
		uint32 ringbufferSize;
		uint32 readIndex;
		uint32 writeIndex;
		// state
		uint32 echoCancellation;
		uint32 autoSelection;
		uint32 gainDB;
		uint32 mute;
	}drc[2];
}MICStatus = {0};

typedef struct  
{
	uint32be size;
	uint32be samples;
}micRingbuffer_t;

struct micStatus_t
{
	betype<MIC_STATUS_FLAGS> flags;
	uint32be numSamplesAvailable;
	uint32be readIndex;
};

static_assert(sizeof(micStatus_t) == 0xC);

bool mic_isConnected(uint32 drcIndex)
{
	if( drcIndex != 0 )
		return false;

	return true;
}

sint32 mic_availableSamples(uint32 drcIndex)
{
	const auto ringBufferSizeSamples = MICStatus.drc[drcIndex].ringbufferSize / (sint32)sizeof(uint16);
	if (ringBufferSizeSamples <= 0)
		return 0;
	return (ringBufferSizeSamples + MICStatus.drc[drcIndex].writeIndex - MICStatus.drc[drcIndex].readIndex) % ringBufferSizeSamples;
}

bool mic_isActive(uint32 drcIndex)
{
	return MICStatus.drc[drcIndex].isOpen;
}

void mic_feedSamples(uint32 drcIndex, sint16* sampleData, sint32 numSamples)
{
	if (!sampleData || numSamples <= 0)
		return;

	uint16* sampleDataU16 = (uint16*)sampleData;
	sint32 ringBufferSizeSamples = MICStatus.drc[drcIndex].ringbufferSize / (sint32)sizeof(uint16);
	uint16* ringBufferBase = (uint16*)MICStatus.drc[drcIndex].ringbufferSampleData;
	if (ringBufferSizeSamples <= 0 || !ringBufferBase)
		return;

	sint32 writeIndex = MICStatus.drc[drcIndex].writeIndex % ringBufferSizeSamples;
	do
	{
		ringBufferBase[writeIndex] = _swapEndianU16(*sampleDataU16);
		sampleDataU16++;
		writeIndex++;
		writeIndex %= ringBufferSizeSamples;
	}while( (--numSamples) > 0 );
	MICStatus.drc[drcIndex].writeIndex = writeIndex;
}

sint32 MICInit(uint32 drcIndex, uint32 format, MEMPTR<micRingbuffer_t> micRingbuffer, MEMPTR<uint32be> outError)
{
	cemuLog_log(LogType::Force, "MICInit(drcIndex={}, format={}, ringbuffer=0x{:08x})", drcIndex, format, micRingbuffer.GetMPTR());
	if (drcIndex > 1 || drcIndex != 0)
	{
		if (!outError.IsNull())
			*outError = (uint32)MIC_RESULT::NOT_CONNECTED;
		return -1;
	}
	if (MICStatus.drc[drcIndex].isInited)
	{
		if (!outError.IsNull())
			*outError = (uint32)MIC_RESULT::ALREADY_OPEN;
		return -1;
	}
	if (micRingbuffer.IsNull())
	{
		if (!outError.IsNull())
			*outError = (uint32)MIC_RESULT::BAD_PARAM;
		return -1;
	}

	MICStatus.drc[drcIndex].ringbufferSampleData = memory_getPointerFromVirtualOffset(micRingbuffer->samples);
	MICStatus.drc[drcIndex].ringbufferSize = micRingbuffer->size;
	MICStatus.drc[drcIndex].readIndex = 0;
	MICStatus.drc[drcIndex].writeIndex = 0;
	MICStatus.drc[drcIndex].isInited = true;
	// init default states
	MICStatus.drc[drcIndex].echoCancellation = 1;
	MICStatus.drc[drcIndex].autoSelection = 1;
	MICStatus.drc[drcIndex].gainDB = 0x0100;
	MICStatus.drc[drcIndex].mute = 0;

	if (!outError.IsNull())
		*outError = 0;

	mic_initializeInputDevice();

	return (drcIndex == 0) ? MIC_HANDLE_DRC0 : MIC_HANDLE_DRC1;
}

sint32 MICOpen(uint32 micHandle)
{
	cemuLog_log(LogType::Force, "MICOpen(handle={})", micHandle);
	if (micHandle != MIC_HANDLE_DRC0 && micHandle != MIC_HANDLE_DRC1)
		return (sint32)MIC_RESULT::BAD_PARAM;
	uint32 drcIndex = (micHandle == MIC_HANDLE_DRC0) ? 0 : 1;
	if (!MICStatus.drc[drcIndex].isInited)
		return (sint32)MIC_RESULT::NOT_INITIALIZED;
	if (MICStatus.drc[drcIndex].isOpen)
		return (sint32)MIC_RESULT::ALREADY_OPEN;

	MICStatus.drc[drcIndex].isOpen = true;

#if BOOST_OS_IOS
	// Permission may have been granted after the game called MICInit.
	mic_initializeInputDevice();
#endif
	std::shared_lock lock(g_audioInputMutex);
	if (g_inputAudio)
		g_inputAudio->Play();

	return (sint32)MIC_RESULT::SUCCESS;
}

sint32 MICClose(uint32 micHandle)
{
	cemuLog_log(LogType::Force, "MICClose(handle={})", micHandle);
	if (micHandle != MIC_HANDLE_DRC0 && micHandle != MIC_HANDLE_DRC1)
		return (sint32)MIC_RESULT::BAD_PARAM;
	uint32 drcIndex = (micHandle == MIC_HANDLE_DRC0) ? 0 : 1;
	if (!MICStatus.drc[drcIndex].isInited)
		return (sint32)MIC_RESULT::NOT_INITIALIZED;
	if (!MICStatus.drc[drcIndex].isOpen)
		return (sint32)MIC_RESULT::ALREADY_OPEN;

	MICStatus.drc[drcIndex].isOpen = false;
	std::shared_lock lock(g_audioInputMutex);
	if (g_inputAudio)
		g_inputAudio->Stop();
	return (sint32)MIC_RESULT::SUCCESS;
}

sint32 MICGetStatus(uint32 micHandle, MEMPTR<micStatus_t> micStatus)
{
	if (micHandle != MIC_HANDLE_DRC0 && micHandle != MIC_HANDLE_DRC1)
		return (sint32)MIC_RESULT::BAD_PARAM;
	uint32 drcIndex = (micHandle == MIC_HANDLE_DRC0) ? 0 : 1;
	if (!MICStatus.drc[drcIndex].isInited)
		return (sint32)MIC_RESULT::NOT_INITIALIZED;
	if (micStatus.IsNull())
		return (sint32)MIC_RESULT::BAD_PARAM;

	MIC_STATUS_FLAGS micFlags = MIC_STATUS_FLAGS::FORMAT_PCM16 | MIC_STATUS_FLAGS::IS_CONNECTED;
	if (MICStatus.drc[drcIndex].isOpen)
		micFlags |= MIC_STATUS_FLAGS::IS_OPEN;
	micStatus->flags = micFlags;
	micStatus->numSamplesAvailable = mic_availableSamples(drcIndex);
	micStatus->readIndex = MICStatus.drc[drcIndex].readIndex;
	return (sint32)MIC_RESULT::SUCCESS;
}

sint32 MICGetState(uint32 micHandle, uint32 stateIdRaw, MEMPTR<uint32be> outValue)
{
	if (micHandle != MIC_HANDLE_DRC0 && micHandle != MIC_HANDLE_DRC1)
		return (sint32)MIC_RESULT::BAD_PARAM;
	uint32 drcIndex = (micHandle == MIC_HANDLE_DRC0) ? 0 : 1;
	if (!MICStatus.drc[drcIndex].isInited)
		return (sint32)MIC_RESULT::NOT_INITIALIZED;
	if (outValue.IsNull())
		return (sint32)MIC_RESULT::BAD_PARAM;

	MIC_STATEID stateId = (MIC_STATEID)stateIdRaw;
	if (stateId == MIC_STATEID::SAMPLERATE)
		*outValue = MIC_SAMPLERATE;
	else if (stateId == MIC_STATEID::AUTO_SELECTION)
		*outValue = MICStatus.drc[drcIndex].autoSelection;
	else if (stateId == MIC_STATEID::GAIN_MIN)
		*outValue = 0x0000;
	else if (stateId == MIC_STATEID::GAIN_MAX)
		*outValue = 0x0200;
	else if (stateId == MIC_STATEID::GAIN_STEP)
		*outValue = 0x0001;
	else if (stateId == MIC_STATEID::ECHO_CANCELLATION)
		*outValue = MICStatus.drc[drcIndex].echoCancellation;
	else if (stateId == MIC_STATEID::GAIN_DB)
		*outValue = MICStatus.drc[drcIndex].gainDB;
	else if (stateId == MIC_STATEID::MUTE)
		*outValue = MICStatus.drc[drcIndex].mute;
	else
		return (sint32)MIC_RESULT::BAD_PARAM;

	return (sint32)MIC_RESULT::SUCCESS;
}

sint32 MICSetState(uint32 micHandle, uint32 stateIdRaw, uint32 newValue)
{
	if (micHandle != MIC_HANDLE_DRC0 && micHandle != MIC_HANDLE_DRC1)
		return (sint32)MIC_RESULT::BAD_PARAM;
	uint32 drcIndex = (micHandle == MIC_HANDLE_DRC0) ? 0 : 1;
	if (!MICStatus.drc[drcIndex].isInited)
		return (sint32)MIC_RESULT::NOT_INITIALIZED;

	MIC_STATEID stateId = (MIC_STATEID)stateIdRaw;
	if (stateId == MIC_STATEID::ECHO_CANCELLATION)
		MICStatus.drc[drcIndex].echoCancellation = (newValue != 0) ? 1 : 0;
	else if (stateId == MIC_STATEID::AUTO_SELECTION)
		MICStatus.drc[drcIndex].autoSelection = (newValue != 0) ? 1 : 0;
	else if (stateId == MIC_STATEID::GAIN_DB)
		MICStatus.drc[drcIndex].gainDB = newValue;
	else if (stateId == MIC_STATEID::MUTE)
		MICStatus.drc[drcIndex].mute = (newValue != 0) ? 1 : 0;
	else
		return (sint32)MIC_RESULT::BAD_PARAM;

	return (sint32)MIC_RESULT::SUCCESS;
}

sint32 MICSetDataConsumed(uint32 micHandle, uint32 numConsumedSamples)
{
	if (micHandle != MIC_HANDLE_DRC0 && micHandle != MIC_HANDLE_DRC1)
		return (sint32)MIC_RESULT::BAD_PARAM;
	uint32 drcIndex = (micHandle == MIC_HANDLE_DRC0) ? 0 : 1;
	if (!MICStatus.drc[drcIndex].isInited)
		return (sint32)MIC_RESULT::NOT_INITIALIZED;
	if (!MICStatus.drc[drcIndex].isOpen)
		return (sint32)MIC_RESULT::NOT_OPEN;

	sint32 ringBufferSizeSamples = MICStatus.drc[drcIndex].ringbufferSize / (sint32)sizeof(uint16);
	if (ringBufferSizeSamples <= 0)
		return (sint32)MIC_RESULT::BAD_PARAM;

	if (mic_availableSamples(drcIndex) < (sint32)numConsumedSamples)
	{
		MICStatus.drc[drcIndex].readIndex = MICStatus.drc[drcIndex].writeIndex;
		return -81;
	}
	else
	{
		MICStatus.drc[drcIndex].readIndex += numConsumedSamples;
		MICStatus.drc[drcIndex].readIndex %= ringBufferSizeSamples;
		return 0;
	}
}

void mic_updateDevicePlayState(bool isPlaying)
{
	if (g_inputAudio)
	{
		if (isPlaying)
			g_inputAudio->Play();
		else
			g_inputAudio->Stop();
	}
}

void mic_updateOnAXFrame()
{
	sint32 drcIndex = 0;
	if( mic_isActive(0) == false ) 
	{
		drcIndex = 1;
		if (mic_isActive(1) == false) 
		{
			std::shared_lock lock(g_audioInputMutex);
			mic_updateDevicePlayState(false);
			return;
		}
	}

#if BOOST_OS_IOS
	if (s_inputDeviceRefreshRequested.exchange(false, std::memory_order_acq_rel))
		mic_initializeInputDevice();
#endif
	std::shared_lock lock(g_audioInputMutex);
	mic_updateDevicePlayState(true);
	if (g_inputAudio)
	{
		sint16 micSampleData[MIC_SAMPLES_PER_3MS_32KHZ];
		g_inputAudio->ConsumeBlock(micSampleData);
		if (MICStatus.drc[drcIndex].mute)
			memset(micSampleData, 0x00, sizeof(micSampleData));
		mic_feedSamples(drcIndex, micSampleData, MIC_SAMPLES_PER_3MS_32KHZ);
	}
	else
	{
		// AX invokes us once per 3 ms, including when capture is unavailable.
		// Feeding 1000 silence samples per callback overran the guest's timing.
#if BOOST_OS_IOS
		const sint32 micSampleCount = MIC_SAMPLES_PER_3MS_32KHZ;
#else
		const sint32 micSampleCount = 32000 / 32;
#endif
		sint16 micSampleData[micSampleCount];

		auto controller = InputManager::instance().get_vpad_controller(drcIndex);
		if( controller && controller->is_mic_active() )
		{
			for(sint32 i=0; i<micSampleCount; i++)
			{
				micSampleData[i] = (sint16)(sin((float)GetTickCount()*0.1f+sin((float)GetTickCount()*0.0001f)*100.0f)*30000.0f);
			}
		}
		else
		{
			memset(micSampleData, 0x00, sizeof(micSampleData));
		}
		mic_feedSamples(drcIndex, micSampleData, micSampleCount);
	}
}

namespace mic
{
	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "mic";
		}

		void RPLMapped() override
		{
			// Games poll these exports continuously. Keep API tracing opt-in;
			// microphone errors still use Force at the failure site.
			cafeExportRegisterFunc(MICInit, "mic", "MICInit", LogType::SoundAPI);
			cafeExportRegisterFunc(MICOpen, "mic", "MICOpen", LogType::SoundAPI);
			cafeExportRegisterFunc(MICClose, "mic", "MICClose", LogType::SoundAPI);
			cafeExportRegisterFunc(MICGetStatus, "mic", "MICGetStatus", LogType::SoundAPI);
			cafeExportRegisterFunc(MICGetState, "mic", "MICGetState", LogType::SoundAPI);
			cafeExportRegisterFunc(MICSetState, "mic", "MICSetState", LogType::SoundAPI);
			cafeExportRegisterFunc(MICSetDataConsumed, "mic", "MICSetDataConsumed", LogType::SoundAPI);
		}
	}s_COSmicModule;

	COSModule* GetModule()
	{
		return &s_COSmicModule;
	}

};
