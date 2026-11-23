#pragma once

#include <Metal/Metal.hpp>
#include <array>

#include "HW/Latte/Core/LatteConst.h"
#include "HW/Latte/ISA/LatteReg.h"

class MetalSamplerCache
{
public:
    MetalSamplerCache(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer} {}
    ~MetalSamplerCache();

    MTL::SamplerState* GetSamplerState(const LatteContextRegister& lcr, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords, bool depthCompare = false, bool supportArgumentBuffers = false, sint32 lodBias = 0);

private:
    class MetalRenderer* m_mtlr;

    std::map<std::pair<uint64, bool>, MTL::SamplerState*> m_samplerCache;
    std::pair<uint64, bool> m_lastSamplerKey{0, false};
    MTL::SamplerState* m_lastSamplerState = nullptr;

    struct FastEntry
    {
        std::pair<uint64, bool> key{0, false};
        MTL::SamplerState* state = nullptr; // Owned by m_samplerCache.
    };
    std::array<FastEntry, 64> m_fastCache{};

    struct StageSlot
    {
        uint32 word0{0};
        uint32 word1{0};
        uint32 word2{0};
        sint32 lodBias{0};
        bool depthCompare{false};
        bool supportArgumentBuffers{false};
        bool valid{false};
        MTL::SamplerState* state{nullptr};
    };
    std::array<std::array<StageSlot, Latte::GPU_LIMITS::NUM_SAMPLERS_PER_STAGE>, 4> m_stageSlots{};

    uint64 CalculateSamplerHash(const LatteContextRegister& lcr, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords, bool depthCompare, sint32 lodBias = 0);
};
