#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalLayerHandle.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalLayer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"

#include "gui/interface/WindowSystem.h"

namespace
{
    std::atomic<uint64> s_mainLayerPresentCount{0};
    std::atomic<uint64> s_padLayerPresentCount{0};
    std::atomic<uint64> s_padPresentationBlockedCount{0};
    std::atomic<uint64> s_lastMainPresentFrame{UINT64_MAX};
    std::atomic<uint64> s_lastPadPresentFrame{UINT64_MAX};
    std::atomic<uint64> s_lastBothLayersFrame{UINT64_MAX};
    std::atomic<uint64> s_sameFrameBothLayerCount{0};
    std::atomic<uint32> s_sameFrameBothLayerLogCount{0};
    std::atomic<uint64> s_mainLayerPtr{0};
    std::atomic<uint64> s_padLayerPtr{0};
    std::atomic<bool> s_presentRoutingValid{false};
    std::atomic<bool> s_lastPresentedPadLayer{false};
    std::atomic<uint64> s_lastPresentedLayerPtr{0};
    std::atomic<uint32> s_lastPresentedDrawableWidth{0};
    std::atomic<uint32> s_lastPresentedDrawableHeight{0};
    std::atomic<uint64> s_lastPresentedFrameId{0};
    constexpr uint32 kMaxSameFrameBothLayerLogs = 16;
}

MetalPresentRoutingDiagnosticSnapshot MetalLayerHandle_GetPresentRoutingDiagnosticSnapshot()
{
    MetalPresentRoutingDiagnosticSnapshot snapshot{};
#ifndef NDEBUG
    snapshot.valid = s_presentRoutingValid.load(std::memory_order_acquire);
    snapshot.lastPresentedPadLayer = s_lastPresentedPadLayer.load(std::memory_order_relaxed);
    snapshot.lastLayerPtr = s_lastPresentedLayerPtr.load(std::memory_order_relaxed);
    snapshot.lastDrawableWidth = s_lastPresentedDrawableWidth.load(std::memory_order_relaxed);
    snapshot.lastDrawableHeight = s_lastPresentedDrawableHeight.load(std::memory_order_relaxed);
    snapshot.lastFrameId = s_lastPresentedFrameId.load(std::memory_order_relaxed);
    snapshot.mainLayerPresentCount = s_mainLayerPresentCount.load(std::memory_order_relaxed);
    snapshot.padLayerPresentCount = s_padLayerPresentCount.load(std::memory_order_relaxed);
    snapshot.padPresentationBlockedCount = s_padPresentationBlockedCount.load(std::memory_order_relaxed);
    snapshot.sameFrameBothLayerCount = s_sameFrameBothLayerCount.load(std::memory_order_relaxed);
#endif
    return snapshot;
}

MetalLayerHandle::MetalLayerHandle(MTL::Device* device, const Vector2i& size, bool mainWindow)
{
    m_mainWindow = mainWindow;
    const auto& windowInfo = (mainWindow ? WindowSystem::GetWindowInfo().window_main : WindowSystem::GetWindowInfo().window_pad);

    m_layer = (CA::MetalLayer*)CreateMetalLayer(windowInfo.surface, m_layerScaleX, m_layerScaleY);
    if (!m_layer)
        return;
    m_layer->setDevice(device);
    m_layer->setDrawableSize(CGSize{(float)size.x * m_layerScaleX, (float)size.y * m_layerScaleY});
    m_layer->setMaximumDrawableCount(3);
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // The hidden pad drawable is copied into a renderer-owned preview texture.
    // Keep the visible TV layer on the faster framebuffer-only path.
    m_layer->setFramebufferOnly(mainWindow);
#else
    m_layer->setFramebufferOnly(true);
#endif
}

MetalLayerHandle::~MetalLayerHandle()
{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    if (m_drawable)
        m_drawable->release();
#endif
    if (m_layer)
        m_layer->release();
}

MetalLayerHandle::MetalLayerHandle(MetalLayerHandle&& other) noexcept
    : m_layer(std::exchange(other.m_layer, nullptr))
    , m_mainWindow(other.m_mainWindow)
    , m_layerScaleX(other.m_layerScaleX)
    , m_layerScaleY(other.m_layerScaleY)
    , m_drawable(std::exchange(other.m_drawable, nullptr))
{
}

MetalLayerHandle& MetalLayerHandle::operator=(MetalLayerHandle&& other) noexcept
{
    if (this != &other)
    {
#if TARGET_OS_IPHONE || TARGET_OS_IOS
        if (m_drawable)
            m_drawable->release();
#endif
        if (m_layer)
            m_layer->release();

        m_layer = std::exchange(other.m_layer, nullptr);
        m_mainWindow = other.m_mainWindow;
        m_layerScaleX = other.m_layerScaleX;
        m_layerScaleY = other.m_layerScaleY;
        m_drawable = std::exchange(other.m_drawable, nullptr);
    }
    return *this;
}

void MetalLayerHandle::Resize(const Vector2i& size)
{
    if (m_layer)
        m_layer->setDrawableSize(CGSize{(float)size.x * m_layerScaleX, (float)size.y * m_layerScaleY});
}

