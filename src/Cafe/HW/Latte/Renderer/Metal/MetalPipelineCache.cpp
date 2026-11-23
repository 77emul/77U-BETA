#include "Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryPolicy.h"

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "Cafe/HW/Latte/Core/LatteShaderCache.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cemu/FileCache/FileCache.h"
#include "Common/precompiled.h"
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"

#include <openssl/sha.h>
#include <algorithm>
#include <deque>
#include <limits>

#if TARGET_OS_IPHONE || TARGET_OS_IOS
extern "C" bool CemuBridge_IsDiagnosticLoggingEnabled(void);
#endif

static bool g_compilePipelineThreadInit{false};
static std::mutex g_compilePipelineMutex;
static std::condition_variable g_compilePipelineCondVar;
// Protected by g_compilePipelineMutex. Counts queued and executing gameplay
// jobs until their compiler (and its shader references) has been destroyed.
static std::condition_variable g_compilePipelineDrained;
static uint32 g_pendingPipelineCompiles = 0;
struct PendingPipelineCompile
{
	MetalPipelineCompiler* compiler;
	PipelineObject* object;
};
static std::deque<PendingPipelineCompile> g_compilePipelineRequests;

static void compileThreadFunc(sint32 threadIndex)
{
	SetThreadName("compilePl");

#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif

	while (true)
	{
		std::unique_lock lock(g_compilePipelineMutex);
		while (g_compilePipelineRequests.empty())
			g_compilePipelineCondVar.wait(lock);

		MetalPipelineCompiler* request = g_compilePipelineRequests.front().compiler;
		g_compilePipelineRequests.pop_front();

		lock.unlock();

		request->Compile(true, false, true);
		delete request;
		RuntimePerformance::pendingPipelines.fetch_sub(1, std::memory_order_relaxed);
        {
            std::lock_guard finishedLock(g_compilePipelineMutex);
            --g_pendingPipelineCompiles;
            if (g_pendingPipelineCompiles == 0)
                g_compilePipelineDrained.notify_all();
        }
	}
}

static void initCompileThread()
{
	uint32 numCompileThreads;

	uint32 cpuCoreCount = GetPhysicalCoreCount();
	if (cpuCoreCount <= 2)
		numCompileThreads = 1;
	else
		numCompileThreads = 2 + (cpuCoreCount - 3); // 2 plus one additionally for every extra core above 3

	numCompileThreads = std::min(numCompileThreads, 8u); // cap at 8
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	// Apple Silicon iPads expose many efficiency cores. On M4 with 9-10 cores,
	// using 6-8 threads allows idle cores to compile pipelines and shaders twice as fast.
	if (cpuCoreCount >= 8)
		numCompileThreads = std::clamp(cpuCoreCount - 2, 1u, 8u);
	else
		numCompileThreads = std::clamp(cpuCoreCount >= 6 ? 4u : 2u, 1u, 4u);
	numCompileThreads = static_cast<uint32>(GetMetalMemoryPolicy().CompilationWorkers(numCompileThreads));
#endif

	for (uint32 i = 0; i < numCompileThreads; i++)
	{
		std::thread compileThread(compileThreadFunc, i);
		compileThread.detach();
	}
}

static void queuePipeline(MetalPipelineCompiler* v, PipelineObject* object)
{
	std::unique_lock lock(g_compilePipelineMutex);
    ++g_pendingPipelineCompiles;
	g_compilePipelineRequests.push_back({v, object});
	RuntimePerformance::pendingPipelines.fetch_add(1, std::memory_order_relaxed);
	lock.unlock();
	g_compilePipelineCondVar.notify_one();
}

void MetalPipelineCache::WaitForPendingCompilations()
{
    // Loader workers also retain shader pointers. Join them before draining
    // the detached gameplay pool, while the shader pool is still available.
    EndLoading();
    std::unique_lock lock(g_compilePipelineMutex);
    g_compilePipelineDrained.wait(lock, [] { return g_pendingPipelineCompiles == 0; });
}

void MetalPipelineCache::PrioritizePendingPipeline(PipelineObject* pipelineObj)
{
	std::lock_guard lock(g_compilePipelineMutex);
	const auto it = std::find_if(g_compilePipelineRequests.begin(), g_compilePipelineRequests.end(),
		[pipelineObj](const PendingPipelineCompile& request) { return request.object == pipelineObj; });
	if (it == g_compilePipelineRequests.end() || it == g_compilePipelineRequests.begin())
		return;
	const auto request = *it;
	g_compilePipelineRequests.erase(it);
	g_compilePipelineRequests.push_front(request);
}

