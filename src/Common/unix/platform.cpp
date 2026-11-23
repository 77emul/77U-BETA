#include <cstdint>
#include <ctime>

uint32_t GetTickCount()
{
#if BOOST_OS_LINUX
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (1000 * ts.tv_sec + ts.tv_nsec / 1000000);
#elif BOOST_OS_MACOS || BOOST_OS_IOS
	// Match Win32 GetTickCount semantics: the 32-bit millisecond value wraps.
	return static_cast<uint32_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000);
#elif BOOST_OS_BSD
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (1000 * ts.tv_sec + ts.tv_nsec / 1000000);
#endif

}
