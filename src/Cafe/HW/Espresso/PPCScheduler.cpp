#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/CafeSystem.h"

uint32 ppcThreadQuantum = 100000; // execute 100000 instructions before thread reschedule happens, this value can be overwritten by game profiles
std::atomic<uint64_t> g_emulatedGuestCycles{0};

namespace
{
	constexpr uint64 CAFE_SIMULATION_PACING_WINDOW_DIVISOR = 4;

	struct CafeSimulationPacingCounters
	{
		std::atomic<uint64> schedulerTimeslices{};
		std::atomic<uint64> waitingToReadyWakeups{};
		std::atomic<uint64> threadResumes{};
		std::atomic<uint64> alarmCallbacks{};
		std::atomic<uint64> alarmCatchupCallbacks{};
		std::atomic<uint64> gx2SwapScanBuffers{};
		std::atomic<uint64> timedVsyncEvents{};
		std::atomic<uint64> metalPresentations{};
		std::atomic<uint32> maxAlarmCallbacksCurrentWindow{};
		std::atomic<uint32> gx2SwapThreadIdentity{};
	};

	CafeSimulationPacingCounters s_cafeSimulationPacingCounters;
	std::atomic_flag s_cafeSimulationPacingWindowLock = ATOMIC_FLAG_INIT;
	HRTick s_cafeSimulationPacingWindowStart{};
	uint64 s_lastSchedulerTimeslices{};
	uint64 s_lastWaitingToReadyWakeups{};
	uint64 s_lastThreadResumes{};
	uint64 s_lastAlarmCallbacks{};
	uint64 s_lastAlarmCatchupCallbacks{};
	uint64 s_lastGX2SwapScanBuffers{};
	uint64 s_lastTimedVsyncEvents{};
	uint64 s_lastMetalPresentations{};

	std::atomic<double> s_pacingWindowSeconds{};
	std::atomic<double> s_schedulerTimeslicesPerSecond{};
	std::atomic<double> s_waitingToReadyWakeupsPerSecond{};
	std::atomic<double> s_threadResumesPerSecond{};
	std::atomic<double> s_alarmCallbacksPerSecond{};
	std::atomic<double> s_alarmCatchupCallbacksPerSecond{};
	std::atomic<uint32> s_maxAlarmCallbacksPerWake{};
	std::atomic<double> s_gx2SwapScanBuffersPerSecond{};
	std::atomic<double> s_timedVsyncEventsPerSecond{};
	std::atomic<double> s_metalPresentationsPerSecond{};
	std::atomic<double> s_gx2SwapToPresentRatio{};
	std::atomic<double> s_alarmCallbacksToPresentRatio{};
	std::atomic<double> s_schedulerTimeslicesToPresentRatio{};

	constexpr uint32 CAFE_BETWEEN_FRAME_THREAD_SLOTS = 8;
	struct CafeBetweenFrameThreadSlot
	{
		std::atomic<uint32> identity{};
		std::atomic<uint64> resumeCount{};
	};

	std::atomic<uint64> s_betweenFramePPCExecutions{};
	std::atomic<uint64> s_betweenFrameThreadResumes{};
	std::atomic<uint64> s_betweenFrameAlarmCallbacks{};
	std::atomic<uint64> s_betweenFrameMetalPresentations{};
	CafeBetweenFrameThreadSlot s_betweenFrameThreadSlots[CAFE_BETWEEN_FRAME_THREAD_SLOTS]{};
	std::atomic<uint64> s_betweenFrameGX2SwapFrameId{};
	std::atomic<uint64> s_betweenFrameGX2SwapTimestamp{};
	std::atomic<double> s_betweenFramePresentIntervalMs{};
	std::atomic<double> s_betweenFramePPCExecutionsPerPresent{};
	std::atomic<double> s_betweenFrameThreadResumesPerPresent{};
	std::atomic<double> s_betweenFrameAlarmCallbacksPerPresent{};
	std::atomic<uint32> s_betweenFrameTopThreadIdentity{};
	std::atomic<uint64> s_betweenFrameTopThreadResumeCount{};
	std::atomic<HRTick> s_betweenFrameLastPresentTimestamp{};