// Like Vulkan's async mode, this is a display-draw heuristic, not a proof of
// future target use. Keep known persistent/data-producing paths complete and
// allow temporary scene pop-in only while async compilation is selected.
bool MetalPipelineCache::CanCompileDrawAsync(const MetalAttachmentsInfo& attachmentsInfo,
    Vector2i extend, const LatteContextRegister& lcr, bool occlusionQueryActive,
    bool readbackAttachment)
{
    if (!GetConfig().async_compile || occlusionQueryActive || readbackAttachment ||
        !lcr.IsRasterizationEnabled() || lcr.GetRawView()[mmVGT_STRMOUT_EN] != 0)
        return false;

    if (extend.x == 1600 && extend.y == 1600)
        return false; // Known Splatoon ink targets; all color-only variants also stay synchronous.

    if (attachmentsInfo.depthFormat == Latte::E_GX2SURFFMT::INVALID_FORMAT)
    {
        // Widescreen scene color passes (1600x900 post-process, tone mapping, bloom)
        // are redrawn continuously and must compile asynchronously to eliminate 450ms stalls.
        // Small/square UI icons, font caches, and intermediate persistent buffers stay synchronous.
        if (extend.x >= 1280 && extend.x != extend.y)
        {
            for (const auto format : attachmentsInfo.colorFormats)
            {
                if (format != Latte::E_GX2SURFFMT::INVALID_FORMAT)
                    return true;
            }
        }
        return false;
    }

    for (const auto format : attachmentsInfo.colorFormats)
    {
        if (format != Latte::E_GX2SURFFMT::INVALID_FORMAT)
            return true;
    }

    // Depth-only scene prepasses (widescreen geometry passes such as 1600x900, 1280x720, 1920x1080)
    // and non-square shadow map passes (such as 768x720) are redrawn every frame and must compile asynchronously.
    // Square shadow maps (192x192, 1024x1024) stay synchronous so static shadow draws are never dropped.
    if ((extend.x >= 1280 && (extend.x * 9 == extend.y * 16 || extend.x * 10 == extend.y * 16)) ||
        (extend.x == 768 && extend.y == 720))
        return true;

    return false; // Square shadow maps and non-scene depth passes stay synchronous.
}

MetalPipelineCache* g_mtlPipelineCache = nullptr;

MetalPipelineCache& MetalPipelineCache::GetInstance()
{
    return *g_mtlPipelineCache;
}

MetalPipelineCache::MetalPipelineCache(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer}
{
    RuntimePerformance::pipelineSyncRecorder.reset();
    g_mtlPipelineCache = this;
}

MetalPipelineCache::~MetalPipelineCache()
{
    WaitForPendingCompilations();
    Close();
    for (auto& [key, pipelineObj] : m_pipelineCache)
    {
        pipelineObj->WaitForCompilation();
        if (auto* pipeline = pipelineObj->GetPipeline()) pipeline->release();
        delete pipelineObj;
    }
    if (g_mtlPipelineCache == this)
        g_mtlPipelineCache = nullptr;
}

