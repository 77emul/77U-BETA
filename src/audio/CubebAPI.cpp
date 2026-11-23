#include "CubebAPI.h"

#if BOOST_OS_WINDOWS
#include <combaseapi.h>
#include <mmreg.h>
#include <mmsystem.h>
#pragma comment(lib, "Avrt.lib")
#pragma comment(lib, "ksuser.lib")
#endif


static void state_cb(cubeb_stream* stream, void* user, cubeb_state state)
{
	if (!stream)
		return;

	/*switch (state)
	{
	case CUBEB_STATE_STARTED:
		fprintf(stderr, "stream started\n");
		break;
	case CUBEB_STATE_STOPPED:
		fprintf(stderr, "stream stopped\n");
		break;
	case CUBEB_STATE_DRAINED:
		fprintf(stderr, "stream drained\n");
		break;
	default:
		fprintf(stderr, "unknown stream state %d\n", state);
	}*/
}

long CubebAPI::data_cb(cubeb_stream* stream, void* user, const void* inputbuffer, void* outputbuffer, long nframes)
{
	auto* thisptr = (CubebAPI*)user;
#if BOOST_OS_IOS
	s_dataCallbacks.fetch_add(1, std::memory_order_relaxed);
#endif
	//const auto size = (size_t)thisptr->m_bytesPerBlock; // (size_t)nframes* thisptr->m_channels;

	// m_bytesPerBlock = samples_per_block * channels * (bits_per_sample / 8);
	const auto size = (size_t)nframes * thisptr->m_channels * (thisptr->m_bitsPerSample/8);

	std::unique_lock lock(thisptr->m_mutex);
	if (thisptr->m_buffer.empty())
	{
#if BOOST_OS_IOS
		s_emptyCallbacks.fetch_add(1, std::memory_order_relaxed);
#endif
		// we got no data, just write silence
		memset(outputbuffer, 0x00, size);
	}
	else
	{
#if BOOST_OS_IOS
		s_callbacksWithData.fetch_add(1, std::memory_order_relaxed);
#endif
		const auto copied = thisptr->m_buffer.read(outputbuffer, size);
		lock.unlock();
		// fill rest with silence
		if (copied != size)
			memset((uint8*)outputbuffer + copied, 0x00, size - copied);
	}

	return nframes;
}

CubebAPI::CubebAPI(cubeb_devid devid, uint32 samplerate, uint32 channels, uint32 samples_per_block,
                   uint32 bits_per_sample)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	cubeb_stream_params output_params;

	output_params.format = CUBEB_SAMPLE_S16LE;
	output_params.rate = samplerate;
	output_params.channels = channels;
	output_params.prefs = CUBEB_STREAM_PREF_NONE;

	switch (channels)
	{
	case 8:
		output_params.layout = CUBEB_LAYOUT_3F4_LFE;
		break;
	case 6:
		output_params.layout = CUBEB_LAYOUT_3F2_LFE_BACK;
		break;
	case 4:
		output_params.layout = CUBEB_LAYOUT_QUAD;
		break;
	case 2:
		output_params.layout = CUBEB_LAYOUT_STEREO;
		break;
	default:
		output_params.layout = CUBEB_LAYOUT_MONO;
		break;
	}

	uint32 latency = 1;
	const int minLatencyResult = cubeb_get_min_latency(s_context, &output_params, &latency);

