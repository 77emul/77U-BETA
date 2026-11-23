#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalGeometryFallback.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalTextureShape.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include "Common/HostUploadMemory.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/CachedFBOMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalOutputShaderCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalDepthStencilCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalSamplerCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureReadbackMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalVoidVertexPipeline.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalQuery.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/UtilityShaderSource.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalBCTranscodeShaderSource.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalFrameGeneration.h"
#include "gui/interface/WindowSystem.h"
#include <cstring>

#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "CafeSystem.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/MemoryBudget.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>

#if TARGET_OS_IPHONE || TARGET_OS_IOS
#include <os/proc.h>
extern "C" void CemuBridge_LogJITMilestone(const char* message);
extern "C" void CemuBridge_UpdatePerfStats(double fps, double cpuWaitMs, double gpuMs, uint32_t drawCalls, uint32_t renderPasses);
extern "C" bool CemuBridge_IsDiagnosticLoggingEnabled(void);
extern "C" const char* CemuBridge_GetThermalStateName(void);
extern "C" int CemuBridge_GetRunningGameFrameGenerationTarget(void);
extern "C" bool CemuBridge_GetRunningGameFrameGenerationFast(void);
#include <mach/mach_time.h>
#include <pthread/qos.h>
#endif

static int GetFrameGenerationTarget()
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    return CemuBridge_GetRunningGameFrameGenerationTarget();
#else
    const int val = GetConfig().frame_generation_target.GetValue();
    if (val == 30 || val == 60 || val == 120)
        return val;
    return 0;
#endif
}

#ifndef IMGUI_IMPL_METAL_CPP
#define IMGUI_IMPL_METAL_CPP
#endif
#include "imgui/imgui_extension.h"
#include "imgui/imgui_impl_metal.h"

extern bool hasValidFramebufferAttached;

float supportBufferData[512 * 4];

namespace
{
    std::atomic<bool> s_frameCaptureRequested{false};
	constexpr uint32_t kMetalResourceCrashEventCapacity = 200;
	struct AtomicMetalResourceCrashEvent
	{
		std::atomic<uint64_t> sequence{0};
		std::atomic<uint64_t> hostTimeNs{0};
		std::atomic<uint32_t> type{0};
		std::atomic<uint64_t> texturePtr{0};
		std::atomic<uint64_t> ownerPtr{0};
		std::atomic<uint64_t> ownerObjectId{0};
		std::atomic<uint64_t> commandBufferPtr{0};
		std::atomic<uint64_t> relatedPtr{0};
		std::atomic<uint32_t> guestAddress{0};
		std::atomic<uint32_t> width{0};
		std::atomic<uint32_t> height{0};
		std::atomic<uint32_t> pitch{0};
		std::atomic<uint32_t> pixelFormat{0};
		std::atomic<uint32_t> tileMode{0};
		std::atomic<uint32_t> commandBufferStatus{0};
		std::atomic<uint32_t> flags{0};
	};
	AtomicMetalResourceCrashEvent s_metalResourceCrashEvents[kMetalResourceCrashEventCapacity];
	std::atomic<uint64_t> s_metalResourceCrashSequence{0};

	std::atomic<uint64_t> s_tvBackbufferCopyCount{0};
	std::atomic<uint64_t> s_drcBackbufferCopyCount{0};
	std::atomic<uint32_t> s_tvBackbufferLogCount{0};
	std::atomic<uint32_t> s_drcBackbufferLogCount{0};
	std::atomic<bool> s_drcRoutingValid{false};
	std::atomic<bool> s_drcRoutingPadView{false};
	std::atomic<bool> s_drcRoutingSelectedPadLayer{false};
	std::atomic<uint64_t> s_drcRoutingSourceTexturePtr{0};
	std::atomic<uint32_t> s_drcRoutingSourceWidth{0};
	std::atomic<uint32_t> s_drcRoutingSourceHeight{0};
	std::atomic<uint64_t> s_drcRoutingDrawableTexturePtr{0};
	std::atomic<uint64_t> s_drcRoutingCopyCounter{0};
	std::atomic<uint64_t> s_drcPreviewPublishCount{0};
	std::atomic<uint64_t> s_drcPreviewSourceChangeCount{0};
	std::atomic<uint64_t> s_drcPreviewSourceTexturePtr{0};
	std::atomic<uint64_t> s_drcPreviewPublishedTexturePtr{0};
	std::atomic<MetalDRCPreviewPublishCallback> s_drcPreviewPublishCallback{nullptr};
	constexpr uint32_t kMaxBackbufferRoutingLogsPerDisplay = 16;
}

void MetalRenderer_RequestFrameCapture()
{
    // Called by the UI without accessing a renderer that may be shutting down.
    s_frameCaptureRequested.store(true, std::memory_order_relaxed);
}

static bool ConsumeMetalFrameCaptureRequest()
{
    // No Metal, clock, allocation or file work while no capture is requested.
    return s_frameCaptureRequested.load(std::memory_order_relaxed) &&
           s_frameCaptureRequested.exchange(false, std::memory_order_relaxed);
}

void MetalResourceCrashDiagnostics_Record(MetalResourceCrashEvent event)
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    if (!CemuBridge_IsDiagnosticLoggingEnabled())
        return;
#endif
	const uint64_t sequence = s_metalResourceCrashSequence.fetch_add(1, std::memory_order_relaxed) + 1;
	event.sequence = sequence;
	event.hostTimeNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
	auto& slot = s_metalResourceCrashEvents[(sequence - 1) % kMetalResourceCrashEventCapacity];
	slot.sequence.store(0, std::memory_order_release);
	slot.hostTimeNs.store(event.hostTimeNs, std::memory_order_relaxed);
	slot.type.store(static_cast<uint32_t>(event.type), std::memory_order_relaxed);
	slot.texturePtr.store(event.texturePtr, std::memory_order_relaxed);
	slot.ownerPtr.store(event.ownerPtr, std::memory_order_relaxed);
	slot.ownerObjectId.store(event.ownerObjectId, std::memory_order_relaxed);
	slot.commandBufferPtr.store(event.commandBufferPtr, std::memory_order_relaxed);
	slot.relatedPtr.store(event.relatedPtr, std::memory_order_relaxed);
	slot.guestAddress.store(event.guestAddress, std::memory_order_relaxed);
	slot.width.store(event.width, std::memory_order_relaxed);
	slot.height.store(event.height, std::memory_order_relaxed);
	slot.pitch.store(event.pitch, std::memory_order_relaxed);
	slot.pixelFormat.store(event.pixelFormat, std::memory_order_relaxed);
	slot.tileMode.store(event.tileMode, std::memory_order_relaxed);
	slot.commandBufferStatus.store(event.commandBufferStatus, std::memory_order_relaxed);
	slot.flags.store(event.flags, std::memory_order_relaxed);
	slot.sequence.store(sequence, std::memory_order_release);
}

bool MetalResourceCrashDiagnostics_ShouldRecordTextureEvent(MetalResourceCrashEventType type)
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // Texture churn can generate thousands of lifecycle events per second.
    // Keep a sparse crash-history sample without formatting or writing each
    // event to the shared atomic ring on the gameplay path.
    static thread_local uint32_t counters[4]{};
    uint32_t index;
    switch (type)
    {
    case MetalResourceCrashEventType::TextureCreate: index = 0; break;
    case MetalResourceCrashEventType::TextureDestroy: index = 1; break;
    case MetalResourceCrashEventType::TextureCacheEvict: index = 2; break;
    case MetalResourceCrashEventType::TextureReplace: index = 3; break;
    default: return CemuBridge_IsDiagnosticLoggingEnabled();
    }
    if ((++counters[index] & 63u) != 0)
        return false;
    return CemuBridge_IsDiagnosticLoggingEnabled();
#else
    (void)type;
    return true;
#endif
}

uint32 MetalResourceCrashDiagnostics_GetEventCount()
{
	return static_cast<uint32>(std::min<uint64_t>(
		s_metalResourceCrashSequence.load(std::memory_order_acquire), kMetalResourceCrashEventCapacity));
}

bool MetalResourceCrashDiagnostics_GetEvent(uint32 index, MetalResourceCrashEvent& event)
{
	const uint64_t newest = s_metalResourceCrashSequence.load(std::memory_order_acquire);
	const uint64_t count = std::min<uint64_t>(newest, kMetalResourceCrashEventCapacity);
	if (index >= count)
		return false;
	const uint64_t target = newest - count + index + 1;
	auto& slot = s_metalResourceCrashEvents[(target - 1) % kMetalResourceCrashEventCapacity];
	if (slot.sequence.load(std::memory_order_acquire) != target)
		return false;
	event.sequence = target;
	event.hostTimeNs = slot.hostTimeNs.load(std::memory_order_relaxed);
	event.type = static_cast<MetalResourceCrashEventType>(slot.type.load(std::memory_order_relaxed));
	event.texturePtr = slot.texturePtr.load(std::memory_order_relaxed);
	event.ownerPtr = slot.ownerPtr.load(std::memory_order_relaxed);
	event.ownerObjectId = slot.ownerObjectId.load(std::memory_order_relaxed);
	event.commandBufferPtr = slot.commandBufferPtr.load(std::memory_order_relaxed);
	event.relatedPtr = slot.relatedPtr.load(std::memory_order_relaxed);
	event.guestAddress = slot.guestAddress.load(std::memory_order_relaxed);
	event.width = slot.width.load(std::memory_order_relaxed);
	event.height = slot.height.load(std::memory_order_relaxed);
	event.pitch = slot.pitch.load(std::memory_order_relaxed);
	event.pixelFormat = slot.pixelFormat.load(std::memory_order_relaxed);
	event.tileMode = slot.tileMode.load(std::memory_order_relaxed);
	event.commandBufferStatus = slot.commandBufferStatus.load(std::memory_order_relaxed);
	event.flags = slot.flags.load(std::memory_order_relaxed);
	return slot.sequence.load(std::memory_order_acquire) == target;
}

static const char* MetalResourceCrashEventTypeName(MetalResourceCrashEventType type)
{
	switch (type)
	{
	case MetalResourceCrashEventType::TextureCreate: return "TEX_CREATE";
	case MetalResourceCrashEventType::TextureDestroy: return "TEX_DESTROY";
	case MetalResourceCrashEventType::TextureCacheEvict: return "CACHE_EVICT";
	case MetalResourceCrashEventType::TextureReplace: return "RT_REPLACE";
	case MetalResourceCrashEventType::PipelineCreateBegin: return "PIPE_BEGIN";
	case MetalResourceCrashEventType::PipelineCreateSuccess: return "PIPE_OK";
	case MetalResourceCrashEventType::PipelineCreateError: return "PIPE_ERROR";
	case MetalResourceCrashEventType::CommandBufferCommit: return "CB_COMMIT";
	case MetalResourceCrashEventType::CommandBufferComplete: return "CB_COMPLETE";
	case MetalResourceCrashEventType::CommandBufferError: return "CB_ERROR";
	}
	return "UNKNOWN";
}

void MetalResourceCrashDiagnostics_DumpToCrashLog()
{
	CrashLog_WriteLine("");
	CrashLog_WriteHeader("Metal resource crash history (oldest to newest)");
	char line[512];
	const uint32 count = MetalResourceCrashDiagnostics_GetEventCount();
	for (uint32 i = 0; i < count; ++i)
	{
		MetalResourceCrashEvent event{};
		if (!MetalResourceCrashDiagnostics_GetEvent(i, event))
			continue;
		snprintf(line, sizeof(line),
			"#%llu %s tex=0x%llx owner=0x%llx obj=%llu guest=%08x %ux%u pitch=%u fmt=%u tile=%u cb=0x%llx status=%u related=0x%llx flags=%08x",
			(unsigned long long)event.sequence, MetalResourceCrashEventTypeName(event.type),
			(unsigned long long)event.texturePtr, (unsigned long long)event.ownerPtr,
			(unsigned long long)event.ownerObjectId, event.guestAddress, event.width, event.height,
			event.pitch, event.pixelFormat, event.tileMode, (unsigned long long)event.commandBufferPtr,
			event.commandBufferStatus, (unsigned long long)event.relatedPtr, event.flags);
		CrashLog_WriteLine(line);
	}
}

static void MetalTextureLifecycleDiagnosticCallback(LatteTextureLifecycleDiagnosticEvent type, const LatteTexture* texture, const LatteTexture* relatedTexture)
{
	if (!texture)
		return;
	const auto eventType = type == LatteTextureLifecycleDiagnosticEvent::Replace
		? MetalResourceCrashEventType::TextureReplace
		: MetalResourceCrashEventType::TextureCacheEvict;
	if (!MetalResourceCrashDiagnostics_ShouldRecordTextureEvent(eventType))
		return;
	const auto* metalTexture = static_cast<const LatteTextureMtl*>(texture);
	MetalResourceCrashEvent event{};
	event.type = eventType;
	event.texturePtr = reinterpret_cast<uint64>(metalTexture->GetTexture());
	event.ownerPtr = reinterpret_cast<uint64>(texture);
	event.ownerObjectId = texture->hostTextureObjectId;
	event.relatedPtr = reinterpret_cast<uint64>(relatedTexture);
	event.guestAddress = texture->physAddress;
	event.width = texture->width;
	event.height = texture->height;
	event.pitch = texture->pitch;
	event.pixelFormat = static_cast<uint32>(metalTexture->GetTexture()->pixelFormat());
	event.tileMode = static_cast<uint32>(texture->tileMode);
	event.flags = (texture->isDepth ? 1u : 0u) | (FormatIsRenderable(texture->format) ? 2u : 0u);
	if (g_renderer)
	{
		auto renderer = MetalRenderer::GetInstance();
		if (renderer && renderer->IsCommandBufferActive())
		{
			auto commandBuffer = renderer->GetCurrentCommandBuffer();
			event.commandBufferPtr = reinterpret_cast<uint64>(commandBuffer);
			event.commandBufferStatus = static_cast<uint32>(commandBuffer->status());
		}
	}
	MetalResourceCrashDiagnostics_Record(event);
}

MetalDRCRoutingDiagnosticSnapshot MetalRenderer_GetDRCRoutingDiagnosticSnapshot()
{
	MetalDRCRoutingDiagnosticSnapshot snapshot{};
	snapshot.valid = s_drcRoutingValid.load(std::memory_order_acquire);
	snapshot.padView = s_drcRoutingPadView.load(std::memory_order_relaxed);
	snapshot.selectedPadLayer = s_drcRoutingSelectedPadLayer.load(std::memory_order_relaxed);
	snapshot.sourceTexturePtr = s_drcRoutingSourceTexturePtr.load(std::memory_order_relaxed);
	snapshot.sourceWidth = s_drcRoutingSourceWidth.load(std::memory_order_relaxed);
	snapshot.sourceHeight = s_drcRoutingSourceHeight.load(std::memory_order_relaxed);
	snapshot.drawableTexturePtr = s_drcRoutingDrawableTexturePtr.load(std::memory_order_relaxed);
	snapshot.copyCounter = s_drcRoutingCopyCounter.load(std::memory_order_relaxed);
	snapshot.tvCopyCount = s_tvBackbufferCopyCount.load(std::memory_order_relaxed);
	snapshot.drcCopyCount = s_drcBackbufferCopyCount.load(std::memory_order_relaxed);
	snapshot.previewPublishCount = s_drcPreviewPublishCount.load(std::memory_order_relaxed);
	snapshot.previewSourceChangeCount = s_drcPreviewSourceChangeCount.load(std::memory_order_relaxed);
	snapshot.previewSourceTexturePtr = s_drcPreviewSourceTexturePtr.load(std::memory_order_relaxed);
	snapshot.previewPublishedTexturePtr = s_drcPreviewPublishedTexturePtr.load(std::memory_order_relaxed);
	return snapshot;
}

void MetalRenderer_SetDRCPreviewPublishCallback(MetalDRCPreviewPublishCallback callback)
{
	s_drcPreviewPublishCallback.store(callback, std::memory_order_release);
}

// Defined in the Common renderer
void LatteDraw_handleSpecialState8_clearAsDepth();

std::vector<MetalRenderer::DeviceInfo> MetalRenderer::GetDevices()
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
    if (device)
        return {{std::string(device->name()->utf8String()), device->registryID()}};
    return {};
#else
    NS_STACK_SCOPED auto devices = MTL::CopyAllDevices();
    std::vector<MetalRenderer::DeviceInfo> result;
    result.reserve(devices->count());
    for (uint32 i = 0; i < devices->count(); i++)
    {
        MTL::Device* device = static_cast<MTL::Device*>(devices->object(i));
        result.push_back({std::string(device->name()->utf8String()), device->registryID()});
    }

    return result;
#endif
}

MetalRenderer::MetalRenderer() : Renderer(RendererAPI::Metal)
{
	LatteTexture_SetLifecycleDiagnosticCallback(MetalTextureLifecycleDiagnosticCallback);
	ExceptionHandler_SetAdditionalLogCallback(MetalResourceCrashDiagnostics_DumpToCrashLog);
    // Options

    // Position invariance
    switch (g_current_game_profile->GetPositionInvariance())
    {
    case PositionInvariance::Auto:
        switch (CafeSystem::GetForegroundTitleId())
        {
        // Bayonetta
        case 0x0005000010157F00: // EUR
        case 0x0005000010157E00: // USA
        case 0x000500001014DB00: // JPN
        // Bayonetta 2
        case 0x0005000010172700: // EUR
        case 0x0005000010172600: // USA
        // Disney Planes
        case 0x0005000010136900: // EUR
        case 0x0005000010136A00: // EUR (TODO: check)
        case 0x0005000010136B00: // EUR (TODO: check)
        case 0x000500001011C500: // USA (TODO: check)
        // LEGO STAR WARS: The Force Awakens
        case 0x00050000101DAA00: // EUR
        case 0x00050000101DAB00: // USA
        // Mario Kart 8
        case 0x000500001010ED00: // EUR
        case 0x000500001010EC00: // USA
        case 0x000500001010EB00: // JPN
        case 0x0005000010183A00: // JPN (TODO: check)
        // Minecraft: Story Mode
        case 0x000500001020A300: // EUR
        case 0x00050000101E0100: // USA
        //case 0x000500001020a200: // USA
        // Ninja Gaiden 3: Razor's Edge
        case 0x0005000010110B00: // EUR
        case 0x0005000010139B00: // EUR (TODO: check)
        case 0x0005000010110A00: // USA
        case 0x0005000010110900: // JPN
        // Resident Evil: Revelations
        case 0x000500001012B400: // EUR
        case 0x000500001012CF00: // USA
        // Star Fox Zero
        case 0x00050000101B0500: // EUR
        case 0x0005000010201C00: // EUR (TODO: check)
        case 0x00050000101B0400: // USA
        case 0x0005000010201B00: // USA (TODO: check)
        // The Legend of Zelda: Breath of the Wild
        case 0x00050000101C9500: // EUR
        case 0x00050000101C9400: // USA
        case 0x00050000101C9300: // JPN
        // Wonderful 101
        case 0x0005000010135300: // EUR
        case 0x000500001012DC00: // USA
        case 0x0005000010116300: // JPN
        case 0x0005000010185600: // JPN (TODO: check)
            m_positionInvariance = true;
            break;
        default:
            m_positionInvariance = false;
            break;
        }
        break;
    case PositionInvariance::False:
        m_positionInvariance = false;
        break;
    case PositionInvariance::True:
        m_positionInvariance = true;
        break;
    }

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    m_device = MTL::CreateSystemDefaultDevice();
#else
    // Pick a device
    auto& config = GetConfig();
    const bool hasDeviceSet = config.mtl_graphic_device_uuid != 0;

    // If a device is set, try to find it
    if (hasDeviceSet)
    {
        NS_STACK_SCOPED auto devices = MTL::CopyAllDevices();
        for (uint32 i = 0; i < devices->count(); i++)
        {
            MTL::Device* device = static_cast<MTL::Device*>(devices->object(i));
            if (device->registryID() == config.mtl_graphic_device_uuid)
            {
                m_device = device;
                break;
            }
        }
    }

    if (!m_device)
    {
        if (hasDeviceSet)
        {
            cemuLog_log(LogType::Force, "The selected GPU ({}) could not be found. Using the system default device.", config.mtl_graphic_device_uuid);
            config.mtl_graphic_device_uuid = 0;
        }
        // Use the system default device
        m_device = MTL::CreateSystemDefaultDevice();
    }
#endif

    // Vendor
    const char* deviceName = m_device->name()->utf8String();
    if (memcmp(deviceName, "Apple", 5) == 0)
        m_vendor = GfxVendor::Apple;
    else if (memcmp(deviceName, "AMD", 3) == 0)
        m_vendor = GfxVendor::AMD;
    else if (memcmp(deviceName, "Intel", 5) == 0)
        m_vendor = GfxVendor::Intel;
    else if (memcmp(deviceName, "NVIDIA", 6) == 0)
        m_vendor = GfxVendor::Nvidia;
    else
        m_vendor = GfxVendor::Generic;

    // Feature support
    m_isAppleGPU = m_device->supportsFamily(MTL::GPUFamilyApple1);
    m_supportsLosslessTextureCompression = m_device->supportsFamily(MTL::GPUFamilyApple5);
    m_supportsFramebufferFetch = GetConfig().framebuffer_fetch.GetValue() ? m_device->supportsFamily(MTL::GPUFamilyApple2) : false;
    m_hasUnifiedMemory = m_device->hasUnifiedMemory();
    m_supportsMetal3 = m_device->supportsFamily(MTL::GPUFamilyMetal3);
    m_supportsMeshShaders = (m_supportsMetal3 && (m_vendor != GfxVendor::Intel || GetConfig().force_mesh_shaders.GetValue())); // Intel GPUs have issues with mesh shaders
    for (uint32_t family = 10; family >= 6; --family)
        if (m_device->supportsFamily(static_cast<MTL::GPUFamily>(1000 + family))) { m_appleFamily = family; break; }
    m_supportsMeshShaders = m_supportsMeshShaders && (!m_appleFamily || m_appleFamily >= 7);
    m_supportsFloat32Filtering = m_device->supports32BitFloatFiltering();
    m_recommendedMaxVRAMUsage = m_device->recommendedMaxWorkingSetSize();
    m_pixelFormatSupport = MetalPixelFormatSupport(m_device);
    MemoryBudget::processTargetEnabled = GetMetalMemoryPolicy().constrained;
    // Without BC sampling, expanded textures do not fit 4/6 GiB devices. Keep
    // them compressed as ETC2/EAC (all Apple GPUs sample it) via a GPU transcode.
    if (!m_pixelFormatSupport.m_supportsBCTextureCompression && m_device->supportsFamily(MTL::GPUFamilyApple2) &&
        GetMetalMemoryPolicy().constrained)
    {
        NS::Error* transcodeError = nullptr;
        NS_STACK_SCOPED MTL::Library* transcodeLibrary = m_device->newLibrary(ToNSString(bcTranscodeShaderSource), nullptr, &transcodeError);
        NS_STACK_SCOPED auto genericConstants = MTL::FunctionConstantValues::alloc()->init();
        NS_STACK_SCOPED MTL::Function* transcodeFunction = transcodeLibrary ? transcodeLibrary->newFunction(ToNSString("transcodeBCToETC"), genericConstants, &transcodeError) : nullptr;
        if (transcodeFunction)
            m_bcTranscodePipeline = m_device->newComputePipelineState(transcodeFunction, &transcodeError);
        m_pixelFormatSupport.m_transcodeBCToETC = m_bcTranscodePipeline != nullptr;
        if (m_bcTranscodePipeline) m_bcTranscodeLibrary = transcodeLibrary->retain();
        if (!m_bcTranscodePipeline)
            cemuLog_log(LogType::Force, "Metal BC->ETC2 transcode unavailable ({}); using CPU BC decode",
                        transcodeError ? transcodeError->localizedDescription()->utf8String() : "unknown");
    }

    CheckForPixelFormatSupport(m_pixelFormatSupport);

    // Command queue
    m_commandQueue = m_device->newCommandQueue();

    // Synchronization resources
    m_event = m_device->newEvent();

    // Resources
    NS_STACK_SCOPED MTL::SamplerDescriptor* samplerDescriptor = MTL::SamplerDescriptor::alloc()->init();
#ifdef CEMU_DEBUG_ASSERT
    samplerDescriptor->setLabel(GetLabel("Nearest sampler state", samplerDescriptor));
#endif
    m_nearestSampler = m_device->newSamplerState(samplerDescriptor);

    samplerDescriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
    samplerDescriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
#ifdef CEMU_DEBUG_ASSERT
    samplerDescriptor->setLabel(GetLabel("Linear sampler state", samplerDescriptor));
#endif
    m_linearSampler = m_device->newSamplerState(samplerDescriptor);

    // Null resources
    NS_STACK_SCOPED MTL::TextureDescriptor* textureDescriptor = MTL::TextureDescriptor::alloc()->init();
    textureDescriptor->setTextureType(MTL::TextureType1D);
    textureDescriptor->setWidth(1);
    textureDescriptor->setUsage(MTL::TextureUsageShaderRead);
    m_nullTexture1D = m_device->newTexture(textureDescriptor);
#ifdef CEMU_DEBUG_ASSERT
    m_nullTexture1D->setLabel(GetLabel("Null texture 1D", m_nullTexture1D));
#endif

    textureDescriptor->setTextureType(MTL::TextureType2D);
    textureDescriptor->setHeight(1);
    textureDescriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    m_nullTexture2D = m_device->newTexture(textureDescriptor);
#ifdef CEMU_DEBUG_ASSERT
    m_nullTexture2D->setLabel(GetLabel("Null texture 2D", m_nullTexture2D));
#endif

    m_memoryManager = new MetalMemoryManager(this);
    m_outputShaderCache = new MetalOutputShaderCache(this);
    m_pipelineCache = new MetalPipelineCache(this);
    m_depthStencilCache = new MetalDepthStencilCache(this);
    m_samplerCache = new MetalSamplerCache(this);

    // Lower the commit treshold when buffer cache needs reduced latency
    if (m_memoryManager->NeedsReducedLatency())
        m_defaultCommitTreshlod = 64;
    else
#if TARGET_OS_IPHONE || TARGET_OS_IOS
        m_defaultCommitTreshlod = 4096;
#else
        m_defaultCommitTreshlod = 512;
#endif

    // Occlusion queries
    m_occlusionQuery.m_resultBuffer = m_device->newBuffer(OCCLUSION_QUERY_POOL_SIZE * sizeof(uint64), MTL::ResourceStorageModeShared);
#ifdef CEMU_DEBUG_ASSERT
    m_occlusionQuery.m_resultBuffer->setLabel(GetLabel("Occlusion query result buffer", m_occlusionQuery.m_resultBuffer));
#endif
    m_occlusionQuery.m_resultsPtr = (uint64*)m_occlusionQuery.m_resultBuffer->contents();

    // Reset vertex and uniform buffers
   	for (uint32 i = 0; i < MAX_MTL_VERTEX_BUFFERS; i++)
    {
        m_state.m_vertexBufferOffsets[i] = INVALID_OFFSET;
        m_state.m_vertexBufferSizes[i] = 0;
    }

   	for (uint32 i = 0; i < METAL_SHADER_TYPE_TOTAL; i++)
    {
        for (uint32 j = 0; j < MAX_MTL_BUFFERS; j++)
        {
            m_state.m_uniformBufferOffsets[i][j] = INVALID_OFFSET;
            m_state.m_uniformBufferSizes[i][j] = 0;
        }
    }

    // Utility shader library

    // Create the library
    NS::Error* error = nullptr;
	NS_STACK_SCOPED MTL::Library* utilityLibrary = m_device->newLibrary(ToNSString(utilityShaderSource), nullptr, &error);
	if (error)
    {
        cemuLog_log(LogType::Force, "failed to create utility library (error: {})", error->localizedDescription()->utf8String());
    }

    // Pipelines
    NS_STACK_SCOPED MTL::Function* vertexFullscreenFunction = utilityLibrary->newFunction(ToNSString("vertexFullscreen"));
    NS_STACK_SCOPED MTL::Function* fragmentCopyDepthToColorFunction = utilityLibrary->newFunction(ToNSString("fragmentCopyDepthToColor"));

    m_copyDepthToColorDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    m_copyDepthToColorDesc->setVertexFunction(vertexFullscreenFunction);
    m_copyDepthToColorDesc->setFragmentFunction(fragmentCopyDepthToColorFunction);

    NS_STACK_SCOPED MTL::Function* fragmentCopyColorToDepthFunction = utilityLibrary->newFunction(ToNSString("fragmentCopyColorToDepth"));
    m_copyColorToDepthDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    m_copyColorToDepthDesc->setVertexFunction(vertexFullscreenFunction);
    m_copyColorToDepthDesc->setFragmentFunction(fragmentCopyColorToDepthFunction);
    NS_STACK_SCOPED MTL::DepthStencilDescriptor* copyDepthState = MTL::DepthStencilDescriptor::alloc()->init();
    copyDepthState->setDepthCompareFunction(MTL::CompareFunctionAlways);
    copyDepthState->setDepthWriteEnabled(true);
    m_copyColorToDepthState = m_device->newDepthStencilState(copyDepthState);

    // Void vertex pipelines
    if (m_isAppleGPU)
        m_copyBufferToBufferPipeline = new MetalVoidVertexPipeline(this, utilityLibrary, "vertexCopyBufferToBuffer");

}

MetalRenderer::~MetalRenderer()
{
	LatteTexture_SetLifecycleDiagnosticCallback(nullptr);
	ExceptionHandler_SetAdditionalLogCallback(nullptr);
	ReleaseDRCPreviewFrames();

    m_frameGeneration.reset();

    if (m_isAppleGPU)
        delete m_copyBufferToBufferPipeline;
    if (m_bcTranscodePipeline)
        m_bcTranscodePipeline->release();
    for (auto* pipeline : m_bcFormatPipelines)
        if (pipeline) pipeline->release();
    if (m_bcTranscodeLibrary) m_bcTranscodeLibrary->release();
    //delete m_copyTextureToTexturePipeline;
    //delete m_restrideBufferPipeline;

    m_copyDepthToColorDesc->release();
    for (const auto [pixelFormat, pipeline] : m_copyDepthToColorPipelines)
        if (pipeline) pipeline->release();
    m_copyColorToDepthDesc->release();
    for (const auto [pixelFormat, pipeline] : m_copyColorToDepthPipelines)
        if (pipeline) pipeline->release();
    m_copyColorToDepthState->release();

    delete m_outputShaderCache;
    delete m_pipelineCache;
    delete m_depthStencilCache;
    delete m_samplerCache;
    delete m_memoryManager;

    m_nullTexture1D->release();
    m_nullTexture2D->release();

    m_nearestSampler->release();
    m_linearSampler->release();

    if (m_readbackBuffer)
        m_readbackBuffer->release();

    if (m_xfbRingBuffer)
        m_xfbRingBuffer->release();

    m_occlusionQuery.m_resultBuffer->release();

    m_event->release();

    m_commandQueue->release();
    m_device->release();
}