bool MetalLayerHandle::AcquireDrawable()
{
    if (m_drawable)
        return true;
    if (!m_layer)
        return false;

    const auto t0 = std::chrono::steady_clock::now();
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // Both the visible TV and hidden GamePad drawables can outlive this call.
    // Retain across the Metal 4 submission, then drain per-frame autoreleases
    // on the long-running Latte thread instead of accumulating them.
    auto pool = NS::AutoreleasePool::alloc()->init();
    m_drawable = m_layer->nextDrawable();
    if (m_drawable)
        m_drawable->retain();
    pool->release();
#else
    m_drawable = m_layer->nextDrawable();
#endif

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    const auto t1 = std::chrono::steady_clock::now();
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    RuntimePerformance::drawableWaitUs.fetch_add(elapsedUs, std::memory_order_relaxed);
#endif

    if (!m_drawable)
    {
        cemuLog_log(LogType::Force, "layer {} failed to acquire next drawable", (void*)this);
        return false;
    }

    return true;
}

std::atomic<uint64_t> g_metalPresentCount{0};

void MetalLayerHandle::PresentDrawable(MTL::CommandBuffer* commandBuffer)
{
	if (!m_drawable)
		return;

#ifndef NDEBUG
	const uint64 frameId = LatteGPUState.frameCounter;
	MTL::Texture* drawableTexture = m_drawable->texture();
	const uint64 layerPtr = (uint64)m_layer;
	bool bothLayersPresentedThisFrame = false;
#endif

#if TARGET_OS_IPHONE || TARGET_OS_IOS
	// Keep the DRC drawable and copy path active, but temporarily prevent the
	// iPadOS pad surface from being scheduled for visible presentation.
	if (!m_mainWindow)
	{
#ifndef NDEBUG
		s_padLayerPtr.store(layerPtr, std::memory_order_relaxed);
		s_padPresentationBlockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
		// The command buffer retains referenced Metal resources, but the layer's
		// drawable itself must stay alive until those commands finish. Transfer
		// our explicit AcquireDrawable() retain to the completion handler; the
		// drawable is then returned to CAMetalLayer without ever being presented.
		auto drawable = m_drawable;
		commandBuffer->addCompletedHandler(^(MTL::CommandBuffer*) {
			drawable->release();
		});
		m_drawable = nullptr;
		return;
	}
#endif

#ifndef NDEBUG
	if (m_mainWindow)
	{
		s_mainLayerPresentCount.fetch_add(1, std::memory_order_relaxed);
		s_mainLayerPtr.store(layerPtr, std::memory_order_relaxed);
		s_lastMainPresentFrame.store(frameId, std::memory_order_release);
		bothLayersPresentedThisFrame = s_lastPadPresentFrame.load(std::memory_order_acquire) == frameId;
	}
	else
	{
		s_padLayerPresentCount.fetch_add(1, std::memory_order_relaxed);
		s_padLayerPtr.store(layerPtr, std::memory_order_relaxed);
		s_lastPadPresentFrame.store(frameId, std::memory_order_release);
		bothLayersPresentedThisFrame = s_lastMainPresentFrame.load(std::memory_order_acquire) == frameId;
	}

	if (bothLayersPresentedThisFrame && s_lastBothLayersFrame.exchange(frameId, std::memory_order_acq_rel) != frameId)
	{
		s_sameFrameBothLayerCount.fetch_add(1, std::memory_order_relaxed);
		const uint32 logIndex = s_sameFrameBothLayerLogCount.fetch_add(1, std::memory_order_relaxed);
		if (logIndex < kMaxSameFrameBothLayerLogs)
		{
			cemuLog_log(LogType::Force,
				"[PRESENT ROUTING] sameFrame={} mainLayer={} padLayer={}",
				frameId,
				(void*)s_mainLayerPtr.load(std::memory_order_relaxed),
				(void*)s_padLayerPtr.load(std::memory_order_relaxed));
		}
	}

	s_lastPresentedPadLayer.store(!m_mainWindow, std::memory_order_relaxed);
	s_lastPresentedLayerPtr.store(layerPtr, std::memory_order_relaxed);
	s_lastPresentedDrawableWidth.store((uint32)drawableTexture->width(), std::memory_order_relaxed);
	s_lastPresentedDrawableHeight.store((uint32)drawableTexture->height(), std::memory_order_relaxed);
	s_lastPresentedFrameId.store(frameId, std::memory_order_relaxed);
	s_presentRoutingValid.store(true, std::memory_order_release);
#endif

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    RuntimePerformance::tvSubmitIntervals.record(RuntimePerformance::nowNs());
#endif
	g_metalPresentCount.fetch_add(1, std::memory_order_relaxed);
#ifdef CEMU_DEBUG_ASSERT
	CafeSimulationPacing_RecordMetalPresentation();
	coreinit::CafeTimeConsumerDiagnostics_RecordWindowTick();
#endif
	commandBuffer->presentDrawable(m_drawable);
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	m_drawable->release();
#endif
	m_drawable = nullptr;
}

void MetalLayerHandle::PresentDrawableAfterExternalSignal()
{
    if (!m_drawable || !m_mainWindow)
        return;

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    RuntimePerformance::tvSubmitIntervals.record(RuntimePerformance::nowNs());
#endif
    g_metalPresentCount.fetch_add(1, std::memory_order_relaxed);
#ifdef CEMU_DEBUG_ASSERT
    CafeSimulationPacing_RecordMetalPresentation();
    coreinit::CafeTimeConsumerDiagnostics_RecordWindowTick();
#endif
    m_drawable->present();
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    m_drawable->release();
#endif
    m_drawable = nullptr;
}

void MetalLayerHandle::DiscardDrawable()
{
    if (m_drawable)
    {
#if TARGET_OS_IPHONE || TARGET_OS_IOS
        m_drawable->release();
#endif
        m_drawable = nullptr;
    }
}
