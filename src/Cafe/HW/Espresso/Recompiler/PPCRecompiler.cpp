#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "PPCFunctionBoundaryTracker.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"
#include "util/containers/RangeStore.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/cpu_features.h"
#include "util/helpers/fspinlock.h"
#include "util/helpers/helpers.h"
#include "util/MemMapper/MemMapper.h"
#include "util/SystemInfo/SystemInfo.h"

#include "IML/IML.h"
#include "IML/IMLRegisterAllocator.h"
#include "BackendX64/BackendX64.h"
#ifdef __aarch64__
#include "BackendAArch64/BackendAArch64.h"
#endif
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include <memory>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#define PPCREC_FORCE_SYNCHRONOUS_COMPILATION	0 // if 1, then function recompilation will block and execute on the thread that called PPCRecompiler_visitAddressNoBlock
#define PPCREC_LOG_RECOMPILATION_RESULTS		0

JITCodeQualityStats g_jitCodeStats;

struct ppcInvalidationRange
{
	MPTR startAddress;
	uint32 size;

	ppcInvalidationRange(MPTR _startAddress, uint32 _size) : startAddress(_startAddress), size(_size) {};
};

struct ppcRecompilerFuncRange
{
	MPTR	ppcStart;
	uint32  ppcSize;
	void*   x86Start;
	size_t  x86Size;
};

struct
{
	std::atomic_bool initialized{false};
	FSpinlock recompilerSpinlock;
	std::queue<MPTR> targetQueue;
	std::vector<ppcInvalidationRange> invalidationRanges;
	std::atomic_int_fast32_t recompilerEnableCount{0};
	// recompiler thread
	std::thread workerThread;
	std::atomic_bool workerThreadStopSignal{false};
	// function storage
	RangeStore<PPCRecFunction_t*, uint32, 7703, 0x2000> functionStorage;
}s_ppcRecompilerState;

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"

extern std::atomic<uint64_t> g_metalSwapCount;
extern std::atomic<uint64_t> g_metalPresentCount;
extern "C" double CemuBridge_GetHostDisplayHz(void);
extern "C" void CemuBridge_SetCurrentFPS(double fps);
extern "C" void CemuBridge_SetGuestClockSpeedPercent(double pct);
extern "C" void CemuBridge_SetEmuSpeedPercent(double pct);

#if defined(__APPLE__)
#include <TargetConditionals.h>
extern "C" void CemuBridge_LogJITMilestone(const char* message);
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include <os/proc.h>
struct JIT26MemoryStats {
    uint64_t allocationCount;
    uint64_t requestedBytes;
    uint64_t allocatedBytes;
    uint64_t largestRequest;
    size_t arenaCapacityMB;
    size_t arenaUsedMB;
};
extern "C" void JIT26_GetMemoryStats(struct JIT26MemoryStats* outStats);
extern "C" void JIT26_GetArenaStats(size_t* outBlocks, size_t* outUsedMB);
extern "C" void JIT26_QueryAndLogVM(const char* label, const void* ptr);
extern "C" bool JIT26_IsAddressInArena(const void* ptr);
extern "C" bool JIT26_RunPreflightSmokeTest(void);
#endif
#else
inline void CemuBridge_LogJITMilestone(const char*) {}
#endif

struct JITRuntimeStats {
	std::atomic<uint64_t> compiledFunctionCount{0};
	std::atomic<uint64_t> compileFailureCount{0};
	std::atomic<uint64_t> jitEnterCount{0};
	std::atomic<uint64_t> jitReturnCount{0};
	std::atomic<uint64_t> workerCompilationCount{0};
	std::atomic<uint64_t> hleCalls{0};
	std::atomic<uint32_t> lastPPCAddressEntered{0};
	std::atomic<uintptr_t> lastHostEntered{0};
	std::atomic<uint64_t> totalCompileTimeUs{0};
	std::atomic<uint64_t> maxCompileTimeUs{0};
	std::atomic<uint64_t> maxQueueDepth{0};
	std::atomic<int64_t> lastHeartbeatMs{0};
};
static JITRuntimeStats s_jitStats;
// Authoritative production JIT execution counters
std::atomic<uint64_t> g_jitCppEnterCount{0};
std::atomic<uint64_t> g_jitCppReturnCount{0};
std::atomic<uint64_t> g_jitDiagnosticInterpreterInstructionCount{0};
std::atomic<uint64_t> g_jitDiagnosticSchedulerQuanta{0};

static std::thread s_jitWatchdogThread;
static std::atomic_bool s_jitWatchdogStopSignal{false};