PipelineObject* MetalPipelineCache::GetRenderPipelineState(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, const MetalAttachmentsInfo& lastUsedAttachmentsInfo, const MetalAttachmentsInfo& activeAttachmentsInfo, const LatteContextRegister& lcr, bool allowAsyncCompile)
{
#ifndef NDEBUG
    LatteGPUState.telemetry.metalPipelineRequests.fetch_add(1, std::memory_order_relaxed);

	const auto guestLogicOp = lcr.CB_COLOR_CONTROL.get_ROP();
	auto& telemetry = LatteGPUState.telemetry;
	telemetry.metalLogicOpTotalRequests.fetch_add(1, std::memory_order_relaxed);
	if (guestLogicOp == Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY)
		telemetry.metalLogicOpDisabledRequests.fetch_add(1, std::memory_order_relaxed);
	else
		telemetry.metalLogicOpEnabledRequests.fetch_add(1, std::memory_order_relaxed);

	switch (guestLogicOp)
	{
	case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::CLEAR:
		telemetry.metalLogicOpClearRequests.fetch_add(1, std::memory_order_relaxed);
		break;
	case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY:
		telemetry.metalLogicOpCopyRequests.fetch_add(1, std::memory_order_relaxed);
		break;
	case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::OR:
		telemetry.metalLogicOpOrRequests.fetch_add(1, std::memory_order_relaxed);
		break;
	case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::SET:
		telemetry.metalLogicOpSetRequests.fetch_add(1, std::memory_order_relaxed);
		break;
	default:
		telemetry.metalLogicOpOtherRequests.fetch_add(1, std::memory_order_relaxed);
		break;
	}
#endif

#ifndef NDEBUG
    const bool hasGS = (geometryShader != nullptr);
    if (hasGS)
    {
        LatteGPUState.telemetry.metalPipelineRequestsWithGeometryShader.fetch_add(1, std::memory_order_relaxed);
    }
#endif

    uint64 hash = CalculatePipelineHash(fetchShader, vertexShader, geometryShader, pixelShader, lastUsedAttachmentsInfo, activeAttachmentsInfo, lcr);

    // The caller supplies the same draw-local policy to the resolver. Pending
    // required hits are prioritized there before waiting, including fast hits.
    if (hash == m_lastPipelineHash && m_lastPipelineObj)
    {
        return m_lastPipelineObj;
    }
    auto& fast = m_fastPipelineCache[(hash ^ (hash >> 32)) & (m_fastPipelineCache.size() - 1)];
    if (fast.object && fast.hash == hash)
    {
        m_lastPipelineHash = hash;
        m_lastPipelineObj = fast.object;
        return fast.object;
    }

    // The pipeline loader inserts on another thread. Protect the map lookup as
    // well as insertion, then keep its stable object pointer in the draw-local
    // direct cache. Only a first use pays for the map lock and tree traversal.
    PipelineObject* pipelineObj;
    bool created = false;
    {
        std::lock_guard<FSpinlock> lock(m_pipelineCacheLock);
        auto [it, inserted] = m_pipelineCache.try_emplace(hash, nullptr);
        if (inserted)
        {
            it->second = new PipelineObject();
            created = true;
        }
        pipelineObj = it->second;
    }
    if (!created)
    {
        m_lastPipelineHash = hash;
        m_lastPipelineObj = pipelineObj;
        fast = {hash, pipelineObj};
#ifndef NDEBUG
        if (hasGS)
        {
            LatteGPUState.telemetry.metalPipelineCacheHitsWithGeometryShader.fetch_add(1, std::memory_order_relaxed);
            uint32_t idx = LatteGPUState.telemetry.gsPipelineCaptureCount.fetch_add(1, std::memory_order_relaxed);
            if (idx < 8)
            {
                auto& log = LatteGPUState.telemetry.gsPipelineLog[idx];
                log.vsBaseHash = vertexShader ? vertexShader->baseHash : 0;
                log.psBaseHash = pixelShader ? (pixelShader->baseHash + pixelShader->auxHash) : 0;
                log.gsBaseHash = geometryShader ? geometryShader->baseHash : 0;
                log.colorFormat0 = (uint32_t)lastUsedAttachmentsInfo.colorFormats[0];
                log.cbColorControl = lcr.GetRawView()[Latte::REGADDR::CB_COLOR_CONTROL];
                log.cbTargetMask = lcr.GetRawView()[Latte::REGADDR::CB_TARGET_MASK];
                log.cbBlend0Control = lcr.GetRawView()[Latte::REGADDR::CB_BLEND0_CONTROL];
                log.isHit = true;
            }
        }
#endif
        return pipelineObj;
    }

#ifndef NDEBUG
    if (hasGS)
    {
        LatteGPUState.telemetry.metalPipelineCacheMissesWithGeometryShader.fetch_add(1, std::memory_order_relaxed);
        uint32_t idx = LatteGPUState.telemetry.gsPipelineCaptureCount.fetch_add(1, std::memory_order_relaxed);
        if (idx < 8)
        {
            auto& log = LatteGPUState.telemetry.gsPipelineLog[idx];
            log.vsBaseHash = vertexShader ? vertexShader->baseHash : 0;
            log.psBaseHash = pixelShader ? (pixelShader->baseHash + pixelShader->auxHash) : 0;
            log.gsBaseHash = geometryShader ? geometryShader->baseHash : 0;
            log.colorFormat0 = (uint32_t)lastUsedAttachmentsInfo.colorFormats[0];
            log.cbColorControl = lcr.GetRawView()[Latte::REGADDR::CB_COLOR_CONTROL];
            log.cbTargetMask = lcr.GetRawView()[Latte::REGADDR::CB_TARGET_MASK];
            log.cbBlend0Control = lcr.GetRawView()[Latte::REGADDR::CB_BLEND0_CONTROL];
            log.isHit = false;
        }
    }
#endif

    bool diagnosticsEnabled = false;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    diagnosticsEnabled = !allowAsyncCompile && CemuBridge_IsDiagnosticLoggingEnabled();
#endif
    MetalPipelineCompiler* compiler = new MetalPipelineCompiler(m_mtlr, *pipelineObj, diagnosticsEnabled);
    compiler->InitFromState(fetchShader, vertexShader, geometryShader, pixelShader, lastUsedAttachmentsInfo, activeAttachmentsInfo, lcr);


	if (allowAsyncCompile)
	{
	    if (!g_compilePipelineThreadInit)
		{
			initCompileThread();
			g_compilePipelineThreadInit = true;
		}

		queuePipeline(compiler, pipelineObj);
	}
	else
	{
	    // Also force compile to ensure that the pipeline is ready.
#if TARGET_OS_IPHONE || TARGET_OS_IOS
        RuntimePerformance::OptionalOperationTimer syncTimer(
            diagnosticsEnabled, RuntimePerformance::pipelineSyncNs);
#endif
        const bool compiled = compiler->Compile(true, true, true);
        if (diagnosticsEnabled)
        {
            using namespace RuntimePerformance;
            PipelineSyncSample sample{};
            const auto context = m_mtlr->GetPipelineDiagnosticContext();
            sample.key = hash;
            sample.vsBase = vertexShader ? vertexShader->baseHash : 0; sample.vsAux = vertexShader ? vertexShader->auxHash : 0;
            sample.gsBase = geometryShader ? geometryShader->baseHash : 0; sample.gsAux = geometryShader ? geometryShader->auxHash : 0;
            sample.psBase = pixelShader ? pixelShader->baseHash : 0; sample.psAux = pixelShader ? pixelShader->auxHash : 0;
            sample.width = context.size.x; sample.height = context.size.y;
            for (size_t i = 0; i < sample.colorFormats.size(); ++i)
            {
                sample.colorFormats[i] = static_cast<uint32_t>(lastUsedAttachmentsInfo.colorFormats[i]);
                if (activeAttachmentsInfo.colorFormats[i] != Latte::E_GX2SURFFMT::INVALID_FORMAT) sample.activeColorMask |= 1u << i;
            }
            sample.depthFormat = static_cast<uint32_t>(lastUsedAttachmentsInfo.depthFormat);
            sample.activeDepthFormat = static_cast<uint32_t>(activeAttachmentsInfo.depthFormat);
            sample.reasons = (!GetConfig().async_compile ? AsyncConfigOff : 0u) |
                (context.queryActive ? Query : 0u) | (context.readbackAttachment ? Readback : 0u) |
                (!lcr.IsRasterizationEnabled() ? RasterKill : 0u) | (lcr.GetRawView()[mmVGT_STRMOUT_EN] ? Streamout : 0u) |
                (sample.width == 1600 && sample.height == 1600 ? Ink1600 : 0u) |
                (activeAttachmentsInfo.depthFormat == Latte::E_GX2SURFFMT::INVALID_FORMAT ? ColorOnly : 0u) |
                (!sample.activeColorMask ? DepthOnly : 0u);
            if (!sample.reasons) sample.reasons = OtherRequired;
            sample.timings = compiler->GetCompileTimings();
            sample.mesh = compiler->IsMeshPipeline(); sample.success = compiled;
            pipelineSyncRecorder.record(true, sample);
        }
        delete compiler;
		if (!compiled)
		{
			delete pipelineObj;
			std::lock_guard<FSpinlock> lock(m_pipelineCacheLock);
			m_pipelineCache.erase(hash);
			return nullptr;
		}
	}

	// Save to cache
    AddCurrentStateToCache(hash, lastUsedAttachmentsInfo);
    m_lastPipelineHash = hash;
    m_lastPipelineObj = pipelineObj;
    fast = {hash, pipelineObj};

    return pipelineObj;
}

