#include "CemuLogging.h"
#include "Common/precompiled.h"
#include "util/helpers/helpers.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"

#include <mutex>
#include <condition_variable>
#include <chrono>

#include <fmt/printf.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

uint64 s_loggingFlagMask = cemuLog_getFlag(LogType::Force);
static std::atomic_bool s_loggingOutputEnabled{true};

void cemuLog_setOutputEnabled(bool enabled)
{
	s_loggingOutputEnabled.store(enabled, std::memory_order_release);
}

bool cemuLog_isOutputEnabled()
{
	return s_loggingOutputEnabled.load(std::memory_order_acquire);
}

class LoggingDispatcher
{
  private:
	static inline class DefaultLoggingCallbacks : public LoggingCallbacks
	{
	} s_defaultCallbacks;

	LoggingCallbacks* m_loggingCallbacks = &s_defaultCallbacks;
	std::shared_mutex m_mutex;

  public:
	void Log(std::string_view filter, std::string_view message)
	{
		std::shared_lock lock(m_mutex);
		m_loggingCallbacks->Log(filter, message);
	}

	void Log(std::string_view message)
	{
		Log("", message);
	}

	void Log(std::string_view filter, std::wstring_view message)
	{
		std::shared_lock lock(m_mutex);
		m_loggingCallbacks->Log(filter, message);
	}

	void Log(std::wstring_view message)
	{
		Log("", message);
	}

	void setCallbacks(LoggingCallbacks* loggingCallbacks)
	{
		std::unique_lock lock(m_mutex);
		cemu_assert_debug(m_loggingCallbacks == &s_defaultCallbacks);
		m_loggingCallbacks = loggingCallbacks;
	}

	void clearCallbacks()
	{
		std::unique_lock lock(m_mutex);
		cemu_assert_debug(m_loggingCallbacks != &s_defaultCallbacks);
		m_loggingCallbacks = &s_defaultCallbacks;
	}
} s_loggingDispatcher;

void cemuLog_setCallbacks(LoggingCallbacks* loggingCallbacks)
{
	s_loggingDispatcher.setCallbacks(loggingCallbacks);
}

void cemuLog_clearCallbacks()
{
	s_loggingDispatcher.clearCallbacks();
}

struct _LogContext
{
	std::condition_variable_any log_condition;
	std::recursive_mutex log_mutex;
	std::ofstream file_stream;
	std::vector<std::string> text_cache;
	bool writerBusy = false;
	std::thread log_writer;
	std::atomic<bool> threadRunning = false;

	~_LogContext()
	{
		// safely shut down the log writer thread
		threadRunning.store(false);
		if (log_writer.joinable())
		{
			log_condition.notify_one();
			log_writer.join();
		}
	}
}LogContext;

const std::map<LogType, std::string> g_logging_window_mapping
{
	{LogType::UnsupportedAPI,     "Unsupported API calls"},
	{LogType::APIErrors, 		  "Invalid API usage"},
	{LogType::CoreinitLogging,    "Coreinit Logging"},
	{LogType::CoreinitFile,       "Coreinit File-Access"},
	{LogType::CoreinitThreadSync, "Coreinit Thread-Synchronization"},
	{LogType::CoreinitMem,        "Coreinit Memory"},
	{LogType::CoreinitMP,         "Coreinit MP"},
	{LogType::CoreinitThread,     "Coreinit Thread"},
	{LogType::NN_NFP,             "nn::nfp"},
	{LogType::NN_FP,              "nn::fp"},
	{LogType::NN_BOSS,            "nn::boss"},
	{LogType::GX2,                "GX2"},
	{LogType::SoundAPI,           "Audio"},
	{LogType::InputAPI,           "Input"},
	{LogType::Socket,             "Socket"},
	{LogType::Save,               "Save"},
	{LogType::H264,               "H264"},
	{LogType::NFC,                "NFC"},
	{LogType::NTAG,               "NTAG"},
	{LogType::Patches,            "Graphic pack patches"},
	{LogType::TextureCache,       "Texture cache"},
	{LogType::TextureReadback,    "Texture readback"},
	{LogType::OpenGLLogging,      "OpenGL debug output"},
	{LogType::VulkanValidation,   "Vulkan validation layer"},
};

bool cemuLog_advancedPPCLoggingEnabled()
{
	return GetConfig().advanced_ppc_logging;
}

void cemuLog_thread()
{
	SetThreadName("cemuLog_thread");
	auto lastFlush = std::chrono::steady_clock::now();
	while (true)
	{
		std::unique_lock lock(LogContext.log_mutex);
		LogContext.log_condition.wait(lock, [] {
			return !LogContext.text_cache.empty() ||
				!LogContext.threadRunning.load(std::memory_order::relaxed);
		});
		if (LogContext.text_cache.empty())
		{
			LogContext.file_stream.flush();
			return;
		}

		std::vector<std::string> cache_copy;
		cache_copy.swap(LogContext.text_cache);
		LogContext.writerBusy = true;
		lock.unlock();

		for (const auto& entry : cache_copy)
			LogContext.file_stream.write(entry.data(), entry.size());

		LogContext.file_stream.flush();
		const auto now = std::chrono::steady_clock::now();
		lastFlush = now;
		lock.lock();
		LogContext.writerBusy = false;
		lock.unlock();
		LogContext.log_condition.notify_all();
	}
}