void PPCRecompiler_watchdogThread()
{
	SetThreadName("JITWatchdog");
	static auto s_lastVsyncCheckTime = std::chrono::steady_clock::now();
	static uint64_t s_lastWaitVsync = 0;
	static uint64_t s_lastSwap = 0;
	static uint64_t s_lastPresent = 0;
	static uint64_t s_lastSchedQ = 0;
	static uint64_t s_lastPpcEnters = 0;
	static uint64_t s_lastPpcReturns = 0;
	static uint64_t s_lastGuestTicks = 0;
	static uint64_t s_lastGuestCycles = 0;
	static uint64_t s_lastGuestSwap = 0;
	static double s_guestSwapHzHistory[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
	static double s_presentHzHistory[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
	static size_t s_historyCount = 0;

	while (!s_jitWatchdogStopSignal.load(std::memory_order_relaxed))
	{
		std::this_thread::sleep_for(std::chrono::seconds(2));
		if (s_jitWatchdogStopSignal.load(std::memory_order_relaxed))
			break;

		if (!s_ppcRecompilerState.initialized)
			continue;

		uint64_t enters = g_jitCppEnterCount.load(std::memory_order_relaxed);
		uint64_t rets = g_jitCppReturnCount.load(std::memory_order_relaxed);
		uint64_t compiled = s_jitStats.compiledFunctionCount.load(std::memory_order_relaxed);
		uint64_t workerCompiled = s_jitStats.workerCompilationCount.load(std::memory_order_relaxed);
		bool workerAlive = !s_ppcRecompilerState.workerThreadStopSignal;
		int32_t enableCount = s_ppcRecompilerState.recompilerEnableCount.load(std::memory_order_relaxed);

		uint64_t schedQuanta = g_jitDiagnosticSchedulerQuanta.load(std::memory_order_relaxed);
		uint64_t interpQuanta = g_jitDiagnosticInterpreterInstructionCount.load(std::memory_order_relaxed);

		size_t qDepth = 0;
		if (s_ppcRecompilerState.recompilerSpinlock.try_lock())
		{
			qDepth = s_ppcRecompilerState.targetQueue.size();
			s_ppcRecompilerState.recompilerSpinlock.unlock();
		}

		(void)rets;
		(void)workerCompiled;
		(void)workerAlive;
		(void)schedQuanta;
		(void)interpQuanta;
		(void)enableCount;

		auto nowTime = std::chrono::steady_clock::now();
		double elapsedSec = std::chrono::duration<double>(nowTime - s_lastVsyncCheckTime).count();
		if (elapsedSec >= 1.9)
		{
			uint64_t curWaitVsync = LatteGPUState.telemetry.gx2WaitForVsyncCount.load(std::memory_order_relaxed);
			uint64_t curSwap = g_metalSwapCount.load(std::memory_order_relaxed);
			uint64_t curPresent = g_metalPresentCount.load(std::memory_order_relaxed);
			uint64_t curGuestSwap = g_guestSwapCount.load(std::memory_order_relaxed);
			uint64_t curSchedQ = g_jitDiagnosticSchedulerQuanta.load(std::memory_order_relaxed);
			uint64_t curPpcEnters = enters;
			uint64_t curGuestTicks = coreinit::OSGetSystemTime();

			uint64_t deltaWaitVsync = curWaitVsync - s_lastWaitVsync;
			uint64_t deltaSwap = curSwap - s_lastSwap;
			uint64_t deltaPresent = curPresent - s_lastPresent;
			uint64_t deltaGuestSwap = (curGuestSwap >= s_lastGuestSwap) ? (curGuestSwap - s_lastGuestSwap) : 0;
			uint64_t deltaSchedQ = curSchedQ - s_lastSchedQ;
			uint64_t deltaPpcEnters = (curPpcEnters >= s_lastPpcEnters) ? (curPpcEnters - s_lastPpcEnters) : 0;
			uint64_t deltaPpcReturns = (rets >= s_lastPpcReturns) ? (rets - s_lastPpcReturns) : 0;
			uint64_t deltaGuestTicks = (curGuestTicks >= s_lastGuestTicks) ? (curGuestTicks - s_lastGuestTicks) : 0;

			double effectiveVsyncHz = (double)deltaWaitVsync / elapsedSec;
			double effectiveSwapHz = (double)deltaSwap / elapsedSec;
			double effectivePresentHz = (double)deltaPresent / elapsedSec;
			double effectiveGuestSwapHz = (double)deltaGuestSwap / elapsedSec;
			double hostHz = CemuBridge_GetHostDisplayHz();

			(void)s_guestSwapHzHistory;
			(void)s_presentHzHistory;
			(void)s_historyCount;
			(void)effectiveSwapHz;
			(void)effectiveGuestSwapHz;
			(void)hostHz;

			CemuBridge_SetCurrentFPS(effectivePresentHz);

			double guestSeconds = (double)deltaGuestTicks / 62156250.0;
			double hostSeconds = elapsedSec;
			double guestClockSpeedPercent = (hostSeconds > 0.0) ? ((guestSeconds / hostSeconds) * 100.0) : 100.0;
			CemuBridge_SetGuestClockSpeedPercent(guestClockSpeedPercent);

			(void)guestSeconds;
			(void)hostSeconds;
			(void)deltaSchedQ;
			(void)deltaPpcEnters;
			(void)effectiveVsyncHz;

			uint64_t curGuestCycles = g_emulatedGuestCycles.load(std::memory_order_relaxed);
			uint64_t deltaGuestCycles = (curGuestCycles >= s_lastGuestCycles) ? (curGuestCycles - s_lastGuestCycles) : 0;
			// In single-core Espresso emulation, expected cycles per host second = 1.243125 GHz (or target quantum rate)
			// Base guest speed on 60Hz * 45000 cycles/quantum * target active core load or nominal 1.243125 GHz clock
			double expectedCycles = elapsedSec * 1243125000.0;
			double cycleSpeedPercent = (expectedCycles > 0.0) ? (((double)deltaGuestCycles / expectedCycles) * 100.0) : 100.0;
			CemuBridge_SetEmuSpeedPercent(cycleSpeedPercent);

			char perfMsg[384];
			if (effectivePresentHz < 1.0)
			{
				uint64_t availableMiB = 0;
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
				availableMiB = os_proc_available_memory() / 1048576;
#endif
				// This runs only in the existing two-second watchdog sample. The
				// deltas distinguish a guest loading pause from a blocked renderer,
				// and active pipeline counts expose a first-use compiler stall.
				uint64_t oldestShaderStart = UINT64_MAX, shaderBaseHash = 0, shaderAuxHash = 0, shaderBytes = 0;
				uint32_t shaderType = 0, preserveInvariance = 0;
				for (const auto& probe : RuntimePerformance::shaderCompileProbes)
				{
					const uint64_t started = probe.startedNs.load(std::memory_order_acquire);
					if (started && started < oldestShaderStart)
					{
						oldestShaderStart = started;
						shaderBaseHash = probe.baseHash.load(std::memory_order_relaxed);
						shaderAuxHash = probe.auxHash.load(std::memory_order_relaxed);
						shaderBytes = probe.sourceBytes.load(std::memory_order_relaxed);
						shaderType = probe.shaderType.load(std::memory_order_relaxed);
						preserveInvariance = probe.preserveInvariance.load(std::memory_order_relaxed);
					}
				}
				snprintf(perfMsg, sizeof(perfMsg),
				         "[STALL] fps=%.1f waitVsync=%llu guestSwap=%llu metalSwap=%llu present=%llu cycles=%llu rssMiB=%llu availMiB=%llu stagingMiB=%llu pipeWait=%u pipeCompile=%u pipeQueue=%llu shaderWait=%u mslCompile=%u fnCreate=%u pipeCreate=%u shader=%016llx_%016llx/%llukiB/%llums/type%u/inv%u",
				         effectivePresentHz,
				         (unsigned long long)deltaWaitVsync,
				         (unsigned long long)deltaGuestSwap,
				         (unsigned long long)deltaSwap,
				         (unsigned long long)deltaPresent,
				         (unsigned long long)deltaGuestCycles,
				         (unsigned long long)(QueryRamUsage() / 1048576),
				         (unsigned long long)availableMiB,
				         (unsigned long long)(RuntimePerformance::stagingBufferBytes.load(std::memory_order_relaxed) / 1048576),
				         RuntimePerformance::activePipelineWaiters.load(std::memory_order_relaxed),
				         RuntimePerformance::activePipelineCompiles.load(std::memory_order_relaxed),
				         (unsigned long long)RuntimePerformance::pendingPipelines.load(std::memory_order_relaxed),
				         RuntimePerformance::activeShaderWaiters.load(std::memory_order_relaxed),
				         RuntimePerformance::activeShaderLibraryCompiles.load(std::memory_order_relaxed),
				         RuntimePerformance::activeShaderFunctionCreates.load(std::memory_order_relaxed),
				         RuntimePerformance::activePipelineCreates.load(std::memory_order_relaxed),
				         (unsigned long long)shaderBaseHash,
				         (unsigned long long)shaderAuxHash,
				         (unsigned long long)(shaderBytes / 1024),
				         (unsigned long long)(oldestShaderStart == UINT64_MAX ? 0 :
				             (RuntimePerformance::shaderProbeNowNs() - oldestShaderStart) / 1000000),
				         shaderType, preserveInvariance);
			}
			else
			{
				snprintf(perfMsg, sizeof(perfMsg),
				         "[PERF] fps=%.1f clock=%.0f%% compiled=%llu queue=%zu",
				         effectivePresentHz,
				         guestClockSpeedPercent,
				         (unsigned long long)compiled,
				         qDepth);
			}
			CemuBridge_LogJITMilestone(perfMsg);

			s_lastVsyncCheckTime = nowTime;
			s_lastWaitVsync = curWaitVsync;
			s_lastSwap = curSwap;
			s_lastPresent = curPresent;
			s_lastGuestSwap = curGuestSwap;
			s_lastSchedQ = curSchedQ;
			s_lastPpcEnters = curPpcEnters;
			s_lastPpcReturns = rets;
			s_lastGuestTicks = curGuestTicks;
			s_lastGuestCycles = curGuestCycles;
		}
	}
}


void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_visited)();
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_unvisited)();

PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;

#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
static std::mutex s_singleRecompilationMutex;
#endif

void PPCRecompiler_recompileAtAddress(uint32 address);

// this function does never block and can fail if the recompiler lock cannot be acquired immediately
void PPCRecompiler_visitAddressNoBlock(uint32 enterAddress)
{
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	s_ppcRecompilerState.recompilerSpinlock.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;
	s_ppcRecompilerState.recompilerSpinlock.unlock();
	s_singleRecompilationMutex.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] == PPCRecompiler_leaveRecompilerCode_visited)
	{
		PPCRecompiler_recompileAtAddress(enterAddress);
	}
	s_singleRecompilationMutex.unlock();
	return;
#endif
	// quick read-only check without lock
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	// try to acquire lock
	if (!s_ppcRecompilerState.recompilerSpinlock.try_lock())
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		// was visited since previous check
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	// add to recompilation queue and flag as visited
	s_ppcRecompilerState.targetQueue.emplace(enterAddress);
	size_t curQ = s_ppcRecompilerState.targetQueue.size();
	uint64_t prevMaxQ = s_jitStats.maxQueueDepth.load(std::memory_order_relaxed);
	while (curQ > prevMaxQ && !s_jitStats.maxQueueDepth.compare_exchange_weak(prevMaxQ, curQ)) {}
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;

	s_ppcRecompilerState.recompilerSpinlock.unlock();
}

