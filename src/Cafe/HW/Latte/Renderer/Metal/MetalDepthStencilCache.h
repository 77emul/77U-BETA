#pragma once

#include <Metal/Metal.hpp>
#include <array>

#include "HW/Latte/ISA/LatteReg.h"

class MetalDepthStencilCache
{
public:
    MetalDepthStencilCache(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer} {}
    ~MetalDepthStencilCache();

    MTL::DepthStencilState* GetDepthStencilState(const LatteContextRegister& lcr);

private:
    class MetalRenderer* m_mtlr;

    std::map<uint64, MTL::DepthStencilState*> m_depthStencilCache;
    uint64 m_lastStateHash = 0;
    MTL::DepthStencilState* m_lastDepthStencilState = nullptr;

    struct FastEntry
    {
        uint64 hash = 0;
        MTL::DepthStencilState* state = nullptr; // Owned by m_depthStencilCache.
    };
    std::array<FastEntry, 64> m_fastCache{};

    uint64 CalculateDepthStencilHash(const LatteContextRegister& lcr);
};