fs::path cemuLog_GetLogFilePath()
{
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return ActiveSettings::GetUserDataPath("Logs/log.txt");
#else
    return ActiveSettings::GetUserDataPath("log.txt");
#endif
}

void cemuLog_createLogFile(bool triggeredByCrash)
{
	// A crash report must remain available even when routine diagnostics are
	// disabled. The process is terminating, so enabling output here cannot add
	// runtime overhead to normal gameplay.
	if (triggeredByCrash)
		cemuLog_setOutputEnabled(true);
	else if (!cemuLog_isOutputEnabled())
		return;

	std::unique_lock lock(LogContext.log_mutex);
	if (LogContext.file_stream.is_open())
		return;

	const auto path = cemuLog_GetLogFilePath();
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Startup normally archives the preceding session. If that move failed,
	// append instead of destroying the remaining evidence.
	LogContext.file_stream.open(path, std::ios::out | std::ios::app);
#else
	LogContext.file_stream.open(path, std::ios::out);
#endif
	if (LogContext.file_stream.fail())
	{
		cemu_assert_debug(false);
		return;
	}

	LogContext.threadRunning.store(true);
	LogContext.log_writer = std::thread(cemuLog_thread);
	lock.unlock();
}

void cemuLog_writeLineToLog(std::string_view text, bool date, bool new_line)
{
	if (!cemuLog_isOutputEnabled())
		return;

	std::string line;
	if (date)
	{
		const auto now = std::chrono::system_clock::now();
		const auto temp_time = std::chrono::system_clock::to_time_t(now);
		struct tm time_buf{};
#if defined(_WIN32)
		localtime_s(&time_buf, &temp_time);
#else
		localtime_r(&temp_time, &time_buf);
#endif

		auto time_str = fmt::format("[{:02d}:{:02d}:{:02d}.{:03d}] ", time_buf.tm_hour, time_buf.tm_min, time_buf.tm_sec,
			std::chrono::duration_cast<std::chrono::milliseconds>(now - std::chrono::time_point_cast<std::chrono::seconds>(now)).count());

		line.reserve(time_str.size() + text.size() + (new_line ? 1 : 0));
		line.append(time_str);
	}
	else
	{
		line.reserve(text.size() + (new_line ? 1 : 0));
	}

	line.append(text);
	if (new_line)
		line.push_back('\n');

	std::unique_lock lock(LogContext.log_mutex);
	const bool wasEmpty = LogContext.text_cache.empty();
	LogContext.text_cache.emplace_back(std::move(line));
	lock.unlock();

	if (wasEmpty)
		LogContext.log_condition.notify_all();
}

bool cemuLog_log(LogType type, std::string_view text)
{
	if (!cemuLog_isLoggingEnabled(type))
		return false;

	if (LaunchSettings::Verbose())
		std::cout << text << std::endl;

	cemuLog_writeLineToLog(text);

	const auto it = std::find_if(g_logging_window_mapping.cbegin(), g_logging_window_mapping.cend(),
		[type](const auto& entry) { return entry.first == type; });
	if (it == g_logging_window_mapping.cend())
		s_loggingDispatcher.Log(text);
	else
		s_loggingDispatcher.Log(it->second, text);

	return true;
}

bool cemuLog_log(LogType type, std::u8string_view text)
{
	std::basic_string_view<char> s((char*)text.data(), text.size());
	return cemuLog_log(type, s);
}

void cemuLog_logHexDump(LogType type, const void* data, size_t size, size_t lineSize)
{
	const uint8* dataU8 = static_cast<const uint8*>(data);
	std::vector<char> hexLine;
	hexLine.resize(6 + lineSize * 3 + 1, ' ');
	for (size_t i = 0; i < size; i += lineSize)
	{
		std::snprintf(hexLine.data(), 7, "%04X: ", static_cast<unsigned>(i));
		size_t remainingSize = std::min<size_t>(lineSize, size - i);
		for (size_t j=0; j<remainingSize; j++)
			std::snprintf(hexLine.data() + 6 + j * 3, 4, "%02X ", dataU8[j]);
		dataU8 += remainingSize;
		if (remainingSize == lineSize)
			hexLine.resize(6 + remainingSize * 3 + 1);
		cemuLog_log(type, hexLine.data());
	}
}

void cemuLog_waitForFlush()
{
	if (!cemuLog_isOutputEnabled())
		return;

	cemuLog_createLogFile(false);
	std::unique_lock lock(LogContext.log_mutex);
	if (!LogContext.file_stream.is_open())
		return;
	LogContext.log_condition.wait(lock, [] {
		return LogContext.text_cache.empty() && !LogContext.writerBusy;
	});
	LogContext.file_stream.flush();
}

// used to atomically write multiple lines to the log
std::unique_lock<decltype(LogContext.log_mutex)> cemuLog_acquire()
{
	return std::unique_lock(LogContext.log_mutex);
}

void cemuLog_setActiveLoggingFlags(uint64 flagMask)
{
	s_loggingFlagMask = flagMask | cemuLog_getFlag(LogType::Force);
}