void PPCRecompiler_recompileIfUnvisited(uint32 enterAddress)
{
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	PPCRecompiler_visitAddressNoBlock(enterAddress);
}

void PPCRecompiler_enter(PPCInterpreter_t* hCPU, PPCREC_JUMP_ENTRY funcPtr)
{
	static std::atomic_uint64_t s_relEnterCount{0};
	const uint64_t relEnterIdx = s_relEnterCount.fetch_add(1, std::memory_order_relaxed) + 1;
	if (relEnterIdx <= 16)
	{
		char msg[192];
		snprintf(msg, sizeof(msg), "[JIT EXEC #%llu] enter PPC=0x%08X host=%p remainingCycles=%d",
		         (unsigned long long)relEnterIdx, hCPU->instructionPointer, (void*)funcPtr, hCPU->remainingCycles);
		CemuBridge_LogJITMilestone(msg);
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		if (relEnterIdx == 1)
		{
			JIT26_QueryAndLogVM("interface", (const void*)PPCRecompiler_enterRecompilerCode);
			JIT26_QueryAndLogVM("destination", (const void*)funcPtr);
		}
#endif
	}

#ifndef NDEBUG
	// Per-entry cross-core counters belong only to diagnostic builds.
	static std::atomic_bool s_firstBlockEntered{false};
	if (!s_firstBlockEntered.exchange(true, std::memory_order_relaxed))
	{
		char msg[128];
		snprintf(msg, sizeof(msg), "[JIT] first compiled block entered: 0x%08X", hCPU->instructionPointer);
		CemuBridge_LogJITMilestone(msg);
	}

	uint64_t enterIdx = ++g_jitCppEnterCount;
	s_jitStats.jitEnterCount.store(enterIdx, std::memory_order_relaxed);
	s_jitStats.lastPPCAddressEntered.store(hCPU->instructionPointer, std::memory_order_relaxed);
	s_jitStats.lastHostEntered.store((uintptr_t)funcPtr, std::memory_order_relaxed);
#endif

#if BOOST_OS_WINDOWS
	uint32 prevState = _controlfp(0, 0);
	_controlfp(_RC_NEAR, _MCW_RC);
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
	_controlfp(prevState, _MCW_RC);
#else
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
#endif

	if (relEnterIdx <= 16)
	{
		const char* exitReason = (hCPU->remainingCycles > 0) ? "UNVISITED_OR_BRANCH" : "CYCLE_EXPIRED";
		char retLog[256];
		snprintf(retLog, sizeof(retLog),
		         "[JIT RETURN #%llu] reason=%s guestPPC=0x%08X remainingCycles=%d",
		         (unsigned long long)relEnterIdx, exitReason, hCPU->instructionPointer, hCPU->remainingCycles);
		CemuBridge_LogJITMilestone(retLog);
	}

#ifndef NDEBUG
	uint64_t retIdx = ++g_jitCppReturnCount;
	s_jitStats.jitReturnCount.store(retIdx, std::memory_order_relaxed);
#endif

	// after leaving recompiler prematurely attempt to recompile the code at the new location
	if (hCPU->remainingCycles > 0)
	{
		PPCRecompiler_visitAddressNoBlock(hCPU->instructionPointer);
	}
}

void PPCRecompiler_attemptEnterWithoutRecompile(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited && funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}

void PPCRecompiler_attemptEnter(PPCInterpreter_t* hCPU, uint32 enterAddress, JITCallerSite caller)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	if (hCPU->remainingCycles <= 0)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr == PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		PPCRecompiler_visitAddressNoBlock(enterAddress);
	}
	else if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		// a compiled entry exists, run it
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}
bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext);