	void CafeBetweenFrameExecution_RecordThread(uint32 threadIdentity)
	{
		if (threadIdentity == 0)
			return;

		for (uint32 i = 0; i < CAFE_BETWEEN_FRAME_THREAD_SLOTS; ++i)
		{
			uint32 slotIdentity = s_betweenFrameThreadSlots[i].identity.load(std::memory_order_relaxed);
			if (slotIdentity == threadIdentity)
			{
				s_betweenFrameThreadSlots[i].resumeCount.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (slotIdentity == 0 && s_betweenFrameThreadSlots[i].identity.compare_exchange_strong(slotIdentity, threadIdentity, std::memory_order_relaxed))
			{
				s_betweenFrameThreadSlots[i].resumeCount.fetch_add(1, std::memory_order_relaxed);
				return;
			}
		}
	}

	void CafeBetweenFrameExecution_CompleteSwapInterval()
	{
		const HRTick now = HighResolutionTimer::now().getTick();
		const uint64 ppcExecutions = s_betweenFramePPCExecutions.exchange(0, std::memory_order_relaxed);
		const uint64 threadResumes = s_betweenFrameThreadResumes.exchange(0, std::memory_order_relaxed);
		const uint64 alarmCallbacks = s_betweenFrameAlarmCallbacks.exchange(0, std::memory_order_relaxed);
		const uint64 presentations = s_betweenFrameMetalPresentations.exchange(0, std::memory_order_relaxed);
		const double presentationDivisor = presentations > 0 ? static_cast<double>(presentations) : 1.0;

		uint32 topThreadIdentity = 0;
		uint64 topThreadResumeCount = 0;
		for (uint32 i = 0; i < CAFE_BETWEEN_FRAME_THREAD_SLOTS; ++i)
		{
			const uint64 resumeCount = s_betweenFrameThreadSlots[i].resumeCount.exchange(0, std::memory_order_relaxed);
			const uint32 identity = s_betweenFrameThreadSlots[i].identity.exchange(0, std::memory_order_relaxed);
			if (resumeCount > topThreadResumeCount)
			{
				topThreadIdentity = identity;
				topThreadResumeCount = resumeCount;
			}
		}

		s_betweenFrameGX2SwapFrameId.fetch_add(1, std::memory_order_relaxed);
		s_betweenFrameGX2SwapTimestamp.store(now, std::memory_order_relaxed);
		s_betweenFramePPCExecutionsPerPresent.store(static_cast<double>(ppcExecutions) / presentationDivisor, std::memory_order_relaxed);
		s_betweenFrameThreadResumesPerPresent.store(static_cast<double>(threadResumes) / presentationDivisor, std::memory_order_relaxed);
		s_betweenFrameAlarmCallbacksPerPresent.store(static_cast<double>(alarmCallbacks) / presentationDivisor, std::memory_order_relaxed);
		s_betweenFrameTopThreadIdentity.store(topThreadIdentity, std::memory_order_relaxed);
		s_betweenFrameTopThreadResumeCount.store(topThreadResumeCount, std::memory_order_relaxed);
	}

	void CafeSimulationPacing_TryCompleteWindow()
	{
		if (s_cafeSimulationPacingWindowLock.test_and_set(std::memory_order_acquire))
			return;

		const HRTick now = HighResolutionTimer::now().getTick();
		const HRTick frequency = HighResolutionTimer::getFrequency();
		if (s_cafeSimulationPacingWindowStart == 0)
		{
			s_cafeSimulationPacingWindowStart = now;
			s_lastSchedulerTimeslices = s_cafeSimulationPacingCounters.schedulerTimeslices.load(std::memory_order_relaxed);
			s_lastWaitingToReadyWakeups = s_cafeSimulationPacingCounters.waitingToReadyWakeups.load(std::memory_order_relaxed);
			s_lastThreadResumes = s_cafeSimulationPacingCounters.threadResumes.load(std::memory_order_relaxed);
			s_lastAlarmCallbacks = s_cafeSimulationPacingCounters.alarmCallbacks.load(std::memory_order_relaxed);
			s_lastAlarmCatchupCallbacks = s_cafeSimulationPacingCounters.alarmCatchupCallbacks.load(std::memory_order_relaxed);
			s_lastGX2SwapScanBuffers = s_cafeSimulationPacingCounters.gx2SwapScanBuffers.load(std::memory_order_relaxed);
			s_lastTimedVsyncEvents = s_cafeSimulationPacingCounters.timedVsyncEvents.load(std::memory_order_relaxed);
			s_lastMetalPresentations = s_cafeSimulationPacingCounters.metalPresentations.load(std::memory_order_relaxed);
			s_cafeSimulationPacingWindowLock.clear(std::memory_order_release);
			return;
		}

		const HRTick elapsedTicks = now - s_cafeSimulationPacingWindowStart;
		if (elapsedTicks < frequency / CAFE_SIMULATION_PACING_WINDOW_DIVISOR)
		{
			s_cafeSimulationPacingWindowLock.clear(std::memory_order_release);
			return;
		}

		const uint64 schedulerTimeslices = s_cafeSimulationPacingCounters.schedulerTimeslices.load(std::memory_order_relaxed);
		const uint64 waitingToReadyWakeups = s_cafeSimulationPacingCounters.waitingToReadyWakeups.load(std::memory_order_relaxed);
		const uint64 threadResumes = s_cafeSimulationPacingCounters.threadResumes.load(std::memory_order_relaxed);
		const uint64 alarmCallbacks = s_cafeSimulationPacingCounters.alarmCallbacks.load(std::memory_order_relaxed);
		const uint64 alarmCatchupCallbacks = s_cafeSimulationPacingCounters.alarmCatchupCallbacks.load(std::memory_order_relaxed);
		const uint64 gx2SwapScanBuffers = s_cafeSimulationPacingCounters.gx2SwapScanBuffers.load(std::memory_order_relaxed);
		const uint64 timedVsyncEvents = s_cafeSimulationPacingCounters.timedVsyncEvents.load(std::memory_order_relaxed);
		const uint64 metalPresentations = s_cafeSimulationPacingCounters.metalPresentations.load(std::memory_order_relaxed);

		const uint64 deltaSchedulerTimeslices = schedulerTimeslices - s_lastSchedulerTimeslices;
		const uint64 deltaWaitingToReadyWakeups = waitingToReadyWakeups - s_lastWaitingToReadyWakeups;
		const uint64 deltaThreadResumes = threadResumes - s_lastThreadResumes;
		const uint64 deltaAlarmCallbacks = alarmCallbacks - s_lastAlarmCallbacks;
		const uint64 deltaAlarmCatchupCallbacks = alarmCatchupCallbacks - s_lastAlarmCatchupCallbacks;
		const uint64 deltaGX2SwapScanBuffers = gx2SwapScanBuffers - s_lastGX2SwapScanBuffers;
		const uint64 deltaTimedVsyncEvents = timedVsyncEvents - s_lastTimedVsyncEvents;
		const uint64 deltaMetalPresentations = metalPresentations - s_lastMetalPresentations;
		const double elapsedSeconds = static_cast<double>(elapsedTicks) / static_cast<double>(frequency);
		const double inverseElapsedSeconds = 1.0 / elapsedSeconds;

		s_pacingWindowSeconds.store(elapsedSeconds, std::memory_order_relaxed);
		s_schedulerTimeslicesPerSecond.store(static_cast<double>(deltaSchedulerTimeslices) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_waitingToReadyWakeupsPerSecond.store(static_cast<double>(deltaWaitingToReadyWakeups) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_threadResumesPerSecond.store(static_cast<double>(deltaThreadResumes) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_alarmCallbacksPerSecond.store(static_cast<double>(deltaAlarmCallbacks) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_alarmCatchupCallbacksPerSecond.store(static_cast<double>(deltaAlarmCatchupCallbacks) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_maxAlarmCallbacksPerWake.store(s_cafeSimulationPacingCounters.maxAlarmCallbacksCurrentWindow.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
		s_gx2SwapScanBuffersPerSecond.store(static_cast<double>(deltaGX2SwapScanBuffers) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_timedVsyncEventsPerSecond.store(static_cast<double>(deltaTimedVsyncEvents) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_metalPresentationsPerSecond.store(static_cast<double>(deltaMetalPresentations) * inverseElapsedSeconds, std::memory_order_relaxed);
		if (deltaMetalPresentations > 0)
		{
			const double inversePresentations = 1.0 / static_cast<double>(deltaMetalPresentations);
			s_gx2SwapToPresentRatio.store(static_cast<double>(deltaGX2SwapScanBuffers) * inversePresentations, std::memory_order_relaxed);
			s_alarmCallbacksToPresentRatio.store(static_cast<double>(deltaAlarmCallbacks) * inversePresentations, std::memory_order_relaxed);
			s_schedulerTimeslicesToPresentRatio.store(static_cast<double>(deltaSchedulerTimeslices) * inversePresentations, std::memory_order_relaxed);
		}
		else
		{
			s_gx2SwapToPresentRatio.store(0.0, std::memory_order_relaxed);
			s_alarmCallbacksToPresentRatio.store(0.0, std::memory_order_relaxed);
			s_schedulerTimeslicesToPresentRatio.store(0.0, std::memory_order_relaxed);
		}

		s_lastSchedulerTimeslices = schedulerTimeslices;
		s_lastWaitingToReadyWakeups = waitingToReadyWakeups;
		s_lastThreadResumes = threadResumes;
		s_lastAlarmCallbacks = alarmCallbacks;
		s_lastAlarmCatchupCallbacks = alarmCatchupCallbacks;
		s_lastGX2SwapScanBuffers = gx2SwapScanBuffers;
		s_lastTimedVsyncEvents = timedVsyncEvents;
		s_lastMetalPresentations = metalPresentations;
		s_cafeSimulationPacingWindowStart = now;
		s_cafeSimulationPacingWindowLock.clear(std::memory_order_release);
	}
}

void CafeSimulationPacing_Reset()
{
	while (s_cafeSimulationPacingWindowLock.test_and_set(std::memory_order_acquire))
		std::this_thread::yield();

	s_cafeSimulationPacingCounters.schedulerTimeslices.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.waitingToReadyWakeups.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.threadResumes.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.alarmCallbacks.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.alarmCatchupCallbacks.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.gx2SwapScanBuffers.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.timedVsyncEvents.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.metalPresentations.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.maxAlarmCallbacksCurrentWindow.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.gx2SwapThreadIdentity.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingWindowStart = 0;
	s_lastSchedulerTimeslices = 0;
	s_lastWaitingToReadyWakeups = 0;
	s_lastThreadResumes = 0;
	s_lastAlarmCallbacks = 0;
	s_lastAlarmCatchupCallbacks = 0;
	s_lastGX2SwapScanBuffers = 0;
	s_lastTimedVsyncEvents = 0;
	s_lastMetalPresentations = 0;
	s_pacingWindowSeconds.store(0.0, std::memory_order_relaxed);
	s_schedulerTimeslicesPerSecond.store(0.0, std::memory_order_relaxed);
	s_waitingToReadyWakeupsPerSecond.store(0.0, std::memory_order_relaxed);
	s_threadResumesPerSecond.store(0.0, std::memory_order_relaxed);
	s_alarmCallbacksPerSecond.store(0.0, std::memory_order_relaxed);
	s_alarmCatchupCallbacksPerSecond.store(0.0, std::memory_order_relaxed);
	s_maxAlarmCallbacksPerWake.store(0, std::memory_order_relaxed);
	s_gx2SwapScanBuffersPerSecond.store(0.0, std::memory_order_relaxed);
	s_timedVsyncEventsPerSecond.store(0.0, std::memory_order_relaxed);
	s_metalPresentationsPerSecond.store(0.0, std::memory_order_relaxed);
	s_gx2SwapToPresentRatio.store(0.0, std::memory_order_relaxed);
	s_alarmCallbacksToPresentRatio.store(0.0, std::memory_order_relaxed);
	s_schedulerTimeslicesToPresentRatio.store(0.0, std::memory_order_relaxed);
	s_betweenFramePPCExecutions.store(0, std::memory_order_relaxed);
	s_betweenFrameThreadResumes.store(0, std::memory_order_relaxed);
	s_betweenFrameAlarmCallbacks.store(0, std::memory_order_relaxed);
	s_betweenFrameMetalPresentations.store(0, std::memory_order_relaxed);
	for (uint32 i = 0; i < CAFE_BETWEEN_FRAME_THREAD_SLOTS; ++i)
	{
		s_betweenFrameThreadSlots[i].identity.store(0, std::memory_order_relaxed);
		s_betweenFrameThreadSlots[i].resumeCount.store(0, std::memory_order_relaxed);
	}
	s_betweenFrameGX2SwapFrameId.store(0, std::memory_order_relaxed);
	s_betweenFrameGX2SwapTimestamp.store(0, std::memory_order_relaxed);
	s_betweenFramePresentIntervalMs.store(0.0, std::memory_order_relaxed);
	s_betweenFramePPCExecutionsPerPresent.store(0.0, std::memory_order_relaxed);
	s_betweenFrameThreadResumesPerPresent.store(0.0, std::memory_order_relaxed);
	s_betweenFrameAlarmCallbacksPerPresent.store(0.0, std::memory_order_relaxed);
	s_betweenFrameTopThreadIdentity.store(0, std::memory_order_relaxed);
	s_betweenFrameTopThreadResumeCount.store(0, std::memory_order_relaxed);
	s_betweenFrameLastPresentTimestamp.store(0, std::memory_order_relaxed);
	s_cafeSimulationPacingWindowLock.clear(std::memory_order_release);
}

#if CEMU_PPC_HOT_DIAGNOSTICS
void CafeSimulationPacing_RecordSchedulerTimeslice()
{
	s_cafeSimulationPacingCounters.schedulerTimeslices.fetch_add(1, std::memory_order_relaxed);
}

void CafeSimulationPacing_RecordWaitingToReadyWakeup()
{
	s_cafeSimulationPacingCounters.waitingToReadyWakeups.fetch_add(1, std::memory_order_relaxed);
}

void CafeSimulationPacing_RecordThreadResume(uint32 threadIdentity)
{
	s_cafeSimulationPacingCounters.threadResumes.fetch_add(1, std::memory_order_relaxed);
	s_betweenFrameThreadResumes.fetch_add(1, std::memory_order_relaxed);
	CafeBetweenFrameExecution_RecordThread(threadIdentity);
}

void CafeSimulationPacing_RecordAlarmCallback(bool isCatchup)
{
	s_cafeSimulationPacingCounters.alarmCallbacks.fetch_add(1, std::memory_order_relaxed);
	s_betweenFrameAlarmCallbacks.fetch_add(1, std::memory_order_relaxed);
	if (isCatchup)
		s_cafeSimulationPacingCounters.alarmCatchupCallbacks.fetch_add(1, std::memory_order_relaxed);
}

void CafeSimulationPacing_RecordAlarmWakeComplete(uint32 callbackCount)
{
	uint32 currentMaximum = s_cafeSimulationPacingCounters.maxAlarmCallbacksCurrentWindow.load(std::memory_order_relaxed);
	while (callbackCount > currentMaximum &&
		!s_cafeSimulationPacingCounters.maxAlarmCallbacksCurrentWindow.compare_exchange_weak(currentMaximum, callbackCount, std::memory_order_relaxed))
	{
	}
}

void CafeSimulationPacing_RecordGX2Swap(uint32 threadIdentity)
{
	CafeBetweenFrameExecution_CompleteSwapInterval();
	s_cafeSimulationPacingCounters.gx2SwapScanBuffers.fetch_add(1, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.gx2SwapThreadIdentity.store(threadIdentity, std::memory_order_relaxed);
}

void CafeSimulationPacing_RecordTimedVsync()
{
	s_cafeSimulationPacingCounters.timedVsyncEvents.fetch_add(1, std::memory_order_relaxed);
}

void CafeSimulationPacing_RecordMetalPresentation()
{
	const HRTick now = HighResolutionTimer::now().getTick();
	const HRTick previous = s_betweenFrameLastPresentTimestamp.exchange(now, std::memory_order_relaxed);
	if (previous != 0 && now >= previous)
	{
		const double intervalMs = static_cast<double>(now - previous) * 1000.0 / static_cast<double>(HighResolutionTimer::getFrequency());
		s_betweenFramePresentIntervalMs.store(intervalMs, std::memory_order_relaxed);
	}
	s_betweenFrameMetalPresentations.fetch_add(1, std::memory_order_relaxed);
	s_cafeSimulationPacingCounters.metalPresentations.fetch_add(1, std::memory_order_relaxed);
	CafeSimulationPacing_TryCompleteWindow();
}

#endif

CafeSimulationPacingSnapshot CafeSimulationPacing_GetSnapshot()
{
	CafeSimulationPacingSnapshot snapshot{};
	snapshot.windowSeconds = s_pacingWindowSeconds.load(std::memory_order_relaxed);
	snapshot.schedulerTimeslicesPerSecond = s_schedulerTimeslicesPerSecond.load(std::memory_order_relaxed);
	snapshot.waitingToReadyWakeupsPerSecond = s_waitingToReadyWakeupsPerSecond.load(std::memory_order_relaxed);
	snapshot.threadResumesPerSecond = s_threadResumesPerSecond.load(std::memory_order_relaxed);
	snapshot.alarmCallbacksPerSecond = s_alarmCallbacksPerSecond.load(std::memory_order_relaxed);
	snapshot.alarmCatchupCallbacksPerSecond = s_alarmCatchupCallbacksPerSecond.load(std::memory_order_relaxed);
	snapshot.maxAlarmCallbacksPerWake = s_maxAlarmCallbacksPerWake.load(std::memory_order_relaxed);
	snapshot.gx2SwapScanBuffersPerSecond = s_gx2SwapScanBuffersPerSecond.load(std::memory_order_relaxed);
	snapshot.timedVsyncEventsPerSecond = s_timedVsyncEventsPerSecond.load(std::memory_order_relaxed);
	snapshot.metalPresentationsPerSecond = s_metalPresentationsPerSecond.load(std::memory_order_relaxed);
	snapshot.gx2SwapToPresentRatio = s_gx2SwapToPresentRatio.load(std::memory_order_relaxed);
	snapshot.alarmCallbacksToPresentRatio = s_alarmCallbacksToPresentRatio.load(std::memory_order_relaxed);
	snapshot.schedulerTimeslicesToPresentRatio = s_schedulerTimeslicesToPresentRatio.load(std::memory_order_relaxed);
	snapshot.gx2SwapThreadIdentity = s_cafeSimulationPacingCounters.gx2SwapThreadIdentity.load(std::memory_order_relaxed);
	if (snapshot.gx2SwapThreadIdentity != 0)
	{
		OSThread_t* thread = MEMPTR<OSThread_t>(snapshot.gx2SwapThreadIdentity).GetPtr();
		if (thread)
		{
			const char* threadName = coreinit::OSGetThreadName(thread);
			if (threadName)
				strncpy(snapshot.gx2SwapThreadName, threadName, sizeof(snapshot.gx2SwapThreadName) - 1);
		}
	}
	return snapshot;
}

#if CEMU_PPC_HOT_DIAGNOSTICS
void CafeBetweenFrameExecution_RecordPPCExecutionCallback()
{
	s_betweenFramePPCExecutions.fetch_add(1, std::memory_order_relaxed);
}

#endif

CafeBetweenFrameExecutionSnapshot CafeBetweenFrameExecution_GetSnapshot()
{
	CafeBetweenFrameExecutionSnapshot snapshot{};
	snapshot.gx2SwapFrameId = s_betweenFrameGX2SwapFrameId.load(std::memory_order_relaxed);
	snapshot.gx2SwapTimestamp = s_betweenFrameGX2SwapTimestamp.load(std::memory_order_relaxed);
	snapshot.presentIntervalMs = s_betweenFramePresentIntervalMs.load(std::memory_order_relaxed);
	snapshot.ppcExecutionsPerPresentedFrame = s_betweenFramePPCExecutionsPerPresent.load(std::memory_order_relaxed);
	snapshot.threadResumesPerPresentedFrame = s_betweenFrameThreadResumesPerPresent.load(std::memory_order_relaxed);
	snapshot.alarmCallbacksPerPresentedFrame = s_betweenFrameAlarmCallbacksPerPresent.load(std::memory_order_relaxed);
	snapshot.topResumedThreadIdentity = s_betweenFrameTopThreadIdentity.load(std::memory_order_relaxed);
	snapshot.topResumedThreadCount = s_betweenFrameTopThreadResumeCount.load(std::memory_order_relaxed);
	if (snapshot.topResumedThreadIdentity != 0)
	{
		OSThread_t* thread = MEMPTR<OSThread_t>(snapshot.topResumedThreadIdentity).GetPtr();
		if (thread)
		{
			const char* threadName = coreinit::OSGetThreadName(thread);
			if (threadName)
				strncpy(snapshot.topResumedThreadName, threadName, sizeof(snapshot.topResumedThreadName) - 1);
		}
	}
	return snapshot;
}

void PPCInterpreter_relinquishTimeslice()
{
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	if( hCPU->remainingCycles >= 0 )
	{
		hCPU->skippedCycles = hCPU->remainingCycles + 1;
		hCPU->remainingCycles = -1;
	}
}

void PPCCore_boostQuantum(sint32 numCycles)
{
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	hCPU->remainingCycles += numCycles;
}

void PPCCore_deboostQuantum(sint32 numCycles)
{
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	hCPU->remainingCycles -= numCycles;
}

namespace coreinit
{
	void __OSThreadSwitchToNext();
}

void PPCCore_switchToScheduler()
{
	cemu_assert_debug(__OSHasSchedulerLock() == false); // scheduler lock must not be hold past thread time slice
	cemu_assert_debug(PPCInterpreter_getCurrentInstance()->coreInterruptMask != 0 || CafeSystem::GetForegroundTitleId() == 0x000500001019e600);
	__OSLockScheduler();
	coreinit::__OSThreadSwitchToNext();
	__OSUnlockScheduler();
}

void PPCCore_switchToSchedulerWithLock()
{
	cemu_assert_debug(__OSHasSchedulerLock() == true); // scheduler lock must be hold
	cemu_assert_debug(PPCInterpreter_getCurrentInstance()->coreInterruptMask != 0 || CafeSystem::GetForegroundTitleId() == 0x000500001019e600);
	coreinit::__OSThreadSwitchToNext();
}

void _PPCCore_callbackExit(PPCInterpreter_t* hCPU)
{
	PPCInterpreter_relinquishTimeslice();
	hCPU->instructionPointer = 0;
}

PPCInterpreter_t* PPCCore_executeCallbackInternal(uint32 functionMPTR)
{
	cemu_assert_debug(functionMPTR != 0);
	CafeBetweenFrameExecution_RecordPPCExecutionCallback();
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	// remember LR and instruction pointer
	uint32 lr = hCPU->spr.LR;
	uint32 ip = hCPU->instructionPointer;
	// save area
	hCPU->gpr[1] -= 16 * 4;
	// set LR
	hCPU->spr.LR = PPCInterpreter_makeCallableExportDepr(_PPCCore_callbackExit);
	// set instruction pointer
	hCPU->instructionPointer = functionMPTR;
	// execute code until we return from the function
	while (true)
	{
#ifndef NDEBUG
		g_jitDiagnosticSchedulerQuanta.fetch_add(1, std::memory_order_relaxed);
#endif
		hCPU->remainingCycles = ppcThreadQuantum;
		hCPU->skippedCycles = 0;
		if (hCPU->remainingCycles > 0)
		{
			// try to enter recompiler immediately
			PPCRecompiler_attemptEnter(hCPU, hCPU->instructionPointer, JITCallerSite::Scheduler);
			// execute any remaining instructions in interpreter
			// coarse diagnostic: only count quanta where interpreter actually runs
			if (hCPU->remainingCycles > 0)
			{
#ifndef NDEBUG
				g_jitDiagnosticInterpreterInstructionCount.fetch_add(1, std::memory_order_relaxed);
#endif
			}
			while ((--hCPU->remainingCycles) >= 0)
			{
				PPCInterpreterSlim_executeInstruction(hCPU);
			};
		}
		sint32 consumed = (sint32)ppcThreadQuantum - std::max<sint32>(0, hCPU->remainingCycles);
		if (consumed > 0)
		{
			g_emulatedGuestCycles.fetch_add((uint64_t)consumed, std::memory_order_relaxed);
		}
		if (hCPU->instructionPointer == 0)
		{
			// restore remaining cycles
			hCPU->remainingCycles += hCPU->skippedCycles;
			hCPU->skippedCycles = 0;
			break;
		}
		coreinit::OSYieldThread();
	}
	// save area
	hCPU->gpr[1] += 16 * 4;
	// restore LR and instruction pointer
	hCPU->spr.LR = lr;
	hCPU->instructionPointer = ip;
	return hCPU;
}

void PPCCore_init()
{
}
