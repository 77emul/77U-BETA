#pragma once

#include <QuartzCore/QuartzCore.hpp>

#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
#include "util/math/vector2.h"

struct MetalPresentRoutingDiagnosticSnapshot
{
    bool valid = false;
    bool lastPresentedPadLayer = false;
    uint64 lastLayerPtr = 0;
    uint32 lastDrawableWidth = 0;
    uint32 lastDrawableHeight = 0;
    uint64 lastFrameId = 0;
    uint64 mainLayerPresentCount = 0;
    uint64 padLayerPresentCount = 0;
    uint64 padPresentationBlockedCount = 0;
    uint64 sameFrameBothLayerCount = 0;
};

MetalPresentRoutingDiagnosticSnapshot MetalLayerHandle_GetPresentRoutingDiagnosticSnapshot();
extern std::atomic<uint64_t> g_metalPresentCount;

class MetalLayerHandle
{
public:
    MetalLayerHandle() = default;
    MetalLayerHandle(MTL::Device* device, const Vector2i& size, bool mainWindow);

    ~MetalLayerHandle();

    MetalLayerHandle(const MetalLayerHandle&) = delete;
    MetalLayerHandle& operator=(const MetalLayerHandle&) = delete;

    MetalLayerHandle(MetalLayerHandle&& other) noexcept;
    MetalLayerHandle& operator=(MetalLayerHandle&& other) noexcept;

    void Resize(const Vector2i& size);

    bool AcquireDrawable();

    void PresentDrawable(MTL::CommandBuffer* commandBuffer);
    void PresentDrawableAfterExternalSignal();
    void DiscardDrawable();

    CA::MetalLayer* GetLayer() const { return m_layer; }

    CA::MetalDrawable* GetDrawable() const { return m_drawable; }

private:
    CA::MetalLayer* m_layer = nullptr;
    bool m_mainWindow = true;
    float m_layerScaleX, m_layerScaleY;

    CA::MetalDrawable* m_drawable = nullptr;
};
