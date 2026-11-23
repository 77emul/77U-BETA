#pragma once

#include "IAudioAPI.h"
#include "AudioByteQueue.h"

#include <cubeb/cubeb.h>

#include <memory>

class CubebAPI : public IAudioAPI
{
public:
	struct Diagnostics
	{
		uint64 initializeAttempts{};
		uint64 initializeSuccesses{};
		uint64 streamCreateSuccesses{};
		uint64 streamCreateFailures{};
		uint64 playAttempts{};
		uint64 playSuccesses{};
		uint64 feedBlocks{};
		uint64 nonSilentFeedBlocks{};
		uint64 rejectedFeedBlocks{};
		uint64 dataCallbacks{};
		uint64 callbacksWithData{};
		uint64 emptyCallbacks{};
		sint32 minLatencyResult{};
		sint32 streamInitResult{};
		uint32 sampleRate{};
		uint32 channels{};
		uint32 streamFormat{};
		uint32 channelLayout{};
		uint32 latencyFrames{};
		bool selectedDefaultDevice{};
		bool paramsValidBeforeInit{};
	};

	class CubebDeviceDescription : public DeviceDescription
	{
	public:
		CubebDeviceDescription(cubeb_devid devid, std::string device_id, const std::wstring& name)
			: DeviceDescription(name), m_devid(devid), m_device_id(std::move(device_id)) { }

		std::wstring GetIdentifier() const override { return  boost::nowide::widen(m_device_id); }
		cubeb_devid GetDeviceId() const { return m_devid; }

	private:
		cubeb_devid m_devid;
		std::string m_device_id;
	};

	using CubebDeviceDescriptionPtr = std::shared_ptr<CubebDeviceDescription>;

	CubebAPI(cubeb_devid devid, uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);
	~CubebAPI();

	AudioAPI GetType() const override { return Cubeb; }
	bool NeedAdditionalBlocks() const override;
	bool FeedBlock(sint16* data) override;
	bool Play() override;
	bool Stop() override;
	void SetVolume(sint32 volume) override;

	static std::vector<DeviceDescriptionPtr> GetDevices();

	static bool InitializeStatic();
	static void Destroy();
	static Diagnostics GetDiagnostics();

private:
	inline static cubeb* s_context = nullptr;
#if BOOST_OS_IOS
	inline static std::atomic<uint64> s_initializeAttempts{};
	inline static std::atomic<uint64> s_initializeSuccesses{};
	inline static std::atomic<uint64> s_streamCreateSuccesses{};
	inline static std::atomic<uint64> s_streamCreateFailures{};
	inline static std::atomic<uint64> s_playAttempts{};
	inline static std::atomic<uint64> s_playSuccesses{};
	inline static std::atomic<uint64> s_feedBlocks{};
	inline static std::atomic<uint64> s_nonSilentFeedBlocks{};
	inline static std::atomic<uint64> s_rejectedFeedBlocks{};
	inline static std::atomic<uint64> s_dataCallbacks{};
	inline static std::atomic<uint64> s_callbacksWithData{};
	inline static std::atomic<uint64> s_emptyCallbacks{};
	inline static std::atomic<sint32> s_minLatencyResult{-2147483647 - 1};
	inline static std::atomic<sint32> s_streamInitResult{-2147483647 - 1};
	inline static std::atomic<uint32> s_sampleRate{};
	inline static std::atomic<uint32> s_channels{};
	inline static std::atomic<uint32> s_streamFormat{};
	inline static std::atomic<uint32> s_channelLayout{};
	inline static std::atomic<uint32> s_latencyFrames{};
	inline static std::atomic<bool> s_selectedDefaultDevice{};
	inline static std::atomic<bool> s_paramsValidBeforeInit{};
#endif

	cubeb_stream* m_stream = nullptr;
	bool m_is_playing = false;

	mutable std::shared_mutex m_mutex;
	AudioByteQueue m_buffer;
	static long data_cb(cubeb_stream* stream, void* user, const void* inputbuffer, void* outputbuffer, long nframes);
};