uint64 MetalPipelineCache::CalculatePipelineHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, const MetalAttachmentsInfo& lastUsedAttachmentsInfo, const MetalAttachmentsInfo& activeAttachmentsInfo, const LatteContextRegister& lcr)
{
	// Fragment state is absent from rasterization-disabled Metal descriptors.
	// Do not compile another streamout PSO when only the bound pixel shader,
	// attachment, write mask or blending state changes.
	const bool rasterizationEnabled = lcr.IsRasterizationEnabled();
	uint64 stateHash = 0;
	if (rasterizationEnabled)
	{
		for (int i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; ++i)
		{
			Latte::E_GX2SURFFMT format = lastUsedAttachmentsInfo.colorFormats[i];
			if (format == Latte::E_GX2SURFFMT::INVALID_FORMAT)
				continue;

			stateHash += GetMtlPixelFormat(format, false) + i * 31;
			stateHash = std::rotl<uint64>(stateHash, 7);

			if (activeAttachmentsInfo.colorFormats[i] == Latte::E_GX2SURFFMT::INVALID_FORMAT)
			{
				stateHash += 1;
				stateHash = std::rotl<uint64>(stateHash, 1);
			}
		}

		if (lastUsedAttachmentsInfo.depthFormat != Latte::E_GX2SURFFMT::INVALID_FORMAT)
		{
			stateHash += GetMtlPixelFormat(lastUsedAttachmentsInfo.depthFormat, true);
			stateHash = std::rotl<uint64>(stateHash, 7);

			if (activeAttachmentsInfo.depthFormat == Latte::E_GX2SURFFMT::INVALID_FORMAT)
			{
				stateHash += 1;
				stateHash = std::rotl<uint64>(stateHash, 1);
			}
		}
	}

	for (auto& group : fetchShader->bufferGroups)
	{
		uint32 bufferStride = group.getCurrentBufferStride(lcr.GetRawView());
		stateHash = std::rotl<uint64>(stateHash, 7);
		stateHash += bufferStride * 3;
	}

	stateHash += fetchShader->getVkPipelineHashFragment();
	stateHash = std::rotl<uint64>(stateHash, 7);

	stateHash += lcr.GetRawView()[mmVGT_STRMOUT_EN];
	stateHash = std::rotl<uint64>(stateHash, 7);

	// Match the compiler's effective rasterization flag, including the guest
	// viewport-offset rule and culling both faces. Individual cull modes,
	// winding, polygon offset and depth clipping are encoder state, not PSO state.
	if (!rasterizationEnabled)
		stateHash += 0x333333;

	stateHash = (stateHash >> 8) + (stateHash * 0x370531ull) % 0x7F980D3BF9B4639Dull;

	uint32* ctxRegister = lcr.GetRawView();

	// Auxiliary hashes identify different executable functions (including
	// streamout strides), even when the guest shader program is unchanged.
	if (vertexShader)
		stateHash += vertexShader->baseHash + vertexShader->auxHash;

	stateHash = std::rotl<uint64>(stateHash, 13);

	if (geometryShader)
		stateHash += geometryShader->baseHash + geometryShader->auxHash;

	stateHash = std::rotl<uint64>(stateHash, 13);

	if (rasterizationEnabled && pixelShader)
		stateHash += pixelShader->baseHash + pixelShader->auxHash;

	stateHash = std::rotl<uint64>(stateHash, 13);

	if (rasterizationEnabled)
	{
		const auto colorControlReg = ctxRegister[Latte::REGADDR::CB_COLOR_CONTROL];
		stateHash += colorControlReg;

		stateHash += ctxRegister[Latte::REGADDR::CB_TARGET_MASK];

		const uint32 blendEnableMask = (colorControlReg >> 8) & 0xFF;
		if (blendEnableMask)
		{
			for (auto i = 0; i < 8; ++i)
			{
				if (((blendEnableMask & (1 << i))) == 0)
					continue;
				stateHash = std::rotl<uint64>(stateHash, 7);
				stateHash += ctxRegister[Latte::REGADDR::CB_BLEND0_CONTROL + i];
			}
		}
	}

	// Mesh pipeline
	// Cache-loader workers restore their own register snapshot. The live GPU
	// registers can describe an unrelated draw (or be zero during startup).
	const LattePrimitiveMode primitiveMode = lcr.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
    bool isPrimitiveRect = (primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS);

    bool usesGeometryShader = (geometryShader != nullptr || isPrimitiveRect);

    if (usesGeometryShader)
    {
        stateHash += lcr.GetRawView()[mmVGT_PRIMITIVE_TYPE];
        stateHash = std::rotl<uint64>(stateHash, 7);
    }

	return stateHash;
}