PPCRecFunction_t* PPCRecompiler_recompileFunction(PPCFunctionBoundaryTracker::PPCRange_t range, std::set<uint32>& entryAddresses, std::vector<std::pair<MPTR, uint32>>& entryPointsOut, PPCFunctionBoundaryTracker& boundaryTracker)
{
	auto tStart = std::chrono::high_resolution_clock::now();
	auto recordFailure = [&]() {
		s_jitStats.compileFailureCount.fetch_add(1, std::memory_order_relaxed);
	};

	if (range.startAddress >= PPC_REC_CODE_AREA_END)
	{
		cemuLog_log(LogType::Force, "Attempting to recompile function outside of allowed code area");
		recordFailure();
		return nullptr;
	}
	uint32 codeGenRangeStart;
	uint32 codeGenRangeSize = 0;
	coreinit::OSGetCodegenVirtAddrRangeInternal(codeGenRangeStart, codeGenRangeSize);
	if (codeGenRangeSize != 0)
	{
		if (range.startAddress >= codeGenRangeStart && range.startAddress < (codeGenRangeStart + codeGenRangeSize))
		{
			if (coreinit::codeGenShouldAvoid())
			{
				recordFailure();
				return nullptr;
			}
		}
	}

	// Compilation can fail before the function reaches the range store.
	// Keep its host metadata owned until publication takes over its lifetime.
	auto functionOwner = std::make_unique<PPCRecFunction_t>();
	PPCRecFunction_t* ppcRecFunc = functionOwner.get();
	ppcRecFunc->ppcAddress = range.startAddress;
	ppcRecFunc->ppcSize = range.length;

#if PPCREC_LOG_RECOMPILATION_RESULTS
	BenchmarkTimer bt;
	bt.Start();
#endif

	// generate intermediate code
	ppcImlGenContext_t ppcImlGenContext = { 0 };
	ppcImlGenContext.debug_entryPPCAddress = range.startAddress;
	bool compiledSuccessfully = PPCRecompiler_generateIntermediateCode(ppcImlGenContext, ppcRecFunc, entryAddresses, boundaryTracker);
	if (compiledSuccessfully == false)
	{
		recordFailure();
		return nullptr;
	}

	uint32 ppcRecLowerAddr = LaunchSettings::GetPPCRecLowerAddr();
	uint32 ppcRecUpperAddr = LaunchSettings::GetPPCRecUpperAddr();

	if (ppcRecLowerAddr != 0 && ppcRecUpperAddr != 0)
	{
		if (ppcRecFunc->ppcAddress < ppcRecLowerAddr || ppcRecFunc->ppcAddress > ppcRecUpperAddr)
		{
			recordFailure();
			return nullptr;
		}
	}

	// apply passes
	if (!PPCRecompiler_ApplyIMLPasses(ppcImlGenContext))
	{
		recordFailure();
		return nullptr;
	}

#if defined(ARCH_X86_64)
	// emit x64 code
	bool x64GenerationSuccess = PPCRecompiler_generateX64Code(ppcRecFunc, &ppcImlGenContext);
	if (x64GenerationSuccess == false)
	{
		recordFailure();
		return nullptr;
	}
#elif defined(__aarch64__)
	bool aarch64GenerationSuccess = PPCRecompiler_generateAArch64Code(ppcRecFunc, &ppcImlGenContext);
	if (aarch64GenerationSuccess == false)
	{
		recordFailure();
		return nullptr;
	}
#endif
	if (ActiveSettings::DumpRecompilerFunctionsEnabled())
	{
		FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(fmt::format("dump/recompiler/ppc_{:08x}.bin", ppcRecFunc->ppcAddress)));
		if (fs)
		{
			fs->writeData(ppcRecFunc->x86Code, ppcRecFunc->x86Size);
			delete fs;
		}
	}

	// collect list of PPC-->x64 entry points
	entryPointsOut.clear();
	for(IMLSegment* imlSegment : ppcImlGenContext.segmentList2)
	{
		if (imlSegment->isEnterable == false)
			continue;

		uint32 ppcEnterOffset = imlSegment->enterPPCAddress;
		uint32 x64Offset = imlSegment->x64Offset;

		entryPointsOut.emplace_back(ppcEnterOffset, x64Offset);
	}

#if PPCREC_LOG_RECOMPILATION_RESULTS
	bt.Stop();
	uint32 codeHash = 0;
	for (uint32 i = 0; i < ppcRecFunc->x86Size; i++)
	{
		codeHash = _rotr(codeHash, 3);
		codeHash += ((uint8*)ppcRecFunc->x86Code)[i];
	}
	cemuLog_log(LogType::Force, "[Recompiler] PPC 0x{:08x} -> x64: 0x{:x} Took {:.4}ms | Size {:04x} CodeHash {:08x}", (uint32)ppcRecFunc->ppcAddress, (uint64)(uintptr_t)ppcRecFunc->x86Code, bt.GetElapsedMilliseconds(), ppcRecFunc->x86Size, codeHash);
#endif

	auto tEnd = std::chrono::high_resolution_clock::now();
	uint64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(tEnd - tStart).count();
	s_jitStats.compiledFunctionCount.fetch_add(1, std::memory_order_relaxed);
	s_jitStats.totalCompileTimeUs.fetch_add(durUs, std::memory_order_relaxed);
	uint64_t prevMax = s_jitStats.maxCompileTimeUs.load(std::memory_order_relaxed);
	while (durUs > prevMax && !s_jitStats.maxCompileTimeUs.compare_exchange_weak(prevMax, durUs)) {}

	return functionOwner.release();
}

void PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext_t& ppcImlGenContext)
{
	IMLRegisterAllocatorParameters raParam;

	for (auto& it : ppcImlGenContext.mappedRegs)
		raParam.regIdToName.try_emplace(it.second.GetRegID(), it.first);

#if defined(ARCH_X86_64)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RAX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBP);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RSI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R8);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R9);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R10);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R11);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R12);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RCX);

	// add XMM registers, except XMM15 which is the temporary register
	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 0);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 1);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 2);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 3);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 4);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 5);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 6);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 7);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 8);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 9);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 10);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 11);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 12);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 13);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 14);
#elif defined(__aarch64__)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	for (auto i = IMLArchAArch64::PHYSREG_GPR_BASE; i < IMLArchAArch64::PHYSREG_GPR_BASE + IMLArchAArch64::PHYSREG_GPR_COUNT; i++)
	{
		if (i == IMLArchAArch64::PHYSREG_GPR_BASE + 18)
			continue; // Skip reserved platform register
		gprPhysPool.SetAvailable(i);
	}

	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	for (auto i = IMLArchAArch64::PHYSREG_FPR_BASE; i < IMLArchAArch64::PHYSREG_FPR_BASE + IMLArchAArch64::PHYSREG_FPR_COUNT; i++)
		fprPhysPool.SetAvailable(i);
#endif

	IMLRegisterAllocator_AllocateRegisters(&ppcImlGenContext, raParam);
}

bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext)
{
	// isolate entry points from function flow (enterable segments must not be the target of any other segment)
	// this simplifies logic during register allocation
	PPCRecompilerIML_isolateEnterableSegments(&ppcImlGenContext);

	// merge certain float load+store patterns
	IMLOptimizer_OptimizeDirectFloatCopies(&ppcImlGenContext);
	// delay byte swapping for certain load+store patterns
	IMLOptimizer_OptimizeDirectIntegerCopies(&ppcImlGenContext);

	IMLOptimizer_StandardOptimizationPass(ppcImlGenContext);

	PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext);

	return true;
}

bool PPCRecompiler_makeRecompiledFunctionActive(uint32 initialEntryPoint, PPCFunctionBoundaryTracker::PPCRange_t& range, PPCRecFunction_t* ppcRecFunc, std::vector<std::pair<MPTR, uint32>>& entryPoints)
{
	std::unique_ptr<PPCRecFunction_t> functionOwner(ppcRecFunc);
	// update jump table
	s_ppcRecompilerState.recompilerSpinlock.lock();

	// check if the initial entrypoint is still flagged for recompilation
	// its possible that the range has been invalidated during the time it took to translate the function
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[initialEntryPoint / 4] != PPCRecompiler_leaveRecompilerCode_visited)
	{
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return false;
	}

	// check if the current range got invalidated during the time it took to recompile it
	bool isInvalidated = false;
	for (auto& invRange : s_ppcRecompilerState.invalidationRanges)
	{
		MPTR rStartAddr = invRange.startAddress;
		MPTR rEndAddr = rStartAddr + invRange.size;
		for (auto& recFuncRange : ppcRecFunc->list_ranges)
		{
			if (recFuncRange.ppcAddress < (rEndAddr) && (recFuncRange.ppcAddress + recFuncRange.ppcSize) > rStartAddr)
			{
				isInvalidated = true;
				break;
			}
		}
	}
	s_ppcRecompilerState.invalidationRanges.clear();
	if (isInvalidated)
	{
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return false;
	}

	// update jump table and remember which entries we updated
	cemu_assert_debug(ppcRecFunc->jumpTableEntries.empty());
	for (auto& itr : entryPoints)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[itr.first / 4] = (PPCREC_JUMP_ENTRY)((uint8*)ppcRecFunc->x86Code + itr.second);
		ppcRecFunc->jumpTableEntries.emplace_back(itr.first, ((uint8*)ppcRecFunc->x86Code + itr.second));
	}

	// due to inlining, some entrypoints can get optimized away
	// therefore we reset all addresses that are still marked as visited (but not recompiled)
	// we dont remove the points from the queue but any address thats not marked as visited won't get recompiled
	// if they are reachable, the interpreter will queue them again
	for (uint32 v = range.startAddress; v < (range.startAddress + range.length); v += 4)
	{
		auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4];
		if (funcPtr == PPCRecompiler_leaveRecompilerCode_visited)
			ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}

	// register ranges
	for (auto& r : ppcRecFunc->list_ranges)
	{
		r.storedRange = s_ppcRecompilerState.functionStorage.storeRange(ppcRecFunc, r.ppcAddress, r.ppcAddress + r.ppcSize);
	}
	// An invalidation can remove the function as soon as this lock is released.
	// Diagnostics below must use a value copy, not the function metadata.
	void* nativeCode = ppcRecFunc->x86Code;
	functionOwner.release();
	s_ppcRecompilerState.recompilerSpinlock.unlock();

	// Milestone logging is done OUTSIDE the spinlock to prevent lock contention,
	// priority inversion on efficiency cores, and thread starvation!
	static std::atomic<uint64_t> s_publishCount{0};
	uint64_t pubIdx = s_publishCount.fetch_add(entryPoints.size(), std::memory_order_relaxed) + entryPoints.size();
	if (pubIdx <= 20 || (pubIdx % 20000 < entryPoints.size()))
	{
		char msg[256];
		snprintf(msg, sizeof(msg),
		         "[JIT TARGET] pub#%llu guestPPC=0x%08X x86Code=%p entries=%zu",
		         (unsigned long long)pubIdx, range.startAddress, nativeCode, entryPoints.size());
		CemuBridge_LogJITMilestone(msg);
	}
	return true;
}

void PPCRecompiler_recompileAtAddress(uint32 address)
{
	cemu_assert_debug(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[address / 4] == PPCRecompiler_leaveRecompilerCode_visited);

	// get size
	PPCFunctionBoundaryTracker funcBoundaries;
	funcBoundaries.trackStartPoint(address);
	// get range that encompasses address
	PPCFunctionBoundaryTracker::PPCRange_t range;
	if (funcBoundaries.getRangeForAddress(address, range) == false)
	{
		cemu_assert_debug(false);
	}

	// todo - use info from previously compiled ranges to determine full size of this function (and merge all the entryAddresses)

	// collect all currently known entry points for this range
	s_ppcRecompilerState.recompilerSpinlock.lock();
	std::set<uint32> entryAddresses;
	entryAddresses.emplace(address);
	s_ppcRecompilerState.recompilerSpinlock.unlock();

	std::vector<std::pair<MPTR, uint32>> functionEntryPoints;
	PPCRecFunction_t* func = PPCRecompiler_recompileFunction(range, entryAddresses, functionEntryPoints, funcBoundaries);
	if (!func)
		return; // recompilation failed
	PPCRecompiler_makeRecompiledFunctionActive(address, range, func, functionEntryPoints);
}

