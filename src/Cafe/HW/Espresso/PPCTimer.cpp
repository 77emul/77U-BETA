#include "Cafe/HW/Espresso/Const.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "config/ActiveSettings.h"
#include "util/helpers/fspinlock.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Common/cpu_features.h"

#if defined(ARCH_X86_64)
#include <immintrin.h>
#pragma intrinsic(__rdtsc)
#endif

uint64 _rdtscLastMeasure = 0;
uint64 _rdtscFrequency = 0;

struct uint128_t
{
	uint64 low;
	uint64 high;
};

static_assert(sizeof(uint128_t) == 16);

uint128_t _rdtscAcc{};

uint64 muldiv64(uint64 a, uint64 b, uint64 d)
{
	uint64 diva = a / d;
	uint64 moda = a % d;
	uint64 divb = b / d;
	uint64 modb = b % d;
	return diva * b + moda * divb + moda * modb / d;
}

uint64 PPCTimer_estimateRDTSCFrequency()
{
#if defined(__aarch64__)
	uint64 freq = 0;
	asm volatile("mrs %0, cntfrq_el0" : "=r" (freq));
	if (freq != 0)
		return freq;
#endif

    #if defined(ARCH_X86_64)
	if (!g_CPUFeatures.x86.invariant_tsc)
		cemuLog_log(LogType::Force, "Invariant TSC not supported");
    #endif

	_mm_mfence();
	uint64 tscStart = __rdtsc();
	unsigned int startTime = GetTickCount();
	HRTick startTick = HighResolutionTimer::now().getTick();
	// wait roughly 3 seconds
	while (true)
	{
		if ((GetTickCount() - startTime) >= 3000)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	_mm_mfence();
	HRTick stopTick = HighResolutionTimer::now().getTick();
	uint64 tscEnd = __rdtsc();
	// derive frequency approximation from measured time difference
	uint64 tsc_diff = tscEnd - tscStart;
	uint64 hrtFreq = 0;
	uint64 hrtDiff = HighResolutionTimer::getTimeDiffEx(startTick, stopTick, hrtFreq);
	uint64 tsc_freq = muldiv64(tsc_diff, hrtFreq, hrtDiff);

	// uint64 freqMultiplier = tsc_freq / hrtFreq;
	//cemuLog_log(LogType::Force, "RDTSC measurement test:");
	//cemuLog_log(LogType::Force, "TSC-diff:   0x{:016x}", tsc_diff);
	//cemuLog_log(LogType::Force, "TSC-freq:   0x{:016x}", tsc_freq);
	//cemuLog_log(LogType::Force, "HPC-diff:   0x{:016x}", qpc_diff);
	//cemuLog_log(LogType::Force, "HPC-freq:   0x{:016x}", (uint64)qpc_freq.QuadPart);
	//cemuLog_log(LogType::Force, "Multiplier: 0x{:016x}", freqMultiplier);

	return tsc_freq;
}

int PPCTimer_initThread()
{
	_rdtscFrequency = PPCTimer_estimateRDTSCFrequency();
	return 0;
}

void PPCTimer_init()
{
#if defined(__aarch64__)
	_rdtscFrequency = PPCTimer_estimateRDTSCFrequency();
	_rdtscLastMeasure = __rdtsc();
#else
	std::thread t(PPCTimer_initThread);
	t.detach();
	_rdtscLastMeasure = __rdtsc();
#endif
}

uint64 _tickSummary = 0;

namespace
{
	struct PPCMFTBDiagnosticSite
	{
		std::atomic<uint64> key{};
		std::atomic<uint32> partMask{};
		std::atomic<uint64> interpreterCount{};
		std::atomic<uint64> jitCount{};
		std::atomic<uint64> firstInterpreterSourceValue{};
		std::atomic<uint64> firstJitSourceValue{};
	};

	std::atomic<uint64> s_mftbExecutionCount{};
	std::atomic<uint64> s_mftbInterpreterCount{};
	std::atomic<uint64> s_mftbJitCount{};
	std::atomic<uint64> s_mftbEngineTransitionCount{};
	std::atomic<uint64> s_mftbInterpreterToJitTransitions{};
	std::atomic<uint64> s_mftbJitToInterpreterTransitions{};
	std::array<std::atomic<uint32>, Espresso::CORE_COUNT> s_mftbLastModeByCore{};
	std::array<PPCMFTBDiagnosticSite, PPC_MFTB_DIAGNOSTIC_SITE_LIMIT> s_mftbSites{};
	std::atomic<uint32> s_mftbSiteCount{};

	FSpinlock s_mftbClockSampleLock;
	HRTick s_mftbClockLastHostTick{};
	uint64 s_mftbClockLastGuestTick{};
	std::atomic<double> s_osGetSystemTimeTicksPerSecond{};
	std::atomic<double> s_estimatedGuestClockPercent{};

	void PPCTimebaseDiagnostics_Reset()
	{
		s_mftbExecutionCount.store(0, std::memory_order_relaxed);
		s_mftbInterpreterCount.store(0, std::memory_order_relaxed);
		s_mftbJitCount.store(0, std::memory_order_relaxed);
		s_mftbEngineTransitionCount.store(0, std::memory_order_relaxed);
		s_mftbInterpreterToJitTransitions.store(0, std::memory_order_relaxed);
		s_mftbJitToInterpreterTransitions.store(0, std::memory_order_relaxed);
		for (auto& lastMode : s_mftbLastModeByCore)
			lastMode.store(0, std::memory_order_relaxed);
		for (auto& site : s_mftbSites)
		{
			site.key.store(0, std::memory_order_relaxed);
			site.partMask.store(0, std::memory_order_relaxed);
			site.interpreterCount.store(0, std::memory_order_relaxed);
			site.jitCount.store(0, std::memory_order_relaxed);
			site.firstInterpreterSourceValue.store(0, std::memory_order_relaxed);
			site.firstJitSourceValue.store(0, std::memory_order_relaxed);
		}
		s_mftbSiteCount.store(0, std::memory_order_relaxed);

		s_mftbClockSampleLock.lock();
		s_mftbClockLastHostTick = HighResolutionTimer::now().getTick();
		s_mftbClockLastGuestTick = 0;
		s_mftbClockSampleLock.unlock();
		s_osGetSystemTimeTicksPerSecond.store(0.0, std::memory_order_relaxed);
		s_estimatedGuestClockPercent.store(0.0, std::memory_order_relaxed);
	}
}

void PPCTimer_start()
{
	_rdtscLastMeasure = __rdtsc();
	_tickSummary = 0;
	PPCTimebaseDiagnostics_Reset();
	CafeSimulationPacing_Reset();
	coreinit::CafeTimeConsumerDiagnostics_Reset();
}

#if CEMU_PPC_HOT_DIAGNOSTICS
void PPCTimebaseDiagnostics_RecordMFTB(PPCInterpreter_t* hCPU, PPCMFTBExecutionMode mode, uint32 pc, uint64 sourceValue, bool upperPart)
{
	s_mftbExecutionCount.fetch_add(1, std::memory_order_relaxed);
	coreinit::CafeTimeConsumerDiagnostics_RecordMFTB(sourceValue);
	const bool isInterpreter = mode == PPCMFTBExecutionMode::Interpreter;
	if (isInterpreter)
		s_mftbInterpreterCount.fetch_add(1, std::memory_order_relaxed);
	else
		s_mftbJitCount.fetch_add(1, std::memory_order_relaxed);

	uint32 coreIndex = hCPU ? PPCInterpreter_getCoreIndex(hCPU) : 0;
	if (coreIndex >= Espresso::CORE_COUNT)
		coreIndex = 0;
	const uint32 currentMode = static_cast<uint32>(mode);
	const uint32 previousMode = s_mftbLastModeByCore[coreIndex].exchange(currentMode, std::memory_order_relaxed);
	if (previousMode != 0 && previousMode != currentMode)
	{
		s_mftbEngineTransitionCount.fetch_add(1, std::memory_order_relaxed);
		if (previousMode == static_cast<uint32>(PPCMFTBExecutionMode::Interpreter))
			s_mftbInterpreterToJitTransitions.fetch_add(1, std::memory_order_relaxed);
		else
			s_mftbJitToInterpreterTransitions.fetch_add(1, std::memory_order_relaxed);
	}

	const uint64 siteKey = static_cast<uint64>(pc) + 1ULL;
	for (auto& site : s_mftbSites)
	{
		uint64 key = site.key.load(std::memory_order_acquire);
		if (key == 0)
		{
			if (site.key.compare_exchange_strong(key, siteKey, std::memory_order_acq_rel))
				s_mftbSiteCount.fetch_add(1, std::memory_order_relaxed);
			else if (key != siteKey)
				continue;
		}
		else if (key != siteKey)
		{
			continue;
		}

		site.partMask.fetch_or(upperPart ? 2U : 1U, std::memory_order_relaxed);
		if (isInterpreter)
		{
			site.interpreterCount.fetch_add(1, std::memory_order_relaxed);
			uint64 emptyValue = 0;
			site.firstInterpreterSourceValue.compare_exchange_strong(emptyValue, sourceValue, std::memory_order_relaxed);
		}
		else
		{
			site.jitCount.fetch_add(1, std::memory_order_relaxed);
			uint64 emptyValue = 0;
			site.firstJitSourceValue.compare_exchange_strong(emptyValue, sourceValue, std::memory_order_relaxed);
		}
		break;
	}
}

#endif

void PPCTimebaseDiagnostics_SampleGuestClock(uint64 osGetSystemTimeTicks)
{
	const HRTick hostTick = HighResolutionTimer::now().getTick();
	s_mftbClockSampleLock.lock();
	if (s_mftbClockLastGuestTick == 0 || hostTick <= s_mftbClockLastHostTick || osGetSystemTimeTicks < s_mftbClockLastGuestTick)
	{
		s_mftbClockLastHostTick = hostTick;
		s_mftbClockLastGuestTick = osGetSystemTimeTicks;
		s_mftbClockSampleLock.unlock();
		return;
	}

	const uint64 hostDelta = hostTick - s_mftbClockLastHostTick;
	if (hostDelta < HighResolutionTimer::getFrequency() / 4ULL)
	{
		s_mftbClockSampleLock.unlock();
		return;
	}
	const uint64 guestDelta = osGetSystemTimeTicks - s_mftbClockLastGuestTick;
	const double hostSeconds = static_cast<double>(hostDelta) / static_cast<double>(HighResolutionTimer::getFrequency());
	const double ticksPerSecond = static_cast<double>(guestDelta) / hostSeconds;
	const double guestClockPercent = ticksPerSecond * 100.0 / static_cast<double>(Espresso::TIMER_CLOCK);
	s_osGetSystemTimeTicksPerSecond.store(ticksPerSecond, std::memory_order_relaxed);
	s_estimatedGuestClockPercent.store(guestClockPercent, std::memory_order_relaxed);
	s_mftbClockLastHostTick = hostTick;
	s_mftbClockLastGuestTick = osGetSystemTimeTicks;
	s_mftbClockSampleLock.unlock();
}

PPCMFTBDiagnosticSnapshot PPCTimebaseDiagnostics_GetSnapshot()
{
	PPCMFTBDiagnosticSnapshot snapshot{};
	snapshot.executionCount = s_mftbExecutionCount.load(std::memory_order_relaxed);
	snapshot.interpreterCount = s_mftbInterpreterCount.load(std::memory_order_relaxed);
	snapshot.jitCount = s_mftbJitCount.load(std::memory_order_relaxed);
	snapshot.engineTransitionCount = s_mftbEngineTransitionCount.load(std::memory_order_relaxed);
	snapshot.interpreterToJitTransitions = s_mftbInterpreterToJitTransitions.load(std::memory_order_relaxed);
	snapshot.jitToInterpreterTransitions = s_mftbJitToInterpreterTransitions.load(std::memory_order_relaxed);
	snapshot.siteCount = std::min<uint32>(s_mftbSiteCount.load(std::memory_order_acquire), PPC_MFTB_DIAGNOSTIC_SITE_LIMIT);
	snapshot.osGetSystemTimeTicksPerSecond = s_osGetSystemTimeTicksPerSecond.load(std::memory_order_relaxed);
	snapshot.estimatedGuestClockPercent = s_estimatedGuestClockPercent.load(std::memory_order_relaxed);
	return snapshot;
}

bool PPCTimebaseDiagnostics_GetSite(uint32 index, PPCMFTBSiteSnapshot& siteOut)
{
	if (index >= PPC_MFTB_DIAGNOSTIC_SITE_LIMIT)
		return false;
	const auto& site = s_mftbSites[index];
	const uint64 key = site.key.load(std::memory_order_acquire);
	if (key == 0)
		return false;

	siteOut.pc = static_cast<uint32>(key - 1ULL);
	siteOut.partMask = site.partMask.load(std::memory_order_relaxed);
	siteOut.interpreterCount = site.interpreterCount.load(std::memory_order_relaxed);
	siteOut.jitCount = site.jitCount.load(std::memory_order_relaxed);
	siteOut.firstInterpreterSourceValue = site.firstInterpreterSourceValue.load(std::memory_order_relaxed);
	siteOut.firstJitSourceValue = site.firstJitSourceValue.load(std::memory_order_relaxed);
	siteOut.interpreterScale = siteOut.interpreterCount ? 20U : 0U;
	siteOut.jitScale = siteOut.jitCount ? 1U : 0U;
	return true;
}

uint64 PPCTimer_getRawTsc()
{
	return __rdtsc();
}

uint64 PPCTimer_microsecondsToTsc(uint64 us)
{
	return (us * _rdtscFrequency) / 1000000ULL;
}

uint64 PPCTimer_tscToMicroseconds(uint64 us)
{
	uint128_t r{};
	r.low = _umul128(us, 1000000ULL, &r.high);

	uint64 remainder;
	const uint64 microseconds = _udiv128(r.high, r.low, _rdtscFrequency, &remainder);

	return microseconds;
}

bool PPCTimer_isReady()
{
	return _rdtscFrequency != 0;
}

void PPCTimer_waitForInit()
{
	while (!PPCTimer_isReady()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

FSpinlock sTimerSpinlock;
std::atomic_bool s_ppcTimerHostSuspended{false};

void PPCTimer_setHostSuspended(bool suspended)
{
	sTimerSpinlock.lock();
	_mm_mfence();
	_rdtscLastMeasure = __rdtsc();
	s_ppcTimerHostSuspended.store(suspended, std::memory_order_release);
	sTimerSpinlock.unlock();
}

// thread safe
uint64 PPCTimer_getFromRDTSC()
{
	sTimerSpinlock.lock();
	_mm_mfence();
	uint64 rdtscCurrentMeasure = __rdtsc();
	if (s_ppcTimerHostSuspended.load(std::memory_order_acquire))
	{
		// Keep moving the host baseline forward while background execution is
		// briefly allowed. This prevents a large delta on foreground resume.
		_rdtscLastMeasure = rdtscCurrentMeasure;
		sTimerSpinlock.unlock();
		return _tickSummary;
	}
	uint64 rdtscDif = rdtscCurrentMeasure - _rdtscLastMeasure;
	// optimized max(rdtscDif, 0) without conditionals
	rdtscDif = rdtscDif & ~(uint64)((sint64)rdtscDif >> 63);

	uint128_t diff{};
	diff.low = _umul128(rdtscDif, Espresso::CORE_CLOCK, &diff.high);

	if(rdtscCurrentMeasure > _rdtscLastMeasure)
		_rdtscLastMeasure = rdtscCurrentMeasure; // only travel forward in time

	uint8 c = 0;
	#if BOOST_OS_WINDOWS
	c = _addcarry_u64(c, _rdtscAcc.low, diff.low, &_rdtscAcc.low);
	_addcarry_u64(c, _rdtscAcc.high, diff.high, &_rdtscAcc.high);
	#else
	// requires casting because of long / long long nonesense
	c = _addcarry_u64(c, _rdtscAcc.low, diff.low, (unsigned long long*)&_rdtscAcc.low);
	_addcarry_u64(c, _rdtscAcc.high, diff.high, (unsigned long long*)&_rdtscAcc.high);
	#endif

	uint64 remainder;
	if (_rdtscFrequency == 0)
	{
		_rdtscFrequency = PPCTimer_estimateRDTSCFrequency();
		if (_rdtscFrequency == 0)
		{
			sTimerSpinlock.unlock();
			return _tickSummary;
		}
	}
	uint64 elapsedTick = _udiv128(_rdtscAcc.high, _rdtscAcc.low, _rdtscFrequency, &remainder);

	_rdtscAcc.low = remainder;
	_rdtscAcc.high = 0;

	// timer scaling
	elapsedTick <<= 3ull; // *8
	uint8 timerShiftFactor = ActiveSettings::GetTimerShiftFactor();
	elapsedTick >>= timerShiftFactor;

	_tickSummary += elapsedTick;

	sTimerSpinlock.unlock();
	return _tickSummary;
}