struct
{
	uint32 pipelineLoadIndex;
	uint32 pipelineMaxFileIndex;

	std::atomic_uint32_t pipelinesQueued;
	std::atomic_uint32_t pipelinesLoaded;
} g_mtlCacheState;

uint32 MetalPipelineCache::BeginLoading(uint64 cacheTitleId)
{
	std::error_code ec;
	fs::create_directories(ActiveSettings::GetCachePath("shaderCache/transferable"), ec);
	const auto pathCacheFile = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_mtlpipeline.bin", cacheTitleId);

	// init cache loader state
	g_mtlCacheState.pipelineLoadIndex = 0;
	g_mtlCacheState.pipelineMaxFileIndex = 0;
	g_mtlCacheState.pipelinesLoaded = 0;
	g_mtlCacheState.pipelinesQueued = 0;

	// start async compilation threads
	m_compilationCount.store(0);
	m_compilationQueue.clear();

	// get core count
	uint32 cpuCoreCount = GetPhysicalCoreCount();
	uint32 numCompilationThreads = std::clamp(cpuCoreCount, 1u, 8u);
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	const auto& memoryPolicy = GetMetalMemoryPolicy();
	// These threads exist only while the loading screen runs (joined in EndLoading).
	numCompilationThreads = static_cast<uint32>(memoryPolicy.LoadingWorkers(numCompilationThreads));
	m_loadingQueueLimit = memoryPolicy.constrained ? 32 : 128;
#endif
	m_compilationThreads.reserve(numCompilationThreads);
	for (uint32 i = 0; i < numCompilationThreads; i++)
		m_compilationThreads.emplace_back(&MetalPipelineCache::CompilerThread, this);

	// open cache file or create it
	cemu_assert_debug(s_cache == nullptr);
	// +1 drops pipelines compiled when BOTH_SRC_ALPHA was mapped to zero.
	s_cache = FileCache::Open(pathCacheFile, true, LatteShaderCache_getPipelineCacheExtraVersion(cacheTitleId) + 1);
	if (!s_cache)
	{
		cemuLog_log(LogType::Force, "Failed to open or create Metal pipeline cache file: {}", _pathToUtf8(pathCacheFile));
		return 0;
	}
	else
	{
		s_cache->UseCompression(false);
		g_mtlCacheState.pipelineMaxFileIndex = s_cache->GetMaximumFileIndex();
	}
	return s_cache->GetFileCount();
}