void MetalRenderer::InitializeLayer(const Vector2i& size, bool mainWindow)
{
    auto& layer = GetLayer(mainWindow);
    layer = MetalLayerHandle(m_device, size, mainWindow);
    layer.GetLayer()->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
}

void MetalRenderer::ShutdownLayer(bool mainWindow)
{
    GetLayer(mainWindow) = MetalLayerHandle();
}

void MetalRenderer::ResizeLayer(const Vector2i& size, bool mainWindow)
{
    GetLayer(mainWindow).Resize(size);
}

void MetalRenderer::Initialize()
{
    s_frameCaptureRequested.store(false, std::memory_order_relaxed);
    Renderer::Initialize();
    // Resolve once per title, keeping other games' allocation policy unchanged.
    m_optimizeZeldaIndexHeap = false;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    switch (CafeSystem::GetForegroundTitleId())
    {
    case 0x00050000101c9300ULL: // Breath of the Wild JPN
    case 0x00050000101c9400ULL: // EUR
    case 0x00050000101c9500ULL: // USA
        m_optimizeZeldaIndexHeap = true;
        break;
    default:
        break;
    }
#endif
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    m_submitBeforeDrawableWait = m_optimizeZeldaIndexHeap && WindowSystem::IsPhone();
    // BotW waits on texture queries mid-frame on M4 iPads.
    m_submitReadbacksEarly = m_submitBeforeDrawableWait ||
        (m_optimizeZeldaIndexHeap && std::strcmp(m_device->name()->utf8String(), "Apple M4 GPU") == 0);
    TCL::TCLGPUSetIdleWaitEnabled(m_submitBeforeDrawableWait);
    // Keep the batching that avoided the fixed-rate lock. More frequent commits
    // increased guest starvation in the on-device 20261015c comparison.
    if (m_submitBeforeDrawableWait)
        m_defaultCommitTreshlod = 4096;
    cemuLog_log(LogType::Force, "[METAL DISPLAY] revision=20261015m prefixBeforeDrawable={} batchSequences={} uniformReuseMaxBytes=256 losslessLightSingleLayer={} demandAliases=false idleEventWait={} earlyReadbacks={} title={:016x}",
        m_submitBeforeDrawableWait, m_defaultCommitTreshlod, UsesPhoneLosslessTextures(), m_submitBeforeDrawableWait, m_submitReadbacksEarly, CafeSystem::GetForegroundTitleId());
#endif
    RendererShaderMtl::Initialize();
}

void MetalRenderer::Shutdown()
{
    s_frameCaptureRequested.store(false, std::memory_order_relaxed);
    if (m_capturing)
        EndCapture();
    // TODO: should shutdown both layers
    ImGui_ImplMetal_Shutdown();
    CommitCommandBuffer();
    while (!m_executingCommandBuffers.empty())
    {
        m_executingCommandBuffers.front()->waitUntilCompleted();
        ProcessFinishedCommandBuffers();
    }
    // Pipelines retain runtime shader pointers. Drain them before the shader
    // pool stops and LatteThread_Exit unloads those shaders.
    m_pipelineCache->WaitForPendingCompilations();
    if (m_readbackBuffer)
    {
        m_readbackBuffer->release();
        m_readbackBuffer = nullptr;
    }
    m_readbackBufferWriteOffset = 0;
    Renderer::Shutdown();
    RendererShaderMtl::Shutdown();
}

bool MetalRenderer::IsPadWindowActive()
{
    return (GetLayer(false).GetLayer() != nullptr);
}

bool MetalRenderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const
{
    // Subtract host memory from total VRAM, since it's shared with the CPU
    usageInMB = (m_device->currentAllocatedSize() - m_memoryManager->GetHostAllocationSize()) / 1024 / 1024;
    totalInMB = m_recommendedMaxVRAMUsage / 1024 / 1024;

    return true;
}

void MetalRenderer::ClearColorbuffer(bool padView)
{
    ClearColorbuffer(padView, 0.0f, 0.0f, 0.0f, 0.0f);
}

void MetalRenderer::ClearColorbuffer(bool padView, float r, float g, float b, float a)
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	if (padView && !ShouldDrawPadDisplay())
	{
		m_padDrawableContentsDefined = false;
		return;
	}
#endif
    const int fgTarget = (!padView) ? ActiveFrameGenerationTarget() : 0;
    if (!padView && fgTarget > 0)
    {
        if (m_mainLayer.GetLayer())
        {
            uint32 presW = (uint32)m_mainLayer.GetLayer()->drawableSize().width;
            uint32 presH = (uint32)m_mainLayer.GetLayer()->drawableSize().height;
            MTL::PixelFormat presFmt = m_state.m_usesSRGB ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm;
            EnsureFrameGenTextures(presW, presH, presFmt, false);
            if (m_frameGenTarget)
            {
                ClearColorTextureInternal(m_frameGenTarget, 0, 0, r, g, b, a);
                m_mainDrawableContentsDefined = true;
                m_frameGenRendered = true;
                return;
            }
        }
    }

    if (!AcquireDrawable(!padView))
        return;

    ClearColorTextureInternal(GetLayer(!padView).GetDrawable()->texture(), 0, 0, r, g, b, a);
	(padView ? m_padDrawableContentsDefined : m_mainDrawableContentsDefined) = true;
}

void MetalRenderer::DrawEmptyFrame(bool mainWindow)
{
    if (!BeginFrame(mainWindow))
		return;
	SwapBuffers(mainWindow, !mainWindow);
}

std::atomic<uint64_t> g_metalSwapCount{0};
static uint32 g_pass_fboChange = 0;
static uint32 g_pass_texBarrier = 0;
static uint32 g_pass_accBarrier = 0;
static uint32 g_pass_blit = 0;
static uint32 g_pass_clearSampled = 0;
static uint32 g_pass_cacheBind = 0;
static uint32 g_pass_clearSlice = 0;
static uint32 g_pass_attachmentChange = 0;
static uint32 g_pass_pendingClear = 0;
static uint32 g_pass_otherEncoder = 0;
static uint32 g_pass_commitClears = 0;
static uint32 g_blit_textureUploads = 0;
static uint32 g_blit_textureCopies = 0;
static uint32 g_blit_bufferCopies = 0;
static uint32 g_bufferCacheCopies = 0;
static uint32 g_streamoutCopies = 0;
static uint32 g_streamoutInPassCopies = 0;
static uint32 g_cacheUploadInPassCalls = 0;
static uint32 g_cacheUploadInPassRanges = 0;
static uint32 g_cacheUploadBlitFallbacks = 0;
static uint32 g_blit_textureCopyTiny = 0;
static uint32 g_blit_textureCopyLarge = 0;

#if TARGET_OS_IPHONE || TARGET_OS_IOS
// Sample one frame every five seconds. Collect fixed-size signatures while
// encoding, then format a few aggregate lines only after that frame ends.
namespace
{
struct AttachmentTransitionSample
{
    uint32 slot = 0, oldAddress = 0, newAddress = 0;
    uint16 oldMip = 0, newMip = 0, oldSlice = 0, newSlice = 0;
    uint32 count = 0;
};
struct TextureCopySample
{
    uint32 source = 0, destination = 0, width = 0, height = 0;
    uint64 sourceObject = 0, destinationObject = 0;
    uint32 sourceFormat = 0, destinationFormat = 0;
    uint32 sameMetalTexture = 0;
    uint32 count = 0;
};
static bool s_sampleFrameTransitions = false;
static uint32 s_sampleFramesRemaining = 0;
static std::array<AttachmentTransitionSample, 64> s_attachmentSamples{};
static std::array<TextureCopySample, 64> s_copySamples{};
static uint32 s_attachmentOverflow = 0, s_copyOverflow = 0;
static uint64 s_sampleAttachmentBytes = 0;
static std::chrono::steady_clock::time_point s_lastTransitionSample{};

static void RecordAttachmentTransition(uint32 slot, LatteTextureView* oldView, LatteTextureView* newView)
{
    if (!s_sampleFrameTransitions || oldView == newView)
        return;
    const uint32 oldAddress = oldView ? oldView->baseTexture->physAddress : 0;
    const uint32 newAddress = newView ? newView->baseTexture->physAddress : 0;
    const uint16 oldMip = oldView ? oldView->firstMip : 0;
    const uint16 newMip = newView ? newView->firstMip : 0;
    const uint16 oldSlice = oldView ? oldView->firstSlice : 0;
    const uint16 newSlice = newView ? newView->firstSlice : 0;
    for (auto& item : s_attachmentSamples)
    {
        if (!item.count)
        {
            item = {slot, oldAddress, newAddress, oldMip, newMip, oldSlice, newSlice, 1};
            return;
        }
        if (item.slot == slot && item.oldAddress == oldAddress && item.newAddress == newAddress &&
            item.oldMip == oldMip && item.newMip == newMip &&
            item.oldSlice == oldSlice && item.newSlice == newSlice)
        {
            ++item.count;
            return;
        }
    }
    ++s_attachmentOverflow;
}

static void RecordTextureCopy(LatteTexture* source, LatteTexture* destination, uint32 width,
                              uint32 height, bool sameMetalTexture)
{
    if (!s_sampleFrameTransitions)
        return;
    for (auto& item : s_copySamples)
    {
        if (!item.count)
        {
            item = {source->physAddress, destination->physAddress, width, height,
                    source->hostTextureObjectId, destination->hostTextureObjectId,
                    (uint32)source->format, (uint32)destination->format,
                    sameMetalTexture ? 1u : 0u, 1};
            return;
        }
        if (item.source == source->physAddress && item.destination == destination->physAddress &&
            item.width == width && item.height == height &&
            item.sourceObject == source->hostTextureObjectId &&
            item.destinationObject == destination->hostTextureObjectId)
        {
            ++item.count;
            item.sameMetalTexture += sameMetalTexture ? 1u : 0u;
            return;
        }
    }
    ++s_copyOverflow;
}
}
#endif

MetalRenderer::PipelineDiagnosticContext MetalRenderer::GetPipelineDiagnosticContext() const
{
    PipelineDiagnosticContext result{};
    result.queryActive = m_occlusionQuery.m_active;
    if (auto* fbo = m_state.m_activeFBO.m_fbo)
    {
        result.size = fbo->m_size;
        for (const auto* texture : fbo->GetTextures()) result.readbackAttachment |= texture->enableReadback;
    }
    return result;
}

void MetalRenderer::SwapBuffers(bool swapTV, bool swapDRC)
{
    g_metalSwapCount.fetch_add(1, std::memory_order_relaxed);
    if (swapTV)
        SwapBuffer(true);
    if (swapDRC)
        SwapBuffer(false);

    const int fgTarget = swapTV ? ActiveFrameGenerationTarget() : 0;
    auto releaseSource = [](MTL::CommandBuffer* source) { if (source) source->release(); };
    std::unique_ptr<MTL::CommandBuffer, decltype(releaseSource)> fgSource(nullptr, releaseSource);
    if (fgTarget > 0 && m_frameGeneration && m_frameGenRendered)
    {
        // A previously committed prefix may already have retired. GetCommandBuffer
        // creates an ordered fence CB in that case; never retain the stale field.
        fgSource.reset(GetCommandBuffer()->retain());
    }
    // Reset the command buffers (they are released by TemporaryBufferAllocator)
    CommitCommandBuffer(true);

    if (swapTV && fgTarget > 0 && m_frameGeneration && m_frameGenRendered)
    {
        // Submit retains the source CB and its immutable texture lease. The FG
        // queue reads it only after this exact producer has completed.
        try
        {
            m_frameGeneration->Submit(fgSource.get(), fgTarget, CemuBridge_GetRunningGameFrameGenerationFast());
        }
        catch (const std::exception& error)
        {
            cemuLog_log(LogType::Force, "[FRAME GEN] Submission failed; keeping original output: {}", error.what());
            m_frameGenMode.Fail();
            m_frameGeneration.reset();
        }
        m_frameGenTarget = nullptr;
        m_frameGenRendered = false;
        if (m_frameGeneration && (g_metalSwapCount.load(std::memory_order_relaxed) % 180) == 1)
        {
            const auto stats = m_frameGeneration->GetStatistics();
            cemuLog_log(LogType::Force, "[FRAME GEN] source:{:.1f} displayed:{:.1f} target:{} interpolationPasses:{} original:{} repeated:{} missed:{} pairUnavailable:{} sourceReady:{:.2f}ms fgGPU:{:.2f}ms",
                stats.sourceFPS, stats.displayedFPS, fgTarget, stats.generated, stats.original, stats.repeated, stats.dropped, stats.starved, stats.sourceReadyMs, stats.gpuMs);
        }
    }
    else if (swapTV && fgTarget == 0 && m_frameGeneration)
    {
        m_frameGeneration.reset();
        m_frameGenTarget = nullptr;
        m_frameGenRendered = false;
    }

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    static auto s_lastFrameTime = std::chrono::steady_clock::now();
    static uint32 s_lastDrawCallCount = 0;
    static uint32 s_accumFrames = 0;
    static double s_accumFrameTime = 0.0;
    static double s_accumCpuWait = 0.0;
    static uint32 s_accumDraws = 0;
    static uint32 s_accumPasses = 0;
    static auto s_lastPerfUpdate = std::chrono::steady_clock::now();
    static auto s_lastHitchLogTime = std::chrono::steady_clock::now();
    static uint64_t s_lastCommandBufferWaitUs = 0;
    static uint64_t s_lastPipelineWaitNs = 0;
    static uint64_t s_lastPipelineWaitCount = 0;
    static uint64_t s_lastDeferredDraws = 0;
    static uint64_t s_lastPipelineCompileNs = 0;
    static uint64_t s_lastPipelineCompileCount = 0;
    static uint64_t s_lastShaderCompileNs = 0, s_lastShaderCompileCount = 0;
    static uint64_t s_lastTextureLoadNs = 0, s_lastTextureUploadNs = 0, s_lastPipelineSyncNs = 0, s_lastDrawableWaitUs = 0;

    auto now = std::chrono::steady_clock::now();
    double frameMs = std::chrono::duration<double, std::milli>(now - s_lastFrameTime).count();
    s_lastFrameTime = now;

    uint32 curDraws = LatteGPUState.drawCallCounter;
    uint32 frameDraws = curDraws >= s_lastDrawCallCount ? (curDraws - s_lastDrawCallCount) : 0;
    s_lastDrawCallCount = curDraws;

    uint32 framePasses = m_performanceMonitor.m_renderPasses;

    uint64 idleTsc = performanceMonitor.gpuTime_idleTime.getPreviousFrameValue();
    double cpuWaitMs = (double)PPCTimer_tscToMicroseconds(idleTsc) / 1000.0;
    // Completed GPU frames only; CPU frame time is not a GPU measurement.
    const double gpuMs = m_lastCompletedGpuMs;
    const uint64_t commandBufferWaitTotal = RuntimePerformance::commandBufferWaitUs.load(std::memory_order_relaxed);
    const double commandBufferWaitMs = double(commandBufferWaitTotal - s_lastCommandBufferWaitUs) / 1000.0;
    s_lastCommandBufferWaitUs = commandBufferWaitTotal;
    const uint64_t deferredDrawTotal = RuntimePerformance::drawsSkippedPendingPipeline.load(std::memory_order_relaxed);
    const uint64_t deferredDraws = deferredDrawTotal - s_lastDeferredDraws;
    s_lastDeferredDraws = deferredDrawTotal;
    const uint64_t pipelineWaitTotal = RuntimePerformance::pipelineWaitNs.load(std::memory_order_relaxed);
    const uint64_t pipelineWaitCountTotal = RuntimePerformance::pipelineWaitCount.load(std::memory_order_relaxed);
    const uint64_t pipelineCompileTotal = RuntimePerformance::pipelineCompileNs.load(std::memory_order_relaxed);
    const uint64_t pipelineCompileCountTotal = RuntimePerformance::pipelineCompileCount.load(std::memory_order_relaxed);
    const double pipelineWaitMs = double(pipelineWaitTotal - s_lastPipelineWaitNs) / 1000000.0;
    const uint64_t pipelineWaits = pipelineWaitCountTotal - s_lastPipelineWaitCount;
    const double pipelineCompileMs = double(pipelineCompileTotal - s_lastPipelineCompileNs) / 1000000.0;
    const uint64_t pipelineCompiles = pipelineCompileCountTotal - s_lastPipelineCompileCount;
    s_lastPipelineWaitNs = pipelineWaitTotal;
    s_lastPipelineWaitCount = pipelineWaitCountTotal;
    s_lastPipelineCompileNs = pipelineCompileTotal;
    s_lastPipelineCompileCount = pipelineCompileCountTotal;
    const auto frameDeltaMs = [](const std::atomic<uint64_t>& total, uint64_t& previous) {
        const uint64_t current = total.load(std::memory_order_relaxed);
        const double result = double(current - previous) / 1000000.0;
        previous = current;
        return result;
    };
    const double shaderCompileMs = frameDeltaMs(RuntimePerformance::shaderCompileNs, s_lastShaderCompileNs);
    const uint64_t shaderCount = RuntimePerformance::shaderCompileCount.load(std::memory_order_relaxed);
    const uint64_t shaderCompiles = shaderCount - s_lastShaderCompileCount;
    s_lastShaderCompileCount = shaderCount;
    const double textureLoadMs = frameDeltaMs(RuntimePerformance::textureLoadNs, s_lastTextureLoadNs);
    const double textureUploadMs = frameDeltaMs(RuntimePerformance::textureUploadNs, s_lastTextureUploadNs);
    const double pipelineSyncMs = frameDeltaMs(RuntimePerformance::pipelineSyncNs, s_lastPipelineSyncNs);
    const uint64_t drawableWaitTotal = RuntimePerformance::drawableWaitUs.load(std::memory_order_relaxed);
    const double drawableWaitMs = double(drawableWaitTotal - s_lastDrawableWaitUs) / 1000.0;
    s_lastDrawableWaitUs = drawableWaitTotal;

    uint32 fbo = std::exchange(g_pass_fboChange, 0);
    uint32 tb = std::exchange(g_pass_texBarrier, 0);
    uint32 acc = std::exchange(g_pass_accBarrier, 0);
    uint32 blit = std::exchange(g_pass_blit, 0);
    uint32 cs = std::exchange(g_pass_clearSampled, 0);
    uint32 cb = std::exchange(g_pass_cacheBind, 0);
    uint32 clr = std::exchange(g_pass_clearSlice, 0);
    uint32 attachmentChanges = std::exchange(g_pass_attachmentChange, 0);
    uint32 pendingClearBreaks = std::exchange(g_pass_pendingClear, 0);
    uint32 otherEncoderBreaks = std::exchange(g_pass_otherEncoder, 0);
    uint32 commitClears = std::exchange(g_pass_commitClears, 0);
    uint32 textureUploads = std::exchange(g_blit_textureUploads, 0);
    uint32 textureCopies = std::exchange(g_blit_textureCopies, 0);
    uint32 bufferCopies = std::exchange(g_blit_bufferCopies, 0);
    uint32 bufferCacheCopies = std::exchange(g_bufferCacheCopies, 0);
    uint32 streamoutCopies = std::exchange(g_streamoutCopies, 0);
    uint32 streamoutInPassCopies = std::exchange(g_streamoutInPassCopies, 0);
    uint32 cacheUploadInPassCalls = std::exchange(g_cacheUploadInPassCalls, 0);
    uint32 cacheUploadInPassRanges = std::exchange(g_cacheUploadInPassRanges, 0);
    uint32 cacheUploadBlitFallbacks = std::exchange(g_cacheUploadBlitFallbacks, 0);
    uint32 tinyTextureCopies = std::exchange(g_blit_textureCopyTiny, 0);
    uint32 largeTextureCopies = std::exchange(g_blit_textureCopyLarge, 0);
    uint32 commandBuffers = m_performanceMonitor.m_commandBuffers;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    if (s_sampleFrameTransitions)
    {
        std::sort(s_attachmentSamples.begin(), s_attachmentSamples.end(),
            [](const auto& a, const auto& b) { return a.count > b.count; });
        std::sort(s_copySamples.begin(), s_copySamples.end(),
            [](const auto& a, const auto& b) { return a.count > b.count; });
        const auto& a0 = s_attachmentSamples[0];
        const auto& a1 = s_attachmentSamples[1];
        const auto& c0 = s_copySamples[0];
        const auto& c1 = s_copySamples[1];
        cemuLog_log(LogType::Force,
            "[FRAME TRACE] frame:{:.1f}ms gpu:{:.1f}ms draws:{} passes:{} attach:{} copies:{} attachmentMiB:{} overflow:{}/{} | a0:{} {:08x}->{:08x} n:{} a1:{} {:08x}->{:08x} n:{} | c0:{:08x}->{:08x} {}x{} n:{} obj:{}/{} fmt:{}/{} sameMetal:{} c1:{:08x}->{:08x} {}x{} n:{} obj:{}/{} fmt:{}/{} sameMetal:{}",
            frameMs, gpuMs, frameDraws, framePasses, attachmentChanges, textureCopies,
            s_sampleAttachmentBytes / (1024 * 1024), s_attachmentOverflow, s_copyOverflow,
            a0.slot, a0.oldAddress, a0.newAddress, a0.count,
            a1.slot, a1.oldAddress, a1.newAddress, a1.count,
            c0.source, c0.destination, c0.width, c0.height, c0.count,
            c0.sourceObject, c0.destinationObject, c0.sourceFormat, c0.destinationFormat, c0.sameMetalTexture,
            c1.source, c1.destination, c1.width, c1.height, c1.count,
            c1.sourceObject, c1.destinationObject, c1.sourceFormat, c1.destinationFormat, c1.sameMetalTexture);
        s_sampleFrameTransitions = --s_sampleFramesRemaining != 0;
        s_attachmentSamples.fill({});
        s_copySamples.fill({});
        s_attachmentOverflow = 0;
        s_copyOverflow = 0;
        s_sampleAttachmentBytes = 0;
    }
    if (!s_sampleFrameTransitions && CemuBridge_IsDiagnosticLoggingEnabled() &&
        frameDraws > 4000 && gpuMs > 20.0 &&
        now - s_lastTransitionSample >= std::chrono::seconds(8))
    {
        s_lastTransitionSample = now;
        s_sampleFramesRemaining = 2;
        s_sampleFrameTransitions = true;
    }
#endif
    // Clear unreported miss context only when diagnostics transition off.
    static bool s_lastPipelineDiagnosticsEnabled = false;
    const bool diagnosticLoggingEnabled = CemuBridge_IsDiagnosticLoggingEnabled();
    if (s_lastPipelineDiagnosticsEnabled && !diagnosticLoggingEnabled)
        RuntimePerformance::pipelineSyncRecorder.reset();
    s_lastPipelineDiagnosticsEnabled = diagnosticLoggingEnabled;
    // Report frame-local pass reasons; GPU duration belongs to the last completed frame.
    if (diagnosticLoggingEnabled && frameMs > 18.2)
    {
        auto msSinceLastHitch = std::chrono::duration<double, std::milli>(now - s_lastHitchLogTime).count();
        if (msSinceLastHitch >= 1000.0)
        {
            s_lastHitchLogTime = now;
            double instFps = 1000.0 / frameMs;
            if (CemuBridge_IsDiagnosticLoggingEnabled())
            {
                // Texture load includes detiling, tracking and the nested upload.
                // Shader/pipeline compilation sums include worker work; sync is
                // render-thread wall time. Do not add these overlapping timings.
                // These are completed command-buffer elapsed times, including
                // scheduling stalls; they are not active shader execution time.
                cemuLog_log(LogType::Force, "[LOAD HITCH] shader:{:.1f}ms/{} pipelineSync:{:.1f}ms texture:{:.1f}ms upload:{:.1f}ms drawableWait:{:.1f}ms displayLink:{:.2f}ms gpuPrefix:{:.2f}ms gpuFinal:{:.2f}ms uniformReuse:{}/uploads:{} libReuseTotal:{} deferredDraws:{}",
                    shaderCompileMs, shaderCompiles, pipelineSyncMs, textureLoadMs, textureUploadMs,
                    drawableWaitMs, RuntimePerformance::displayLinkPeriodUs.load(std::memory_order_relaxed) / 1000.0,
                    m_lastCompletedGpuRenderMs, m_lastCompletedGpuOutputMs,
                    m_supportBufferReuseHits, m_supportBufferUploads,
                    RendererShaderMtl::GetLibraryReuseCount(), deferredDraws);
                const auto pipelineWindow = RuntimePerformance::pipelineSyncRecorder.takeReport(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
                if (pipelineWindow.slowCount)
                {
                    const auto& p = pipelineWindow.strongest;
                    const auto& t = p.timings;
                    const char* phase = t.initNs >= t.shaderWaitNs && t.initNs >= t.createNs ? "init" :
                        (t.shaderWaitNs >= t.createNs ? "shaderReady" : "newRenderPipelineState");
                    const auto& c = pipelineWindow.reasonCounts;
                    cemuLog_log(LogType::Force, "[PIPELINE HITCH] windowSinceLastReport required:{}/slow:{} reasons[asyncOff:{} query:{} readback:{} rasterKill:{} streamout:{} ink1600:{} colorOnly:{} depthOnly:{} other:{}] total[init:{:.1f}ms shaderReady:{:.1f}ms create:{:.1f}ms] max[key:{:016x} VS:{:016x}/{:016x} GS:{:016x}/{:016x} PS:{:016x}/{:016x} fbo:{}x{} colors:{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x} depth:{:x} activeColors:{:x} activeDepth:{:x} reasons:{:x} pipeline:{} result:{} phase:{} init:{:.1f}ms shaderReady:{:.1f}ms create:{:.1f}ms]",
                        pipelineWindow.count, pipelineWindow.slowCount, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8],
                        pipelineWindow.totals.initNs / 1e6, pipelineWindow.totals.shaderWaitNs / 1e6, pipelineWindow.totals.createNs / 1e6,
                        p.key, p.vsBase, p.vsAux, p.gsBase, p.gsAux, p.psBase, p.psAux, p.width, p.height,
                        p.colorFormats[0], p.colorFormats[1], p.colorFormats[2], p.colorFormats[3], p.colorFormats[4], p.colorFormats[5], p.colorFormats[6], p.colorFormats[7],
                        p.depthFormat, p.activeColorMask, p.activeDepthFormat, p.reasons, p.mesh ? "mesh" : "render", p.success ? "ready" : "failed", phase,
                        t.initNs / 1e6, t.shaderWaitNs / 1e6, t.createNs / 1e6);
                }
                cemuLog_log(LogType::Force, "[CEMU HITCH] FPS: {:.1f} ({:.1f}ms) | Guest GPU idle: {:.1f}ms | GPU: {:.1f}ms | cbWait:{:.1f}ms idleReadbacks:{} | pipelineWait:{:.1f}ms/{} compile:{:.1f}ms/{} pending:{} | Thermal: {} | Draws: {} | Passes: {} | CmdBufs: {} | BlitOps [texUpload:{} texCopy:{} tiny:{} large:{} bufCopy:{} cacheCopy:{} streamoutCopy:{} inPass:{}] [cacheUpload inPass:{}/{} blitFallback:{}] [fbo:{} attach:{} clear:{} other:{} tb:{} acc:{} blit:{} cs:{} cb:{} clr:{} commitClear:{}]",
                       instFps, frameMs, cpuWaitMs, gpuMs, commandBufferWaitMs, m_idleReadbackSubmissions, pipelineWaitMs, pipelineWaits, pipelineCompileMs, pipelineCompiles, RuntimePerformance::pendingPipelines.load(std::memory_order_relaxed), CemuBridge_GetThermalStateName(), frameDraws, framePasses, commandBuffers, textureUploads, textureCopies, tinyTextureCopies, largeTextureCopies, bufferCopies, bufferCacheCopies, streamoutCopies, streamoutInPassCopies, cacheUploadInPassCalls, cacheUploadInPassRanges, cacheUploadBlitFallbacks, fbo, attachmentChanges, pendingClearBreaks, otherEncoderBreaks, tb, acc, blit, cs, cb, clr, commitClears);
            }
        }
    }

    s_accumFrames++;
    s_accumFrameTime += frameMs;
    s_accumCpuWait += cpuWaitMs;
    s_accumDraws += frameDraws;
    s_accumPasses += framePasses;

    auto timeSincePerfUpdate = std::chrono::duration<double, std::milli>(now - s_lastPerfUpdate).count();
    if (timeSincePerfUpdate >= 250.0 && s_accumFrames > 0)
    {
        const int currentFgTarget = ActiveFrameGenerationTarget();
        double avgFps;
        if (currentFgTarget > 0 && m_frameGeneration)
        {
            avgFps = m_frameGeneration->PresentedFPS();
        }
        else
        {
            avgFps = (s_accumFrames * 1000.0) / s_accumFrameTime;
        }
        double avgCpuWait = s_accumCpuWait / s_accumFrames;
        double avgGpu = m_completedGpuWindowFrames ? m_completedGpuWindowMs / m_completedGpuWindowFrames : m_lastCompletedGpuMs;
        uint32 avgDraws = s_accumDraws / s_accumFrames;
        uint32 avgPasses = (uint32)std::round((double)s_accumPasses / s_accumFrames);

        CemuBridge_UpdatePerfStats(avgFps, avgCpuWait, avgGpu, avgDraws, avgPasses);

        s_accumFrames = 0;
        s_accumFrameTime = 0.0;
        s_accumCpuWait = 0.0;
        m_completedGpuWindowMs = 0.0;
        m_completedGpuWindowFrames = 0;
        s_accumDraws = 0;
        s_accumPasses = 0;
        s_lastPerfUpdate = now;
    }
#endif

    m_idleReadbackSubmissions = 0;
    m_recordedDrawcalls = 0;
    m_supportBufferReuseHits = 0;
    m_supportBufferUploads = 0;
    m_commitTreshold = m_defaultCommitTreshlod;

    // Debug
    m_performanceMonitor.ResetPerFrameData();

    // GPU capture
    if (m_capturing)
    {
        EndCapture();
    }
    else if (ConsumeMetalFrameCaptureRequest() || m_captureFrame)
    {
        StartCapture();
        m_captureFrame = false;
    }
    if (swapTV)
        m_frameGenMode.FinishFrame();
}

void MetalRenderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView) {
	if (!m_screenshot_requested && m_screenshot_state == ScreenshotState::None)
		return;

	if (m_mainLayer.GetDrawable())
	{
		// we already took a pad view screenshow and want a main window screenshot
		if (m_screenshot_state == ScreenshotState::Main && padView)
			return;

		if (m_screenshot_state == ScreenshotState::Pad && !padView)
			return;

		// remember which screenshot is left to take
		if (m_screenshot_state == ScreenshotState::None)
			m_screenshot_state = padView ? ScreenshotState::Main : ScreenshotState::Pad;
		else
			m_screenshot_state = ScreenshotState::None;
	}
	else
		m_screenshot_state = ScreenshotState::None;

	auto texMtl = static_cast<LatteTextureMtl*>(texView->baseTexture);
	FlushPendingTextureClears(texMtl, 0, 1, 0, 1);

	int width, height;
	texMtl->GetEffectiveSize(width, height, 0);

	uint32 bytesPerRow = GetMtlTextureBytesPerRow(texMtl->format, texMtl->isDepth, width);
	uint32 size = GetMtlTextureBytesPerImage(texMtl->format, texMtl->isDepth, height, bytesPerRow);

	auto blitCommandEncoder = GetBlitCommandEncoder();

	auto& bufferAllocator = m_memoryManager->GetStagingAllocator();
	auto buffer = bufferAllocator.AllocateBufferMemory(size, 1);

	blitCommandEncoder->copyFromTexture(texMtl->GetTexture(), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1), buffer.mtlBuffer, buffer.bufferOffset, bytesPerRow, 0);

	bool formatValid = true;
	std::vector<uint8> rgb_data;
	rgb_data.reserve(3 * width * height);

	auto pixelFormat = texMtl->GetTexture()->pixelFormat();
	// TODO: implement more formats
	switch (pixelFormat)
	{
	case MTL::PixelFormatRGBA8Unorm:
		for (auto ptr = buffer.memPtr; ptr < buffer.memPtr + size; ptr += 4)
		{
			rgb_data.emplace_back(*ptr);
			rgb_data.emplace_back(*(ptr + 1));
			rgb_data.emplace_back(*(ptr + 2));
		}
		break;
	case MTL::PixelFormatRGBA8Unorm_sRGB:
		for (auto ptr = buffer.memPtr; ptr < buffer.memPtr + size; ptr += 4)
		{
			rgb_data.emplace_back(SRGBComponentToRGB(*ptr));
			rgb_data.emplace_back(SRGBComponentToRGB(*(ptr + 1)));
			rgb_data.emplace_back(SRGBComponentToRGB(*(ptr + 2)));
		}
		break;
	default:
		cemuLog_log(LogType::Force, "Unsupported screenshot texture pixel format {}", pixelFormat);
		formatValid = false;
		break;
	}

	if (formatValid)
		SaveScreenshot(rgb_data, width, height, !padView);
}

void MetalRenderer::DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter,
								sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight,
								bool padView, bool clearBackground)
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	// The Wii U still renders its GamePad texture. Only skip compositing that
	// texture into the hidden iPad drawable when no UI is consuming its preview.
	// A screenshot still takes the normal path through HandleScreenshotRequest.
	if (padView && !ShouldDrawPadDisplay())
	{
		m_padDrawableContentsDefined = false;
		return;
	}
#endif
    auto& layer = GetLayer(!padView);
    const int fgTarget = !padView ? ActiveFrameGenerationTarget() : 0;
    MTL::Texture* targetTexture = nullptr;
    if (!padView && fgTarget > 0)
    {
        if (m_mainLayer.GetLayer())
        {
            uint32 presW = (uint32)m_mainLayer.GetLayer()->drawableSize().width;
            uint32 presH = (uint32)m_mainLayer.GetLayer()->drawableSize().height;
            MTL::PixelFormat presFmt = m_state.m_usesSRGB ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm;
            EnsureFrameGenTextures(presW, presH, presFmt);
            targetTexture = m_frameGenTarget;
        }
    }
    if (!targetTexture)
    {
        if (!AcquireDrawable(!padView))
            return;
        targetTexture = layer.GetDrawable()->texture();
    }
    if (!targetTexture)
        return;

    if (texView->baseTexture && texView->firstMip >= 0 && texView->firstSlice >= 0)
        FlushPendingTextureClears(texView->baseTexture, (uint32)texView->firstMip, (uint32)std::max(texView->numMip, 1), (uint32)texView->firstSlice, (uint32)std::max(texView->numSlice, 1));

    MTL::Texture* presentTexture = static_cast<LatteTextureViewMtl*>(texView)->GetRGBAView();

	const uint64_t displayCopyCount = (padView ? s_drcBackbufferCopyCount : s_tvBackbufferCopyCount).fetch_add(1, std::memory_order_relaxed) + 1;
	const uint64_t otherDisplayCopyCount = (padView ? s_tvBackbufferCopyCount : s_drcBackbufferCopyCount).load(std::memory_order_relaxed);
	if (padView)
	{
		// This diagnostic describes the DRC route. Do not let a subsequent TV
		// copy overwrite it with an unrelated source texture.
		s_drcRoutingPadView.store(true, std::memory_order_relaxed);
		s_drcRoutingSelectedPadLayer.store(true, std::memory_order_relaxed);
		s_drcRoutingSourceTexturePtr.store((uint64_t)presentTexture, std::memory_order_relaxed);
		s_drcRoutingSourceWidth.store(texView->baseTexture->width, std::memory_order_relaxed);
		s_drcRoutingSourceHeight.store(texView->baseTexture->height, std::memory_order_relaxed);
		s_drcRoutingDrawableTexturePtr.store((uint64_t)targetTexture, std::memory_order_relaxed);
		s_drcRoutingCopyCounter.store(displayCopyCount, std::memory_order_relaxed);
		s_drcRoutingValid.store(true, std::memory_order_release);
	}
	auto& displayLogCount = padView ? s_drcBackbufferLogCount : s_tvBackbufferLogCount;
	const uint32_t logIndex = displayLogCount.fetch_add(1, std::memory_order_relaxed);
	if (logIndex < kMaxBackbufferRoutingLogsPerDisplay)
	{
		const uint64_t tvCopies = padView ? otherDisplayCopyCount : displayCopyCount;
		const uint64_t drcCopies = padView ? displayCopyCount : otherDisplayCopyCount;
		cemuLog_log(LogType::Force,
			"[DRC ROUTING] padView={} selected={} layer={} sourceGuest={}x{} sourceMetal={}x{} destinationTexture={} tvCopies={} drcCopies={}",
			padView,
			padView ? "pad" : "main",
			(void*)layer.GetLayer(),
			texView->baseTexture->width,
			texView->baseTexture->height,
			presentTexture->width(),
			presentTexture->height(),
			(void*)targetTexture,
			tvCopies,
			drcCopies);
	}

    // Find out which shader we are using
    uint8 shaderIndex = 255;
    if (shader == RendererOutputShader::s_copy_shader) shaderIndex = 0;
    else if (shader == RendererOutputShader::s_bicubic_shader) shaderIndex = 1;
    else if (shader == RendererOutputShader::s_hermit_shader) shaderIndex = 2;
    else if (shader == RendererOutputShader::s_copy_shader_ud) shaderIndex = 3;
    else if (shader == RendererOutputShader::s_bicubic_shader_ud) shaderIndex = 4;
    else if (shader == RendererOutputShader::s_hermit_shader_ud) shaderIndex = 5;

    uint8 shaderType = shaderIndex % 3;

    // Get the render pipeline state
    auto renderPipelineState = m_outputShaderCache->GetPipeline(shader, shaderIndex, m_state.m_usesSRGB);

    float outputSize[2] = {(float)imageWidth, (float)imageHeight};

    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(targetTexture);
    colorAttachment->setLoadAction(clearBackground ? MTL::LoadActionClear :
        (m_frameGenTarget && targetTexture == m_frameGenTarget ? MTL::LoadActionLoad : MTL::LoadActionDontCare));
    colorAttachment->setStoreAction(MTL::StoreActionStore);

    auto renderCommandEncoder = GetTemporaryRenderCommandEncoder(renderPassDescriptor);

    renderCommandEncoder->setRenderPipelineState(renderPipelineState);
    renderCommandEncoder->setFragmentTexture(presentTexture, 0);
    renderCommandEncoder->setFragmentSamplerState((useLinearTexFilter ? m_linearSampler : m_nearestSampler), 0);

    if (shaderType == 2)
        renderCommandEncoder->setFragmentBytes(outputSize, sizeof(outputSize), 0);

    renderCommandEncoder->setViewport(MTL::Viewport{(double)imageX, (double)imageY, (double)imageWidth, (double)imageHeight, 0.0, 1.0});
    renderCommandEncoder->setScissorRect(MTL::ScissorRect{(uint32)imageX, (uint32)imageY, (uint32)imageWidth, (uint32)imageHeight});
    renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));

    EndEncoding();

    (padView ? m_padDrawableContentsDefined : m_mainDrawableContentsDefined) = true;
    if (!padView && fgTarget > 0 && m_frameGeneration)
    {
        m_frameGenRendered = true;
    }

	if (padView)
		PublishDRCPreviewFrame(targetTexture, presentTexture);
}

void MetalRenderer::PublishDRCPreviewFrame(MTL::Texture* padDrawableTexture, MTL::Texture* renderTargetTexture)
{
	if (!s_drcPreviewPublishCallback.load(std::memory_order_acquire))
		return;
	if (!padDrawableTexture || !renderTargetTexture || !m_currentCommandBuffer.m_commandBuffer)
		return;

	MetalDRCPreviewPublishCallback callback = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_drcPreviewPublicationMutex);
		const uint64 generation = ++m_drcPreviewPublicationGeneration;
		auto& publication = m_drcPreviewPublications[generation % DRC_PREVIEW_PUBLICATION_COUNT];
		const uint32 width = static_cast<uint32>(padDrawableTexture->width());
		const uint32 height = static_cast<uint32>(padDrawableTexture->height());

		if (!publication.texture || publication.width != width || publication.height != height ||
			publication.texture->pixelFormat() != padDrawableTexture->pixelFormat())
		{
			if (publication.texture)
			{
				MetalResourceCrashEvent destroyEvent{};
				destroyEvent.type = MetalResourceCrashEventType::TextureDestroy;
				destroyEvent.texturePtr = reinterpret_cast<uint64>(publication.texture);
				destroyEvent.ownerPtr = reinterpret_cast<uint64>(&publication);
				destroyEvent.commandBufferPtr = reinterpret_cast<uint64>(publication.commandBuffer);
				destroyEvent.commandBufferStatus = publication.commandBuffer
					? static_cast<uint32>(publication.commandBuffer->status()) : 0;
				destroyEvent.width = publication.width;
				destroyEvent.height = publication.height;
				destroyEvent.pixelFormat = static_cast<uint32>(publication.texture->pixelFormat());
				destroyEvent.flags = 4u;
				MetalResourceCrashDiagnostics_Record(destroyEvent);
				publication.texture->release();
			}
			NS_STACK_SCOPED auto descriptor = MTL::TextureDescriptor::alloc()->init();
			descriptor->setTextureType(MTL::TextureType2D);
			descriptor->setPixelFormat(padDrawableTexture->pixelFormat());
			descriptor->setWidth(width);
			descriptor->setHeight(height);
			descriptor->setMipmapLevelCount(1);
			descriptor->setStorageMode(MTL::StorageModePrivate);
			descriptor->setUsage(MTL::TextureUsageShaderRead);
			publication.texture = m_device->newTexture(descriptor);
			if (publication.texture)
			{
				MetalResourceCrashEvent createEvent{};
				createEvent.type = MetalResourceCrashEventType::TextureCreate;
				createEvent.texturePtr = reinterpret_cast<uint64>(publication.texture);
				createEvent.ownerPtr = reinterpret_cast<uint64>(&publication);
				createEvent.commandBufferPtr = reinterpret_cast<uint64>(m_currentCommandBuffer.m_commandBuffer);
				createEvent.commandBufferStatus = static_cast<uint32>(m_currentCommandBuffer.m_commandBuffer->status());
				createEvent.width = width;
				createEvent.height = height;
				createEvent.pixelFormat = static_cast<uint32>(publication.texture->pixelFormat());
				createEvent.flags = 4u;
				MetalResourceCrashDiagnostics_Record(createEvent);
			}
		}

		if (!publication.texture)
			return;

		auto blitEncoder = GetBlitCommandEncoder();
		blitEncoder->copyFromTexture(
			padDrawableTexture, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1),
			publication.texture, 0, 0, MTL::Origin(0, 0, 0));

		if (publication.commandBuffer)
			publication.commandBuffer->release();
		publication.commandBuffer = m_currentCommandBuffer.m_commandBuffer;
		publication.commandBuffer->retain();
		publication.generation = generation;
		publication.width = width;
		publication.height = height;

		if (m_lastDRCPreviewSourceTexture != renderTargetTexture)
		{
			m_lastDRCPreviewSourceTexture = renderTargetTexture;
			s_drcPreviewSourceChangeCount.fetch_add(1, std::memory_order_relaxed);
		}
		s_drcPreviewSourceTexturePtr.store((uint64)renderTargetTexture, std::memory_order_relaxed);
		s_drcPreviewPublishedTexturePtr.store((uint64)publication.texture, std::memory_order_relaxed);
		s_drcPreviewPublishCount.fetch_add(1, std::memory_order_relaxed);
		callback = s_drcPreviewPublishCallback.load(std::memory_order_acquire);
	}

	if (callback)
		callback();
}

MetalDRCPreviewFrame MetalRenderer::AcquireCompletedDRCPreviewFrame()
{
	MetalDRCPreviewFrame result{};
	std::lock_guard<std::mutex> lock(m_drcPreviewPublicationMutex);

	for (const auto& publication : m_drcPreviewPublications)
	{
		if (!publication.texture || !publication.commandBuffer ||
			publication.generation <= result.generation ||
			publication.commandBuffer->status() != MTL::CommandBufferStatusCompleted)
		{
			continue;
		}

		result.valid = true;
		result.texture = publication.texture;
		result.generation = publication.generation;
		result.width = publication.width;
		result.height = publication.height;
	}

	if (result.valid)
	{
		result.texture->retain();
		m_commandQueue->retain();
		result.commandQueue = m_commandQueue;
	}
	return result;
}

void MetalRenderer::ReleaseDRCPreviewFrames()
{
	std::lock_guard<std::mutex> lock(m_drcPreviewPublicationMutex);
	for (auto& publication : m_drcPreviewPublications)
	{
		if (publication.texture)
		{
			MetalResourceCrashEvent event{};
			event.type = MetalResourceCrashEventType::TextureDestroy;
			event.texturePtr = reinterpret_cast<uint64>(publication.texture);
			event.ownerPtr = reinterpret_cast<uint64>(&publication);
			event.commandBufferPtr = reinterpret_cast<uint64>(publication.commandBuffer);
			event.commandBufferStatus = publication.commandBuffer
				? static_cast<uint32>(publication.commandBuffer->status()) : 0;
			event.width = publication.width;
			event.height = publication.height;
			event.pixelFormat = static_cast<uint32>(publication.texture->pixelFormat());
			event.flags = 4u;
			MetalResourceCrashDiagnostics_Record(event);
			publication.texture->release();
		}
		if (publication.commandBuffer)
			publication.commandBuffer->release();
		publication = {};
	}
}

bool MetalRenderer::BeginFrame(bool mainWindow)
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	if (!mainWindow && !ShouldDrawPadDisplay())
		return false;
#endif
    const int fgTarget = mainWindow ? ActiveFrameGenerationTarget() : 0;
    if (mainWindow && fgTarget > 0)
    {
        ClearColorbuffer(!mainWindow, 0.0f, 0.0f, 0.0f, 1.0f);
        return true;
    }

    if (!AcquireDrawable(mainWindow))
        return false;

    // Startup/empty and shader-cache frames can be presented without a game
    // copy. A fresh or recycled drawable has undefined contents until written.
    // Match the other backends and initialize it before scheduling presentation.
    ClearColorbuffer(!mainWindow, 0.0f, 0.0f, 0.0f, 1.0f);
    return true;
}

void MetalRenderer::Flush(bool waitIdle)
{
    if (m_recordedDrawcalls > 0 || waitIdle)
        CommitCommandBuffer();

    if (waitIdle && m_executingCommandBuffers.size() != 0)
        m_executingCommandBuffers.back()->waitUntilCompleted();
}

void MetalRenderer::NotifyLatteCommandProcessorIdle()
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // An async readback cannot be encoded into an open render pass. Start the
    // copy once the guest has left the texture alone. On the fast path the copy
    // stays on this command buffer and is submitted with the frame. Committing
    // here as well gives every gameplay frame two submits, and Metal blocks the
    // second one once the GPU is a frame behind.
    const bool eligible = LatteTextureReadback_HasEligibleTransfers();
    if (!RequiresAccurateBarriers())
    {
        if (!eligible)
            return;
        EndEncoding();
        LatteTextureReadback_Update(false);
        return;
    }
    const bool pendingCopy = IsCommandBufferActive() && m_currentCommandBuffer.m_readbackPending;
    if (!pendingCopy && !eligible)
        return;
    EndEncoding();
    LatteTextureReadback_Update(false);
    if (IsCommandBufferActive() && m_currentCommandBuffer.m_readbackPending)
    {
        CommitCommandBuffer();
        if (!IsCommandBufferActive())
            ++m_idleReadbackSubmissions;
    }
#endif
}

bool MetalRenderer::ImguiBegin(bool mainWindow)
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	if (!mainWindow && !ShouldDrawPadDisplay())
		return false;
#endif
    if (!Renderer::ImguiBegin(mainWindow))
		return false;

    const int fgTarget = mainWindow ? ActiveFrameGenerationTarget() : 0;
    MTL::Texture* targetTexture = nullptr;
    if (mainWindow && fgTarget > 0)
    {
        if (m_mainLayer.GetLayer())
        {
            uint32 presW = (uint32)m_mainLayer.GetLayer()->drawableSize().width;
            uint32 presH = (uint32)m_mainLayer.GetLayer()->drawableSize().height;
            MTL::PixelFormat presFmt = m_state.m_usesSRGB ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm;
            EnsureFrameGenTextures(presW, presH, presFmt);
            targetTexture = m_frameGenTarget;
        }
    }
    if (!targetTexture)
    {
        if (!AcquireDrawable(mainWindow))
            return false;
        targetTexture = GetLayer(mainWindow).GetDrawable()->texture();
    }
    if (!targetTexture)
        return false;

	EnsureImGuiBackend();

	// Check if the font texture needs to be built
	ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts->IsBuilt())
        ImGui_ImplMetal_CreateFontsTexture(m_device);

	bool& drawableContentsDefined = mainWindow ? m_mainDrawableContentsDefined : m_padDrawableContentsDefined;

	// Render pass descriptor
	NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(targetTexture);
	if (drawableContentsDefined)
	{
		colorAttachment->setLoadAction(MTL::LoadActionLoad);
	}
	else
	{
		colorAttachment->setLoadAction(MTL::LoadActionClear);
		colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
	}
    colorAttachment->setStoreAction(MTL::StoreActionStore);

    // New frame
	ImGui_ImplMetal_NewFrame(renderPassDescriptor);
	ImGui_UpdateWindowInformation(mainWindow);
	ImGui::NewFrame();

	if (m_encoderType != MetalEncoderType::Render)
	    GetTemporaryRenderCommandEncoder(renderPassDescriptor);
	drawableContentsDefined = true;

	return true;
}

void MetalRenderer::ImguiEnd()
{
    EnsureImGuiBackend();

    if (m_encoderType != MetalEncoderType::Render)
    {
        cemuLog_logOnce(LogType::Force, "no render command encoder, cannot draw ImGui");
        return;
    }

    ImGui::Render();
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), GetCurrentCommandBuffer(), (MTL::RenderCommandEncoder*)m_commandEncoder);
	//ImGui::EndFrame();

	EndEncoding();
    if (ActiveFrameGenerationTarget() > 0 && m_frameGenTarget)
    {
        m_frameGenRendered = true;
    }
}

ImTextureID MetalRenderer::GenerateTexture(const std::vector<uint8>& data, const Vector2i& size)
{
    try
	{
		std::vector <uint8> tmp(size.x * size.y * 4);
		for (size_t i = 0; i < data.size() / 3; ++i)
		{
			tmp[(i * 4) + 0] = data[(i * 3) + 0];
			tmp[(i * 4) + 1] = data[(i * 3) + 1];
			tmp[(i * 4) + 2] = data[(i * 3) + 2];
			tmp[(i * 4) + 3] = 0xFF;
		}

		NS_STACK_SCOPED MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
		desc->setTextureType(MTL::TextureType2D);
		desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
		desc->setWidth(size.x);
		desc->setHeight(size.y);
		desc->setStorageMode(m_isAppleGPU ? MTL::StorageModeShared : MTL::StorageModeManaged);
		desc->setUsage(MTL::TextureUsageShaderRead);

		MTL::Texture* texture = m_device->newTexture(desc);

		// TODO: do a GPU copy?
		texture->replaceRegion(MTL::Region(0, 0, size.x, size.y), 0, 0, tmp.data(), size.x * 4, 0);

		return (ImTextureID)texture;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't generate imgui texture: {}", ex.what());
		return nullptr;
	}
}

void MetalRenderer::DeleteTexture(ImTextureID id)
{
    EnsureImGuiBackend();

    ((MTL::Texture*)id)->release();
}

void MetalRenderer::DeleteFontTextures()
{
    EnsureImGuiBackend();

    ImGui_ImplMetal_DestroyFontsTexture();
}

void MetalRenderer::AppendOverlayDebugInfo()
{
    ImGui::Text("--- GPU info ---");
    ImGui::Text("GPU                        %s", m_device->name()->utf8String());
    ImGui::Text("Is Apple GPU               %s", (m_isAppleGPU ? "yes" : "no"));
    ImGui::Text("Supports framebuffer fetch %s", (m_supportsFramebufferFetch ? "yes" : "no"));
    ImGui::Text("Has unified memory         %s", (m_hasUnifiedMemory ? "yes" : "no"));
    ImGui::Text("Supports Metal3            %s", (m_supportsMetal3 ? "yes" : "no"));

    ImGui::Text("--- Metal info ---");
    ImGui::Text("Render pipeline states     %zu", m_pipelineCache->GetPipelineCacheSize());

    ImGui::Text("--- Metal info (per frame) ---");
    ImGui::Text("Command buffers            %u", m_performanceMonitor.m_commandBuffers);
    ImGui::Text("Render passes              %u", m_performanceMonitor.m_renderPasses);
    ImGui::Text("Clears                     %u", m_performanceMonitor.m_clears);
    ImGui::Text("Manual vertex fetch draws  %u (mesh draws: %u)", m_performanceMonitor.m_manualVertexFetchDraws, m_performanceMonitor.m_meshDraws);
    ImGui::Text("Triangle fans              %u", m_performanceMonitor.m_triangleFans);

    ImGui::Text("--- Cache debug info ---");

	uint32 bufferCacheHeapSize = 0;
	uint32 bufferCacheAllocationSize = 0;
	uint32 bufferCacheNumAllocations = 0;

	LatteBufferCache_getStats(bufferCacheHeapSize, bufferCacheAllocationSize, bufferCacheNumAllocations);

	ImGui::Text("Buffer");
	ImGui::SameLine(60.0f);
	ImGui::Text("%06uKB / %06uKB Allocs: %u", (uint32)(bufferCacheAllocationSize + 1023) / 1024, ((uint32)bufferCacheHeapSize + 1023) / 1024, (uint32)bufferCacheNumAllocations);

	uint32 numBuffers;
	size_t totalSize, freeSize;

	m_memoryManager->GetStagingAllocator().GetStats(numBuffers, totalSize, freeSize);
	ImGui::Text("Staging");
	ImGui::SameLine(60.0f);
	ImGui::Text("%06uKB / %06uKB Buffers: %u", ((uint32)(totalSize - freeSize) + 1023) / 1024, ((uint32)totalSize + 1023) / 1024, (uint32)numBuffers);

	m_memoryManager->GetIndexAllocator().GetStats(numBuffers, totalSize, freeSize);
	ImGui::Text("Index");
	ImGui::SameLine(60.0f);
	ImGui::Text("%06uKB / %06uKB Buffers: %u", ((uint32)(totalSize - freeSize) + 1023) / 1024, ((uint32)totalSize + 1023) / 1024, (uint32)numBuffers);
}

void MetalRenderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ)
{
    // halfZ is handled in the shader

    m_state.m_viewport = MTL::Viewport{x, y, width, height, nearZ, farZ};
}

void MetalRenderer::renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight)
{
    m_state.m_scissor = MTL::ScissorRect{(uint32)scissorX, (uint32)scissorY, (uint32)scissorWidth, (uint32)scissorHeight};
}

LatteCachedFBO* MetalRenderer::rendertarget_createCachedFBO(uint64 key)
{
	return new CachedFBOMtl(this, key);
}

void MetalRenderer::rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo)
{
	if (cfbo == (LatteCachedFBO*)m_state.m_lastUsedFBO.m_fbo)
	{
		// A live encoder and the next FBO comparison must not retain a
		// framebuffer whose views are about to be destroyed.
		if (m_encoderType == MetalEncoderType::Render)
			EndEncoding();
		m_state.m_lastUsedFBO = {nullptr};
	}
	if (cfbo == (LatteCachedFBO*)m_state.m_activeFBO.m_fbo)
	{
		m_state.m_activeFBO = {nullptr, {}, false};
		m_state.m_fboChanged = true;
	}
}

void MetalRenderer::rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo)
{
	if (m_state.m_activeFBO.m_fbo == cfbo)
		return;
	bool hasReadback = false;
	if (cfbo)
	{
		for (const auto* texture : cfbo->GetTextures())
			hasReadback |= texture->enableReadback;
	}
	m_state.m_activeFBO = {(CachedFBOMtl*)cfbo, MetalAttachmentsInfo((CachedFBOMtl*)cfbo), hasReadback};
	m_state.m_fboChanged = true;
}

void* MetalRenderer::texture_acquireTextureUploadBuffer(uint32 size)
{
    return m_memoryManager->AcquireTextureUploadBuffer(size);
}

void* MetalRenderer::texture_acquireTextureUploadBufferForTexture(uint32 size, LatteTexture* texture)
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // Apple7/8 cannot sample BC textures. Only those compressed inputs still
    // need separate CPU storage for expansion; ordinary decoder output can use
    // the same lifetime-protected direct path as a native-BC device.
    const bool matchesUpload = !Latte::IsCompressedFormat(texture->format);
    return m_memoryManager->AcquireTextureUploadBuffer(size, matchesUpload);
#else
    return texture_acquireTextureUploadBuffer(size);
#endif
}

void MetalRenderer::texture_releaseTextureUploadBuffer(uint8* mem)
{
    m_memoryManager->ReleaseTextureUploadBuffer(mem);
}

TextureDecoder* MetalRenderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height)
{
    return GetMtlPixelFormatInfo(format, isDepth).textureDecoder;
}

bool MetalRenderer::PendingClearOverlaps(const PendingTextureClear& entry, LatteTexture* texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount)
{
	if (!texture || entry.hostTexture != texture || mipCount == 0 || sliceCount == 0)
		return false;
	if (entry.mip < firstMip || entry.mip >= firstMip + mipCount)
		return false;
	if (entry.slice < firstSlice || entry.slice >= firstSlice + sliceCount)
		return false;
	return true;
}

static bool AttachmentRendersSlice(const LatteTextureView* view, uint32 slice, uint32 mip)
{
	if (!view || !view->baseTexture || mip != (uint32)view->firstMip)
		return false;
	// The cached pass renders level 0 / slice 0 of the view. That view starts at firstMip/firstSlice,
	// except 3D views, whose descriptor slice stays 0 and therefore only draws depth plane 0.
	if (view->baseTexture->Is3DTexture())
		return slice == 0;
	return slice == (uint32)view->firstSlice;
}

bool MetalRenderer::ClearTargetsAttachment(const PendingTextureClear& entry, const LatteTextureView* view, bool asDepth)
{
	if (!view || entry.isDepth != asDepth || entry.hostTexture != view->baseTexture)
		return false;
	return AttachmentRendersSlice(view, entry.slice, entry.mip);
}

static bool ClearViewFormatMatches(const LatteTextureView* view)
{
	if (!view)
		return false;
	auto* viewMtl = static_cast<const LatteTextureViewMtl*>(view);
	auto* base = static_cast<const LatteTextureMtl*>(view->baseTexture);
	MTL::Texture* rgba = viewMtl->GetRGBAView();
	MTL::Texture* parent = base ? base->GetTexture() : nullptr;
	if (!rgba || !parent)
		return false;
	if (rgba != parent && rgba->parentTexture() != parent)
		return false;
	return rgba->pixelFormat() == parent->pixelFormat();
}

void MetalRenderer::QueueOrReplaceColorClear(LatteTexture* texture, uint32 slice, uint32 mip, float r, float g, float b, float a)
{
	for (auto& entry : m_pendingClears)
	{
		if (entry.hostTexture != texture || entry.slice != slice || entry.mip != mip)
			continue;
		entry = {};
		entry.hostTexture = texture;
		entry.slice = slice;
		entry.mip = mip;
		entry.isDepth = false;
		entry.r = r;
		entry.g = g;
		entry.b = b;
		entry.a = a;
		return;
	}
	PendingTextureClear entry{};
	entry.hostTexture = texture;
	entry.slice = slice;
	entry.mip = mip;
	entry.r = r;
	entry.g = g;
	entry.b = b;
	entry.a = a;
	m_pendingClears.push_back(entry);
}

void MetalRenderer::QueueOrReplaceDepthClear(LatteTexture* texture, uint32 slice, uint32 mip, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
	for (auto& entry : m_pendingClears)
	{
		if (entry.hostTexture != texture || entry.slice != slice || entry.mip != mip)
			continue;
		if (!entry.isDepth)
		{
			entry = {};
			entry.hostTexture = texture;
			entry.slice = slice;
			entry.mip = mip;
			entry.isDepth = true;
		}
		if (clearDepth)
		{
			entry.clearDepth = true;
			entry.depth = depthValue;
		}
		if (clearStencil)
		{
			entry.clearStencil = true;
			entry.stencil = stencilValue;
		}
		return;
	}
	PendingTextureClear entry{};
	entry.hostTexture = texture;
	entry.slice = slice;
	entry.mip = mip;
	entry.isDepth = true;
	entry.clearDepth = clearDepth;
	entry.clearStencil = clearStencil;
	entry.depth = depthValue;
	entry.stencil = stencilValue;
	m_pendingClears.push_back(entry);
}

void MetalRenderer::DropPendingTextureClears(LatteTexture* texture)
{
    if (texture)
        std::erase_if(m_pendingClears, [texture](const auto& entry) { return entry.hostTexture == texture; });
}

void MetalRenderer::DropPendingClearsOverlapping(LatteTexture* texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount)
{
    if (texture)
        std::erase_if(m_pendingClears, [&](const auto& entry) {
            return PendingClearOverlaps(entry, texture, firstMip, mipCount, firstSlice, sliceCount);
        });
}

void MetalRenderer::ExecutePendingClears(const std::vector<PendingTextureClear>& clears)
{
	if (clears.empty())
		return;
	g_pass_clearSlice += static_cast<uint32>(clears.size());
	const bool wasFlushing = m_flushingPendingClears;
	m_flushingPendingClears = true;
	for (const auto& entry : clears)
	{
		if (!entry.hostTexture)
			continue;
		if (!entry.isDepth)
			texture_clearColorSlice(entry.hostTexture, (sint32)entry.slice, (sint32)entry.mip, entry.r, entry.g, entry.b, entry.a);
		else if (entry.clearDepth || entry.clearStencil)
			texture_clearDepthSlice(entry.hostTexture, entry.slice, (sint32)entry.mip, entry.clearDepth, entry.clearStencil, entry.depth, entry.stencil);
	}
	m_flushingPendingClears = wasFlushing;
}

