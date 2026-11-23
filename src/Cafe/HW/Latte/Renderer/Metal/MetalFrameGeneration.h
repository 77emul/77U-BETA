#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <array>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

// Every resource and worker here belongs exclusively to the opt-in FG path.
// Source textures remain leased until all GPU readers have completed.
class MetalFrameGeneration
{
public:
    struct Statistics
    {
        double displayedFPS = 0, sourceFPS = 0, gpuMs = 0, sourceReadyMs = 0;
        uint64_t generated = 0, repeated = 0, original = 0, dropped = 0, starved = 0;
        std::array<uint32_t, 8> audit{};
        double auditPosition = 0, auditAgeSeconds = 0;
        double shortestPresentMs = 0, longestPresentMs = 0;
        uint64_t displayTicks = 0, acceptedTicks = 0; // display-link callbacks received / used for output
        double displayPeriodMs = 0;
        uint64_t submitted = 0, skippedByDisplay = 0, noDrawable = 0; // presents sent / never shown / not sent
    };
    explicit MetalFrameGeneration(MTL::Device* device);
    ~MetalFrameGeneration();
    MetalFrameGeneration(const MetalFrameGeneration&) = delete;
    MetalFrameGeneration& operator=(const MetalFrameGeneration&) = delete;

    MTL::Texture* Begin(CA::MetalLayer* layer, unsigned width, unsigned height, MTL::PixelFormat format);
    // fast: cheaper flow search for GPUs that cannot afford the default quality.
    void Submit(MTL::CommandBuffer* sourceReady, int targetFps, bool fast = false);
    void Reset();
    double PresentedFPS() const;
    Statistics GetStatistics() const;
    bool Failed() const;
    const std::string& Error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// The view sends actual display deadlines. A disabled controller returns
// immediately; this does not notify guest VSync or alter guest clock speed.
void CemuMetal_SetFrameGenerationSuspended(bool suspended);
extern "C" void CemuMetal_FrameGenerationDisplayTick(void* layer, double targetSeconds, double displayPeriod);
extern "C" void CemuMetal_GetFrameGenerationRates(double* source, double* displayed);