void PPCRecompiler_thread()
{
	SetThreadName("PPCRecompiler");
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	return;
#endif

	while (true)
	{
        if(s_ppcRecompilerState.workerThreadStopSignal)
            return;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		// asynchronous recompilation:
		// 1) take address from queue
		// 2) check if address is still marked as visited
		// 3) if yes -> calculate size, gather all entry points, recompile and update jump table
		while (true)
		{
			s_ppcRecompilerState.recompilerSpinlock.lock();
			if (s_ppcRecompilerState.targetQueue.empty())
			{
				s_ppcRecompilerState.recompilerSpinlock.unlock();
				break;
			}
			auto enterAddress = s_ppcRecompilerState.targetQueue.front();
			s_ppcRecompilerState.targetQueue.pop();

			auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
			if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
			{
				// only recompile functions if marked as visited
				s_ppcRecompilerState.recompilerSpinlock.unlock();
				continue;
			}
			s_ppcRecompilerState.recompilerSpinlock.unlock();

			s_jitStats.workerCompilationCount.fetch_add(1, std::memory_order_relaxed);
			PPCRecompiler_recompileAtAddress(enterAddress);
			if(s_ppcRecompilerState.workerThreadStopSignal)
				return;
		}
	}
}

#define PPC_REC_ALLOC_BLOCK_SIZE	(4*1024*1024) // 4MB

constexpr uint32 PPCRecompiler_GetNumAddressSpaceBlocks()
{
    return (MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE + PPC_REC_ALLOC_BLOCK_SIZE - 1) / PPC_REC_ALLOC_BLOCK_SIZE;
}

std::bitset<PPCRecompiler_GetNumAddressSpaceBlocks()> ppcRecompiler_reservedBlockMask;