void MetalRenderer::FlushPendingTextureClears(LatteTexture* texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount)
{
	if (!texture || m_pendingClears.empty() || m_flushingPendingClears)
		return;
	std::vector<PendingTextureClear> now;
	size_t kept = 0;
	for (const auto& entry : m_pendingClears)
	{
		if (PendingClearOverlaps(entry, texture, firstMip, mipCount, firstSlice, sliceCount))
			now.push_back(entry);
		else
			m_pendingClears[kept++] = entry;
	}
	if (now.empty())
		return;
	m_pendingClears.resize(kept);
	ExecutePendingClears(now);
}

void MetalRenderer::FlushAllPendingClears()
{
	if (m_pendingClears.empty() || m_flushingPendingClears)
		return;
	std::vector<PendingTextureClear> clears;
	clears.swap(m_pendingClears);
	ExecutePendingClears(clears);
}

void MetalRenderer::FlushPendingClearsSampledByDraw(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader)
{
	if (m_pendingClears.empty())
		return;

	LatteTextureView* views[96];
	uint32 viewCount = 0;
	auto addShader = [&](LatteDecompilerShader* shader)
	{
		if (!shader)
			return;
		const sint32 textureCount = shader->resourceMapping.getTextureCount();
		for (sint32 i = 0; i < textureCount; ++i)
		{
			const auto relative = shader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
			if (m_supportsFramebufferFetch && shader->textureRenderTargetIndex[relative] != 255)
				continue;
			uint32 hostTextureUnit = relative;
			switch (shader->shaderType)
			{
			case LatteConst::ShaderType::Vertex:
				hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
				break;
			case LatteConst::ShaderType::Pixel:
				hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
				break;
			case LatteConst::ShaderType::Geometry:
				hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
				break;
			default:
				continue;
			}
			LatteTextureViewMtl* view = m_state.m_textures[hostTextureUnit];
			if (!view || viewCount >= std::size(views))
				continue;
			views[viewCount++] = view;
		}
	};
	addShader(vertexShader);
	addShader(geometryShader);
	addShader(pixelShader);
	if (viewCount == 0)
		return;

	std::vector<PendingTextureClear> now;
	size_t kept = 0;
	for (const auto& entry : m_pendingClears)
	{
		bool sampled = false;
		for (uint32 i = 0; i < viewCount; ++i)
		{
			LatteTextureView* view = views[i];
			if (!view || view->firstMip < 0 || view->firstSlice < 0)
			{
				if (view && entry.hostTexture == view->baseTexture)
					sampled = true;
				continue;
			}
			if (PendingClearOverlaps(entry, view->baseTexture, (uint32)view->firstMip, (uint32)std::max(view->numMip, 1), (uint32)view->firstSlice, (uint32)std::max(view->numSlice, 1)))
			{
				sampled = true;
				break;
			}
		}
		if (sampled)
			now.push_back(entry);
		else
			m_pendingClears[kept++] = entry;
	}
	if (now.empty())
		return;
	m_pendingClears.resize(kept);
	g_pass_clearSampled += static_cast<uint32>(now.size());
	ExecutePendingClears(now);
}

bool MetalRenderer::PendingClearAffectsFBO(const CachedFBOMtl* fbo) const
{
	if (!fbo)
		return false;
	for (const auto& entry : m_pendingClears)
	{
		if (!entry.isDepth)
		{
			for (int i = 0; i < 8; ++i)
			{
				if (ClearTargetsAttachment(entry, fbo->colorBuffer[i].texture, false))
					return true;
			}
			continue;
		}
		LatteTextureView* view = fbo->depthBuffer.texture;
		if (!ClearTargetsAttachment(entry, view, true))
			continue;
		if (entry.clearDepth)
			return true;
		if (entry.clearStencil && fbo->depthBuffer.hasStencil && GetMtlPixelFormatInfo(view->format, true).hasStencil)
			return true;
	}
	return false;
}

static bool DepthStencilAttachmentPresent(const LatteCachedFBO* fbo, const LatteTextureView* view)
{
	return fbo && view && fbo->depthBuffer.hasStencil && GetMtlPixelFormatInfo(view->format, true).hasStencil;
}

bool MetalRenderer::ClearMustRunBeforePass(const PendingTextureClear& entry, const CachedFBOMtl* fbo)
{
	if (!fbo)
		return false;
	if (!entry.isDepth)
	{
		for (int i = 0; i < 8; ++i)
		{
			LatteTextureView* view = fbo->colorBuffer[i].texture;
			if (ClearTargetsAttachment(entry, view, false) && !ClearViewFormatMatches(view))
				return true;
		}
		return false;
	}
	LatteTextureView* view = fbo->depthBuffer.texture;
	if (!ClearTargetsAttachment(entry, view, true))
		return false;
	const bool loadsDepth = entry.clearDepth;
	const bool loadsStencil = entry.clearStencil && DepthStencilAttachmentPresent(fbo, view);
	if (!loadsDepth && !loadsStencil)
		return false;
	return !ClearViewFormatMatches(view);
}

bool MetalRenderer::FoldPendingClear(PendingTextureClear& entry, CachedFBOMtl* fbo, MTL::RenderPassDescriptor* descriptor, uint8& colorMask, bool& clearedDepth, bool& clearedStencil)
{
	if (!fbo || !descriptor)
		return false;
	if (!entry.isDepth)
	{
		bool hit = false;
		for (int i = 0; i < 8; ++i)
		{
			LatteTextureView* view = fbo->colorBuffer[i].texture;
			if (!ClearTargetsAttachment(entry, view, false) || !ClearViewFormatMatches(view))
				continue;
			auto attachment = descriptor->colorAttachments()->object(i);
			attachment->setClearColor(MTL::ClearColor(entry.r, entry.g, entry.b, entry.a));
			attachment->setLoadAction(MTL::LoadActionClear);
			colorMask = (uint8)(colorMask | (1u << i));
			hit = true;
		}
		return hit;
	}

	LatteTextureView* view = fbo->depthBuffer.texture;
	if (!ClearTargetsAttachment(entry, view, true) || !ClearViewFormatMatches(view))
		return false;
	if (entry.clearDepth)
	{
		auto attachment = descriptor->depthAttachment();
		attachment->setClearDepth(entry.depth);
		attachment->setLoadAction(MTL::LoadActionClear);
		clearedDepth = true;
		entry.clearDepth = false;
	}
	if (entry.clearStencil && DepthStencilAttachmentPresent(fbo, view))
	{
		auto attachment = descriptor->stencilAttachment();
		attachment->setClearStencil(entry.stencil);
		attachment->setLoadAction(MTL::LoadActionClear);
		clearedStencil = true;
		entry.clearStencil = false;
	}
	return !entry.clearDepth && !entry.clearStencil;
}

void MetalRenderer::ResolvePendingClearsForRenderPass()
{
	m_loadClearedColorMask = 0;
	m_loadClearedDepth = false;
	m_loadClearedStencil = false;
	CachedFBOMtl* fbo = m_state.m_activeFBO.m_fbo;
	if (!fbo || m_pendingClears.empty() || m_flushingPendingClears)
		return;

	std::vector<PendingTextureClear> executeNow;
	size_t kept = 0;
	for (const auto& entry : m_pendingClears)
	{
		if (ClearMustRunBeforePass(entry, fbo))
			executeNow.push_back(entry);
		else
			m_pendingClears[kept++] = entry;
	}
	m_pendingClears.resize(kept);
	ExecutePendingClears(executeNow);

	MTL::RenderPassDescriptor* descriptor = fbo->GetRenderPassDescriptor();
	kept = 0;
	for (auto& entry : m_pendingClears)
	{
		if (!FoldPendingClear(entry, fbo, descriptor, m_loadClearedColorMask, m_loadClearedDepth, m_loadClearedStencil))
			m_pendingClears[kept++] = entry;
	}
	m_pendingClears.resize(kept);
}

void MetalRenderer::RestoreFoldedLoadActions(MTL::RenderPassDescriptor* descriptor)
{
	if (!descriptor)
		return;
	for (int i = 0; i < 8; ++i)
	{
		if (m_loadClearedColorMask & (1u << i))
			descriptor->colorAttachments()->object(i)->setLoadAction(MTL::LoadActionLoad);
	}
	if (m_loadClearedDepth)
		descriptor->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
	if (m_loadClearedStencil)
		descriptor->stencilAttachment()->setLoadAction(MTL::LoadActionLoad);
}

void MetalRenderer::texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex)
{
    LatteGPUState.telemetry.clears.fetch_add(1, std::memory_order_relaxed);
    if (hostTexture->isDepth)
    {
        texture_clearDepthSlice(hostTexture, sliceIndex, mipIndex, true, hostTexture->hasStencil, 0.0f, 0);
    }
    else
    {
        texture_clearColorSlice(hostTexture, sliceIndex, mipIndex, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

MTL::BlitOption GetBlitOptionForTexture(const LatteTextureMtl* texture) {
    switch (texture->GetTexture()->pixelFormat()) {
        case MTL::PixelFormatDepth16Unorm:
        case MTL::PixelFormatDepth32Float:
            return MTL::BlitOptionDepthFromDepthStencil;

        case MTL::PixelFormatStencil8:
            return MTL::BlitOptionStencilFromDepthStencil;

        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatDepth32Float_Stencil8:
            // Depth and stencil cannot be copied in one call; the caller picks one.
            // Default to depth
            return MTL::BlitOptionDepthFromDepthStencil;

        default:
            return MTL::BlitOptionNone;
    }
}

bool IsDepthStencilFormat(MTL::PixelFormat format) {
    return format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
           format == MTL::PixelFormatDepth32Float_Stencil8;
}

// TODO: do a cpu copy on Apple Silicon?
void MetalRenderer::texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    RuntimePerformance::OptionalOperationTimer uploadTimer(
        CemuBridge_IsDiagnosticLoggingEnabled(), RuntimePerformance::textureUploadNs);
#endif
    auto textureMtl = (LatteTextureMtl*)hostTexture;

    const bool decodeBC = !m_pixelFormatSupport.m_supportsBCTextureCompression &&
                          Latte::IsCompressedFormat(textureMtl->format);
    BCFormat bcFormat = BCFormat::BC1;
    if (decodeBC)
    {
        switch (textureMtl->format)
        {
        case Latte::E_GX2SURFFMT::BC1_UNORM:
        case Latte::E_GX2SURFFMT::BC1_SRGB:
            // Follow the storage format chosen for this texture (16-bit on small devices).
            bcFormat = textureMtl->GetTexture()->pixelFormat() == MTL::PixelFormatBGR5A1Unorm
                ? BCFormat::BC1Packed16 : BCFormat::BC1;
            break;
        case Latte::E_GX2SURFFMT::BC2_UNORM:
        case Latte::E_GX2SURFFMT::BC2_SRGB: bcFormat = BCFormat::BC2; break;
        case Latte::E_GX2SURFFMT::BC3_UNORM:
        case Latte::E_GX2SURFFMT::BC3_SRGB: bcFormat = BCFormat::BC3; break;
        case Latte::E_GX2SURFFMT::BC4_UNORM: bcFormat = BCFormat::BC4Unorm; break;
        case Latte::E_GX2SURFFMT::BC4_SNORM: bcFormat = BCFormat::BC4Snorm; break;
        case Latte::E_GX2SURFFMT::BC5_UNORM: bcFormat = BCFormat::BC5Unorm; break;
        case Latte::E_GX2SURFFMT::BC5_SNORM: bcFormat = BCFormat::BC5Snorm; break;
        default:
            cemuLog_logOnce(LogType::Force, "Metal BC fallback does not support guest format {}", (uint32)textureMtl->format);
            return;
        }
    }

    uint32 offsetZ = 0;
    if (textureMtl->Is3DTexture())
    {
        offsetZ = sliceIndex;
        sliceIndex = 0;
    }

    MTL::Texture* destinationTexture = textureMtl->GetTexture();
    if (!destinationTexture || mipIndex < 0 || mipIndex >= (sint32)destinationTexture->mipmapLevelCount())
    {
        cemuLog_logOnce(LogType::Force, "Metal texture upload skipped mip {} outside the host texture", mipIndex);
        return;
    }
    size_t bytesPerRow = GetMtlTextureBytesPerRow(textureMtl->format, textureMtl->isDepth, width);
    // 1D/3D BC textures cannot be ETC2/EAC; they store the CPU-decoded texels (MetalTextureShape.h).
    const bool transcodeToETC = decodeBC && m_bcTranscodePipeline && IsETCPixelFormat(destinationTexture->pixelFormat());
    if (decodeBC && IsETCPixelFormat(GetMtlPixelFormat(textureMtl->format, false)) && !transcodeToETC)
    {
        uint32 bytesPerPixel;
        GetETCDecodedPixelFormat(GetMtlPixelFormat(textureMtl->format, false), bytesPerPixel);
        bytesPerRow = size_t(width) * bytesPerPixel;
    }
    // No need to set bytesPerImage for 3D textures, since we always load just one slice
    //size_t bytesPerImage = GetMtlTextureBytesPerImage(textureMtl->GetFormat(), textureMtl->isDepth, height, bytesPerRow);
    //if (m_isAppleGPU)
    //{
    //    textureMtl->GetTexture()->replaceRegion(MTL::Region(0, 0, offsetZ, width, height, 1), mipIndex, sliceIndex, pixelData, bytesPerRow, 0);
    //}
    //else
    //{
    // ETC2/EAC destination: upload the BC blocks as they are and let the GPU
    // transcode them, so neither staging nor the texture is ever expanded.
    if (transcodeToETC)
    {
        const uint32 blocksWide = (uint32(width) + 3) / 4, blocksHigh = (uint32(height) + 3) / 4;
        const bool singleChannelBlocks = bcFormat == BCFormat::BC1 || bcFormat == BCFormat::BC4Unorm || bcFormat == BCFormat::BC4Snorm;
        const size_t sourceSize = size_t(blocksWide) * blocksHigh * (singleChannelBlocks ? 8 : 16);
        const size_t destinationSize = bytesPerRow * blocksHigh;
        if (compressedImageSize < sourceSize)
        {
            cemuLog_logOnce(LogType::Force, "Metal BC transcode skipped short block data ({}x{}, format {})",
                            width, height, (uint32)textureMtl->format);
            return;
        }
        uint32 transcodeFormat = 0;
        switch (bcFormat)
        {
        case BCFormat::BC1: transcodeFormat = 0; break;
        case BCFormat::BC2: transcodeFormat = 1; break;
        case BCFormat::BC3: transcodeFormat = 2; break;
        case BCFormat::BC4Unorm: transcodeFormat = 3; break;
        case BCFormat::BC4Snorm: transcodeFormat = 4; break;
        case BCFormat::BC5Unorm: transcodeFormat = 5; break;
        case BCFormat::BC5Snorm: transcodeFormat = 6; break;
        default: return;
        }
        auto* transcodePipeline = GetBCTranscodePipeline(transcodeFormat);
        // Same pending-clear handling as the regular upload below. Done before
        // reserving staging: a flush may submit, and a reservation is only
        // protected by the command buffer current when it was made.
        if (0 <= mipIndex && mipIndex < hostTexture->mipLevels && (textureMtl->Is3DTexture() || 0 <= sliceIndex))
        {
            const uint32 clearSlice = textureMtl->Is3DTexture() ? offsetZ : (uint32)sliceIndex;
            if (width >= hostTexture->GetMipWidth(mipIndex) && height >= hostTexture->GetMipHeight(mipIndex))
                DropPendingClearsOverlapping(hostTexture, (uint32)mipIndex, 1, clearSlice, 1);
            else
                FlushPendingTextureClears(hostTexture, (uint32)mipIndex, 1, clearSlice, 1);
        }

        auto& staging = m_memoryManager->GetStagingAllocator();
        auto source = staging.AllocateBufferMemory((uint32)sourceSize, 16);
        memcpy(source.memPtr, pixelData, sourceSize);
        staging.FlushReservation(source);
        // GPU-written only; reclaimed with the command buffer like other staging.
        auto destination = staging.AllocateBufferMemory((uint32)destinationSize, 16);

        struct
        {
            uint32 blocksWide, blocksHigh, format, width, height, reserved[3];
        } params{blocksWide, blocksHigh, transcodeFormat, uint32(width), uint32(height), {}};
        auto computeEncoder = GetComputeCommandEncoder();
        computeEncoder->setComputePipelineState(transcodePipeline);
        computeEncoder->setBuffer(source.mtlBuffer, source.bufferOffset, 0);
        computeEncoder->setBuffer(destination.mtlBuffer, destination.bufferOffset, 1);
        computeEncoder->setBytes(&params, sizeof(params), 2);
        computeEncoder->dispatchThreads(MTL::Size(blocksWide, blocksHigh, 1), MTL::Size(8, 8, 1));

        ++g_blit_textureUploads;
        // Staging buffers are hazard-tracked: the blit waits for the transcode.
        GetBlitCommandEncoder()->copyFromBuffer(destination.mtlBuffer, destination.bufferOffset, bytesPerRow, 0,
            MTL::Size(width, height, 1), textureMtl->GetTexture(), sliceIndex, mipIndex, MTL::Origin(0, 0, offsetZ));
        FinishTextureUpload(sourceSize + destinationSize);
        return;
    }

    // Allocate a temporary buffer
    auto& bufferAllocator = m_memoryManager->GetStagingAllocator();
    const size_t uploadSize = decodeBC ? bytesPerRow * size_t(height) : compressedImageSize;
    MetalSynchronizedRingAllocator::AllocatorReservation_t allocation{};
    const bool decodedInStaging = !decodeBC &&
        m_memoryManager->ResolveTextureUploadBuffer(pixelData, uploadSize, allocation);
    if (!decodedInStaging)
    {
        allocation = bufferAllocator.AllocateBufferMemory(uploadSize, 1);
        if (decodeBC)
        {
            if (!DecodeBCImage(bcFormat, static_cast<const uint8_t*>(pixelData), compressedImageSize,
                               (uint32)width, (uint32)height, allocation.memPtr, uploadSize))
            {
                cemuLog_logOnce(LogType::Force, "Metal BC texture upload has invalid block data ({}x{}, format {})",
                                width, height, (uint32)textureMtl->format);
                memset(allocation.memPtr, 0, uploadSize);
            }
        }
        else
            memcpy(allocation.memPtr, pixelData, compressedImageSize);
    }
    bufferAllocator.FlushReservation(allocation);

    if (mipIndex >= 0 && mipIndex < hostTexture->mipLevels && (textureMtl->Is3DTexture() || sliceIndex >= 0))
    {
        const uint32 clearSlice = textureMtl->Is3DTexture() ? offsetZ : (uint32)sliceIndex;
        const bool replacesWholeSlice = width >= hostTexture->GetMipWidth(mipIndex) && height >= hostTexture->GetMipHeight(mipIndex);
        if (replacesWholeSlice)
            DropPendingClearsOverlapping(hostTexture, (uint32)mipIndex, 1, clearSlice, 1);
        else
            FlushPendingTextureClears(hostTexture, (uint32)mipIndex, 1, clearSlice, 1);
    }

    ++g_blit_textureUploads;
    auto blitCommandEncoder = GetBlitCommandEncoder();

    // Copy the data from the temporary buffer to the texture
	if (IsDepthStencilFormat(textureMtl->GetTexture()->pixelFormat())) {
        // Metal doesn't allow copying depth and stencil data at the same time, so we need to do two copies for combined depth/stencil formats
        blitCommandEncoder->copyFromBuffer(allocation.mtlBuffer, allocation.bufferOffset, bytesPerRow, 0, MTL::Size(width, height, 1), textureMtl->GetTexture(), sliceIndex, mipIndex, MTL::Origin(0, 0, offsetZ), MTL::BlitOptionDepthFromDepthStencil);
        blitCommandEncoder->copyFromBuffer(allocation.mtlBuffer, allocation.bufferOffset, bytesPerRow, 0, MTL::Size(width, height, 1), textureMtl->GetTexture(), sliceIndex, mipIndex, MTL::Origin(0, 0, offsetZ), MTL::BlitOptionStencilFromDepthStencil);
    } else {
        blitCommandEncoder->copyFromBuffer(allocation.mtlBuffer, allocation.bufferOffset, bytesPerRow, 0, MTL::Size(width, height, 1), textureMtl->GetTexture(), sliceIndex, mipIndex, MTL::Origin(0, 0, offsetZ), GetBlitOptionForTexture(textureMtl));
    }
    FinishTextureUpload(uploadSize);
    //}
}

void MetalRenderer::FinishTextureUpload(size_t uploadedBytes)
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // This budget addresses expanded BC uploads on GPUs without BC support.
    // Native BC devices retain their existing submission and frame pacing
    // unless iOS headroom runs low (checked once per batch, below).
    const bool nativeBC = m_pixelFormatSupport.m_supportsBCTextureCompression;

    // Loading a course may upload hundreds of expanded BC textures before
    // the first present. Draw-count submission alone cannot bound that burst.
    // Submit only after the complete copy, so no caller holds an ended encoder.
    // Expanded BC images use much more upload storage than their guest data.
    // Keep fewer upload-only batches outstanding on 4/6 GiB devices. This
    // never changes texture data, frame presentation, or native-BC submission.
    const bool constrained = GetMetalMemoryPolicy().constrained;
    const size_t kUploadBatchBytes = (constrained ? 8u : 32u) * 1024 * 1024;
    const size_t kMaxUploadCommandBuffers = constrained ? 2 : 4;
    m_currentCommandBuffer.m_textureUploadBytes += uploadedBytes;
    if (m_currentCommandBuffer.m_textureUploadBytes < kUploadBatchBytes)
        return;
    // iPhone 18 Pro without the increased-memory entitlement (3.3 GiB limit) was
    // killed by jetsam loading a BotW scene: 7857 uploads kept in one command
    // buffer, so no staging could be recycled before the frame ended.
    constexpr size_t kNativeBCLowHeadroom = 1024u * 1024 * 1024;
    if (nativeBC)
    {
        const size_t available = os_proc_available_memory();
        if (!available || available >= kNativeBCLowHeadroom)
        {
            m_currentCommandBuffer.m_textureUploadBytes = 0;
            return;
        }
    }

    CommitCommandBuffer();
    // Never reclaim staging until the GPU has consumed it. Backpressure is
    // confined to large upload bursts; ordinary frames do not enter this path.
    if (m_executingCommandBuffers.size() >= kMaxUploadCommandBuffers)
    {
        auto* oldest = m_executingCommandBuffers.front();
        if (!CommandBufferCompleted(oldest))
            oldest->waitUntilCompleted();
        ProcessFinishedCommandBuffers();
    }
#endif
}

void MetalRenderer::texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
    if (!FormatIsRenderable(hostTexture->format))
    {
        cemuLog_logOnce(LogType::Force, "cannot clear color texture with format {}, because it's not renderable", hostTexture->format);
        return;
    }

    if (!m_flushingPendingClears && sliceIndex >= 0 && mipIndex >= 0)
    {
        QueueOrReplaceColorClear(hostTexture, (uint32)sliceIndex, (uint32)mipIndex, r, g, b, a);
        return;
    }

    auto mtlTexture = static_cast<LatteTextureMtl*>(hostTexture)->GetTexture();

    ClearColorTextureInternal(mtlTexture, sliceIndex, mipIndex, r, g, b, a);
}

void MetalRenderer::texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
    clearStencil = (clearStencil && GetMtlPixelFormatInfo(hostTexture->format, true).hasStencil);
    if (!clearDepth && !clearStencil)
    {
        cemuLog_logOnce(LogType::Force, "skipping depth/stencil clear");
        return;
    }

    if (!m_flushingPendingClears && mipIndex >= 0)
    {
        QueueOrReplaceDepthClear(hostTexture, sliceIndex, (uint32)mipIndex, clearDepth, clearStencil, depthValue, stencilValue);
        return;
    }

    auto mtlTexture = static_cast<LatteTextureMtl*>(hostTexture)->GetTexture();

    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    if (clearDepth)
    {
        auto depthAttachment = renderPassDescriptor->depthAttachment();
        depthAttachment->setTexture(mtlTexture);
        depthAttachment->setClearDepth(depthValue);
        depthAttachment->setLoadAction(MTL::LoadActionClear);
        depthAttachment->setStoreAction(MTL::StoreActionStore);
        depthAttachment->setSlice(sliceIndex);
        depthAttachment->setLevel(mipIndex);
    }
    if (clearStencil)
    {
        auto stencilAttachment = renderPassDescriptor->stencilAttachment();
        stencilAttachment->setTexture(mtlTexture);
        stencilAttachment->setClearStencil(stencilValue);
        stencilAttachment->setLoadAction(MTL::LoadActionClear);
        stencilAttachment->setStoreAction(MTL::StoreActionStore);
        stencilAttachment->setSlice(sliceIndex);
        stencilAttachment->setLevel(mipIndex);
    }

    GetTemporaryRenderCommandEncoder(renderPassDescriptor);
    EndEncoding();

    // Debug
    m_performanceMonitor.m_clears++;
}

LatteTexture* MetalRenderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
    return new LatteTextureMtl(this, dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
}

void MetalRenderer::texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit)
{
    m_state.m_textures[textureUnit] = static_cast<LatteTextureViewMtl*>(textureView);
}


void MetalRenderer::texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth_)
{
    // Source size seems to apply to the destination texture as well, therefore we need to adjust it when block size doesn't match
    auto mtlSrc = static_cast<LatteTextureMtl*>(src)->GetTexture();
    auto mtlDst = static_cast<LatteTextureMtl*>(dst)->GetTexture();
    if (!mtlSrc || !mtlDst || srcMip < 0 || dstMip < 0 || srcMip >= (sint32)mtlSrc->mipmapLevelCount() || dstMip >= (sint32)mtlDst->mipmapLevelCount())
        return;
    // A 1D/3D BC texture holds decoded texels instead of ETC2/EAC (MetalTextureShape.h):
    // block sizes follow the stored format, and decoded <-> ETC copies are not byte compatible.
    const auto storedBlockSize = [](LatteTexture* texture, MTL::Texture* mtlTexture) {
        Uvec2 size = GetMtlPixelFormatInfo(texture->format, texture->isDepth).blockTexelSize;
        if (!texture->isDepth && IsETCPixelFormat(GetMtlPixelFormat(texture->format, false)) && !IsETCPixelFormat(mtlTexture->pixelFormat()))
            size = {1, 1};
        return size;
    };
    if (src->IsCompressedFormat() && dst->IsCompressedFormat() && IsETCPixelFormat(mtlSrc->pixelFormat()) != IsETCPixelFormat(mtlDst->pixelFormat()))
    {
        cemuLog_logOnce(LogType::Force, "Metal texture copy skipped between ETC2/EAC and decoded BC storage");
        return;
    }
    Uvec2 srcBlockTexelSize = storedBlockSize(src, mtlSrc);
    Uvec2 dstBlockTexelSize = storedBlockSize(dst, mtlDst);
    if (srcBlockTexelSize.x != dstBlockTexelSize.x || srcBlockTexelSize.y != dstBlockTexelSize.y)
    {
        uint32 multX = (srcBlockTexelSize.x > dstBlockTexelSize.x ? srcBlockTexelSize.x / dstBlockTexelSize.x : dstBlockTexelSize.x / srcBlockTexelSize.x);
        effectiveCopyWidth *= multX;

        uint32 multY = (srcBlockTexelSize.y > dstBlockTexelSize.y ? srcBlockTexelSize.y / dstBlockTexelSize.y : dstBlockTexelSize.y / srcBlockTexelSize.y);
        effectiveCopyHeight *= multY;
    }

    uint32 srcBaseLayer = 0;
    uint32 dstBaseLayer = 0;
    uint32 srcOffsetZ = 0;
    uint32 dstOffsetZ = 0;
    uint32 srcLayerCount = 1;
    uint32 dstLayerCount = 1;
    uint32 srcDepth = 1;
    uint32 dstDepth = 1;

	if (src->Is3DTexture())
	{
		srcOffsetZ = srcSlice;
		srcDepth = srcDepth_;
	}
	else
	{
		srcBaseLayer = srcSlice;
		srcLayerCount = srcDepth_;
	}

	if (dst->Is3DTexture())
	{
		dstOffsetZ = dstSlice;
		dstDepth = srcDepth_;
	}
	else
	{
		dstBaseLayer = dstSlice;
		dstLayerCount = srcDepth_;
	}

	const uint32 srcFirst = src->Is3DTexture() ? srcOffsetZ : srcBaseLayer;
	const uint32 srcCount = src->Is3DTexture() ? srcDepth : srcLayerCount;
	const uint32 dstFirst = dst->Is3DTexture() ? dstOffsetZ : dstBaseLayer;
	const uint32 dstCount = dst->Is3DTexture() ? dstDepth : dstLayerCount;
	if (srcMip >= 0)
		FlushPendingTextureClears(src, (uint32)srcMip, 1, srcFirst, srcCount);
	if (dstMip >= 0 && dstMip < dst->mipLevels)
	{
		const bool replacesWholeSlice = effectiveSrcX == 0 && effectiveSrcY == 0 && effectiveDstX == 0 && effectiveDstY == 0
			&& effectiveCopyWidth >= dst->GetMipWidth(dstMip) && effectiveCopyHeight >= dst->GetMipHeight(dstMip)
			&& (!dst->Is3DTexture() || dstDepth >= (uint32)dst->GetMipDepth(dstMip));
		if (replacesWholeSlice)
			DropPendingClearsOverlapping(dst, (uint32)dstMip, 1, dstFirst, dstCount);
		else
			FlushPendingTextureClears(dst, (uint32)dstMip, 1, dstFirst, dstCount);
	}

    auto* rootSrc = mtlSrc->parentTexture() ? mtlSrc->parentTexture() : mtlSrc;
    auto* rootDst = mtlDst->parentTexture() ? mtlDst->parentTexture() : mtlDst;

    if (rootSrc == rootDst && srcMip == dstMip && srcFirst == dstFirst &&
        effectiveSrcX == effectiveDstX && effectiveSrcY == effectiveDstY)
    {
        return;
    }

    if (effectiveCopyWidth <= 0 || effectiveCopyHeight <= 0)
    {
        return;
    }

    if (src->physAddress == dst->physAddress && src->physAddress != 0 &&
        src->width == dst->width && src->height == dst->height &&
        src->format == dst->format && src->dim == dst->dim &&
        src->depth == dst->depth && src->mipLevels == dst->mipLevels &&
        src->isDepth == dst->isDepth)
    {
        auto* mtlSrcTex = static_cast<LatteTextureMtl*>(src);
        auto* mtlDstTex = static_cast<LatteTextureMtl*>(dst);
        if (mtlSrcTex && mtlDstTex)
        {
            if (mtlSrcTex->GetTexture() == mtlDstTex->GetTexture())
                return;
            mtlDstTex->AdoptSharedTexture(mtlSrcTex->GetTexture());
            return;
        }
    }

	++g_blit_textureCopies;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    RecordTextureCopy(src, dst, (uint32)std::max(effectiveCopyWidth, 0),
        (uint32)std::max(effectiveCopyHeight, 0), mtlSrc == mtlDst);
#endif
	const uint64 copyPixels = uint64(std::max(effectiveCopyWidth, 0)) * uint64(std::max(effectiveCopyHeight, 0));
	if (copyPixels <= 64u * 64u)
		++g_blit_textureCopyTiny;
	else if (copyPixels > 512u * 512u)
		++g_blit_textureCopyLarge;
	auto blitCommandEncoder = GetBlitCommandEncoder();

	// If copying whole textures, we can do a more efficient copy
    if (effectiveSrcX == 0 && effectiveSrcY == 0 && effectiveDstX == 0 && effectiveDstY == 0 &&
        srcOffsetZ == 0 && dstOffsetZ == 0 &&
        effectiveCopyWidth == src->GetMipWidth(srcMip) && effectiveCopyHeight == src->GetMipHeight(srcMip) && srcDepth == src->GetMipDepth(srcMip) &&
        effectiveCopyWidth == dst->GetMipWidth(dstMip) && effectiveCopyHeight == dst->GetMipHeight(dstMip) && dstDepth == dst->GetMipDepth(dstMip) &&
        srcLayerCount == dstLayerCount)
    {
        blitCommandEncoder->copyFromTexture(mtlSrc, srcBaseLayer, srcMip, mtlDst, dstBaseLayer, dstMip, srcLayerCount, 1);
    }
    else
    {
        if (srcLayerCount == dstLayerCount)
        {
            for (uint32 i = 0; i < srcLayerCount; i++)
            {
                blitCommandEncoder->copyFromTexture(mtlSrc, srcBaseLayer + i, srcMip, MTL::Origin(effectiveSrcX, effectiveSrcY, srcOffsetZ), MTL::Size(effectiveCopyWidth, effectiveCopyHeight, srcDepth), mtlDst, dstBaseLayer + i, dstMip, MTL::Origin(effectiveDstX, effectiveDstY, dstOffsetZ));
            }
        }
        else
        {
            for (uint32 i = 0; i < std::max(srcLayerCount, dstLayerCount); i++)
            {
                // A volume uses Z while an array uses source/destinationSlice.
                // Start at the requested slice, including i == 0. Incrementing
                // Z first skipped that slice and could run past the volume;
                // incrementing the unused srcSlice/dstSlice repeated array 0.
                const uint32 sourceLayer = src->Is3DTexture() ? 0 : srcBaseLayer + i;
                const uint32 destinationLayer = dst->Is3DTexture() ? 0 : dstBaseLayer + i;
                const uint32 sourceZ = src->Is3DTexture() ? srcOffsetZ + i : 0;
                const uint32 destinationZ = dst->Is3DTexture() ? dstOffsetZ + i : 0;
                blitCommandEncoder->copyFromTexture(mtlSrc, sourceLayer, srcMip,
                    MTL::Origin(effectiveSrcX, effectiveSrcY, sourceZ), MTL::Size(effectiveCopyWidth, effectiveCopyHeight, 1),
                    mtlDst, destinationLayer, dstMip, MTL::Origin(effectiveDstX, effectiveDstY, destinationZ));
            }
        }
    }
}

