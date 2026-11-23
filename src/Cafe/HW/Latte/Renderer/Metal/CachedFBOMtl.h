#pragma once

#include <Metal/Metal.hpp>

#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"

class CachedFBOMtl : public LatteCachedFBO
{
public:
	CachedFBOMtl(class MetalRenderer* metalRenderer, uint64 key);

	~CachedFBOMtl();

	MTL::RenderPassDescriptor* GetRenderPassDescriptor()
	{
	    return m_renderPassDescriptor;
	}

	uint64 GetEstimatedColorBytes() const { return m_estimatedColorBytes; }
	uint64 GetEstimatedColorBytes(uint8 ct) const { return (ct < 8) ? m_estimatedColorBytesAttachment[ct] : 0; }
	uint64 GetEstimatedDepthBytes() const { return m_estimatedDepthBytes; }

private:
    MTL::RenderPassDescriptor* m_renderPassDescriptor = nullptr;
    uint64 m_estimatedColorBytes{0};
    uint64 m_estimatedColorBytesAttachment[8]{0};
    uint64 m_estimatedDepthBytes{0};
};