void PPCRecompiler_reserveLookupTableBlock(uint32 offset)
{
	uint32 blockIndex = offset / PPC_REC_ALLOC_BLOCK_SIZE;
	offset = blockIndex * PPC_REC_ALLOC_BLOCK_SIZE;

	if (ppcRecompiler_reservedBlockMask[blockIndex])
		return;
	ppcRecompiler_reservedBlockMask[blockIndex] = true;

	void* p = MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), MemMapper::PAGE_PERMISSION::P_RW, true);
	if( !p )
	{
		cemuLog_log(LogType::Force, "Failed to allocate memory for recompiler (0x{:08x})", offset);
		cemu_assert(false);
		return;
	}
	for(uint32 i=0; i<PPC_REC_ALLOC_BLOCK_SIZE/4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4+i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	uint32 endAddress = (startAddress + size + PPC_REC_ALLOC_BLOCK_SIZE - 1) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = (startAddress) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = std::min(startAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	endAddress = std::min(endAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	for (uint32 i = startAddress; i < endAddress; i += PPC_REC_ALLOC_BLOCK_SIZE)
	{
		PPCRecompiler_reserveLookupTableBlock(i);
	}
}

bool PPCRecompiler_findFuncRanges(uint32 addr, ppcRecompilerFuncRange* rangesOut, size_t* countInOut)
{
	s_ppcRecompilerState.recompilerSpinlock.lock();
	size_t countIn = *countInOut;
	size_t countOut = 0;

	s_ppcRecompilerState.functionStorage.findRanges(addr, addr + 4, [rangesOut, countIn, &countOut](uint32 start, uint32 end, PPCRecFunction_t* func)
	{
		if (countOut < countIn)
		{
			rangesOut[countOut].ppcStart = start;
			rangesOut[countOut].ppcSize = (end-start);
			rangesOut[countOut].x86Start = func->x86Code;
			rangesOut[countOut].x86Size = func->x86Size;
		}
		countOut++;
	}
	);
	s_ppcRecompilerState.recompilerSpinlock.unlock();
	*countInOut = countOut;
	if (countOut > countIn)
		return false;
	return true;
}

extern "C" DLLEXPORT uintptr_t* PPCRecompiler_getJumpTableBase()
{
	if (ppcRecompilerInstanceData == nullptr)
		return nullptr;
	return (uintptr_t*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
}

void PPCRecompiler_deleteFunction(PPCRecFunction_t* func)
{
	cemu_assert_debug(s_ppcRecompilerState.recompilerSpinlock.is_locked());
	// unlink entrypoints from JumpTable
	for (auto& entrypoint : func->jumpTableEntries)
	{
		if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[entrypoint.ppcAddr / 4] == entrypoint.hostEntrypoint)
			ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[entrypoint.ppcAddr / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
	// delete from storage
	for (auto& r : func->list_ranges)
	{
		if(r.storedRange)
			s_ppcRecompilerState.functionStorage.deleteRange(r.storedRange);
		r.storedRange = nullptr;
	}
	// Native code may still be executing on another guest core. Its existing
	// arena lifetime is unchanged; only CPU-side range/jump-table metadata dies.
	delete func;
}

void PPCRecompiler_invalidateRange(uint32 startAddr, uint32 endAddr)
{
	if (!s_ppcRecompilerState.initialized)
		return;
	if (startAddr >= PPC_REC_CODE_AREA_SIZE)
		return;
	cemu_assert_debug(endAddr >= startAddr);

	s_ppcRecompilerState.recompilerSpinlock.lock();

	uint32 rStart;
	uint32 rEnd;
	PPCRecFunction_t* rFunc;
	// delete functions which intersect the invalidated range
	while (s_ppcRecompilerState.functionStorage.findFirstRange(startAddr, endAddr, rStart, rEnd, rFunc))
		PPCRecompiler_deleteFunction(rFunc);
	// add entry to invalidation queue, this is used to invalidate functions for which recompilation has already started
	s_ppcRecompilerState.invalidationRanges.emplace_back(startAddr, endAddr-startAddr);

	s_ppcRecompilerState.recompilerSpinlock.unlock();
}

#if defined(ARCH_X86_64)
void PPCRecompiler_initPlatform()
{
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[1] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[0] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[1] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[1] = ~0ULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[1] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[0] = ~(1 << 31);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[1] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[2] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[3] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_singleWordMask[0] = 0xFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_singleWordMask[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[0] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[1] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[0] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[1] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[0] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[1] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[0] = 1.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[1] = 1.0f;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[0] = 0x00800000;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[1] = 0x00800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[0] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[1] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[2] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[3] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[0] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[1] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[2] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[3] = ~0x80000000;

	// mxcsr
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOn = 0x1F80 | 0x8000;
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOff = 0x1F80;
}
#else
void PPCRecompiler_initPlatform()
{

}
#endif

void PPCRecompiler_init()
{
	CemuBridge_LogJITMilestone("[JIT] PPCRecompiler_init entered");
	s_ppcRecompilerState.recompilerEnableCount = 0;
	if (ActiveSettings::GetCPUMode() == CPUMode::SinglecoreInterpreter)
	{
		cemuLog_log(LogType::Force, "Using singlecore interpreter");
		return;
	}
	if (LaunchSettings::ForceInterpreter() || LaunchSettings::ForceMultiCoreInterpreter())
	{
		cemuLog_log(LogType::Force, "Recompiler disabled. Command line --force-interpreter or force-multicore-interpreter was passed");
		return;
	}

	if (ppcRecompilerInstanceData)
	{
		MemMapper::FreeReservation(ppcRecompilerInstanceData, sizeof(PPCRecompilerInstanceData_t));
		ppcRecompilerInstanceData = nullptr;
	}
	cemuLog_logDebug(LogType::Force, "Reserving {}MB for recompiler instance data", (sint32)(sizeof(PPCRecompilerInstanceData_t) / 1024 / 1024));
	ppcRecompilerInstanceData = (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, sizeof(PPCRecompilerInstanceData_t), MemMapper::PAGE_PERMISSION::P_RW);
	MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom), sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW, true);
#ifdef ARCH_X86_64
	PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
#elif defined(__aarch64__)
	PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();
#endif
    PPCRecompiler_allocateRange(0, 0x1000); // the first entry is used for fallback to interpreter
    PPCRecompiler_allocateRange(mmuRange_TRAMPOLINE_AREA.getBase(), mmuRange_TRAMPOLINE_AREA.getSize());
    PPCRecompiler_allocateRange(mmuRange_CODECAVE.getBase(), mmuRange_CODECAVE.getSize());

    PPCRecompiler_initPlatform();

	cemuLog_log(LogType::Force, "Recompiler initialized");

	s_ppcRecompilerState.initialized = true;
	s_ppcRecompilerState.recompilerEnableCount = 1; // enabled

	// launch recompilation thread
    s_ppcRecompilerState.workerThreadStopSignal = false;
    s_ppcRecompilerState.workerThread = std::thread(PPCRecompiler_thread);

	// launch watchdog monitor thread
	s_jitWatchdogStopSignal = false;
	if (!s_jitWatchdogThread.joinable())
		s_jitWatchdogThread = std::thread(PPCRecompiler_watchdogThread);
}

void PPCRecompiler_Shutdown()
{
	// shut down watchdog thread
	s_jitWatchdogStopSignal = true;
	if (s_jitWatchdogThread.joinable())
		s_jitWatchdogThread.join();

    // shut down recompiler thread
    s_ppcRecompilerState.workerThreadStopSignal = true;
    if(s_ppcRecompilerState.workerThread.joinable())
        s_ppcRecompilerState.workerThread.join();
    // clean up queues
    while(!s_ppcRecompilerState.targetQueue.empty())
        s_ppcRecompilerState.targetQueue.pop();
    s_ppcRecompilerState.invalidationRanges.clear();
    // The range store owns entries, not function metadata. The unlink callback
    // removes all ranges of each function without allocating during teardown.
    s_ppcRecompilerState.recompilerSpinlock.lock();
    s_ppcRecompilerState.functionStorage.drain([](PPCRecFunction_t* function)
    {
        PPCRecompiler_deleteFunction(function);
    });
    s_ppcRecompilerState.recompilerSpinlock.unlock();
    // clean up memory
    uint32 numBlocks = PPCRecompiler_GetNumAddressSpaceBlocks();
    for(uint32 i=0; i<numBlocks; i++)
    {
        if(!ppcRecompiler_reservedBlockMask[i])
            continue;
        // deallocate
        uint64 offset = i * PPC_REC_ALLOC_BLOCK_SIZE;
        MemMapper::FreeMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), true);
        // mark as unmapped
        ppcRecompiler_reservedBlockMask[i] = false;
    }
	s_ppcRecompilerState.recompilerEnableCount = 0;
	s_ppcRecompilerState.initialized = false;
}

// For each Enable call, Disable needs to be called once and vice versa
void PPCRecompiler_Enable()
{
	if (s_ppcRecompilerState.initialized)
		s_ppcRecompilerState.recompilerEnableCount++;
}

void PPCRecompiler_Disable()
{
	if (s_ppcRecompilerState.initialized)
		s_ppcRecompilerState.recompilerEnableCount--;
}