LatteTextureReadbackInfo* MetalRenderer::texture_createReadback(LatteTextureView* textureView)
{
    if (!textureView || !textureView->baseTexture)
        return nullptr;
    if (m_submitBeforeDrawableWait && textureView->baseTexture->reloadFromDynamicTextures)
    {
        LatteTexture_UpdateCacheFromDynamicTextures(textureView->baseTexture);
        textureView->baseTexture->reloadFromDynamicTextures = false;
    }
    auto* baseTexMtl = static_cast<LatteTextureMtl*>(textureView->baseTexture);
    auto* mtlTex = baseTexMtl ? baseTexMtl->GetTexture() : nullptr;
    if (!mtlTex)
        return nullptr;
    size_t uploadSize = mtlTex->allocatedSize();

    if ((m_readbackBufferWriteOffset + uploadSize) > TEXTURE_READBACK_SIZE)
	{
		m_readbackBufferWriteOffset = 0;
	}

    auto* result = new LatteTextureReadbackInfoMtl(this, textureView, m_readbackBufferWriteOffset);
    m_readbackBufferWriteOffset += uploadSize;

	return result;
}

MTL::ComputePipelineState* MetalRenderer::GetBCTranscodePipeline(uint32 format)
{
    if (format >= m_bcFormatPipelines.size() || !m_bcTranscodeLibrary)
        return m_bcTranscodePipeline;
    // Specialize once per encountered format; retain the generic pipeline on failure.
    if (!m_bcFormatPipelineAttempted[format])
    {
        m_bcFormatPipelineAttempted[format] = true;
        NS_STACK_SCOPED auto values = MTL::FunctionConstantValues::alloc()->init();
        values->setConstantValue(&format, MTL::DataTypeUInt, NS::UInteger(0));
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto function = m_bcTranscodeLibrary->newFunction(ToNSString("transcodeBCToETC"), values, &error);
        if (function)
            m_bcFormatPipelines[format] = m_device->newComputePipelineState(function, &error);
        if (!m_bcFormatPipelines[format])
            cemuLog_log(LogType::Force, "Metal BC format {} specialization unavailable; using generic transcode", format);
    }
    return m_bcFormatPipelines[format] ? m_bcFormatPipelines[format] : m_bcTranscodePipeline;
}

void MetalRenderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
    // scale copy size to effective size
	sint32 effectiveCopyWidth = width;
	sint32 effectiveCopyHeight = height;
	LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);

	if (sourceTexture->isDepth != destinationTexture->isDepth)
	{
		// Both utility shaders read texture2d. An array's one-slice view must
		// also be 2D, even though the guest surface itself remains an array.
		auto srcView = static_cast<LatteTextureViewMtl*>(sourceTexture->GetOrCreateView(
			Latte::E_DIM::DIM_2D, sourceTexture->format, srcMip, 1, srcSlice, 1));
		auto dstView = static_cast<LatteTextureViewMtl*>(destinationTexture->GetOrCreateView(dstMip, 1, dstSlice, 1));
		MTL::Texture* srcMtl = srcView ? srcView->GetRGBAView() : nullptr;
		MTL::Texture* dstMtl = dstView ? dstView->GetRGBAView() : nullptr;
		if (!srcMtl || !dstMtl)
		{
			cemuLog_log(LogType::Force, "failed to create depth/color copy texture view");
			return;
		}

		EndEncoding();
		FlushPendingTextureClears(sourceTexture, (uint32)srcMip, 1, (uint32)srcSlice, 1);
		// Preserve the uncopied pixels and stencil, and prevent a deferred
		// destination clear from overwriting the copy when it is later used.
		FlushPendingTextureClears(destinationTexture, (uint32)dstMip, 1, (uint32)dstSlice, 1);
		const bool toDepth = destinationTexture->isDepth;
		const MTL::PixelFormat dstFormat = dstMtl->pixelFormat();
		const bool hasStencil = toDepth && (dstFormat == MTL::PixelFormatDepth32Float_Stencil8 || dstFormat == MTL::PixelFormatDepth24Unorm_Stencil8);
		auto* desc = toDepth ? m_copyColorToDepthDesc : m_copyDepthToColorDesc;
		auto& pipelines = toDepth ? m_copyColorToDepthPipelines : m_copyDepthToColorPipelines;
		auto& pipeline = pipelines[dstFormat];
		if (!pipeline)
		{
			if (toDepth)
			{
				desc->setDepthAttachmentPixelFormat(dstFormat);
				desc->setStencilAttachmentPixelFormat(hasStencil ? dstFormat : MTL::PixelFormatInvalid);
			}
			else
				desc->colorAttachments()->object(0)->setPixelFormat(dstFormat);
			NS::Error* error = nullptr;
			pipeline = m_device->newRenderPipelineState(desc, &error);
			if (!pipeline)
			{
				cemuLog_log(LogType::Force, "failed to create depth/color copy pipeline (error: {})",
					error ? error->localizedDescription()->utf8String() : "unknown");
				// A color/depth blit is invalid in Metal and cannot be a fallback.
				return;
			}
		}

		NS_STACK_SCOPED MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::alloc()->init();
		MTL::RenderPassAttachmentDescriptor* attachment = toDepth
			? static_cast<MTL::RenderPassAttachmentDescriptor*>(rpd->depthAttachment())
			: static_cast<MTL::RenderPassAttachmentDescriptor*>(rpd->colorAttachments()->object(0));
		attachment->setTexture(dstMtl);
		const bool replacesWholeSlice = effectiveCopyWidth >= dstMtl->width() && effectiveCopyHeight >= dstMtl->height();
		attachment->setLoadAction(replacesWholeSlice ? MTL::LoadActionDontCare : MTL::LoadActionLoad);
		attachment->setStoreAction(MTL::StoreActionStore);
		if (hasStencil)
		{
			rpd->stencilAttachment()->setTexture(dstMtl);
			rpd->stencilAttachment()->setLoadAction(MTL::LoadActionLoad);
			rpd->stencilAttachment()->setStoreAction(MTL::StoreActionStore);
		}

		auto renderCommandEncoder = GetTemporaryRenderCommandEncoder(rpd);
		renderCommandEncoder->setRenderPipelineState(pipeline);
		if (toDepth)
			renderCommandEncoder->setDepthStencilState(m_copyColorToDepthState);
		renderCommandEncoder->setViewport(MTL::Viewport{0.0, 0.0, (double)effectiveCopyWidth, (double)effectiveCopyHeight, 0.0, 1.0});
		SetTexture(renderCommandEncoder, METAL_SHADER_TYPE_FRAGMENT, srcMtl, GET_HELPER_TEXTURE_BINDING(0));
		renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
		EndEncoding();
		return;
	}

    texture_copyImageSubData(sourceTexture, srcMip, 0, 0, srcSlice, destinationTexture, dstMip, 0, 0, dstSlice, effectiveCopyWidth, effectiveCopyHeight, 1);
}

bool MetalRenderer::CacheBindNeedsBlit(uint32 cacheOffset, uint32 size) const
{
    if (!m_memoryManager || !m_memoryManager->HasDeferredUploads())
        return false;
    MTL::Buffer* buffer = nullptr;
    uint32 bindOffset = 0;
    if (m_memoryManager->ResolveDeferredUpload(cacheOffset, size, buffer, bindOffset))
        return false;
    // A pending upload for a different buffer is already in the private cache's
    // untouched range. Flushing it would close the render pass on every draw.
    return m_memoryManager->DeferredUploadOverlaps(cacheOffset, size);
}

void MetalRenderer::FlushCacheBindsBeforeRenderPass(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, bool usesGeometryShader)
{
    if (!m_memoryManager || !m_memoryManager->HasDeferredUploads())
        return;

    bool split = false;
    auto consider = [&](size_t offset, uint32 size)
    {
        if (!split && offset != INVALID_OFFSET && CacheBindNeedsBlit((uint32)offset, size))
            split = true;
    };
    if (const auto* fetchShader = LatteSHRC_GetActiveFetchShader())
        for (const auto& group : fetchShader->bufferGroups)
        {
            if (split)
                break;
            if (group.attributeBufferIndex < MAX_MTL_VERTEX_BUFFERS)
                consider(m_state.m_vertexBufferOffsets[group.attributeBufferIndex],
                    m_state.m_vertexBufferSizes[group.attributeBufferIndex]);
        }

    auto considerShader = [&](LatteDecompilerShader* shader)
    {
        if (!shader || split)
            return;
        const auto stage = GetMtlGeneralShaderType(shader->shaderType);
        for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++i)
        {
            if (split)
                break;
            if (shader->resourceMapping.uniformBuffersBindingPoint[i] < 0)
                continue;
            consider(m_state.m_uniformBufferOffsets[stage][i], m_state.m_uniformBufferSizes[stage][i]);
        }
    };
    considerShader(vertexShader);
    if (usesGeometryShader)
        considerShader(geometryShader);
    considerShader(pixelShader);
    if (!split)
        return;

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // Private-cache uploads normally force a blit encoder, ending the tile
    // render pass even when the following draw uses the same attachments.
    // The existing vector-copy pipeline preserves those bytes inside the pass
    // when every pending range is aligned and no earlier fragment draw reads
    // its destination. Apple tile GPUs cannot wait for fragments inside a pass.
    if (m_isAppleGPU && m_copyBufferToBufferPipeline &&
        m_encoderType == MetalEncoderType::Render && m_commandEncoder)
    {
        auto uploads = m_memoryManager->TakeAlignedDeferredUploads(64 * 1024);
        if (uploads)
        {
            ++g_cacheUploadInPassCalls;
            g_cacheUploadInPassRanges += static_cast<uint32>(uploads->size());
            auto* encoder = static_cast<MTL::RenderCommandEncoder*>(m_commandEncoder);
            encoder->memoryBarrier(MTL::BarrierScopeBuffers,
                SupportedStages(MTL::RenderStageVertex | MTL::RenderStageObject | MTL::RenderStageMesh), MTL::RenderStageVertex);
            encoder->setRenderPipelineState(m_copyBufferToBufferPipeline->GetRenderPipelineState());
            for (const auto& [cacheOffset, upload] : *uploads)
            {
                encoder->setVertexBuffer(upload.buffer, upload.bufferOffset, GET_HELPER_BUFFER_BINDING(0));
                encoder->setVertexBuffer(m_memoryManager->GetBufferCache(), cacheOffset, GET_HELPER_BUFFER_BINDING(1));
                encoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0), NS::UInteger(upload.size / 16));
            }
            encoder->memoryBarrier(MTL::BarrierScopeBuffers,
                MTL::RenderStageVertex, SupportedStages(ALL_MTL_RENDER_STAGES));
            m_state.m_encoderState.m_renderPipelineState = nullptr;
            return;
        }
    }
#endif
    ++g_cacheUploadBlitFallbacks;
    EndEncoding();
    m_memoryManager->FlushDeferredUploads();
    EndEncoding();
}

MTL::RenderCommandEncoder* MetalRenderer::LiveRenderEncoder(MTL::RenderCommandEncoder* encoder)
{
    if (m_commandEncoder && m_encoderType == MetalEncoderType::Render && m_commandEncoder == encoder)
        return encoder;
    return GetRenderCommandEncoder();
}

void MetalRenderer::BindBufferCache(MTL::RenderCommandEncoder*& encoder, MetalShaderType shaderType, uint32 cacheOffset, uint32 size, uint32 binding)
{
    MTL::Buffer* buffer = m_memoryManager->GetBufferCache();
    uint32 bindOffset = cacheOffset;
    if (m_memoryManager->HasDeferredUploads())
    {
        if (!m_memoryManager->ResolveDeferredUpload(cacheOffset, size, buffer, bindOffset))
        {
            if (m_memoryManager->DeferredUploadOverlaps(cacheOffset, size))
            {
                m_memoryManager->FlushDeferredUploads();
                EndEncoding();
            }
            buffer = m_memoryManager->GetBufferCache();
            bindOffset = cacheOffset;
        }
    }
    encoder = LiveRenderEncoder(encoder);
    SetBuffer(encoder, shaderType, buffer, bindOffset, binding);
}

void MetalRenderer::bufferCache_init(const sint32 bufferSize)
{
    m_memoryManager->InitBufferCache(bufferSize);
}

bool MetalRenderer::bufferCache_ensureCapacity(uint32 requiredEnd)
{
    return m_memoryManager->EnsureBufferCacheCapacity(requiredEnd);
}

void MetalRenderer::bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset)
{
    m_memoryManager->UploadToBufferCache(buffer, bufferOffset, size);
}

void MetalRenderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
    ++g_bufferCacheCopies;
    m_memoryManager->CopyBufferCache(srcOffset, dstOffset, size);
}

void MetalRenderer::bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
    ++g_streamoutCopies;
    if (m_memoryManager->UseHostMemoryForCache())
        dstOffset -= m_memoryManager->GetImportedMemBaseAddress();

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // A streamout draw often immediately copies its output into the vertex
    // cache. The blit path ends the render pass, stores its attachments and
    // reloads them for the next draw. For small aligned ranges, keep the copy
    // in the active render pass instead. Barriers cover the shader write,
    // helper read/write, and subsequent vertex/fragment reads.
    if (m_isAppleGPU && m_copyBufferToBufferPipeline &&
        m_encoderType == MetalEncoderType::Render && m_commandEncoder &&
        !m_memoryManager->DeferredUploadOverlaps(dstOffset, size) &&
        size >= 16 && size <= 64 * 1024 &&
        ((srcOffset | dstOffset | size) & 15u) == 0)
    {
        auto* encoder = static_cast<MTL::RenderCommandEncoder*>(m_commandEncoder);
        encoder->memoryBarrier(MTL::BarrierScopeBuffers,
            SupportedStages(MTL::RenderStageVertex | MTL::RenderStageObject | MTL::RenderStageMesh),
            MTL::RenderStageVertex);
        encoder->setRenderPipelineState(m_copyBufferToBufferPipeline->GetRenderPipelineState());
        encoder->setVertexBuffer(GetXfbRingBuffer(), srcOffset, GET_HELPER_BUFFER_BINDING(0));
        encoder->setVertexBuffer(m_memoryManager->GetBufferCache(), dstOffset, GET_HELPER_BUFFER_BINDING(1));
        encoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0), NS::UInteger(size / 16));
        encoder->memoryBarrier(MTL::BarrierScopeBuffers,
            MTL::RenderStageVertex, SupportedStages(ALL_MTL_RENDER_STAGES));
        // The next game draw must restore its pipeline. Helper bindings use
        // reserved slots and do not disturb the game's cached bindings.
        m_state.m_encoderState.m_renderPipelineState = nullptr;
        ++g_streamoutInPassCopies;
        return;
    }
#endif
    m_memoryManager->FlushDeferredUploads();
    CopyBufferToBuffer(GetXfbRingBuffer(), srcOffset, m_memoryManager->GetBufferCache(), dstOffset, size, MTL::RenderStageVertex | MTL::RenderStageMesh, ALL_MTL_RENDER_STAGES);
}

void MetalRenderer::buffer_bindVertexBuffer(uint32 bufferIndex, uint32 offset, uint32 size)
{
    cemu_assert_debug(!m_memoryManager->UseHostMemoryForCache());
    cemu_assert_debug(bufferIndex < LATTE_MAX_VERTEX_BUFFERS);

    m_state.m_vertexBufferOffsets[bufferIndex] = offset;
    m_state.m_vertexBufferSizes[bufferIndex] = size;
}

void MetalRenderer::buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size)
{
    cemu_assert_debug(!m_memoryManager->UseHostMemoryForCache());

    m_state.m_uniformBufferOffsets[GetMtlGeneralShaderType(shaderType)][bufferIndex] = offset;
    m_state.m_uniformBufferSizes[GetMtlGeneralShaderType(shaderType)][bufferIndex] = size;
}

RendererShader* MetalRenderer::shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackShader)
{
    return new RendererShaderMtl(this, type, baseHash, auxHash, isGameShader, isGfxPackShader, source);
}

void MetalRenderer::streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize)
{
    m_state.m_streamoutState.buffers[bufferIndex].enabled = true;
	m_state.m_streamoutState.buffers[bufferIndex].ringBufferOffset = ringBufferOffset;
}

void MetalRenderer::streamout_begin()
{
    // Do nothing
}

void MetalRenderer::streamout_rendererFinishDrawcall()
{
    // Do nothing
}

void MetalRenderer::draw_beginSequence()
{
    LatteGPUState.telemetry.metalDrawBeginSeqCount++;
    m_state.m_skipDrawSequence = false;
    m_state.m_sequenceRasterized = false;

    bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;

    // update shader state
	LatteSHRC_UpdateActiveShaders();
	if (LatteGPUState.activeShaderHasError)
	{
		cemuLog_logOnce(LogType::Force, "Skipping drawcalls due to shader error\n");
		m_state.m_skipDrawSequence = true;
		LatteGPUState.telemetry.metalSkipDrawSequence = true;
		cemu_assert_debug(false);
		return;
	}

	// update render target and texture state
	LatteGPUState.requiresTextureBarrier = false;
	for (int textureInitAttempt = 0; textureInitAttempt < 4; ++textureInitAttempt)
	{
		LatteGPUState.repeatTextureInitialization = false;
		if (!LatteMRT::UpdateCurrentFBO())
		{
			cemuLog_logOnce(LogType::Force, "Rendertarget invalid\n");
			m_state.m_skipDrawSequence = true;
			LatteGPUState.telemetry.metalSkipDrawSequence = true;
			return; // no render target
		}

		if (!hasValidFramebufferAttached && !streamoutEnable)
		{
			cemuLog_logOnce(LogType::Force, "Drawcall with no color buffer or depth buffer attached\n");
			m_state.m_skipDrawSequence = true;
			LatteGPUState.telemetry.metalSkipDrawSequence = true;
			return; // no render target
		}
		// When no render encoder is active, flush any pending dynamic texture
		// copies so they run in a blit pass before any render pass begins.
		if (!m_commandEncoder || m_encoderType != MetalEncoderType::Render)
		{
			LatteTexture_FlushPendingDynamicReloads(true);
		}
		// A streamout-only Metal pipeline has no fragment function. Its pixel
		// textures cannot be sampled, so leave their alias synchronization until
		// a draw that actually uses the fragment stage.
		LatteTexture_updateTextures(LatteGPUState.contextNew.IsRasterizationEnabled());
		if (!LatteGPUState.repeatTextureInitialization)
			break;
	}
	if (LatteGPUState.repeatTextureInitialization)
	{
		m_state.m_skipDrawSequence = true;
		LatteGPUState.telemetry.metalSkipDrawSequence = true;
		return;
	}

	// apply render target
	LatteMRT::ApplyCurrentState();

	// viewport and scissor box
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();

	if (!LatteGPUState.contextNew.IsRasterizationEnabled() && !streamoutEnable)
		m_state.m_skipDrawSequence = true;

	LatteGPUState.telemetry.metalSkipDrawSequence = m_state.m_skipDrawSequence;
}

static MTL::RenderPipelineState* ResolvePipelineForDraw(PipelineObject* pipelineObject, bool allowAsyncCompile = false)
{
    if (!pipelineObject)
        return nullptr;
    auto* pipeline = pipelineObject->GetPipeline();
    // The policy is per draw: the same PSO can be used for presentation first
    // and a required query later. Never attach skip eligibility to the PSO.
    if (!pipeline && !allowAsyncCompile)
    {
        const auto waitStart = std::chrono::steady_clock::now();
        MetalPipelineCache::GetInstance().PrioritizePendingPipeline(pipelineObject);
        pipelineObject->WaitForCompilation();
        RuntimePerformance::pipelineWaitNs.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - waitStart).count(), std::memory_order_relaxed);
        RuntimePerformance::pipelineWaitCount.fetch_add(1, std::memory_order_relaxed);
        pipeline = pipelineObject->GetPipeline();
    }
    else if (!pipeline)
    {
        RuntimePerformance::drawsSkippedPendingPipeline.fetch_add(1, std::memory_order_relaxed);
    }
    return pipeline;
}

void MetalRenderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext)
{
    LatteGPUState.telemetry.metalDrawExecuteCount++;
    if (m_state.m_skipDrawSequence)
	{
	    LatteGPUState.drawCallCounter++;
		return;
	}

	// fast clear color as depth
	if (LatteGPUState.contextNew.GetSpecialStateValues()[8] != 0)
	{
		LatteDraw_handleSpecialState8_clearAsDepth();
		LatteGPUState.drawCallCounter++;
		return;
	}
	else if (LatteGPUState.contextNew.GetSpecialStateValues()[5] != 0)
	{
		draw_handleSpecialState5();
		LatteGPUState.drawCallCounter++;
		return;
	}

	auto& encoderState = m_state.m_encoderState;

    // Shaders
    LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
    LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();
    LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
    const auto fetchShader = LatteSHRC_GetActiveFetchShader();
    const bool rasterizationEnabled = LatteGPUState.contextNew.IsRasterizationEnabled();
    LatteDecompilerShader* sampledPixelShader = rasterizationEnabled ? pixelShader : nullptr;

    bool neverSkipAccurateBarrier = false;

    // "Accurate barriers" is usually enabled globally but since the CPU cost is substantial we allow users to disable it (debug -> 'Accurate barriers' option)
	// We always force accurate barriers for known problematic shaders
	if (sampledPixelShader)
	{
		if (pixelShader->baseHash == 0x6f6f6e7b9aae57af && pixelShader->auxHash == 0x00078787f9249249) // BotW lava
			neverSkipAccurateBarrier = true;
		if (pixelShader->baseHash == 0x4c0bd596e3aef4a6 && pixelShader->auxHash == 0x003c3c3fc9269249) // BotW foam layer for water on the bottom of waterfalls
			neverSkipAccurateBarrier = true;
		if (pixelShader->baseHash == 0x3a7e6b48dae31305 && pixelShader->auxHash == 0x003c3c3fc9269249) // BotW Gerudo Town water
			neverSkipAccurateBarrier = true;
	}

	// Check if we need to end the render pass due to sampling an active attachment that was written
	if (!m_state.m_isFirstDrawInRenderPass && (GetConfig().vk_accurate_barriers || neverSkipAccurateBarrier))
	{
		// Fragment shader is most likely to require a render pass flush, so check for it first
		bool endRenderPass = CheckIfRenderPassNeedsFlush(sampledPixelShader, neverSkipAccurateBarrier);
		if (!endRenderPass)
			endRenderPass = CheckIfRenderPassNeedsFlush(vertexShader, neverSkipAccurateBarrier);
		if (!endRenderPass && geometryShader)
			endRenderPass = CheckIfRenderPassNeedsFlush(geometryShader, neverSkipAccurateBarrier);

		if (endRenderPass)
		{
			++g_pass_accBarrier;
			EndEncoding();
		}
	}

    // Primitive type
    const LattePrimitiveMode primitiveMode = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
    auto mtlPrimitiveType = GetMtlPrimitiveType(primitiveMode);

    bool usesGeometryShader = UseGeometryShader(LatteGPUState.contextNew, geometryShader != nullptr);
    const bool fallbackGeometry = usesGeometryShader && !m_supportsMeshShaders;
    if (fallbackGeometry && indexType != LatteIndexType::AUTO && LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_EN.get_RESET_EN()) {
        cemuLog_logOnce(LogType::Force, "Metal geometry fallback primitive restart is unsupported");
        return;
    }

    bool fetchVertexManually = (usesGeometryShader || fetchShader->mtlFetchVertexManually);

	// Index buffer
	Renderer::INDEX_TYPE hostIndexType;
	uint32 hostIndexCount;
	uint32 indexMax = 0;
	Renderer::IndexAllocation indexAllocation;
	LatteIndices_decode(memory_getPointerFromVirtualOffset(indexDataMPTR), indexType, count, primitiveMode, indexMax, hostIndexType, hostIndexCount, indexAllocation);
	auto indexAllocationMtl = static_cast<MetalSynchronizedHeapAllocator::AllocatorReservation*>(indexAllocation.rendererInternal);
	if (hostIndexType != INDEX_TYPE::NONE && (!indexAllocationMtl || !indexAllocationMtl->mtlBuffer))
	{
		if (indexAllocationMtl)
			g_renderer->indexData_releaseIndexMemory(indexAllocation);
		return;
	}

	// Buffer cache
	if (m_memoryManager->UseHostMemoryForCache())
	{
		// direct memory access (Wii U memory space imported as a buffer), update buffer bindings
		draw_updateVertexBuffersDirectAccess();
		if (vertexShader)
			draw_updateUniformBuffersDirectAccess(vertexShader, mmSQ_VTX_UNIFORM_BLOCK_START);
		if (geometryShader)
			draw_updateUniformBuffersDirectAccess(geometryShader, mmSQ_GS_UNIFORM_BLOCK_START);
		if (pixelShader)
			draw_updateUniformBuffersDirectAccess(pixelShader, mmSQ_PS_UNIFORM_BLOCK_START);
	}
	else
	{
    	// synchronize vertex and uniform cache and update buffer bindings
    	// We need to call this before getting the render command encoder, since it can cause buffer copies
		uint8 stageUniformModifiedMask = 0;
		if (drawcallContext.isFirst)
		{
			LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, stageUniformModifiedMask, false);
		}
		else
		{
			LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount, drawcallContext.vertexBufferDirtyMask, drawcallContext.vsUniformBufferDirtyMask, drawcallContext.psUniformBufferDirtyMask, drawcallContext.gsUniformBufferDirtyMask, stageUniformModifiedMask, true);
		}
	}

	LatteGPUState.requiresTextureBarrier = false;

	// A deferred clear that this draw samples has to land in memory first.
	// The same clear can still fold into the pass below when nothing samples it.
	FlushPendingClearsSampledByDraw(vertexShader, geometryShader, sampledPixelShader);
	// Copy any buffer-cache upload that cannot be bound from staging before the
	// render pass exists. Doing that copy later closes the encoder, and the draw
	// was still talking to the released one.
	FlushCacheBindsBeforeRenderPass(vertexShader, geometryShader, sampledPixelShader, usesGeometryShader);

	// Render pass
	auto renderCommandEncoder = GetRenderCommandEncoder();
	const uint64 renderEncoderSerialAtSetup = m_renderEncoderSerial;

    // Decide at each draw, including cache hits: query and streamout semantics
    // must not depend on whether this pipeline was previously queued for display.
    const bool allowAsyncCompile = MetalPipelineCache::CanCompileDrawAsync(
        m_state.m_activeFBO.m_attachmentsInfo, m_state.m_activeFBO.m_fbo->m_size,
        LatteGPUState.contextNew, m_occlusionQuery.m_active, m_state.m_activeFBO.m_hasReadbackAttachment);
    PipelineObject* pipelineObj = m_pipelineCache->GetRenderPipelineState(fetchShader,
        vertexShader, geometryShader, pixelShader, m_state.m_lastUsedFBO.m_attachmentsInfo,
        m_state.m_activeFBO.m_attachmentsInfo, LatteGPUState.contextNew, allowAsyncCompile);
    auto* pipeline = ResolvePipelineForDraw(pipelineObj, allowAsyncCompile);
    if (!pipeline)
    {
        LatteGPUState.drawCallCounter++;
        return;
    }

    if (pipeline != encoderState.m_renderPipelineState)
   	{
        renderCommandEncoder->setRenderPipelineState(pipeline);
  		encoderState.m_renderPipelineState = pipeline;
   	}

	// Depth stencil state

	// Disable depth/stencil operations when there is no depth/stencil attachment
	auto& depthControl = LatteGPUState.contextNew.DB_DEPTH_CONTROL;
	const uint32 depthControlRaw = depthControl.getRawValue();
	const uint32 stencilMaskRaw = LatteGPUState.contextNew.DB_STENCILREFMASK.getRawValue();
	const uint32 stencilMaskBfRaw = LatteGPUState.contextNew.DB_STENCILREFMASK_BF.getRawValue();

	bool hasDepth = (m_state.m_activeFBO.m_fbo->depthBuffer.texture != nullptr);
	bool hasStencil = (hasDepth && GetMtlPixelFormatInfo(m_state.m_activeFBO.m_fbo->depthBuffer.texture->format, true).hasStencil);

	MTL::DepthStencilState* depthStencilState;
	if (m_cachedDepthStencilState &&
	    m_cachedDepthControlRaw == depthControlRaw &&
	    m_cachedStencilRefMaskRaw == stencilMaskRaw &&
	    m_cachedStencilRefMaskBfRaw == stencilMaskBfRaw &&
	    m_cachedHasDepth == hasDepth &&
	    m_cachedHasStencil == hasStencil)
	{
		depthStencilState = m_cachedDepthStencilState;
	}
	else
	{
		bool origDepthEnable = depthControl.get_Z_ENABLE();
		bool origDepthWriteEnable = depthControl.get_Z_WRITE_ENABLE();
		bool origStencilEnable = depthControl.get_STENCIL_ENABLE();

		if (!hasDepth)
		{
			depthControl.set_Z_ENABLE(false);
			depthControl.set_Z_WRITE_ENABLE(false);
		}
		if (!hasStencil)
		{
			depthControl.set_STENCIL_ENABLE(false);
		}

		depthStencilState = m_depthStencilCache->GetDepthStencilState(LatteGPUState.contextNew);

		// Restore the original depth/stencil state
		depthControl.set_Z_ENABLE(origDepthEnable);
		depthControl.set_Z_WRITE_ENABLE(origDepthWriteEnable);
		depthControl.set_STENCIL_ENABLE(origStencilEnable);

		m_cachedDepthControlRaw = depthControlRaw;
		m_cachedStencilRefMaskRaw = stencilMaskRaw;
		m_cachedStencilRefMaskBfRaw = stencilMaskBfRaw;
		m_cachedHasDepth = hasDepth;
		m_cachedHasStencil = hasStencil;
		m_cachedDepthStencilState = depthStencilState;
	}

	if (depthStencilState != encoderState.m_depthStencilState)
	{
	    renderCommandEncoder->setDepthStencilState(depthStencilState);
		encoderState.m_depthStencilState = depthStencilState;
	}

	// Stencil reference
	bool stencilEnable = LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE();
	if (stencilEnable)
	{
	    bool backStencilEnable = LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_BACK_STENCIL_ENABLE();
		uint32 stencilRefFront = LatteGPUState.contextNew.DB_STENCILREFMASK.get_STENCILREF_F();
    	uint32 stencilRefBack;
        if (backStencilEnable)
            stencilRefBack = LatteGPUState.contextNew.DB_STENCILREFMASK_BF.get_STENCILREF_B();
        else
            stencilRefBack = stencilRefFront;

	    if (stencilRefFront != encoderState.m_stencilRefFront || stencilRefBack != encoderState.m_stencilRefBack)
		{
		    renderCommandEncoder->setStencilReferenceValues(stencilRefFront, stencilRefBack);

			encoderState.m_stencilRefFront = stencilRefFront;
			encoderState.m_stencilRefBack = stencilRefBack;
		}
	}

	// Blend color
	uint32* blendColorConstantU32 = LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED;

	if (blendColorConstantU32[0] != encoderState.m_blendColor[0] || blendColorConstantU32[1] != encoderState.m_blendColor[1] || blendColorConstantU32[2] != encoderState.m_blendColor[2] || blendColorConstantU32[3] != encoderState.m_blendColor[3])
	{
    	float* blendColorConstant = (float*)LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED;
    	renderCommandEncoder->setBlendColor(blendColorConstant[0], blendColorConstant[1], blendColorConstant[2], blendColorConstant[3]);

        encoderState.m_blendColor[0] = blendColorConstantU32[0];
        encoderState.m_blendColor[1] = blendColorConstantU32[1];
        encoderState.m_blendColor[2] = blendColorConstantU32[2];
        encoderState.m_blendColor[3] = blendColorConstantU32[3];
	}

	// polygon control
	const auto& polygonControlReg = LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL;
	const auto frontFace = polygonControlReg.get_FRONT_FACE();
	uint32 cullFront = polygonControlReg.get_CULL_FRONT();
	uint32 cullBack = polygonControlReg.get_CULL_BACK();
	uint32 polyOffsetFrontEnable = polygonControlReg.get_OFFSET_FRONT_ENABLED();

	if (polyOffsetFrontEnable)
	{
    	uint32 frontScaleU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_SCALE.getRawValue();
    	uint32 frontOffsetU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_OFFSET.getRawValue();
    	uint32 offsetClampU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_CLAMP.getRawValue();

        if (frontOffsetU32 != encoderState.m_depthBias || frontScaleU32 != encoderState.m_depthSlope || offsetClampU32 != encoderState.m_depthClamp)
        {
           	float frontScale = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE();
           	float frontOffset = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
           	float offsetClamp = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();

           	frontScale /= 16.0f;

            renderCommandEncoder->setDepthBias(frontOffset, frontScale, offsetClamp);

            encoderState.m_depthBias = frontOffsetU32;
            encoderState.m_depthSlope = frontScaleU32;
            encoderState.m_depthClamp = offsetClampU32;
        }
	}
	else
	{
	    if (0 != encoderState.m_depthBias || 0 != encoderState.m_depthSlope || 0 != encoderState.m_depthClamp)
		{
	        renderCommandEncoder->setDepthBias(0.0f, 0.0f, 0.0f);

			encoderState.m_depthBias = 0;
			encoderState.m_depthSlope = 0;
			encoderState.m_depthClamp = 0;
		}
	}

	// Depth clip mode
	cemu_assert_debug(LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() == LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE()); // near or far clipping can be disabled individually
	bool zClipEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE() == false;

	if (zClipEnable != encoderState.m_depthClipEnable)
	{
	    renderCommandEncoder->setDepthClipMode(zClipEnable ? MTL::DepthClipModeClip : MTL::DepthClipModeClamp);
        encoderState.m_depthClipEnable = zClipEnable;
	}

	// Visibility result mode
	if (m_occlusionQuery.m_active)
	{
	    auto mode = (m_occlusionQuery.m_currentIndex == INVALID_UINT32 ? MTL::VisibilityResultModeDisabled : MTL::VisibilityResultModeCounting);
	    renderCommandEncoder->setVisibilityResultMode(mode, m_occlusionQuery.m_currentIndex * sizeof(uint64));
	}

	// todo - how does culling behave with rects?
	// right now we just assume that their winding is always CW
	if (primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS)
	{
		if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CW)
			cullFront = cullBack;
		else
			cullBack = cullFront;
	}

	// Cull mode

	// Cull front and back is handled by disabling rasterization
	if (!(cullFront && cullBack))
	{
        MTL::CullMode cullMode;
       	if (cullFront)
      		cullMode = MTL::CullModeFront;
       	else if (cullBack)
      		cullMode = MTL::CullModeBack;
       	else
      		cullMode = MTL::CullModeNone;

        if (cullMode != encoderState.m_cullMode)
       	{
       	    renderCommandEncoder->setCullMode(cullMode);
      		encoderState.m_cullMode = cullMode;
       	}
	}

	// Front face
	MTL::Winding frontFaceWinding;
	if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW)
		frontFaceWinding = MTL::WindingCounterClockwise;
	else
		frontFaceWinding = MTL::WindingClockwise;

    if (frontFaceWinding != encoderState.m_frontFaceWinding)
   	{
   	    renderCommandEncoder->setFrontFacingWinding(frontFaceWinding);
  		encoderState.m_frontFaceWinding = frontFaceWinding;
   	}

    // Viewport
    if (m_state.m_viewport.originX != encoderState.m_viewport.originX ||
        m_state.m_viewport.originY != encoderState.m_viewport.originY ||
        m_state.m_viewport.width != encoderState.m_viewport.width ||
        m_state.m_viewport.height != encoderState.m_viewport.height ||
        m_state.m_viewport.znear != encoderState.m_viewport.znear ||
        m_state.m_viewport.zfar != encoderState.m_viewport.zfar)
    {
        renderCommandEncoder->setViewport(m_state.m_viewport);

        encoderState.m_viewport = m_state.m_viewport;
    }

    // Scissor
    if (m_state.m_scissor.x != encoderState.m_scissor.x ||
        m_state.m_scissor.y != encoderState.m_scissor.y ||
        m_state.m_scissor.width != encoderState.m_scissor.width ||
        m_state.m_scissor.height != encoderState.m_scissor.height)
    {
        encoderState.m_scissor = m_state.m_scissor;

        // TODO: clamp scissor to render target dimensions?
        //scissor.width = ;
        //scissor.height = ;
        renderCommandEncoder->setScissorRect(encoderState.m_scissor);
    }

	// Resources

	// Vertex buffers
	if (fetchShader)
	{
		for (const auto& bufferGroup : fetchShader->bufferGroups)
		{
			uint32 i = bufferGroup.attributeBufferIndex;
			if (i < MAX_MTL_VERTEX_BUFFERS)
			{
				size_t offset = m_state.m_vertexBufferOffsets[i];
				if (offset != INVALID_OFFSET)
				{
					BindBufferCache(renderCommandEncoder, GetMtlShaderType(vertexShader->shaderType, usesGeometryShader), (uint32)offset, m_state.m_vertexBufferSizes[i], GET_MTL_VERTEX_BUFFER_INDEX(i));
				}
			}
		}
	}
	else
	{
		for (uint8 i = 0; i < MAX_MTL_VERTEX_BUFFERS; i++)
		{
			size_t offset = m_state.m_vertexBufferOffsets[i];
			if (offset != INVALID_OFFSET)
			{
				BindBufferCache(renderCommandEncoder, GetMtlShaderType(vertexShader->shaderType, usesGeometryShader), (uint32)offset, m_state.m_vertexBufferSizes[i], GET_MTL_VERTEX_BUFFER_INDEX(i));
			}
		}
	}

	// Prepare streamout
	m_state.m_streamoutState.verticesPerInstance = count;
	LatteStreamout_PrepareDrawcall(count, instanceCount);

	const auto savedDepthState = encoderState.m_depthStencilState;
	const auto savedCull = encoderState.m_cullMode;
	const auto savedWinding = encoderState.m_frontFaceWinding;
	const auto savedViewport = m_state.m_viewport;
	const auto savedScissor = m_state.m_scissor;
	const bool savedDepthClip = encoderState.m_depthClipEnable;
	const uint32 savedStencilFront = encoderState.m_stencilRefFront;
	const uint32 savedStencilBack = encoderState.m_stencilRefBack;

	// Uniform buffers, textures and samplers
	BindStageResources(renderCommandEncoder, vertexShader, usesGeometryShader);
    MTL::Buffer* fallbackPayload = nullptr;
    std::unique_ptr<MTL::Buffer, void(*)(MTL::Buffer*)> fallbackPayloadOwner(nullptr, [](MTL::Buffer* value) { if (value) value->release(); });
    uint32 fallbackPrimitiveCount = 0;
    if (fallbackGeometry) {
        const uint32 inputVertices = GetVerticesPerPrimitive(primitiveMode);
        const uint32 perInstance = inputVertices && count >= inputVertices ?
            (PrimitiveRequiresConnection(primitiveMode) ? count - inputVertices + 1 : count / inputVertices) : 0;
        const uint64 total = uint64(perInstance) * instanceCount;
        const uint64 bytes = total * pipelineObj->m_payloadStride;
        if (!total) return;
        if (!pipelineObj->m_capturePipeline || !bytes || bytes > m_device->maxBufferLength() ||
            total > UINT32_MAX / std::max(inputVertices, pipelineObj->m_expandedVertices)) {
            cemuLog_logOnce(LogType::Force, "Metal geometry fallback cannot represent this draw"); return;
        }
        fallbackPrimitiveCount = static_cast<uint32>(total);
        fallbackPayload = m_device->newBuffer(bytes, MTL::ResourceStorageModePrivate);
        fallbackPayloadOwner.reset(fallbackPayload);
        if (!fallbackPayload) {
            cemuLog_logOnce(LogType::Force, "Metal geometry fallback payload allocation failed"); return;
        }
        if (hostIndexType != INDEX_TYPE::NONE)
            SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_VERTEX, indexAllocationMtl->mtlBuffer,
                indexAllocationMtl->bufferOffset, vertexShader->resourceMapping.indexBufferBinding);
        // A valid harmless buffer is also required for the non-indexed entry;
        // indexType=0 prevents reading it.
        else SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_VERTEX, fallbackPayload, 0, vertexShader->resourceMapping.indexBufferBinding);
        const uint8 kind = static_cast<uint8>(hostIndexType);
        renderCommandEncoder->setVertexBytes(&kind, sizeof(kind), vertexShader->resourceMapping.indexTypeBinding);
        encoderState.m_buffers[METAL_SHADER_TYPE_VERTEX][vertexShader->resourceMapping.indexTypeBinding] = {nullptr};
        const MetalGeometryFallback::Draw draw{static_cast<int32_t>(baseVertex), baseInstance, count, perInstance};
        renderCommandEncoder->setVertexBytes(&draw, sizeof(draw), MetalGeometryFallback::DrawBinding);
        encoderState.m_buffers[METAL_SHADER_TYPE_VERTEX][MetalGeometryFallback::DrawBinding] = {nullptr};
        SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_VERTEX, fallbackPayload, 0, MetalGeometryFallback::PayloadBinding);
        renderCommandEncoder->setRenderPipelineState(pipelineObj->m_capturePipeline);
        renderCommandEncoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0);
        renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0), NS::UInteger(total * inputVertices));
        renderCommandEncoder->memoryBarrier(MTL::BarrierScopeBuffers, MTL::RenderStageVertex, MTL::RenderStageVertex);
        renderCommandEncoder->setRenderPipelineState(pipeline);
        // Retain locally through the final draw too: a later resource bind can
        // split/complete the capture command buffer before output is encoded.
    }
	if (usesGeometryShader && geometryShader)
	    BindStageResources(renderCommandEncoder, geometryShader, usesGeometryShader);
	if (sampledPixelShader)
		BindStageResources(renderCommandEncoder, sampledPixelShader, usesGeometryShader);

	if (m_renderEncoderSerial != renderEncoderSerialAtSetup || m_commandEncoder != renderCommandEncoder)
	{
		renderCommandEncoder = LiveRenderEncoder(nullptr);
		if (pipeline)
			renderCommandEncoder->setRenderPipelineState(pipeline);
		if (savedDepthState)
			renderCommandEncoder->setDepthStencilState(savedDepthState);
		renderCommandEncoder->setDepthClipMode(savedDepthClip ? MTL::DepthClipModeClip : MTL::DepthClipModeClamp);
		renderCommandEncoder->setCullMode(savedCull);
		renderCommandEncoder->setFrontFacingWinding(savedWinding);
		renderCommandEncoder->setStencilReferenceValues(savedStencilFront, savedStencilBack);
		renderCommandEncoder->setViewport(savedViewport);
		renderCommandEncoder->setScissorRect(savedScissor);
	}

	// Draw
    if (fallbackGeometry) {
        // GS resources share the conventional vertex stage with the capture VS.
        // Bind the captured payload again after all resource changes.
        SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_VERTEX, fallbackPayload, 0, MetalGeometryFallback::PayloadBinding);
        if (m_occlusionQuery.m_active && m_occlusionQuery.m_currentIndex != INVALID_UINT32)
            renderCommandEncoder->setVisibilityResultMode(MTL::VisibilityResultModeCounting, m_occlusionQuery.m_currentIndex * sizeof(uint64));
        const auto output = pipelineObj->m_fallbackOutputTopology == 0 ? MTL::PrimitiveTypePoint :
            pipelineObj->m_fallbackOutputTopology == 1 ? MTL::PrimitiveTypeLine : MTL::PrimitiveTypeTriangle;
        renderCommandEncoder->drawPrimitives(output, NS::UInteger(0), NS::UInteger(fallbackPrimitiveCount) * pipelineObj->m_expandedVertices);
    }
	else if (usesGeometryShader)
	{
	    if (hostIndexType != INDEX_TYPE::NONE)
		    SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_OBJECT, indexAllocationMtl->mtlBuffer, indexAllocationMtl->bufferOffset, vertexShader->resourceMapping.indexBufferBinding);

		uint8 hostIndexTypeU8 = (uint8)hostIndexType;
		renderCommandEncoder->setObjectBytes(&hostIndexTypeU8, sizeof(hostIndexTypeU8), vertexShader->resourceMapping.indexTypeBinding);
        encoderState.m_buffers[METAL_SHADER_TYPE_OBJECT][vertexShader->resourceMapping.indexTypeBinding] = {nullptr};

		uint32 verticesPerPrimitive = GetVerticesPerPrimitive(primitiveMode);
		uint32 threadgroupCount = count * instanceCount;
		if (PrimitiveRequiresConnection(primitiveMode))
		    threadgroupCount -= verticesPerPrimitive - 1;
		else
		    threadgroupCount /= verticesPerPrimitive;

		if (UsesMeshDrawBatching()) {
            for (uint32 first = 0; first < threadgroupCount; first += 1024) {
                renderCommandEncoder->setObjectBytes(&first, sizeof(first), 30);
                encoderState.m_buffers[METAL_SHADER_TYPE_OBJECT][30] = {nullptr};
                renderCommandEncoder->drawMeshThreadgroups(MTL::Size(std::min(1024u, threadgroupCount - first), 1, 1), MTL::Size(verticesPerPrimitive, 1, 1), MTL::Size(1, 1, 1));
            }
        } else renderCommandEncoder->drawMeshThreadgroups(MTL::Size(threadgroupCount, 1, 1), MTL::Size(verticesPerPrimitive, 1, 1), MTL::Size(1, 1, 1));
	}
	else
	{
        if (hostIndexType != INDEX_TYPE::NONE)
       	{
       	    auto mtlIndexType = GetMtlIndexType(hostIndexType);
      		renderCommandEncoder->drawIndexedPrimitives(mtlPrimitiveType, hostIndexCount, mtlIndexType, indexAllocationMtl->mtlBuffer, indexAllocationMtl->bufferOffset, instanceCount, baseVertex, baseInstance);
       	}
       	else
       	{
      		renderCommandEncoder->drawPrimitives(mtlPrimitiveType, baseVertex, count, instanceCount, baseInstance);
       	}
	}

	m_state.m_isFirstDrawInRenderPass = false;

	if (rasterizationEnabled)
	{
		m_state.m_sequenceRasterized = true;
		if (LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_Z_WRITE_ENABLE() || LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE())
			m_depthWrittenInCurrentEncoder = true;
		if (pixelShader)
		{
			uint8 colorMask = pixelShader->pixelColorOutputMask;
			uint32 targetMask = LatteGPUState.contextNew.CB_TARGET_MASK.get_MASK();
			for (uint32 i = 0; i < LATTE_NUM_COLOR_TARGET; i++)
			{
				if (((targetMask >> (i * 4)) & 0xF) && (colorMask & (1 << i)))
					m_colorWrittenMaskInCurrentEncoder |= (1 << i);
			}
		}
	}

	// Occlusion queries
	if (m_occlusionQuery.m_active)
	    m_occlusionQuery.m_currentIndex = (m_occlusionQuery.m_currentIndex + 1) % OCCLUSION_QUERY_POOL_SIZE;

	// Streamout
	LatteStreamout_FinishDrawcall(m_memoryManager->UseHostMemoryForCache());

	// Debug
	if (fetchVertexManually)
	    m_performanceMonitor.m_manualVertexFetchDraws++;
	if (usesGeometryShader)
	    m_performanceMonitor.m_meshDraws++;
	if (primitiveMode == LattePrimitiveMode::TRIANGLE_FAN)
	    m_performanceMonitor.m_triangleFans++;

	LatteGPUState.drawCallCounter++;
}

void MetalRenderer::draw_endSequence()
{
    LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	// post-drawcall logic
	if (pixelShader && m_state.m_sequenceRasterized)
	{
		LatteRenderTarget_trackUpdates();
		TrackActiveAttachmentWritesForCurrentEncoder();
	}
	bool hasReadback = LatteTextureReadback_Update();
	m_recordedDrawcalls++;
	// Submit bounded batches so CPU encoding can overlap GPU execution.
	// Submit stable readback batches early so GX2DrawDone need not wait
	// for later rendering too. Mandatory completion remains unchanged.
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	if ((hasReadback && ShouldSubmitReadbacksEarly()) || m_recordedDrawcalls >= m_commitTreshold * 2 ||
        (m_recordedDrawcalls >= m_commitTreshold && m_encoderType != MetalEncoderType::Render))
#else
	if (hasReadback || m_recordedDrawcalls >= m_commitTreshold * 2 ||
        (m_recordedDrawcalls >= m_commitTreshold && m_encoderType != MetalEncoderType::Render))
#endif
	{
		CommitCommandBuffer();
		LatteTextureReadback_UpdateFinishedTransfers(false);
	}
}

void MetalRenderer::draw_updateVertexBuffersDirectAccess()
{
	LatteFetchShader* parsedFetchShader = LatteSHRC_GetActiveFetchShader();
	if (!parsedFetchShader)
		return;

	for (auto& bufferGroup : parsedFetchShader->bufferGroups)
	{
		uint32 bufferIndex = bufferGroup.attributeBufferIndex;
		uint32 bufferBaseRegisterIndex = mmSQ_VTX_ATTRIBUTE_BLOCK_START + bufferIndex * 7;
		MPTR bufferAddress = LatteGPUState.contextRegister[bufferBaseRegisterIndex + 0];

		if (bufferAddress == MPTR_NULL) [[unlikely]]
			bufferAddress = m_memoryManager->GetImportedMemBaseAddress();

		m_state.m_vertexBufferOffsets[bufferIndex] = bufferAddress - m_memoryManager->GetImportedMemBaseAddress();
	}
}

void MetalRenderer::draw_updateUniformBuffersDirectAccess(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset)
{
	if (shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
	{
		for (const auto& buf : shader->list_quickBufferList)
		{
			sint32 i = buf.index;
			MPTR physicalAddr = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 0];
			uint32 uniformSize = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 1] + 1;

			if (physicalAddr == MPTR_NULL) [[unlikely]]
			{
				cemu_assert_unimplemented();
				continue;
			}
			uniformSize = std::min<uint32>(uniformSize, buf.size);

			cemu_assert_debug(physicalAddr < 0x50000000);

			uint32 bufferIndex = i;
			cemu_assert_debug(bufferIndex < 16);

			m_state.m_uniformBufferOffsets[GetMtlGeneralShaderType(shader->shaderType)][bufferIndex] = physicalAddr - m_memoryManager->GetImportedMemBaseAddress();
		}
	}
}

void MetalRenderer::draw_handleSpecialState5()
{
    LatteMRT::UpdateCurrentFBO();
    LatteRenderTarget_updateViewport();

    LatteTextureView* colorBuffer = LatteMRT::GetColorAttachment(0);
    LatteTextureView* depthBuffer = LatteMRT::GetDepthAttachment();
    if (!colorBuffer || !depthBuffer || !colorBuffer->baseTexture || !depthBuffer->baseTexture)
        return;

    sint32 vpWidth, vpHeight;
    LatteMRT::GetVirtualViewportDimensions(vpWidth, vpHeight);

    // This utility pipeline has only a color attachment. Reusing the game's
    // FBO also attaches the sampled depth texture and violates the pipeline's
    // depth format. The conversion path stores depth before sampling it in a
    // separate color-only pass, using the requested mip and slice.
    surfaceCopy_copySurfaceWithFormatConversion(
        depthBuffer->baseTexture, depthBuffer->firstMip, depthBuffer->firstSlice,
        colorBuffer->baseTexture, colorBuffer->firstMip, colorBuffer->firstSlice,
        vpWidth, vpHeight);
}

Renderer::IndexAllocation MetalRenderer::indexData_reserveIndexMemory(uint32 size)
{
    auto allocation = m_memoryManager->GetIndexAllocator().AllocateBufferMemory(size, 128, m_optimizeZeldaIndexHeap);

    return {allocation->memPtr, allocation};
}

void MetalRenderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
    m_memoryManager->GetIndexAllocator().FreeReservation(static_cast<MetalSynchronizedHeapAllocator::AllocatorReservation*>(allocation.rendererInternal));
}

void MetalRenderer::indexData_uploadIndexMemory(IndexAllocation& allocation)
{
    m_memoryManager->GetIndexAllocator().FlushReservation(static_cast<MetalSynchronizedHeapAllocator::AllocatorReservation*>(allocation.rendererInternal));
}

LatteQueryObject* MetalRenderer::occlusionQuery_create() {
	return new LatteQueryObjectMtl(this);
}

void MetalRenderer::occlusionQuery_destroy(LatteQueryObject* queryObj) {
    auto queryObjMtl = static_cast<LatteQueryObjectMtl*>(queryObj);
    delete queryObjMtl;
}

bool MetalRenderer::RequiresAccurateBarriers() const
{
    return GetConfig().vk_accurate_barriers;
}

bool MetalRenderer::RequiresAccurateQueryResults() const
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // Splatoon uses visibility counters for ink gameplay. A fabricated hit
    // classifies even unpainted ground as enemy ink. Keep the v2 policy for
    // other titles so this correction does not add waits to their render loop.
    switch (CafeSystem::GetForegroundTitleId())
    {
    case 0x0005000010176900ULL: // USA
    case 0x0005000010176a00ULL: // EUR
    case 0x0005000010162b00ULL: // JPN
    case 0x00050002101ce400ULL: // Testfire
        return true;
    default:
        break;
    }
#endif
    return RequiresAccurateBarriers();
}

void MetalRenderer::occlusionQuery_flush() {
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    if (!RequiresAccurateQueryResults())
        return;
#endif
    auto* commandBuffer = m_occlusionQuery.m_lastCommandBuffer;
    if (!commandBuffer || CommandBufferCompleted(commandBuffer))
        return;

    // Mandatory GX2 completion is independent of optional attachment barriers.
    // The last query may still belong to the open buffer; waiting before its
    // submission would deadlock (or fail Metal validation).
    if (IsCommandBufferActive() && GetCurrentCommandBuffer() == commandBuffer)
        CommitCommandBuffer();
    commandBuffer->waitUntilCompleted();
}

void MetalRenderer::occlusionQuery_updateState() {
    ProcessFinishedCommandBuffers();
}

void MetalRenderer::SetBuffer(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Buffer* buffer, size_t offset, uint32 index)
{
    if (!m_supportsMeshShaders && (shaderType == METAL_SHADER_TYPE_OBJECT || shaderType == METAL_SHADER_TYPE_MESH))
        shaderType = METAL_SHADER_TYPE_VERTEX;
    if (!renderCommandEncoder || !buffer)
        return;
    auto& boundBuffer = m_state.m_encoderState.m_buffers[shaderType][index];
    if (buffer == boundBuffer.m_buffer && offset == boundBuffer.m_offset)
        return;

    if (buffer == boundBuffer.m_buffer)
    {
        // Update just the offset
        boundBuffer.m_offset = offset;

        switch (shaderType)
        {
        case METAL_SHADER_TYPE_VERTEX:
            renderCommandEncoder->setVertexBufferOffset(offset, index);
            break;
        case METAL_SHADER_TYPE_OBJECT:
            renderCommandEncoder->setObjectBufferOffset(offset, index);
            break;
        case METAL_SHADER_TYPE_MESH:
            renderCommandEncoder->setMeshBufferOffset(offset, index);
            break;
        case METAL_SHADER_TYPE_FRAGMENT:
            renderCommandEncoder->setFragmentBufferOffset(offset, index);
            break;
        }

        return;
    }

    boundBuffer = {buffer, offset};

    switch (shaderType)
    {
    case METAL_SHADER_TYPE_VERTEX:
        renderCommandEncoder->setVertexBuffer(buffer, offset, index);
        break;
    case METAL_SHADER_TYPE_OBJECT:
        renderCommandEncoder->setObjectBuffer(buffer, offset, index);
        break;
    case METAL_SHADER_TYPE_MESH:
        renderCommandEncoder->setMeshBuffer(buffer, offset, index);
        break;
    case METAL_SHADER_TYPE_FRAGMENT:
        renderCommandEncoder->setFragmentBuffer(buffer, offset, index);
        break;
    }
}

void MetalRenderer::SetTexture(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Texture* texture, uint32 index)
{
    if (!m_supportsMeshShaders && (shaderType == METAL_SHADER_TYPE_OBJECT || shaderType == METAL_SHADER_TYPE_MESH))
        shaderType = METAL_SHADER_TYPE_VERTEX;
    auto& boundTexture = m_state.m_encoderState.m_textures[shaderType][index];
    if (texture == boundTexture)
        return;

    boundTexture = texture;

    switch (shaderType)
    {
    case METAL_SHADER_TYPE_VERTEX:
        renderCommandEncoder->setVertexTexture(texture, index);
        break;
    case METAL_SHADER_TYPE_OBJECT:
        renderCommandEncoder->setObjectTexture(texture, index);
        break;
    case METAL_SHADER_TYPE_MESH:
        renderCommandEncoder->setMeshTexture(texture, index);
        break;
    case METAL_SHADER_TYPE_FRAGMENT:
        renderCommandEncoder->setFragmentTexture(texture, index);
        break;
    }
}

void MetalRenderer::SetSamplerState(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::SamplerState* samplerState, uint32 index)
{
    if (!m_supportsMeshShaders && (shaderType == METAL_SHADER_TYPE_OBJECT || shaderType == METAL_SHADER_TYPE_MESH))
        shaderType = METAL_SHADER_TYPE_VERTEX;
    auto& boundSamplerState = m_state.m_encoderState.m_samplers[shaderType][index];
    if (samplerState == boundSamplerState)
        return;

    boundSamplerState = samplerState;

    switch (shaderType)
    {
    case METAL_SHADER_TYPE_VERTEX:
        renderCommandEncoder->setVertexSamplerState(samplerState, index);
        break;
    case METAL_SHADER_TYPE_OBJECT:
        renderCommandEncoder->setObjectSamplerState(samplerState, index);
        break;
    case METAL_SHADER_TYPE_MESH:
        renderCommandEncoder->setMeshSamplerState(samplerState, index);
        break;
    case METAL_SHADER_TYPE_FRAGMENT:
        renderCommandEncoder->setFragmentSamplerState(samplerState, index);
        break;
    }
}

MTL::CommandBuffer* MetalRenderer::GetCommandBuffer()
{
    bool needsNewCommandBuffer = (!m_currentCommandBuffer.m_commandBuffer || m_currentCommandBuffer.m_commited);
    if (needsNewCommandBuffer)
	{
        // Debug
        //m_commandQueue->insertDebugCaptureBoundary();

        auto pool = NS::AutoreleasePool::alloc()->init();
	    MTL::CommandBuffer* mtlCommandBuffer = m_commandQueue->commandBuffer()->retain();
		pool->release();
		m_currentCommandBuffer = {mtlCommandBuffer};
        ++m_commandBufferSerial;

		// Wait for the previous command buffer (only when strict barrier synchronization is requested)
		if (m_eventValue != 0 && GetConfig().vk_accurate_barriers)
		    mtlCommandBuffer->encodeWait(m_event, m_eventValue);

		m_recordedDrawcalls = 0;
		m_commitTreshold = m_defaultCommitTreshlod;

        // Debug
        m_performanceMonitor.m_commandBuffers++;

		return mtlCommandBuffer;
	}
	else
	{
	    return m_currentCommandBuffer.m_commandBuffer;
	}
}

