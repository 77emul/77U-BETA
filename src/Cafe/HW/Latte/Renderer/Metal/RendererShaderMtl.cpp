#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include "Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalShaderLibraryCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryPolicy.h"
#include "Common/MemoryBudget.h"
#include <os/proc.h>

#include "Cemu/Logging/CemuLogging.h"
#include "Common/precompiled.h"
#include "GameProfile/GameProfile.h"
#include "util/helpers/helpers.h"


static bool s_isLoadingShadersMtl{false};
static MetalShaderLibraryCache<MTL::Library> s_shaderLibraryCache;
// Share immutable entry functions by exact emitted program, device and options.
// A guest shader keeps its own retain across bounded eviction or cache clearing.
static MetalShaderLibraryCache<MTL::Function> s_shaderFunctionCache;

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

class ShaderMtlThreadPool
{
public:
	void StartThreads()
	{
		if (m_threadsActive.exchange(true))
			return;

		// Create thread pool
		uint32 threadCount = 2;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
		uint32 cpuCoreCount = GetPhysicalCoreCount();
		if (cpuCoreCount >= 8)
			threadCount = std::min(8u, cpuCoreCount - 2);
		else if (cpuCoreCount >= 6)
			threadCount = 4;
		// Each worker can own a Metal compiler and its transient allocations.
		// Leave the existing parallelism intact on devices with more than 6 GiB.
		m_legacyWorkerCount = threadCount;
		threadCount = static_cast<uint32>(GetMetalMemoryPolicy().CompilationWorkers(threadCount));
#endif
		for (uint32 i = 0; i < threadCount; ++i)
			s_threads.emplace_back(&ShaderMtlThreadPool::CompilerThreadFunc, this, false);

	}

	void StopThreads()
	{
		StopLoadingThreads();
		if (!m_threadsActive.exchange(false))
			return;
		for (uint32 i = 0; i < s_threads.size(); ++i)
			s_compilationQueueCount.increment();
		for (auto& it : s_threads)
			it.join();
		s_threads.clear();

	}

	~ShaderMtlThreadPool()
	{
		StopThreads();
	}

	// Extra workers for the shader cache loading screen only (constrained
	// devices keep two gameplay workers). They share the job semaphore with
	// the gameplay workers, so a single wake-up per extra worker is not
	// enough: a gameplay worker may consume it, find the queue empty and go
	// back to sleep, leaving the extra worker blocked and join() waiting
	// forever (seen as a hang right after "Compiling cached shaders").
	// StopLoadingThreads therefore keeps waking until every extra worker
	// has exited. Surplus wake-ups only make a gameplay worker look at an
	// empty queue once; queued jobs never lose their count.
	void StartLoadingThreads(size_t count)
	{
		if (!m_threadsActive.load() || m_loadingActive.exchange(true))
			return;
		for (size_t i = 0; i < count; ++i)
		{
			m_loadingAlive.fetch_add(1, std::memory_order_acq_rel);
			m_loadingThreads.emplace_back(&ShaderMtlThreadPool::CompilerThreadFunc, this, true);
		}
	}