bool MetalPipelineCache::UpdateLoading(uint32& pipelinesLoadedTotal, uint32& pipelinesMissingShaders)
{
	pipelinesLoadedTotal = g_mtlCacheState.pipelinesLoaded;
	pipelinesMissingShaders = 0;
	if (!s_cache)
		return false;
	while (g_mtlCacheState.pipelineLoadIndex <= g_mtlCacheState.pipelineMaxFileIndex)
	{
		if (m_compilationQueue.size() >= m_loadingQueueLimit)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			return true; // queue up to 128 entries at a time
		}

		uint64 fileNameA, fileNameB;
		std::vector<uint8> fileData;
		if (s_cache->GetFileByIndex(g_mtlCacheState.pipelineLoadIndex, &fileNameA, &fileNameB, fileData))
		{
			// queue for async compilation
			g_mtlCacheState.pipelinesQueued++;
			m_compilationQueue.push(std::move(fileData));
			g_mtlCacheState.pipelineLoadIndex++;
			continue;
		}
		g_mtlCacheState.pipelineLoadIndex++;
	}
	if (g_mtlCacheState.pipelinesLoaded != g_mtlCacheState.pipelinesQueued)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return true; // pipelines still compiling
	}
	return false; // done
}

void MetalPipelineCache::EndLoading()
{
	for (size_t i = 0; i < m_compilationThreads.size(); ++i)
		m_compilationQueue.push({});
	for (auto& thread : m_compilationThreads)
		thread.join();
	m_compilationThreads.clear();
	// Keep the cache file open for new pipelines during gameplay.
}

void MetalPipelineCache::Close()
{
    RuntimePerformance::pipelineSyncRecorder.reset();
    EndLoading();
    if (m_pipelineCacheStoreThread.joinable())
    {
        m_pipelineCachingQueue.push(nullptr);
        m_pipelineCacheStoreThread.join();
    }
    if (s_cache)
    {
        delete s_cache;
        s_cache = nullptr;
    }
    m_lastPipelineHash = 0;
    m_lastPipelineObj = nullptr;
    m_fastPipelineCache = {};
}

struct CachedPipeline
{
	struct ShaderHash
	{
		uint64 baseHash;
		uint64 auxHash;
		bool isPresent{};

		void set(uint64 baseHash, uint64 auxHash)
		{
			this->baseHash = baseHash;
			this->auxHash = auxHash;
			this->isPresent = true;
		}
	};

	ShaderHash vsHash; // includes fetch shader
	ShaderHash gsHash;
	ShaderHash psHash;

	MetalAttachmentsInfo lastUsedAttachmentsInfo;

	Latte::GPUCompactedRegisterState gpuState;
};