MTL::RenderCommandEncoder* MetalRenderer::GetTemporaryRenderCommandEncoder(MTL::RenderPassDescriptor* renderPassDescriptor)
{
    EndEncoding();

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    auto renderCommandEncoder = commandBuffer->renderCommandEncoder(renderPassDescriptor)->retain();
    pool->release();
#ifdef CEMU_DEBUG_ASSERT
    renderCommandEncoder->setLabel(GetLabel("Temporary render command encoder", renderCommandEncoder));
#endif
	m_commandEncoder = renderCommandEncoder;
	m_encoderType = MetalEncoderType::Render;
	++m_renderEncoderSerial;
	BeginRenderEncoderDiagnosticPass();

    // Debug
    m_performanceMonitor.m_renderPasses++;

    return renderCommandEncoder;
}

bool MetalRenderer::CanReuseRenderPassAttachments()
{
    const auto* active = m_state.m_activeFBO.m_fbo;
    const auto* physical = m_state.m_lastUsedFBO.m_fbo;
    if (!active || !physical || active->m_size != physical->m_size)
        return false;

    // Keep depth/stencil boundaries unchanged, or allow an omitted depth target
    // to remain attached with writes disabled by depth-stencil state.
    if (active->depthBuffer.texture)
    {
        auto* activeDepthMtl = static_cast<LatteTextureViewMtl*>(active->depthBuffer.texture);
        auto* physDepthMtl = static_cast<LatteTextureViewMtl*>(physical->depthBuffer.texture);
        if (!physDepthMtl ||
            (activeDepthMtl != physDepthMtl && activeDepthMtl->GetRGBAView() != physDepthMtl->GetRGBAView()) ||
            active->depthBuffer.hasStencil != physical->depthBuffer.hasStencil)
            return false;
    }
    else if (active->depthBuffer.hasStencil && !physical->depthBuffer.texture)
    {
        return false;
    }

    uint8 physicalColorMask = 0;
    for (uint8 slot = 0; slot < LATTE_NUM_COLOR_TARGET; ++slot)
    {
        auto* oldView = static_cast<LatteTextureViewMtl*>(physical->colorBuffer[slot].texture);
        auto* newView = static_cast<LatteTextureViewMtl*>(active->colorBuffer[slot].texture);
        if (oldView)
            physicalColorMask |= uint8(1u << slot);
        if (newView)
        {
            // Distinct guest view objects can still bind exactly the same Metal
            // subresource. Adding or replacing a physical target ends the pass.
            if (!oldView || newView->GetRGBAView() != oldView->GetRGBAView())
                return false;
        }
    }
    if (!physicalColorMask && !physical->depthBuffer.texture)
        return true;

    // An attachment sampled through a texture must be stored before reading.
    // Check the entire physical set, including a just-reactivated target. This
    // guard also runs when only the shader changes, and remains mandatory when
    // the optional accurate-barrier setting is off.
    LatteDecompilerShader* shaders[] = {LatteSHRC_GetActiveVertexShader(),
        LatteSHRC_GetActivePixelShader(), LatteSHRC_GetActiveGeometryShader()};
    for (const auto* shader : shaders)
    {
        if (!shader)
            continue;
        uint32 stageBase;
        switch (shader->shaderType)
        {
        case LatteConst::ShaderType::Vertex: stageBase = LATTE_CEMU_VS_TEX_UNIT_BASE; break;
        case LatteConst::ShaderType::Pixel: stageBase = LATTE_CEMU_PS_TEX_UNIT_BASE; break;
        case LatteConst::ShaderType::Geometry: stageBase = LATTE_CEMU_GS_TEX_UNIT_BASE; break;
        default: return false;
        }
        for (uint8 i = 0; i < shader->textureUnitListCount; ++i)
        {
            const auto* sampled = m_state.m_textures[stageBase + shader->textureUnitList[i]];
            if (!sampled)
                continue;
            const uint8 fetchTarget = shader->textureRenderTargetIndex[shader->textureUnitList[i]];
            if (shader->shaderType == LatteConst::ShaderType::Pixel && m_supportsFramebufferFetch &&
                fetchTarget < LATTE_NUM_COLOR_TARGET && active->colorBuffer[fetchTarget].texture &&
                physical->colorBuffer[fetchTarget].texture->baseTexture == sampled->baseTexture)
                continue; // This read comes from the still-active attachment, not a texture.
            for (uint8 slot = 0; slot < LATTE_NUM_COLOR_TARGET; ++slot)
            {
                if ((physicalColorMask & (1u << slot)) &&
                    physical->colorBuffer[slot].texture->baseTexture == sampled->baseTexture)
                    return false;
            }
            if (physical->depthBuffer.texture &&
                physical->depthBuffer.texture->baseTexture == sampled->baseTexture)
                return false;
        }
    }
    return true;
}

// Some render passes clear the attachments, forceRecreate is supposed to be used in those cases
MTL::RenderCommandEncoder* MetalRenderer::GetRenderCommandEncoder(bool forceRecreate)
{
    bool fboChanged = m_state.m_fboChanged;
    m_state.m_fboChanged = false;
	if (fboChanged)
		LatteGPUState.telemetry.metalFBOChanges.fetch_add(1, std::memory_order_relaxed);

    // Check if we need to begin a new render pass
    if (m_commandEncoder)
    {
        bool pendingClearBreak = false;
        bool attachmentBreak = false;
        if (!forceRecreate)
        {
            if (m_encoderType == MetalEncoderType::Render)
            {
                bool needsNewRenderPass = false;
                if (fboChanged || m_state.m_activeFBO.m_fbo != m_state.m_lastUsedFBO.m_fbo)
                {
                    needsNewRenderPass = !CanReuseRenderPassAttachments();
                }

                attachmentBreak = needsNewRenderPass;
                if (!needsNewRenderPass && PendingClearAffectsFBO(m_state.m_activeFBO.m_fbo))
                {
                    needsNewRenderPass = true;
                    pendingClearBreak = true;
                }

                if (!needsNewRenderPass)
                {
					if (fboChanged)
					{
						LatteGPUState.telemetry.metalEncoderReusesAfterFBOChange.fetch_add(1, std::memory_order_relaxed);
						TrackRetainedAttachmentsForReusedEncoder();
						// Keep the physical attachment set for PSO formats and for
						// detecting later reads of an omitted target.
					}
                    return (MTL::RenderCommandEncoder*)m_commandEncoder;
                }
            }
        }

        ++g_pass_fboChange;
        if (attachmentBreak)
        {
            ++g_pass_attachmentChange;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
            if (s_sampleFrameTransitions && m_state.m_activeFBO.m_fbo && m_state.m_lastUsedFBO.m_fbo)
            {
                for (uint32 slot = 0; slot < 8; ++slot)
                    RecordAttachmentTransition(slot,
                        m_state.m_lastUsedFBO.m_fbo->colorBuffer[slot].texture,
                        m_state.m_activeFBO.m_fbo->colorBuffer[slot].texture);
                RecordAttachmentTransition(8,
                    m_state.m_lastUsedFBO.m_fbo->depthBuffer.texture,
                    m_state.m_activeFBO.m_fbo->depthBuffer.texture);
            }
#endif
        }
        else if (pendingClearBreak)
            ++g_pass_pendingClear;
        else if (m_encoderType != MetalEncoderType::Render)
            ++g_pass_otherEncoder;
        EndEncoding();
    }

    auto commandBuffer = GetCommandBuffer();

    LatteTexture_FlushPendingDynamicReloads(true);
    if (m_commandEncoder && m_encoderType == MetalEncoderType::Blit)
    {
        EndEncoding();
    }

    ResolvePendingClearsForRenderPass();

    auto pool = NS::AutoreleasePool::alloc()->init();
    auto renderPassDescriptor = m_state.m_activeFBO.m_fbo->GetRenderPassDescriptor();
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    if (s_sampleFrameTransitions)
    {
        const auto* fbo = static_cast<CachedFBOMtl*>(m_state.m_activeFBO.m_fbo);
        s_sampleAttachmentBytes += fbo->GetEstimatedColorBytes() + fbo->GetEstimatedDepthBytes();
    }
#endif
    auto renderCommandEncoder = commandBuffer->renderCommandEncoder(renderPassDescriptor)->retain();
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    if (m_capturing || s_sampleFrameTransitions)
    {
        const auto* fbo = m_state.m_activeFBO.m_fbo;
        const auto* color = fbo->colorBuffer[0].texture;
        const auto* depth = fbo->depthBuffer.texture;
        renderCommandEncoder->setLabel(ToNSString(fmt::format(
            "GX2 pass {} color0:{:08x} depth:{:08x}", m_performanceMonitor.m_renderPasses,
            color ? color->baseTexture->physAddress : 0,
            depth ? depth->baseTexture->physAddress : 0)));
    }
#endif
    RestoreFoldedLoadActions(renderPassDescriptor);
    pool->release();
#ifdef CEMU_DEBUG_ASSERT
    renderCommandEncoder->setLabel(GetLabel("Render command encoder", renderCommandEncoder));
#endif
	m_commandEncoder = renderCommandEncoder;
	m_encoderType = MetalEncoderType::Render;
	++m_renderEncoderSerial;
	m_depthWrittenInCurrentEncoder = false;
	m_colorWrittenMaskInCurrentEncoder = 0;
	BeginRenderEncoderDiagnosticPass();

    // Track active attachments load/store actions for this pass
    if (m_state.m_activeFBO.m_fbo)
    {
        for (int i = 0; i < 8; ++i)
        {
            if (m_state.m_activeFBO.m_fbo->colorBuffer[i].texture)
            {
                LatteGPUState.telemetry.loadActions.fetch_add(1, std::memory_order_relaxed);
                LatteGPUState.telemetry.storeActions.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (m_state.m_activeFBO.m_fbo->depthBuffer.texture)
        {
            LatteGPUState.telemetry.loadActions.fetch_add(1, std::memory_order_relaxed);
            LatteGPUState.telemetry.storeActions.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Update state
    m_state.m_lastUsedFBO = m_state.m_activeFBO;
    m_state.m_isFirstDrawInRenderPass = true;

    ResetEncoderState();

    // Debug
    m_performanceMonitor.m_renderPasses++;

    return renderCommandEncoder;
}

MTL::ComputeCommandEncoder* MetalRenderer::GetComputeCommandEncoder()
{
    if (m_commandEncoder)
    {
        if (m_encoderType == MetalEncoderType::Compute)
        {
            return (MTL::ComputeCommandEncoder*)m_commandEncoder;
        }

        EndEncoding();
    }

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    auto computeCommandEncoder = commandBuffer->computeCommandEncoder()->retain();
    pool->release();
    m_commandEncoder = computeCommandEncoder;
    m_encoderType = MetalEncoderType::Compute;

    ResetEncoderState();

    return computeCommandEncoder;
}

MTL::BlitCommandEncoder* MetalRenderer::GetBlitCommandEncoder()
{
    if (m_commandEncoder)
    {
        if (m_encoderType == MetalEncoderType::Blit)
        {
            return (MTL::BlitCommandEncoder*)m_commandEncoder;
        }

        ++g_pass_blit;
        EndEncoding();
    }

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    auto blitCommandEncoder = commandBuffer->blitCommandEncoder()->retain();
    pool->release();
    m_commandEncoder = blitCommandEncoder;
    m_encoderType = MetalEncoderType::Blit;

    ResetEncoderState();

    return blitCommandEncoder;
}

void MetalRenderer::EndEncoding()
{
    if (m_commandEncoder)
    {
        // Keep the stored picture. DontCare on a loaded color target makes Zelda
        // water sample white and the inventory blend over black.

        m_commandEncoder->endEncoding();
        m_commandEncoder->release();
        m_commandEncoder = nullptr;
        m_encoderType = MetalEncoderType::None;

        // Submission belongs at draw_endSequence(), not inside an encoder switch.
        // Upload reservations already refer to this command buffer; submitting here
        // could retire them before the operation that requested the switch uses them.
    }

	m_depthWrittenInCurrentEncoder = false;
	m_colorWrittenMaskInCurrentEncoder = 0;
	m_loadClearedColorMask = 0;
	m_loadClearedDepth = false;
	m_loadClearedStencil = false;
	ResetRetainedAttachmentDiagnostics();
}

void MetalRenderer::ResetRetainedAttachmentDiagnostics()
{
	m_retainedAttachmentDiagnosticCount = 0;
	m_retainedAttachmentActiveFBOIdentity = 0;
	m_retainedAttachmentLastUsedFBOIdentity = 0;
}

void MetalRenderer::BeginRenderEncoderDiagnosticPass()
{
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
	++m_renderEncoderDiagnosticGeneration;
	if (m_renderEncoderDiagnosticGeneration == 0)
		++m_renderEncoderDiagnosticGeneration;
	for (auto& write : m_activeAttachmentWriteDiagnostics)
		write = {};
#endif
}

void MetalRenderer::TrackActiveAttachmentWritesForCurrentEncoder()
{
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
	if (!m_commandEncoder || m_encoderType != MetalEncoderType::Render || !m_state.m_activeFBO.m_fbo)
		return;
	if (LatteGPUState.telemetry.activeAttachmentSampleCaptureCount.load(std::memory_order_relaxed) >= 16)
		return;

	auto recordWrite = [this](LatteTextureView* view, uint32 slot)
	{
		if (!view)
			return;
		auto* baseTexture = view->baseTexture;
		if (baseTexture->lastUnflushedRTDrawcallIndex != LatteGPUState.drawCallCounter)
			return;

		auto& write = m_activeAttachmentWriteDiagnostics[slot];
		write.baseTexture = baseTexture;
		write.encoderGeneration = m_renderEncoderDiagnosticGeneration;
		write.lastWriteDrawIndex = baseTexture->lastUnflushedRTDrawcallIndex;
		write.lastWriteGeneration = baseTexture->lastGPUWriteGeneration;
	};

	for (uint32 i = 0; i < LATTE_NUM_COLOR_TARGET; ++i)
		recordWrite(m_state.m_activeFBO.m_fbo->colorBuffer[i].texture, i);
	recordWrite(m_state.m_activeFBO.m_fbo->depthBuffer.texture, 8);
#endif
}

void MetalRenderer::RecordActiveAttachmentSample(LatteDecompilerShader* shader, uint32 textureUnit, LatteTextureViewMtl* sampledView, MTL::Texture* sampledMetalView)
{
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
	if (!shader || !sampledView || !sampledMetalView || !m_commandEncoder || m_encoderType != MetalEncoderType::Render || !m_state.m_activeFBO.m_fbo)
		return;
	if (LatteGPUState.telemetry.activeAttachmentSampleCaptureCount.load(std::memory_order_relaxed) >= 16)
		return;

	auto& telemetry = LatteGPUState.telemetry;
	telemetry.metalNormalTextureBindsWhileRenderEncoderOpen.fetch_add(1, std::memory_order_relaxed);

	auto* sampledBase = sampledView->baseTexture;
	auto* activeFBO = m_state.m_activeFBO.m_fbo;

	auto inspectAttachment = [&](LatteTextureView* attachmentView, uint32 attachmentType, uint32 attachmentIndex, uint32 writeSlot) -> bool
	{
		if (!attachmentView || attachmentView->baseTexture != sampledBase)
			return false;

		telemetry.metalActiveAttachmentBaseMatches.fetch_add(1, std::memory_order_relaxed);
		const auto& write = m_activeAttachmentWriteDiagnostics[writeSlot];
		const bool writtenThisEncoder = write.baseTexture == sampledBase &&
			write.encoderGeneration == m_renderEncoderDiagnosticGeneration &&
			write.lastWriteGeneration != 0;
		if (!writtenThisEncoder)
			return true;

		telemetry.metalActiveAttachmentSamplesAfterWrite.fetch_add(1, std::memory_order_relaxed);
		if (telemetry.activeAttachmentSampleCaptureCount.load(std::memory_order_relaxed) >= 16)
			return true;

		uint64 dedupKey = sampledBase->hostTextureObjectId;
		auto mixKey = [&dedupKey](uint64 value)
		{
			dedupKey ^= value + 0x9E3779B97F4A7C15ull + (dedupKey << 6) + (dedupKey >> 2);
		};
		mixKey(shader->baseHash);
		mixKey((uint32)shader->shaderType);
		mixKey(textureUnit);
		mixKey(attachmentType);
		mixKey(attachmentIndex);
		mixKey((uint32)sampledView->firstMip);
		mixKey((uint32)sampledView->firstSlice);
		mixKey((uint32)attachmentView->firstMip);
		mixKey((uint32)attachmentView->firstSlice);

		if (telemetry.activeAttachmentSampleCaptureLock.test_and_set(std::memory_order_acquire))
			return true;

		uint32 count = telemetry.activeAttachmentSampleCaptureCount.load(std::memory_order_relaxed);
		for (uint32 i = 0; i < count && i < 16; ++i)
		{
			if (telemetry.activeAttachmentSampleDedupKeys[i] == dedupKey)
			{
				telemetry.activeAttachmentSampleCaptureLock.clear(std::memory_order_release);
				return true;
			}
		}

		if (count < 16)
		{
			auto mipWidth = [](LatteTextureView* view) -> uint32
			{
				return std::max<uint32>(1, (uint32)view->baseTexture->width >> view->firstMip);
			};
			auto mipHeight = [](LatteTextureView* view) -> uint32
			{
				return std::max<uint32>(1, (uint32)view->baseTexture->height >> view->firstMip);
			};

			telemetry.activeAttachmentSampleDedupKeys[count] = dedupKey;
			auto& event = telemetry.activeAttachmentSampleLog[count];
			event.drawIndex = LatteGPUState.drawCallCounter;
			event.shaderStage = (uint32)shader->shaderType;
			event.textureUnit = textureUnit;
			event.shaderHash = shader->baseHash;
			event.attachmentType = attachmentType;
			event.attachmentIndex = attachmentIndex;
			event.baseTextureObjectId = sampledBase->hostTextureObjectId;
			event.guestBaseAddress = sampledBase->physAddress;
			event.sampledMip = sampledView->firstMip;
			event.sampledMipCount = sampledView->numMip;
			event.sampledSlice = sampledView->firstSlice;
			event.sampledSliceCount = sampledView->numSlice;
			event.sampledWidth = mipWidth(sampledView);
			event.sampledHeight = mipHeight(sampledView);
			event.sampledFormat = (uint32)sampledView->format;
			event.attachmentMip = attachmentView->firstMip;
			event.attachmentSlice = attachmentView->firstSlice;
			event.attachmentWidth = mipWidth(attachmentView);
			event.attachmentHeight = mipHeight(attachmentView);
			event.attachmentFormat = (uint32)attachmentView->format;
			event.encoderGeneration = m_renderEncoderDiagnosticGeneration;
			event.lastRTWriteDrawIndex = write.lastWriteDrawIndex;
			event.lastRTWriteGeneration = write.lastWriteGeneration;
			event.sampledMetalBasePointer = (uint64)(uintptr_t)static_cast<LatteTextureMtl*>(sampledBase)->GetTexture();
			event.sampledMetalViewPointer = (uint64)(uintptr_t)sampledMetalView;
			event.attachmentMetalBasePointer = (uint64)(uintptr_t)static_cast<LatteTextureMtl*>(attachmentView->baseTexture)->GetTexture();
			event.attachmentMetalViewPointer = (uint64)(uintptr_t)static_cast<LatteTextureViewMtl*>(attachmentView)->GetRGBAView();
			event.attachmentWrittenThisEncoder = true;
			event.framebufferFetchClassificationAbsent = true;
			telemetry.activeAttachmentSampleCaptureCount.store(count + 1, std::memory_order_release);
		}

		telemetry.activeAttachmentSampleCaptureLock.clear(std::memory_order_release);
		return true;
	};

	for (uint32 i = 0; i < LATTE_NUM_COLOR_TARGET; ++i)
	{
		if (inspectAttachment(activeFBO->colorBuffer[i].texture, 0, i, i))
			return;
	}
	if (activeFBO->depthBuffer.texture)
	{
		const uint32 attachmentType = activeFBO->depthBuffer.hasStencil ? 2 : 1;
		inspectAttachment(activeFBO->depthBuffer.texture, attachmentType, 0, 8);
	}
#endif
}

void MetalRenderer::TrackRetainedAttachmentsForReusedEncoder()
{
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
	ResetRetainedAttachmentDiagnostics();

	auto* activeFBO = m_state.m_activeFBO.m_fbo;
	auto* lastUsedFBO = m_state.m_lastUsedFBO.m_fbo;
	if (!activeFBO || !lastUsedFBO)
		return;

	m_retainedAttachmentActiveFBOIdentity = (uint64)(uintptr_t)activeFBO;
	m_retainedAttachmentLastUsedFBOIdentity = (uint64)(uintptr_t)lastUsedFBO;

	auto addAttachment = [this](LatteTextureView* view, uint8 attachmentType, uint8 attachmentIndex)
	{
		if (!view || m_retainedAttachmentDiagnosticCount >= std::size(m_retainedAttachmentDiagnostics))
			return;

		auto* texture = view->baseTexture;
		auto& entry = m_retainedAttachmentDiagnostics[m_retainedAttachmentDiagnosticCount++];
		entry.baseTexture = texture;
		entry.guestBaseAddress = texture->physAddress;
		entry.width = texture->width;
		entry.height = texture->height;
		entry.pitch = texture->pitch;
		entry.format = (uint32)view->format;
		entry.tileMode = (uint32)texture->tileMode;
		entry.mip = (uint32)view->firstMip;
		entry.slice = (uint32)view->firstSlice;
		entry.attachmentType = attachmentType;
		entry.attachmentIndex = attachmentIndex;
	};

	for (uint8 i = 0; i < LATTE_NUM_COLOR_TARGET; ++i)
	{
		auto* oldAttachment = lastUsedFBO->colorBuffer[i].texture;
		auto* newAttachment = activeFBO->colorBuffer[i].texture;
		if (oldAttachment && !newAttachment)
			addAttachment(oldAttachment, 0, i);
	}

	if (lastUsedFBO->depthBuffer.texture && !activeFBO->depthBuffer.texture)
	{
		const uint8 attachmentType = lastUsedFBO->depthBuffer.hasStencil ? 2 : 1;
		addAttachment(lastUsedFBO->depthBuffer.texture, attachmentType, 0);
	}

	if (m_retainedAttachmentDiagnosticCount != 0)
	{
		LatteGPUState.telemetry.metalRemovedAttachmentsRetainedDuringReuse.fetch_add(
			m_retainedAttachmentDiagnosticCount, std::memory_order_relaxed);
	}
#endif
}

void MetalRenderer::RecordStaleAttachmentSample(LatteDecompilerShader* shader, uint32 textureUnit, const MetalRetainedAttachmentDiagnostic& attachment)
{
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
	auto& telemetry = LatteGPUState.telemetry;
	telemetry.metalStaleAttachmentSampleHazards.fetch_add(1, std::memory_order_relaxed);

	if (telemetry.staleAttachmentSampleCaptureCount.load(std::memory_order_relaxed) >= 16)
		return;

	uint64 dedupKey = (uint64)(uintptr_t)attachment.baseTexture;
	auto mixKey = [&dedupKey](uint64 value)
	{
		dedupKey ^= value + 0x9E3779B97F4A7C15ull + (dedupKey << 6) + (dedupKey >> 2);
	};
	mixKey((uint32)shader->shaderType);
	mixKey(textureUnit);
	mixKey(attachment.attachmentType);
	mixKey(attachment.attachmentIndex);
	mixKey(m_retainedAttachmentActiveFBOIdentity);
	mixKey(m_retainedAttachmentLastUsedFBOIdentity);

	if (telemetry.staleAttachmentSampleCaptureLock.test_and_set(std::memory_order_acquire))
		return;

	uint32 count = telemetry.staleAttachmentSampleCaptureCount.load(std::memory_order_relaxed);
	for (uint32 i = 0; i < count && i < 16; ++i)
	{
		if (telemetry.staleAttachmentSampleDedupKeys[i] == dedupKey)
		{
			telemetry.staleAttachmentSampleCaptureLock.clear(std::memory_order_release);
			return;
		}
	}

	if (count < 16)
	{
		telemetry.staleAttachmentSampleDedupKeys[count] = dedupKey;
		auto& event = telemetry.staleAttachmentSampleLog[count];
		event.drawIndex = LatteGPUState.drawCallCounter;
		event.shaderStage = (uint32)shader->shaderType;
		event.textureUnit = textureUnit;
		event.formerAttachmentType = attachment.attachmentType;
		event.formerAttachmentIndex = attachment.attachmentIndex;
		event.guestBaseAddress = attachment.guestBaseAddress;
		event.width = attachment.width;
		event.height = attachment.height;
		event.pitch = attachment.pitch;
		event.format = attachment.format;
		event.tileMode = attachment.tileMode;
		event.mip = attachment.mip;
		event.slice = attachment.slice;
		event.activeFBOIdentity = m_retainedAttachmentActiveFBOIdentity;
		event.lastUsedFBOIdentity = m_retainedAttachmentLastUsedFBOIdentity;
		event.encoderReused = true;
		event.isDepthStencilAttachment = attachment.attachmentType != 0;
		telemetry.staleAttachmentSampleCaptureCount.store(count + 1, std::memory_order_release);
	}

	telemetry.staleAttachmentSampleCaptureLock.clear(std::memory_order_release);
#endif
}

void MetalRenderer::CommitCommandBuffer(bool endFrame)
{
    if (m_committingCommandBuffer)
        return;
    // Drawable acquisition can fail after its GPU prefix has been submitted.
    // Close that frame once, including when completion already retired it.
    if (endFrame && m_currentCommandBuffer.m_commandBuffer &&
        m_currentCommandBuffer.m_commited && !m_currentCommandBuffer.m_frameEndMarked)
    {
        m_currentCommandBuffer.m_frameEndMarked = true;
        auto* buffer = m_currentCommandBuffer.m_commandBuffer;
        if (std::find(m_executingCommandBuffers.begin(), m_executingCommandBuffers.end(), buffer) != m_executingCommandBuffers.end())
            m_frameEndCommandBuffers.insert(buffer);
        else
            FinishGpuFrameTiming();
    }
    if (!m_currentCommandBuffer.m_commandBuffer && m_pendingClears.empty())
        return;

    m_committingCommandBuffer = true;
    EndEncoding();
    g_pass_commitClears += static_cast<uint32>(m_pendingClears.size());
    FlushAllPendingClears();
    if (m_memoryManager)
        m_memoryManager->FlushDeferredUploads();
    EndEncoding();
    if (!m_currentCommandBuffer.m_commandBuffer)
    {
        m_committingCommandBuffer = false;
        return;
    }

    ProcessFinishedCommandBuffers();

    // Commit the command buffer
    if (!m_currentCommandBuffer.m_commited)
    {
        // Handled differently, since it seems like Metal doesn't always call the completion handler
        //commandBuffer.m_commandBuffer->addCompletedHandler(^(MTL::CommandBuffer*) {
        //    m_memoryManager->GetTemporaryBufferAllocator().CommandBufferFinished(commandBuffer.m_commandBuffer);
        //});

        // Signal event monotonically
        m_eventValue++;
        auto mtlCommandBuffer = m_currentCommandBuffer.m_commandBuffer;
        mtlCommandBuffer->encodeSignalEvent(m_event, m_eventValue);
        if (endFrame)
        {
            m_frameEndCommandBuffers.insert(mtlCommandBuffer);
            m_currentCommandBuffer.m_frameEndMarked = true;
        }

		MetalResourceCrashEvent event{};
		event.type = MetalResourceCrashEventType::CommandBufferCommit;
		event.commandBufferPtr = reinterpret_cast<uint64>(mtlCommandBuffer);
		event.commandBufferStatus = static_cast<uint32>(mtlCommandBuffer->status());
		event.flags = static_cast<uint32>(m_executingCommandBuffers.size());
		MetalResourceCrashDiagnostics_Record(event);

        mtlCommandBuffer->commit();
        m_currentCommandBuffer.m_commited = true;

        m_executingCommandBuffers.push_back(mtlCommandBuffer);

        // Debug
        //m_commandQueue->insertDebugCaptureBoundary();
    }
    m_committingCommandBuffer = false;
}

void MetalRenderer::FinishGpuFrameTiming()
{
    if (m_pendingGpuFrameValid)
    {
        m_lastCompletedGpuMs = m_pendingGpuFrameMs;
        m_lastCompletedGpuRenderMs = m_pendingGpuRenderMs;
        m_lastCompletedGpuOutputMs = m_pendingGpuOutputMs;
        m_completedGpuWindowMs += m_pendingGpuFrameMs;
        ++m_completedGpuWindowFrames;
    }
    m_pendingGpuFrameMs = 0.0;
    m_pendingGpuRenderMs = 0.0;
    m_pendingGpuOutputMs = 0.0;
    m_pendingGpuFrameValid = true;
}

void MetalRenderer::ProcessFinishedCommandBuffers()
{
    // Check for finished command buffers
    for (auto it = m_executingCommandBuffers.begin(); it != m_executingCommandBuffers.end();)
    {
        auto commandBuffer = *it;
        if (CommandBufferCompleted(commandBuffer))
        {
			MetalResourceCrashEvent event{};
			event.type = commandBuffer->status() == MTL::CommandBufferStatusError
				? MetalResourceCrashEventType::CommandBufferError
				: MetalResourceCrashEventType::CommandBufferComplete;
			event.commandBufferPtr = reinterpret_cast<uint64>(commandBuffer);
			event.commandBufferStatus = static_cast<uint32>(commandBuffer->status());
			if (commandBuffer->status() == MTL::CommandBufferStatusError)
			{
				auto error = commandBuffer->error();
				event.relatedPtr = reinterpret_cast<uint64>(error);
				cemuLog_log(LogType::Force, "[MTL COMMAND BUFFER ERROR] cb={} status={} error={}",
					(void*)commandBuffer, static_cast<uint32>(commandBuffer->status()),
					error ? error->localizedDescription()->utf8String() : "unknown");
			}
			MetalResourceCrashDiagnostics_Record(event);
            const double start = commandBuffer->GPUStartTime();
            const double end = commandBuffer->GPUEndTime();
            const bool finalBuffer = m_frameEndCommandBuffers.erase(commandBuffer) != 0;
            if (commandBuffer->status() == MTL::CommandBufferStatusCompleted && start > 0.0 && end >= start)
            {
                const double elapsedMs = (end - start) * 1000.0;
                m_pendingGpuFrameMs += elapsedMs;
                (finalBuffer ? m_pendingGpuOutputMs : m_pendingGpuRenderMs) += elapsedMs;
            }
            else
                m_pendingGpuFrameValid = false;
            if (finalBuffer)
            {
                FinishGpuFrameTiming();
            }
            m_memoryManager->CleanupBuffers(commandBuffer);
            commandBuffer->release();
            it = m_executingCommandBuffers.erase(it);
        }
        else
        {
            break; // Preserve submission order when accumulating completed frames.
        }
    }
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // BC fallback expands texture storage on A-series GPUs. Retaining the peak
    // upload scratch alongside the live textures leaves no compiler headroom.
    // Poll infrequently, independently of diagnostic logging, on the renderer
    // thread only; never wait for the GPU or touch an in-flight reservation.
    // Safety net on every device: watch the real iOS headroom (what jetsam
    // acts on) and free everything reconstructable before the process is
    // killed. Independent of the 2.5 GiB target.
    if (m_memoryManager && (m_memoryPressurePollCounter & 15u) == 0)
    {
        constexpr size_t kEmergencyHeadroom = 256u * 1024 * 1024;
        const size_t realAvailable = os_proc_available_memory();
        // A device app can reach zero headroom. Simulator may report zero without a limit.
        if (realAvailable < kEmergencyHeadroom && (!TARGET_OS_SIMULATOR || realAvailable != 0))
        {
            LatteTC_RequestEmergencyMemoryCleanup();
            const size_t shaderBytes = RendererShaderMtl::TrimReuseCaches();
            // Staging release is only validated on constrained devices.
            const size_t stagingBytes = GetMetalMemoryPolicy().constrained ? m_memoryManager->GetStagingAllocator().TrimIdleBuffers() : 0;
            static auto s_lastGuardLog = std::chrono::steady_clock::time_point{};
            const auto guardNow = std::chrono::steady_clock::now();
            if (guardNow - s_lastGuardLog >= std::chrono::seconds(2))
            {
                s_lastGuardLog = guardNow;
                cemuLog_log(LogType::Force, "[MEMORY GUARD] iOS headroom {} MiB: emergency cleanup (textures, shaders {} KiB, staging {} MiB)",
                    realAvailable / (1024 * 1024), shaderBytes / 1024, stagingBytes / (1024 * 1024));
            }
        }
    }
    if (m_memoryManager && (++m_memoryPressurePollCounter & 63u) == 0)
    {
        if (GetMetalMemoryPolicy().constrained)
        {
            // Apple7 profile measurements retained 96 MiB of idle staging long
            // after loading. Retire only long-idle, completed excess chunks on
            // the owner thread; never release a live decoder/GPU reservation.
            m_memoryManager->GetStagingAllocator().TrimLongIdleBuffers(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }
        constexpr size_t MiB = 1024u * 1024u;
        // Headroom against min(2.5 GiB, iOS limit - margin).
        const size_t availableBytes = MemoryBudget::EffectiveAvailableMemory(os_proc_available_memory());
        // Start shortly before the target: earlier cleanup deleted textures
        // that were needed again soon, costing uploads and frame rate.
        if (availableBytes < 256u * MiB)
        {
            const size_t releasedBytes = m_memoryManager->GetStagingAllocator().TrimIdleBuffers();
            // Immutable reuse entries are optional. Live shaders own independent
            // function references; compiling entries and executable JIT remain.
            // Only 4/6 GiB devices reclaim them; larger devices keep their existing policy.
            if (GetMetalMemoryPolicy().constrained)
                RendererShaderMtl::TrimReuseCaches();
            LatteTC_RequestMemoryPressureCleanup();
            if (releasedBytes && CemuBridge_IsDiagnosticLoggingEnabled())
                cemuLog_log(LogType::Force,
                    "[METAL MEMORY] availableMiB:{} releasedIdleStagingMiB:{}",
                    availableBytes / MiB, releasedBytes / MiB);
        }
    }
#endif
}

bool MetalRenderer::AcquireDrawable(bool mainWindow)
{
    auto& layer = GetLayer(mainWindow);
    if (!layer.GetLayer())
        return false;
	const bool alreadyAcquired = layer.GetDrawable() != nullptr;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // nextDrawable may block on the display's pool. Submit independent offscreen
    // work first so the GPU can execute it during that wait, on the same queue.
    // Leave the output copy and presentation in the following command buffer.
    if (m_submitBeforeDrawableWait && mainWindow && !alreadyAcquired && IsCommandBufferActive() &&
        (m_commandEncoder || m_recordedDrawcalls || !m_pendingClears.empty() ||
         (m_memoryManager && m_memoryManager->HasDeferredUploads())))
        CommitCommandBuffer();
#endif


    const bool latteBufferUsesSRGB = mainWindow ? LatteGPUState.tvBufferUsesSRGB : LatteGPUState.drcBufferUsesSRGB;
    if (latteBufferUsesSRGB != m_state.m_usesSRGB)
    {
        layer.GetLayer()->setPixelFormat(latteBufferUsesSRGB ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm);
        m_state.m_usesSRGB = latteBufferUsesSRGB;
    }

	if (!layer.AcquireDrawable())
		return false;
	if (!alreadyAcquired)
		(mainWindow ? m_mainDrawableContentsDefined : m_padDrawableContentsDefined) = false;
	return true;
}

bool MetalRenderer::CheckIfRenderPassNeedsFlush(LatteDecompilerShader* shader, bool forceAccurateBarrier)
{
    if (!shader || !m_state.m_activeFBO.m_fbo)
        return false;

    if (!m_colorWrittenMaskInCurrentEncoder && !m_depthWrittenInCurrentEncoder && !forceAccurateBarrier)
        return false;

    sint32 textureCount = shader->resourceMapping.getTextureCount();
	for (int i = 0; i < textureCount; ++i)
	{
		const auto relative_textureUnit = shader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
		auto hostTextureUnit = relative_textureUnit;


		// Don't flush if accessed via framebuffer fetch
		if (m_supportsFramebufferFetch && shader->textureRenderTargetIndex[relative_textureUnit] != 255)
		    continue;

		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
			break;
		case LatteConst::ShaderType::Pixel:
			hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
			break;
		case LatteConst::ShaderType::Geometry:
			hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
			break;
		default:
			UNREACHABLE;
		}

		auto textureView = m_state.m_textures[hostTextureUnit];
		if (!textureView)
            continue;

		LatteTexture* baseTexture = textureView->baseTexture;
        if (!baseTexture)
            continue;

        for (uint8 ct = 0; ct < LATTE_NUM_COLOR_TARGET; ct++)
        {
            auto colorTarget = m_state.m_activeFBO.m_fbo->colorBuffer[ct].texture;
            if (colorTarget && colorTarget->baseTexture == baseTexture)
            {
                if ((m_colorWrittenMaskInCurrentEncoder & (1 << ct)) != 0 || forceAccurateBarrier)
                    return true;
            }
        }
        auto depthTarget = m_state.m_activeFBO.m_fbo->depthBuffer.texture;
        if (depthTarget && (depthTarget->baseTexture == baseTexture || (depthTarget->baseTexture && baseTexture->physAddress != 0 && depthTarget->baseTexture->physAddress == baseTexture->physAddress)))
        {
            if (m_depthWrittenInCurrentEncoder || forceAccurateBarrier)
                return true;
        }
	}

	return false;
}

void MetalRenderer::BindSupportBuffer(MTL::RenderCommandEncoder* encoder, MetalShaderType stage,
    uint32 binding, const void* data, size_t size)
{
    // Compare exact bytes, including shader-generated padding. A different
    // program can reuse them only if the complete buffer contents are identical.
    MetalSupportBufferBinding* cached = nullptr;
    MetalSupportBufferBinding* previous = nullptr;
    if (m_submitBeforeDrawableWait && binding < MAX_MTL_BUFFERS)
    {
        cached = &m_supportBufferBindings[stage][binding];
        previous = &m_previousSupportBufferBindings[stage][binding];
        if (size > 0 && size <= sizeof(cached->data))
        {
            const auto matches = [&](const MetalSupportBufferBinding& entry) {
                return entry.commandBufferSerial == m_commandBufferSerial && entry.size == size &&
                    std::memcmp(entry.data, data, size) == 0;
            };
            bool reused = matches(*cached);
            if (!reused && matches(*previous))
            {
                std::swap(*cached, *previous);
                reused = true;
            }
            if (reused)
            {
                ++m_supportBufferReuseHits;
                SetBuffer(encoder, stage, cached->buffer, cached->offset, binding);
                return;
            }
        }
        else
        {
            // Large remapped blocks keep the existing direct upload path.
            cached->size = previous->size = 0;
            cached = previous = nullptr;
        }
    }

    auto& allocator = m_memoryManager->GetStagingAllocator();
    auto allocation = allocator.AllocateBufferMemory(size, 1);
    std::memcpy(allocation.memPtr, data, size);
    allocator.FlushReservation(allocation);
    ++m_supportBufferUploads;
    if (cached)
    {
        *previous = *cached;
        cached->commandBufferSerial = m_commandBufferSerial;
        cached->buffer = allocation.mtlBuffer;
        cached->offset = allocation.bufferOffset;
        cached->size = size;
        std::memcpy(cached->data, data, size);
    }
    SetBuffer(encoder, stage, allocation.mtlBuffer, allocation.bufferOffset, binding);
}

void MetalRenderer::BindStageResources(MTL::RenderCommandEncoder*& renderCommandEncoder, LatteDecompilerShader* shader, bool usesGeometryShader)
{
    auto mtlShaderType = GetMtlShaderType(shader->shaderType, usesGeometryShader);

    sint32 textureCount = shader->resourceMapping.getTextureCount();
	for (int i = 0; i < textureCount; ++i)
	{
		const auto relative_textureUnit = shader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
		auto hostTextureUnit = relative_textureUnit;
        const auto manualFilterOffset = shader->uniform.loc_manualFloatFilter[relative_textureUnit];
        // Support storage is reused across draws/stages. Missing or changed
        // bindings MUST reset the selector rather than inherit an earlier draw.
        if (manualFilterOffset >= 0)
            std::memset(reinterpret_cast<char*>(supportBufferData) + manualFilterOffset, 0, sizeof(uint32));

		// Don't bind textures that are accessed with a framebuffer fetch
		if (m_supportsFramebufferFetch && shader->textureRenderTargetIndex[relative_textureUnit] != 255)
		{
			LatteGPUState.telemetry.acceptedFramebufferFetchCount.fetch_add(1, std::memory_order_relaxed);
			// Current eligibility proves only address/format/tile identity; same-pixel
			// sampling semantics remain unproven for every accepted mapping.
			LatteGPUState.telemetry.unsafeFramebufferFetchCount.fetch_add(1, std::memory_order_relaxed);
            continue;
		}

		auto textureDim = shader->textureUnitDim[relative_textureUnit];
		auto texUnitRegIndex = hostTextureUnit * 7;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
			break;
		case LatteConst::ShaderType::Pixel:
			hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
			break;
		case LatteConst::ShaderType::Geometry:
			hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
			break;
		default:
			UNREACHABLE;
		}

		// TODO: correct?
		uint32 binding = shader->resourceMapping.getTextureBaseBindingPoint() + i;
		if (binding >= MAX_MTL_TEXTURES)
		{
		    cemuLog_logOnce(LogType::Force, "invalid texture binding {}", binding);
            continue;
		}

		auto textureView = m_state.m_textures[hostTextureUnit];
		if (!textureView)
		{
            if (textureDim == Latte::E_DIM::DIM_1D)
                SetTexture(renderCommandEncoder, mtlShaderType, m_nullTexture1D, binding);
           	else
                SetTexture(renderCommandEncoder, mtlShaderType, m_nullTexture2D, binding);
            SetSamplerState(renderCommandEncoder, mtlShaderType, m_nearestSampler, binding);
            continue;
		}

		if (textureDim == Latte::E_DIM::DIM_1D && (textureView->dim != Latte::E_DIM::DIM_1D))
		{
		    SetTexture(renderCommandEncoder, mtlShaderType, m_nullTexture1D, binding);
			continue;
		}
		else if (textureDim == Latte::E_DIM::DIM_2D && (textureView->dim != Latte::E_DIM::DIM_2D && textureView->dim != Latte::E_DIM::DIM_2D_MSAA))
		{
		    SetTexture(renderCommandEncoder, mtlShaderType, m_nullTexture2D, binding);
			continue;
		}

		LatteTexture* baseTexture = textureView->baseTexture;
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
		if (m_retainedAttachmentDiagnosticCount != 0 &&
		    LatteGPUState.telemetry.staleAttachmentSampleCaptureCount.load(std::memory_order_relaxed) < 16)
		{
			for (uint8 retainedIndex = 0; retainedIndex < m_retainedAttachmentDiagnosticCount; ++retainedIndex)
			{
				const auto& retainedAttachment = m_retainedAttachmentDiagnostics[retainedIndex];
				if (retainedAttachment.baseTexture == baseTexture)
				{
					RecordStaleAttachmentSample(shader, relative_textureUnit, retainedAttachment);
					break;
				}
			}
		}

#endif

		uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[relative_textureUnit];
		MTL::SamplerState* sampler;
		if (stageSamplerIndex != LATTE_DECOMPILER_SAMPLER_NONE)
		{
		    uint32 samplerIndex = stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
			_LatteRegisterSetSampler* samplerWords = LatteGPUState.contextNew.SQ_TEX_SAMPLER + samplerIndex;

			// Overwriting

            sint32 lodBias = samplerWords->WORD1.get_LOD_BIAS();
            if (baseTexture->overwriteInfo.hasLodBias)
                lodBias = baseTexture->overwriteInfo.lodBias;
            else if (baseTexture->overwriteInfo.hasRelativeLodBias)
                lodBias += baseTexture->overwriteInfo.relativeLodBias;

            if (baseTexture->overwriteInfo.anisotropicLevel >= 0)
                samplerWords->WORD0.set_MAX_ANISO_RATIO(baseTexture->overwriteInfo.anisotropicLevel);

            bool depthCompare = (shader->textureUsesDepthCompare[relative_textureUnit] != 0);
            if (!SupportsFloat32Filtering() && shader->textureCanFilter[relative_textureUnit]) {
                const auto* sampledView = textureView->GetRGBAView();
                const auto format = sampledView ? sampledView->pixelFormat() : MTL::PixelFormatInvalid;
                const bool float32 = format == MTL::PixelFormatR32Float || format == MTL::PixelFormatRG32Float || format == MTL::PixelFormatRGBA32Float || format == MTL::PixelFormatDepth32Float || format == MTL::PixelFormatDepth32Float_Stencil8;
                const auto minFilter = samplerWords->WORD0.get_XY_MIN_FILTER();
                const auto magFilter = samplerWords->WORD0.get_XY_MAG_FILTER();
                if (float32 && (minFilter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::BILINEAR ||
                    magFilter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::BILINEAR ||
                    minFilter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_BILINEAR ||
                    magFilter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_BILINEAR)) {
                    const bool manualFilter = !shader->isCustomShader && manualFilterOffset >= 0 && sampledView->mipmapLevelCount() == 1 &&
                        minFilter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::BILINEAR &&
                        magFilter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::BILINEAR &&
                        samplerWords->WORD0.get_MAX_ANISO_RATIO() == 0;
                    if (manualFilter) {
                        const uint32 enabled = 1;
                        std::memcpy(reinterpret_cast<char*>(supportBufferData) + manualFilterOffset, &enabled, sizeof(enabled));
                    } else {
                    cemuLog_logOnce(LogType::Force, "32-bit float filtering candidate is unsupported: shader={:016x} stage={} unit={} metalFormat={} guestFormat={} size={}x{} mips={} dim={} depthCompare={} sampler={:08x}/{:08x}/{:08x}",
                        shader->baseHash, uint32_t(shader->shaderType), relative_textureUnit, uint32_t(format), uint32_t(baseTexture->format),
                        baseTexture->width, baseTexture->height, baseTexture->mipLevels, uint32_t(baseTexture->dim), depthCompare,
                        samplerWords->WORD0.getRawValue(), samplerWords->WORD1.getRawValue(), samplerWords->WORD2.getRawValue());
                    }
                }
            }
    		sampler = m_samplerCache->GetSamplerState(LatteGPUState.contextNew, shader->shaderType, stageSamplerIndex, samplerWords, depthCompare, false, lodBias);
		}
		else
		{
		    sampler = m_nearestSampler;
		}
        SetSamplerState(renderCommandEncoder, mtlShaderType, sampler, binding);

		// get texture register word 0
		uint32 word4 = LatteGPUState.contextRegister[texUnitRegIndex + 4];
		auto& boundTexture = m_state.m_encoderState.m_textures[mtlShaderType][binding];
		MTL::Texture* mtlTexture = textureView->GetSwizzledView(word4);
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
		if (shader->textureRenderTargetIndex[relative_textureUnit] == 255)
			RecordActiveAttachmentSample(shader, relative_textureUnit, textureView, mtlTexture);
#endif
		SetTexture(renderCommandEncoder, mtlShaderType, mtlTexture, binding);
	}

	// Support buffer
	auto GET_UNIFORM_DATA_PTR = [&](size_t index) { return supportBufferData + (index / 4); };

	sint32 shaderAluConst;
	sint32 shaderUniformRegisterOffset;

	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		shaderAluConst = 0x400;
		shaderUniformRegisterOffset = mmSQ_VTX_UNIFORM_BLOCK_START;
		break;
	case LatteConst::ShaderType::Pixel:
		shaderAluConst = 0;
		shaderUniformRegisterOffset = mmSQ_PS_UNIFORM_BLOCK_START;
		break;
	case LatteConst::ShaderType::Geometry:
		shaderAluConst = 0; // geometry shader has no ALU const
		shaderUniformRegisterOffset = mmSQ_GS_UNIFORM_BLOCK_START;
		break;
	default:
		UNREACHABLE;
	}

	if (shader->resourceMapping.uniformVarsBufferBindingPoint >= 0)
	{
		if (shader->uniform.list_ufTexRescale.empty() == false)
		{
			for (auto& entry : shader->uniform.list_ufTexRescale)
			{
				float* xyScale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
				memcpy(entry.currentValue, xyScale, sizeof(float) * 2);
				memcpy(GET_UNIFORM_DATA_PTR(entry.uniformLocation), xyScale, sizeof(float) * 2);
			}
		}
		if (shader->uniform.loc_alphaTestRef >= 0)
		{
			*GET_UNIFORM_DATA_PTR(shader->uniform.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
		}
		if (shader->uniform.loc_pointSize >= 0)
		{
			const auto& pointSizeReg = LatteGPUState.contextNew.PA_SU_POINT_SIZE;
			float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
			if (pointWidth == 0.0f)
				pointWidth = 1.0f / 8.0f; // minimum size
			*GET_UNIFORM_DATA_PTR(shader->uniform.loc_pointSize) = pointWidth;
		}
		if (shader->uniform.loc_remapped >= 0)
		{
			LatteBufferCache_LoadRemappedUniforms(shader, GET_UNIFORM_DATA_PTR(shader->uniform.loc_remapped), true, (1<<LATTE_NUM_MAX_UNIFORM_BUFFERS)-1);
		}
		if (shader->uniform.loc_uniformRegister >= 0)
		{
			uint32* uniformRegData = (uint32*)(LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst);
			memcpy(GET_UNIFORM_DATA_PTR(shader->uniform.loc_uniformRegister), uniformRegData, shader->uniform.count_uniformRegister * 16);
		}
		if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
		{
			sint32 viewportWidth;
			sint32 viewportHeight;
			LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportWidth, &viewportHeight); // always call after _updateViewport()
			float* v = GET_UNIFORM_DATA_PTR(shader->uniform.loc_windowSpaceToClipSpaceTransform);
			v[0] = 2.0f / (float)viewportWidth;
			v[1] = 2.0f / (float)viewportHeight;
		}
		if (shader->uniform.loc_fragCoordScale >= 0)
		{
			LatteMRT::GetCurrentFragCoordScale(GET_UNIFORM_DATA_PTR(shader->uniform.loc_fragCoordScale));
		}
		if (shader->uniform.loc_verticesPerInstance >= 0)
		{
			*(int*)(supportBufferData + ((size_t)shader->uniform.loc_verticesPerInstance / 4)) = m_state.m_streamoutState.verticesPerInstance;
			for (sint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
			{
				if (shader->uniform.loc_streamoutBufferBase[b] >= 0)
				{
					*(uint32*)GET_UNIFORM_DATA_PTR(shader->uniform.loc_streamoutBufferBase[b]) = m_state.m_streamoutState.buffers[b].ringBufferOffset;
				}
			}
		}

		BindSupportBuffer(renderCommandEncoder, mtlShaderType,
            shader->resourceMapping.uniformVarsBufferBindingPoint, supportBufferData, shader->uniform.uniformRangeSize);
	}

	// Uniform buffers
	for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
	{
		if (shader->resourceMapping.uniformBuffersBindingPoint[i] >= 0)
		{
    		uint32 binding = shader->resourceMapping.uniformBuffersBindingPoint[i];
    		if (binding >= MAX_MTL_BUFFERS)
    		{
    		    cemuLog_logOnce(LogType::Force, "invalid buffer binding {}", binding);
    			continue;
    		}

    		const auto stage = GetMtlGeneralShaderType(shader->shaderType);
    		size_t offset = m_state.m_uniformBufferOffsets[stage][i];
    		if (offset == INVALID_OFFSET)
                continue;

            BindBufferCache(renderCommandEncoder, mtlShaderType, (uint32)offset, m_state.m_uniformBufferSizes[stage][i], binding);
		}
	}

	// Storage buffer
	if (shader->resourceMapping.tfStorageBindingPoint >= 0)
	{
        // The first streamout draw writes this buffer before any copyback can
        // create it. Allocate lazily here, before binding the shader storage.
        SetBuffer(renderCommandEncoder, mtlShaderType, GetXfbRingBuffer(), 0, shader->resourceMapping.tfStorageBindingPoint);
	}
}

void MetalRenderer::ClearColorTextureInternal(MTL::Texture* mtlTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(mtlTexture);
    colorAttachment->setClearColor(MTL::ClearColor(r, g, b, a));
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionStore);
    colorAttachment->setSlice(sliceIndex);
    colorAttachment->setLevel(mipIndex);

    GetTemporaryRenderCommandEncoder(renderPassDescriptor);
    EndEncoding();

    // Debug
    m_performanceMonitor.m_clears++;
}

void MetalRenderer::CopyBufferToBuffer(MTL::Buffer* src, uint32 srcOffset, MTL::Buffer* dst, uint32 dstOffset, uint32 size, MTL::RenderStages after, MTL::RenderStages before)
{
    ++g_blit_bufferCopies;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // Cache writes are produced by vertex/mesh work, never fragments. Keep
    // small, disjoint copies in the existing tile pass, but preserve any
    // cache bytes which earlier fragments still need to read.
    if (m_isAppleGPU && m_commandEncoder && m_encoderType == MetalEncoderType::Render &&
        dst == m_memoryManager->GetBufferCache() &&
        m_copyBufferToBufferPipeline && size > 0 && size <= 64 * 1024 &&
        ((srcOffset | dstOffset | size) & 15u) == 0 &&
        (src != dst || uint64_t(srcOffset) + size <= dstOffset || uint64_t(dstOffset) + size <= srcOffset))
    {
        auto* encoder = static_cast<MTL::RenderCommandEncoder*>(m_commandEncoder);
        encoder->memoryBarrier(MTL::BarrierScopeBuffers,
            SupportedStages(MTL::RenderStageVertex | MTL::RenderStageObject | MTL::RenderStageMesh), MTL::RenderStageVertex);
        encoder->setRenderPipelineState(m_copyBufferToBufferPipeline->GetRenderPipelineState());
        encoder->setVertexBuffer(src, srcOffset, GET_HELPER_BUFFER_BINDING(0));
        encoder->setVertexBuffer(dst, dstOffset, GET_HELPER_BUFFER_BINDING(1));
        encoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0), NS::UInteger(size / 16));
        encoder->memoryBarrier(MTL::BarrierScopeBuffers, MTL::RenderStageVertex, SupportedStages(before));
        m_state.m_encoderState.m_renderPipelineState = nullptr;
        return;
    }
#endif
    auto blitCommandEncoder = GetBlitCommandEncoder();
    blitCommandEncoder->copyFromBuffer(src, srcOffset, dst, dstOffset, size);
}