	void StopLoadingThreads()
	{
		if (!m_loadingActive.exchange(false))
			return;
		while (m_loadingAlive.load(std::memory_order_acquire) > 0)
		{
			s_compilationQueueCount.increment();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		for (auto& it : m_loadingThreads)
			it.join();
		m_loadingThreads.clear();
	}

	void CompilerThreadFunc(bool loadingOnly = false)
	{
		SetThreadName(loadingOnly ? "mtlShaderLoad" : "mtlShaderComp");
#if defined(__APPLE__)
		// First-use shaders gate visible geometry, even with async compilation.
		// Keep the pool bounded, but do not schedule it as background maintenance.
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
		while (m_threadsActive.load(std::memory_order::relaxed) &&
			   (!loadingOnly || m_loadingActive.load(std::memory_order::relaxed)))
		{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
			// Back off while the process nears its memory target; the regular
			// workers keep compiling, so progress never stops.
			if (loadingOnly && MemoryBudget::EffectiveAvailableMemory(os_proc_available_memory()) < 768u * 1024 * 1024)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}
#endif
			s_compilationQueueCount.decrementWithWait();
			if (loadingOnly && !m_loadingActive.load(std::memory_order::relaxed))
			{
				// The count may belong to a queued job: give it back so a
				// gameplay worker still compiles that job.
				s_compilationQueueCount.increment();
				break;
			}
			s_compilationQueueMutex.lock();
			if (s_compilationQueue.empty())
			{
				// queue empty again, shaders compiled synchronously via PreponeCompilation()
				s_compilationQueueMutex.unlock();
				continue;
			}
			RendererShaderMtl* job = s_compilationQueue.front();
			s_compilationQueue.pop_front();
			// set compilation state
			cemu_assert_debug(job->m_compilationState.getValue() == RendererShaderMtl::COMPILATION_STATE::QUEUED);
			job->m_compilationState.setValue(RendererShaderMtl::COMPILATION_STATE::COMPILING);
			s_compilationQueueMutex.unlock();
			// compile
			job->CompileInternal();
			if (job->ShouldCountCompilation())
			    ++g_compiled_shaders_async;
			// mark as compiled
			cemu_assert_debug(job->m_compilationState.getValue() == RendererShaderMtl::COMPILATION_STATE::COMPILING);
			job->m_compilationState.setValue(RendererShaderMtl::COMPILATION_STATE::DONE);
		}
		if (loadingOnly)
			m_loadingAlive.fetch_sub(1, std::memory_order_acq_rel);
	}


	bool HasThreadsRunning() const { return m_threadsActive; }

public:
	std::vector<std::thread> s_threads;
	std::vector<std::thread> m_loadingThreads;
	size_t m_legacyWorkerCount = 2; // device default before the memory policy cap

	std::deque<RendererShaderMtl*> s_compilationQueue;
	CounterSemaphore s_compilationQueueCount;
	std::mutex s_compilationQueueMutex;


private:
	std::atomic<bool> m_threadsActive;
	std::atomic<bool> m_loadingActive{false};
	std::atomic<int> m_loadingAlive{0}; // extra loading workers that have not exited yet
} shaderMtlThreadPool;

// Keep startup on bounded per-program compilation. Opening an aggregate Metal
// IR archive before the workers caused multi-GiB host allocations on device.
// Existing driver and transferable caches remain available; do not bulk-load IR.
void RendererShaderMtl::ShaderCacheLoading_begin(uint64 cacheTitleId)
{
    s_isLoadingShadersMtl = true;
    s_shaderFunctionCache.Clear();
    s_shaderLibraryCache.Clear();


    // Maximize shader compilation speed
    static_cast<MetalRenderer*>(g_renderer.get())->SetShouldMaximizeConcurrentCompilation(true);
    // Constrained devices compile with two workers during gameplay; the
    // loading screen may use more (see MetalMemoryPolicy::LoadingWorkers).
    const auto& policy = GetMetalMemoryPolicy();
    const size_t loading = policy.LoadingWorkers(shaderMtlThreadPool.m_legacyWorkerCount);
    const size_t gameplay = shaderMtlThreadPool.s_threads.size();
    if (loading > gameplay)
        shaderMtlThreadPool.StartLoadingThreads(loading - gameplay);
}

void RendererShaderMtl::ShaderCacheLoading_end()
{
    s_isLoadingShadersMtl = false;
    shaderMtlThreadPool.StopLoadingThreads();

    // Reset shader compilation speed
    static_cast<MetalRenderer*>(g_renderer.get())->SetShouldMaximizeConcurrentCompilation(false);
}

void RendererShaderMtl::ShaderCacheLoading_Close()
{
}

size_t RendererShaderMtl::TrimReuseCaches() {
    return s_shaderLibraryCache.TrimCompleted() + s_shaderFunctionCache.TrimCompleted();
}
size_t CemuMetalShaderReuseSourceBytes() { return RendererShaderMtl::ReuseSourceBytes(); }
MetalShaderCacheStatistics CemuMetalShaderLibraryStatistics() { return s_shaderLibraryCache.GetStatistics(); }
MetalShaderCacheStatistics CemuMetalShaderFunctionStatistics() { return s_shaderFunctionCache.GetStatistics(); }
size_t RendererShaderMtl::ReuseSourceBytes() {
    return s_shaderLibraryCache.RetainedSourceBytes() + s_shaderFunctionCache.RetainedSourceBytes();
}
void RendererShaderMtl::Initialize()
{
    shaderMtlThreadPool.StartThreads();
}