#if BOOST_OS_IOS
	s_minLatencyResult.store(minLatencyResult, std::memory_order_relaxed);
	s_sampleRate.store(samplerate, std::memory_order_relaxed);
	s_channels.store(channels, std::memory_order_relaxed);
	s_streamFormat.store((uint32)output_params.format, std::memory_order_relaxed);
	s_channelLayout.store((uint32)output_params.layout, std::memory_order_relaxed);
	s_latencyFrames.store(latency, std::memory_order_relaxed);
	s_selectedDefaultDevice.store(devid == nullptr, std::memory_order_relaxed);
	const bool paramsValid = samplerate >= 1000 && samplerate <= 192000 &&
	                         channels >= 1 && channels <= UINT8_MAX &&
	                         latency >= 1 && latency <= 96000 &&
	                         (output_params.format == CUBEB_SAMPLE_S16LE ||
	                          output_params.format == CUBEB_SAMPLE_S16BE ||
	                          output_params.format == CUBEB_SAMPLE_FLOAT32LE ||
	                          output_params.format == CUBEB_SAMPLE_FLOAT32BE);
	s_paramsValidBeforeInit.store(paramsValid, std::memory_order_relaxed);
#endif

	m_buffer.reserve((size_t)m_bytesPerBlock * kBlockCount);

	const int streamInitResult = cubeb_stream_init(s_context, &m_stream, "Cemu Cubeb output",
	                                              nullptr, nullptr,
	                                              devid, &output_params,
	                                              latency, data_cb, state_cb, this);
#if BOOST_OS_IOS
	s_streamInitResult.store(streamInitResult, std::memory_order_relaxed);
#endif
	if (streamInitResult != CUBEB_OK)
	{
#if BOOST_OS_IOS
		s_streamCreateFailures.fetch_add(1, std::memory_order_relaxed);
#endif
		throw std::runtime_error("can't initialize cubeb device");
	}
#if BOOST_OS_IOS
	s_streamCreateSuccesses.fetch_add(1, std::memory_order_relaxed);
#endif
}

CubebAPI::~CubebAPI()
{
	if (m_stream)
	{
		Stop();
		cubeb_stream_destroy(m_stream);
	}
}

bool CubebAPI::NeedAdditionalBlocks() const
{
	std::shared_lock lock(m_mutex);
	return m_buffer.size() < GetAudioDelay() * m_bytesPerBlock;
}

bool CubebAPI::FeedBlock(sint16* data)
{
#if BOOST_OS_IOS
	s_feedBlocks.fetch_add(1, std::memory_order_relaxed);
	const size_t sampleCount = m_bytesPerBlock / sizeof(sint16);
	for (size_t i = 0; i < sampleCount; ++i)
	{
		if (data[i] != 0)
		{
			s_nonSilentFeedBlocks.fetch_add(1, std::memory_order_relaxed);
			break;
		}
	}
#endif

	std::unique_lock lock(m_mutex);
	if (m_buffer.capacity() <= m_buffer.size() + m_bytesPerBlock)
	{
#if BOOST_OS_IOS
		s_rejectedFeedBlocks.fetch_add(1, std::memory_order_relaxed);
#endif
		cemuLog_logDebug(LogType::SoundAPI, "dropped direct sound block since too many buffers are queued");
		return false;
	}

	m_buffer.write(reinterpret_cast<const uint8*>(data), m_bytesPerBlock);
	return true;
}

bool CubebAPI::Play()
{
	if (m_is_playing)
		return true;

#if BOOST_OS_IOS
	s_playAttempts.fetch_add(1, std::memory_order_relaxed);
#endif
	if (cubeb_stream_start(m_stream) == CUBEB_OK)
	{
		m_is_playing = true;
#if BOOST_OS_IOS
		s_playSuccesses.fetch_add(1, std::memory_order_relaxed);
#endif
		return true;
	}

	return false;
}

bool CubebAPI::Stop()
{
	if (!m_is_playing)
		return true;

	if (cubeb_stream_stop(m_stream) == CUBEB_OK)
	{
		m_is_playing = false;
		return true;
	}

	return false;
}

void CubebAPI::SetVolume(sint32 volume)
{
	IAudioAPI::SetVolume(volume);
	cubeb_stream_set_volume(m_stream, (float)volume / 100.0f);
}