bool MetalRenderer::ShouldDrawPadDisplay() const
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	return m_screenshot_requested || m_screenshot_state != ScreenshotState::None ||
		s_drcPreviewPublishCallback.load(std::memory_order_acquire) != nullptr;
#else
	return true;
#endif
}

int MetalRenderer::ActiveFrameGenerationTarget()
{
    if (m_frameGeneration && m_frameGeneration->Failed())
    {
        const auto& reason = m_frameGeneration->Error();
        const auto stats = m_frameGeneration->GetStatistics();
        const auto message = fmt::format("[FRAME GEN] Using native output: {} (ticks={} submitted={} skipped={} noDrawable={})",
            reason.empty() ? "presentation worker stopped" : reason, stats.displayTicks, stats.submitted, stats.skippedByDisplay, stats.noDrawable);
        cemuLog_log(LogType::Force, "{}", message);
#if TARGET_OS_IPHONE || TARGET_OS_IOS
        CemuBridge_LogJITMilestone(message.c_str());
#endif
        m_frameGenMode.Fail();
    }
    const int target = m_frameGenMode.ForFrame(GetFrameGenerationTarget());
    if (target == 0 && m_frameGeneration)
    {
        m_frameGeneration.reset();
        m_frameGenTarget = nullptr;
        m_frameGenRendered = false;
    }
    return target;
}

void MetalRenderer::EnsureFrameGenTextures(uint32 width, uint32 height, MTL::PixelFormat pixelFormat, bool initialize)
{
    if (!width || !height || !m_mainLayer.GetLayer())
    {
        if (m_frameGeneration) m_frameGeneration->Reset();
        m_frameGenTarget = nullptr;
        m_frameGenRendered = false;
        return; // a temporarily detached/resizing view is not an FG failure
    }
    auto* previousTarget = m_frameGenTarget;
    try
    {
        // AcquireDrawable normally establishes TV color space. FG deliberately
        // avoids acquiring that drawable, so establish the same state here.
        const bool srgb = LatteGPUState.tvBufferUsesSRGB;
        pixelFormat = srgb ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm;
        m_state.m_usesSRGB = srgb;
        if (m_mainLayer.GetLayer()->pixelFormat() != pixelFormat)
            m_mainLayer.GetLayer()->setPixelFormat(pixelFormat);
        if (!m_frameGeneration)
            m_frameGeneration = std::make_unique<MetalFrameGeneration>(m_device);
        m_frameGenTarget = m_frameGeneration->Begin(m_mainLayer.GetLayer(), width, height, pixelFormat);
    }
    catch (const std::exception& error)
    {
        cemuLog_log(LogType::Force, "[FRAME GEN] Resource creation failed: {}", error.what());
        m_frameGenTarget = nullptr;
    }
    if (!m_frameGenTarget)
    {
        cemuLog_log(LogType::Force, "[FRAME GEN] Using original presentation: {}", m_frameGeneration ? m_frameGeneration->Error() : "unavailable resources");
        m_frameGenMode.Fail();
        m_frameGeneration.reset();
        m_frameGenRendered = false;
    }
    else if (initialize && previousTarget != m_frameGenTarget)
    {
        // A freshly leased image has no defined pixels outside the game quad.
        // Initialize letterboxing only in the FG path.
        ClearColorTextureInternal(m_frameGenTarget, 0, 0, 0, 0, 0, 1);
    }
}

void MetalRenderer::SwapBuffer(bool mainWindow)
{
    const int fgTarget = mainWindow ? ActiveFrameGenerationTarget() : 0;
    if (mainWindow && fgTarget > 0)
    {
        // The opt-in controller owns display-paced presentation after the
        // source command buffer completes. Native presentation is unchanged.
        return;
    }

#if TARGET_OS_IPHONE || TARGET_OS_IOS
	// Do not acquire a hidden GamePad drawable just to discard it. If a view
	// became hidden after acquiring one, present it once to release its retain.
	if (!mainWindow && !m_padLayer.GetDrawable() && !ShouldDrawPadDisplay())
		return;
#endif
    if (!AcquireDrawable(mainWindow))
        return;

    auto commandBuffer = GetCommandBuffer();
    GetLayer(mainWindow).PresentDrawable(commandBuffer);
}

void MetalRenderer::EnsureImGuiBackend()
{
    if (!ImGui::GetIO().BackendRendererUserData)
    {
        ImGui_ImplMetal_Init(m_device);
        //ImGui_ImplMetal_CreateFontsTexture(m_device);
    }
}

void MetalRenderer::StartCapture()
{
    auto captureManager = MTL::CaptureManager::sharedCaptureManager();
    auto desc = MTL::CaptureDescriptor::alloc()->init();
    desc->setCaptureObject(m_device);

    // An iPad trace must be retrievable without an attached Xcode debugger.
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    const bool captureToFile = true;
#else
    const bool captureToFile = !captureManager->supportsDestination(MTL::CaptureDestinationDeveloperTools);
#endif
    std::string capturePath;
    if (!captureToFile)
    {
        desc->setDestination(MTL::CaptureDestinationDeveloperTools);
    }
    else
    {
        std::string captureDirectory = GetConfig().gpu_capture_dir.GetValue();
#if TARGET_OS_IPHONE || TARGET_OS_IOS
        if (captureDirectory.empty())
            captureDirectory = ActiveSettings::GetUserDataPath("GPUCaptures").string();
        std::error_code directoryError;
        std::filesystem::create_directories(captureDirectory, directoryError);
        if (directoryError)
        {
            cemuLog_log(LogType::Force, "GPU capture directory error: {}", directoryError.message());
            desc->release();
            return;
        }
#endif
        if (captureDirectory.empty())
        {
            cemuLog_log(LogType::Force, "No GPU capture directory specified, cannot do a GPU capture");
            desc->release();
            return;
        }
        if (!captureManager->supportsDestination(MTL::CaptureDestinationGPUTraceDocument))
        {
            cemuLog_log(LogType::Force, "GPU trace document destination is not available, cannot do a GPU capture");
            desc->release();
            return;
        }

        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&now_time), "%Y-%m-%d_%H-%M-%S");
        capturePath = fmt::format("{}/cemu_{}.gputrace", captureDirectory, oss.str());
        desc->setDestination(MTL::CaptureDestinationGPUTraceDocument);
        desc->setOutputURL(ToNSURL(capturePath));
    }

    NS::Error* error = nullptr;
    const bool started = captureManager->startCapture(desc, &error);
    desc->release();
    if (!started)
    {
        cemuLog_log(LogType::Force, "Failed to start GPU capture: {}",
                    error ? error->localizedDescription()->utf8String() : "unknown error");
        return;
    }
    if (!capturePath.empty())
        cemuLog_log(LogType::Force, "[GPU CAPTURE] Started one gameplay frame: {}", capturePath);
    m_capturing = true;
}

void MetalRenderer::EndCapture()
{
    auto captureManager = MTL::CaptureManager::sharedCaptureManager();
    captureManager->stopCapture();
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    cemuLog_log(LogType::Force, "[GPU CAPTURE] Finished one gameplay frame");
#endif
    m_capturing = false;
}