void RendererShaderMtl::Shutdown()
{
    shaderMtlThreadPool.StopThreads();
    s_shaderFunctionCache.Clear();
    s_shaderLibraryCache.Clear();
}

RendererShaderMtl::RendererShaderMtl(MetalRenderer* mtlRenderer, ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& mslCode)
	: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPackShader), m_mtlr{mtlRenderer}, m_mslCode{mslCode}
{
	// start async compilation
	shaderMtlThreadPool.s_compilationQueueMutex.lock();
	m_compilationState.setValue(COMPILATION_STATE::QUEUED);
	shaderMtlThreadPool.s_compilationQueue.push_back(this);
	shaderMtlThreadPool.s_compilationQueueCount.increment();
	shaderMtlThreadPool.s_compilationQueueMutex.unlock();
	cemu_assert_debug(shaderMtlThreadPool.HasThreadsRunning()); // make sure .StartThreads() was called
}

RendererShaderMtl::~RendererShaderMtl()
{
    if (m_function)
        m_function->release();
}

void RendererShaderMtl::PreponeCompilation(bool isRenderThread)
{
	shaderMtlThreadPool.s_compilationQueueMutex.lock();
	bool isStillQueued = m_compilationState.hasState(COMPILATION_STATE::QUEUED);
	if (isStillQueued)
	{
		// remove from queue
		shaderMtlThreadPool.s_compilationQueue.erase(std::remove(shaderMtlThreadPool.s_compilationQueue.begin(), shaderMtlThreadPool.s_compilationQueue.end(), this), shaderMtlThreadPool.s_compilationQueue.end());
		m_compilationState.setValue(COMPILATION_STATE::COMPILING);
	}
	shaderMtlThreadPool.s_compilationQueueMutex.unlock();
	if (!isStillQueued)
	{
		RuntimePerformance::activeShaderWaiters.fetch_add(1, std::memory_order_relaxed);
		m_compilationState.waitUntilValue(COMPILATION_STATE::DONE);
		RuntimePerformance::activeShaderWaiters.fetch_sub(1, std::memory_order_relaxed);
		if (ShouldCountCompilation())
		    --g_compiled_shaders_async; // compilation caused a stall so we don't consider this one async
		return;
	}
	else
	{
		// compile synchronously
		CompileInternal();
		m_compilationState.setValue(COMPILATION_STATE::DONE);
	}
}

bool RendererShaderMtl::IsCompiled()
{
	return m_compilationState.hasState(COMPILATION_STATE::DONE);
};

bool RendererShaderMtl::WaitForCompiled()
{
	m_compilationState.waitUntilValue(COMPILATION_STATE::DONE);
	return true;
}

bool RendererShaderMtl::ShouldCountCompilation() const
{
    return !s_isLoadingShadersMtl && m_isGameShader;
}

uint64_t RendererShaderMtl::GetLibraryReuseCount()
{
    return s_shaderLibraryCache.GetReuseCount();
}