void MetalPipelineCache::LoadPipelineFromCache(std::span<uint8> fileData)
{
	if (fileData.size() > static_cast<size_t>(std::numeric_limits<sint32>::max()))
		return;
	// deserialize file
	auto lcr = std::make_unique<LatteContextRegister>();
	auto cachedPipeline = std::make_unique<CachedPipeline>();

	MemStreamReader streamReader(fileData.data(), static_cast<sint32>(fileData.size()));
	if (!DeserializePipeline(streamReader, *cachedPipeline))
	{
		// failed to deserialize
		return;
	}
	// restored register view from compacted state
	Latte::LoadGPURegisterState(*lcr, cachedPipeline->gpuState);

	LatteDecompilerShader* vertexShader = nullptr;
	LatteDecompilerShader* geometryShader = nullptr;
	LatteDecompilerShader* pixelShader = nullptr;
	// find vertex shader
	if (cachedPipeline->vsHash.isPresent)
	{
		vertexShader = LatteSHRC_FindVertexShader(cachedPipeline->vsHash.baseHash, cachedPipeline->vsHash.auxHash);
		if (!vertexShader)
		{
			cemuLog_log(LogType::Force, "Vertex shader not found in cache");
			return;
		}
	}
	// find geometry shader
	if (cachedPipeline->gsHash.isPresent)
	{
		geometryShader = LatteSHRC_FindGeometryShader(cachedPipeline->gsHash.baseHash, cachedPipeline->gsHash.auxHash);
		if (!geometryShader)
		{
			cemuLog_log(LogType::Force, "Geometry shader not found in cache");
			return;
		}
	}
	// find pixel shader
	if (cachedPipeline->psHash.isPresent)
	{
		pixelShader = LatteSHRC_FindPixelShader(cachedPipeline->psHash.baseHash, cachedPipeline->psHash.auxHash);
		if (!pixelShader)
		{
			cemuLog_log(LogType::Force, "Pixel shader not found in cache");
			return;
		}
	}

	if (!pixelShader)
	{
		cemu_assert_debug(false);
		return;
	}

	MetalAttachmentsInfo attachmentsInfo(*lcr, pixelShader);

	// Older cache files can contain several register snapshots mapping to one
	// effective descriptor. Reserve the key before calling Metal so parallel
	// loader workers do not compile and discard the same PSO repeatedly.
	// Gameplay starts only after EndLoading joins all loader workers.
	const uint64 pipelineStateHash = CalculatePipelineHash(vertexShader->compatibleFetchShader, vertexShader, geometryShader, pixelShader, cachedPipeline->lastUsedAttachmentsInfo, attachmentsInfo, *lcr);
	auto pipelineObject = std::make_unique<PipelineObject>();
	{
		std::lock_guard<FSpinlock> lock(m_pipelineCacheLock);
		if (!m_pipelineCache.emplace(pipelineStateHash, pipelineObject.get()).second)
			return;
	}

	// compile
	bool compiled = false;
	{
		MetalPipelineCompiler pp(m_mtlr, *pipelineObject);
		pp.InitFromState(vertexShader->compatibleFetchShader, vertexShader, geometryShader, pixelShader, cachedPipeline->lastUsedAttachmentsInfo, attachmentsInfo, *lcr);
		compiled = pp.Compile(true, true, false);
		// destroy pp early
	}
	if (!compiled || !pipelineObject->GetPipeline())
	{
		std::lock_guard<FSpinlock> lock(m_pipelineCacheLock);
		m_pipelineCache.erase(pipelineStateHash);
		return; // Do not retain an unusable entry; a later record may retry it.
	}
	pipelineObject.release(); // The map owns the successful, published object.
}

void MetalPipelineCache::AddCurrentStateToCache(uint64 pipelineStateHash, const MetalAttachmentsInfo& lastUsedAttachmentsInfo)
{
	if (!m_pipelineCacheStoreThread.joinable())
		m_pipelineCacheStoreThread = std::thread(&MetalPipelineCache::WorkerThread, this);
	// fill job structure with cached GPU state
	// for each cached pipeline we store:
	// - Active shaders (referenced by hash)
	// - An almost-complete register state of the GPU (minus some ALU uniform constants which aren't relevant)
	CachedPipeline* job = new CachedPipeline();
	auto vs = LatteSHRC_GetActiveVertexShader();
	auto gs = LatteSHRC_GetActiveGeometryShader();
	auto ps = LatteSHRC_GetActivePixelShader();
	if (vs)
		job->vsHash.set(vs->baseHash, vs->auxHash);
	if (gs)
		job->gsHash.set(gs->baseHash, gs->auxHash);
	if (ps)
		job->psHash.set(ps->baseHash, ps->auxHash);
	job->lastUsedAttachmentsInfo = lastUsedAttachmentsInfo;
	Latte::StoreGPURegisterState(LatteGPUState.contextNew, job->gpuState);
	// queue job
	m_pipelineCachingQueue.push(job);
}