bool CubebAPI::InitializeStatic()
{
#if BOOST_OS_IOS
	s_initializeAttempts.fetch_add(1, std::memory_order_relaxed);
#endif
	if (cubeb_init(&s_context, "Cemu Cubeb", nullptr))
	{
		cemuLog_log(LogType::Force, "can't create cubeb audio api");
		return false;
	}
#if BOOST_OS_IOS
	s_initializeSuccesses.fetch_add(1, std::memory_order_relaxed);
#endif
	return true;
}

CubebAPI::Diagnostics CubebAPI::GetDiagnostics()
{
	Diagnostics diagnostics{};
#if BOOST_OS_IOS
	diagnostics.initializeAttempts = s_initializeAttempts.load(std::memory_order_relaxed);
	diagnostics.initializeSuccesses = s_initializeSuccesses.load(std::memory_order_relaxed);
	diagnostics.streamCreateSuccesses = s_streamCreateSuccesses.load(std::memory_order_relaxed);
	diagnostics.streamCreateFailures = s_streamCreateFailures.load(std::memory_order_relaxed);
	diagnostics.playAttempts = s_playAttempts.load(std::memory_order_relaxed);
	diagnostics.playSuccesses = s_playSuccesses.load(std::memory_order_relaxed);
	diagnostics.feedBlocks = s_feedBlocks.load(std::memory_order_relaxed);
	diagnostics.nonSilentFeedBlocks = s_nonSilentFeedBlocks.load(std::memory_order_relaxed);
	diagnostics.rejectedFeedBlocks = s_rejectedFeedBlocks.load(std::memory_order_relaxed);
	diagnostics.dataCallbacks = s_dataCallbacks.load(std::memory_order_relaxed);
	diagnostics.callbacksWithData = s_callbacksWithData.load(std::memory_order_relaxed);
	diagnostics.emptyCallbacks = s_emptyCallbacks.load(std::memory_order_relaxed);
	diagnostics.minLatencyResult = s_minLatencyResult.load(std::memory_order_relaxed);
	diagnostics.streamInitResult = s_streamInitResult.load(std::memory_order_relaxed);
	diagnostics.sampleRate = s_sampleRate.load(std::memory_order_relaxed);
	diagnostics.channels = s_channels.load(std::memory_order_relaxed);
	diagnostics.streamFormat = s_streamFormat.load(std::memory_order_relaxed);
	diagnostics.channelLayout = s_channelLayout.load(std::memory_order_relaxed);
	diagnostics.latencyFrames = s_latencyFrames.load(std::memory_order_relaxed);
	diagnostics.selectedDefaultDevice = s_selectedDefaultDevice.load(std::memory_order_relaxed);
	diagnostics.paramsValidBeforeInit = s_paramsValidBeforeInit.load(std::memory_order_relaxed);
#endif
	return diagnostics;
}

void CubebAPI::Destroy()
{
	if (s_context)
		cubeb_destroy(s_context);
}

std::vector<IAudioAPI::DeviceDescriptionPtr> CubebAPI::GetDevices()
{
	std::vector<DeviceDescriptionPtr> result;
	// Add the default device to the list
	auto defaultDevice = std::make_shared<CubebDeviceDescription>(nullptr, "default", L"Default Device");
	result.emplace_back(defaultDevice);

	cubeb_device_collection devices;
	if (cubeb_enumerate_devices(s_context, CUBEB_DEVICE_TYPE_OUTPUT, &devices) != CUBEB_OK)
		return result;

	result.reserve(devices.count + 1); // The default device already occupies one element

	for (size_t i = 0; i < devices.count; ++i)
	{
		// const auto& device = devices.device[i];
		if (devices.device[i].state == CUBEB_DEVICE_STATE_ENABLED)
		{
			auto device = std::make_shared<CubebDeviceDescription>(devices.device[i].devid, devices.device[i].device_id,
																   boost::nowide::widen(
																	   devices.device[i].friendly_name));
			result.emplace_back(device);
		}
	}

	cubeb_device_collection_destroy(s_context, &devices);

	return result;
}