MTL::Library* RendererShaderMtl::LibraryFromSource()
{
    // Compile from source
    NS_STACK_SCOPED MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    // Metal defaults to fast math enabled. Set both values explicitly so the
    // per-game disabled setting also reaches the actual compiler.
    options->setFastMathEnabled(g_current_game_profile->GetShaderFastMath());

    if (m_mtlr->GetPositionInvariance())
    {
        // TODO: filter out based on GPU state
        options->setPreserveInvariance(true);
    }

    const uint32_t compileFlags = (g_current_game_profile->GetShaderFastMath() ? 1u : 0u) |
        (m_mtlr->GetPositionInvariance() ? 2u : 0u);
    const auto sourceKey = MetalShaderLibrarySourceKey(m_mslCode);
    auto library = s_shaderLibraryCache.GetOrCompile(m_mtlr->GetDevice(), compileFlags, sourceKey, [&] {
        NS::Error* error = nullptr;
#if BOOST_OS_IOS
        const auto compileStart = std::chrono::steady_clock::now();
#endif
        RuntimePerformance::ShaderCompileProbe* probe = nullptr;
        for (auto& candidate : RuntimePerformance::shaderCompileProbes)
        {
            uint64_t freeSlot = 0;
            if (candidate.startedNs.compare_exchange_strong(freeSlot, UINT64_MAX, std::memory_order_acq_rel))
            {
                probe = &candidate;
                break;
            }
        }
        if (probe)
        {
            probe->baseHash.store(m_baseHash, std::memory_order_relaxed);
            probe->auxHash.store(m_auxHash, std::memory_order_relaxed);
            probe->sourceBytes.store(m_mslCode.size(), std::memory_order_relaxed);
            probe->shaderType.store(static_cast<uint32_t>(m_type), std::memory_order_relaxed);
            probe->preserveInvariance.store(m_mtlr->GetPositionInvariance(), std::memory_order_relaxed);
            probe->startedNs.store(RuntimePerformance::shaderProbeNowNs(), std::memory_order_release);
        }
        RuntimePerformance::activeShaderLibraryCompiles.fetch_add(1, std::memory_order_relaxed);
        // Apple's compiler cache keys the submitted source bytes as well. Use
        // the same exact program as our reuse key, including after eviction or
        // a title restart; guest-hash comments must not trigger recompilation.
        // Construct this only on a cache miss, never on a draw or reuse hit.
        MTL::Library* library = m_mtlr->GetDevice()->newLibrary(ToNSString(std::string(sourceKey)), options, &error);
        RuntimePerformance::activeShaderLibraryCompiles.fetch_sub(1, std::memory_order_relaxed);
        if (probe)
            probe->startedNs.store(0, std::memory_order_release);
#if BOOST_OS_IOS
        RuntimePerformance::shaderCompileNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - compileStart).count(), std::memory_order_relaxed);
        RuntimePerformance::shaderCompileCount.fetch_add(1, std::memory_order_relaxed);
#endif
        if (!library)
        {
            cemuLog_log(LogType::Force, "Metal shader {:016x}_{:016x} failed: {}", m_baseHash, m_auxHash,
                error ? error->localizedDescription()->utf8String() : "Compiler returned no library");
#ifndef NDEBUG
            cemuLog_log(LogType::Force, "{}", m_mslCode);
#endif
            return std::shared_ptr<MTL::Library>{};
        }

        return std::shared_ptr<MTL::Library>(library, [](MTL::Library* value) { value->release(); });
    });
    return library ? library->retain() : nullptr;
}


void RendererShaderMtl::CompileInternal()
{
    // std::thread workers have no UIKit run loop to drain autoreleased
    // compiler temporaries. Bound their lifetime to one job, including errors.
    // newLibrary/newFunction return owned objects and survive this pool.
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    const uint32_t compileFlags = (g_current_game_profile->GetShaderFastMath() ? 1u : 0u) |
        (m_mtlr->GetPositionInvariance() ? 2u : 0u);
    auto function = s_shaderFunctionCache.GetOrCompile(m_mtlr->GetDevice(), compileFlags,
        MetalShaderLibrarySourceKey(m_mslCode), [&] {
            MTL::Library* library = LibraryFromSource();
            if (!library)
                return std::shared_ptr<MTL::Function>{};
            RuntimePerformance::activeShaderFunctionCreates.fetch_add(1, std::memory_order_relaxed);
            auto* entryFunction = library->newFunction(ToNSString("main0"));
            RuntimePerformance::activeShaderFunctionCreates.fetch_sub(1, std::memory_order_relaxed);
            library->release();
            if (!entryFunction)
                return std::shared_ptr<MTL::Function>{};
            return std::shared_ptr<MTL::Function>(entryFunction,
                [](MTL::Function* value) { value->release(); });
        });
    FinishCompilation();
    if (!function) { return; }
    m_function = function->retain();

	// Count shader compilation
	if (ShouldCountCompilation())
	    g_compiled_shaders_total++;
}


void RendererShaderMtl::FinishCompilation()
{
    m_mslCode.clear();
    m_mslCode.shrink_to_fit();
}