bool MetalPipelineCache::SerializePipeline(MemStreamWriter& memWriter, CachedPipeline& cachedPipeline)
{
	memWriter.writeBE<uint8>(0x01); // version
	uint8 presentMask = 0;
	if (cachedPipeline.vsHash.isPresent)
		presentMask |= 1;
	if (cachedPipeline.gsHash.isPresent)
		presentMask |= 2;
	if (cachedPipeline.psHash.isPresent)
		presentMask |= 4;
	memWriter.writeBE<uint8>(presentMask);
	if (cachedPipeline.vsHash.isPresent)
	{
		memWriter.writeBE<uint64>(cachedPipeline.vsHash.baseHash);
		memWriter.writeBE<uint64>(cachedPipeline.vsHash.auxHash);
	}
	if (cachedPipeline.gsHash.isPresent)
	{
		memWriter.writeBE<uint64>(cachedPipeline.gsHash.baseHash);
		memWriter.writeBE<uint64>(cachedPipeline.gsHash.auxHash);
	}
	if (cachedPipeline.psHash.isPresent)
	{
		memWriter.writeBE<uint64>(cachedPipeline.psHash.baseHash);
		memWriter.writeBE<uint64>(cachedPipeline.psHash.auxHash);
	}

	for (uint8 i = 0; i < LATTE_NUM_COLOR_TARGET; i++)
	    memWriter.writeBE<uint16>((uint16)cachedPipeline.lastUsedAttachmentsInfo.colorFormats[i]);
	memWriter.writeBE<uint16>((uint16)cachedPipeline.lastUsedAttachmentsInfo.depthFormat);

	Latte::SerializeRegisterState(cachedPipeline.gpuState, memWriter);

	return true;
}

bool MetalPipelineCache::DeserializePipeline(MemStreamReader& memReader, CachedPipeline& cachedPipeline)
{
	// version
	if (memReader.readBE<uint8>() != 1)
	{
		cemuLog_log(LogType::Force, "Cached Metal pipeline corrupted or has unknown version");
		return false;
	}
	// shader hashes
	uint8 presentMask = memReader.readBE<uint8>();
	if (presentMask & 1)
	{
		uint64 baseHash = memReader.readBE<uint64>();
		uint64 auxHash = memReader.readBE<uint64>();
		cachedPipeline.vsHash.set(baseHash, auxHash);
	}
	if (presentMask & 2)
	{
		uint64 baseHash = memReader.readBE<uint64>();
		uint64 auxHash = memReader.readBE<uint64>();
		cachedPipeline.gsHash.set(baseHash, auxHash);
	}
	if (presentMask & 4)
	{
		uint64 baseHash = memReader.readBE<uint64>();
		uint64 auxHash = memReader.readBE<uint64>();
		cachedPipeline.psHash.set(baseHash, auxHash);
	}

	for (uint8 i = 0; i < LATTE_NUM_COLOR_TARGET; i++)
	    cachedPipeline.lastUsedAttachmentsInfo.colorFormats[i] = (Latte::E_GX2SURFFMT)memReader.readBE<uint16>();
	cachedPipeline.lastUsedAttachmentsInfo.depthFormat = (Latte::E_GX2SURFFMT)memReader.readBE<uint16>();

	// deserialize GPU state
	if (!Latte::DeserializeRegisterState(cachedPipeline.gpuState, memReader))
	{
		return false;
	}
	cemu_assert_debug(!memReader.hasError());

	return true;
}

int MetalPipelineCache::CompilerThread()
{
	SetThreadName("plCacheCompiler");
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
	while (true)
	{
		std::vector<uint8> pipelineData = m_compilationQueue.pop();
		if (pipelineData.empty())
			break;
		LoadPipelineFromCache(pipelineData);
		++g_mtlCacheState.pipelinesLoaded;
	}
	return 0;
}

void MetalPipelineCache::WorkerThread()
{
	SetThreadName("plCacheWriter");
	while (true)
	{
		CachedPipeline* job;
		m_pipelineCachingQueue.pop(job);
		if (!job)
			break;
		if (!s_cache)
		{
			delete job;
			continue;
		}
		// serialize
		MemStreamWriter memWriter(1024 * 4);
		SerializePipeline(memWriter, *job);
		auto blob = memWriter.getResult();
		// file name is derived from data hash
		uint8 hash[SHA256_DIGEST_LENGTH];
		SHA256(blob.data(), blob.size(), hash);
		uint64 nameA = *(uint64be*)(hash + 0);
		uint64 nameB = *(uint64be*)(hash + 8);
		if (blob.size() <= static_cast<size_t>(std::numeric_limits<sint32>::max()))
			s_cache->AddFileAsync({ nameA, nameB }, blob.data(), static_cast<sint32>(blob.size()));
		delete job;
	}
}
