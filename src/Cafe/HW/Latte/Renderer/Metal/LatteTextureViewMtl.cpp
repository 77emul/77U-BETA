#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.h"
#include <algorithm>
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Metal/MTLTexture.hpp"

uint32 LatteTextureMtl_AdjustTextureCompSel(Latte::E_GX2SURFFMT format, uint32 compSel)
{
	switch (format)
	{
	case Latte::E_GX2SURFFMT::R8_UNORM: // R8 is replicated on all channels (while OpenGL would return 1.0 for BGA instead)
	case Latte::E_GX2SURFFMT::R8_SNORM: // probably the same as _UNORM, but needs testing
		if (compSel >= 1 && compSel <= 3)
			compSel = 0;
		break;
	case Latte::E_GX2SURFFMT::A1_B5_G5_R5_UNORM: // order of components is reversed (RGBA -> ABGR)
		if (compSel >= 0 && compSel <= 3)
			compSel = 3 - compSel;
		break;
	case Latte::E_GX2SURFFMT::BC4_UNORM:
	case Latte::E_GX2SURFFMT::BC4_SNORM:
		if (compSel >= 1 && compSel <= 3)
			compSel = 0;
		break;
	case Latte::E_GX2SURFFMT::BC5_UNORM:
	case Latte::E_GX2SURFFMT::BC5_SNORM:
		// RG maps to RG
		// B maps to ?
		// A maps to G (guessed)
		if (compSel == 3)
			compSel = 1; // read Alpha as Green
		break;
	case Latte::E_GX2SURFFMT::A2_B10_G10_R10_UNORM:
		// reverse components (Wii U: ABGR, OpenGL: RGBA)
		// used in Resident Evil Revelations
		if (compSel >= 0 && compSel <= 3)
			compSel = 3 - compSel;
		break;
	case Latte::E_GX2SURFFMT::X24_G8_UINT:
		// map everything to alpha?
		if (compSel >= 0 && compSel <= 3)
			compSel = 3;
		break;
	case Latte::E_GX2SURFFMT::R4_G4_UNORM:
		// red and green swapped
		if (compSel == 0)
			compSel = 1;
		else if (compSel == 1)
			compSel = 0;
		break;
	default:
		break;
	}
	return compSel;
}

static void ClampMetalViewRange(MTL::Texture* parent, const LatteTextureMtl* base, MTL::TextureType targetType, uint32& baseLevel, uint32& levelCount, uint32& baseLayer, uint32& layerCount)
{
	const uint32 mipCap = parent ? (uint32)parent->mipmapLevelCount() : (uint32)std::max(base->maxPossibleMipLevels, 1);
	if (baseLevel >= mipCap)
		baseLevel = 0;
	if (levelCount == 0 || levelCount > mipCap - baseLevel)
		levelCount = mipCap - baseLevel;

	const uint32 sliceCap = parent ? GetMetalTextureSliceCount(parent) : (uint32)std::max(base->depth, 1);

	if (targetType == MTL::TextureTypeCube)
	{
		baseLayer = (baseLayer / 6) * 6;
		if (baseLayer + 6 > sliceCap)
			baseLayer = 0;
		layerCount = 6;
	}
	else if (targetType == MTL::TextureTypeCubeArray)
	{
		baseLayer = (baseLayer / 6) * 6;
		if (baseLayer >= sliceCap)
			baseLayer = 0;
		layerCount = (layerCount / 6) * 6;
		if (layerCount == 0)
			layerCount = 6;
		if (baseLayer + layerCount > sliceCap)
		{
			layerCount = ((sliceCap - baseLayer) / 6) * 6;
			if (layerCount == 0 && sliceCap >= 6)
			{
				baseLayer = 0;
				layerCount = (sliceCap / 6) * 6;
			}
		}
	}
	else if (targetType == MTL::TextureType2D || targetType == MTL::TextureType1D || targetType == MTL::TextureType3D)
	{
		if (baseLayer >= sliceCap)
			baseLayer = 0;
		layerCount = 1;
	}
	else
	{
		if (baseLayer >= sliceCap)
			baseLayer = 0;
		if (layerCount == 0 || layerCount > sliceCap - baseLayer)
			layerCount = sliceCap - baseLayer;
	}
}

static MTL::Texture* NewClampedTextureView(LatteTextureMtl* base, MTL::PixelFormat pixelFormat, MTL::TextureType textureType, uint32 baseLevel, uint32 levelCount, uint32 baseLayer, uint32 layerCount, const MTL::TextureSwizzleChannels* swizzle)
{
	MTL::Texture* parent = base ? base->GetTexture() : nullptr;
	if (!parent)
		return nullptr;

	MTL::Texture* rootParent = parent->parentTexture() ? parent->parentTexture() : parent;

	ClampMetalViewRange(rootParent, base, textureType, baseLevel, levelCount, baseLayer, layerCount);

	// Final sanity check for Cube/CubeArray slice counts
	if (textureType == MTL::TextureTypeCube && layerCount != 6)
	{
		return rootParent->retain();
	}
	if (textureType == MTL::TextureTypeCubeArray && (layerCount == 0 || layerCount % 6 != 0))
	{
		return rootParent->retain();
	}

	// Never pass swizzle on depth or stencil textures, and force rootParent pixelFormat for depth
	const MTL::TextureSwizzleChannels* effectiveSwizzle = swizzle;
	if (IsAnyDepthOrStencilFormat(pixelFormat) || IsAnyDepthOrStencilFormat(rootParent->pixelFormat()))
	{
		effectiveSwizzle = nullptr;
		pixelFormat = rootParent->pixelFormat();
	}
	else if (pixelFormat != rootParent->pixelFormat())
	{
		const bool hasFormatViewUsage = (rootParent->usage() & MTL::TextureUsagePixelFormatView) != 0;
		if (!hasFormatViewUsage || !ArePixelFormatsViewCompatible(rootParent->pixelFormat(), pixelFormat))
		{
			pixelFormat = rootParent->pixelFormat();
		}
	}

	// If view would be identical to rootParent and no swizzle, return rootParent directly
	const uint32 rootSliceCap = GetMetalTextureSliceCount(rootParent);
	if (textureType == rootParent->textureType() && pixelFormat == rootParent->pixelFormat() &&
	    baseLevel == 0 && levelCount == rootParent->mipmapLevelCount() &&
	    baseLayer == 0 && layerCount == rootSliceCap && !effectiveSwizzle)
	{
		return rootParent->retain();
	}

	MTL::Texture* view = effectiveSwizzle
		? rootParent->newTextureView(pixelFormat, textureType, NS::Range::Make(baseLevel, levelCount), NS::Range::Make(baseLayer, layerCount), *effectiveSwizzle)
		: rootParent->newTextureView(pixelFormat, textureType, NS::Range::Make(baseLevel, levelCount), NS::Range::Make(baseLayer, layerCount));
	if (!view)
	{
		cemuLog_logOnce(LogType::Force,
			"[MTL VIEW FAILURE] baseFormat={} viewFormat={} baseType={} viewType={} usage={} mip={}/{} slice={}/{} swizzle={}",
			(uint32)rootParent->pixelFormat(), (uint32)pixelFormat,
			(uint32)rootParent->textureType(), (uint32)textureType, (uint32)rootParent->usage(),
			baseLevel, levelCount, baseLayer, layerCount, effectiveSwizzle != nullptr);
		return rootParent->retain();
	}
	return view;
}

LatteTextureViewMtl::LatteTextureViewMtl(MetalRenderer* mtlRenderer, LatteTextureMtl* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
	: LatteTextureView(texture, firstMip, mipCount, firstSlice, sliceCount, dim, format), m_mtlr(mtlRenderer), m_baseTexture(texture)
{
    m_rgbaView = CreateUnswizzledView();
}

LatteTextureViewMtl::~LatteTextureViewMtl()
{
    if (m_rgbaView)
        m_rgbaView->release();
	for (sint32 i = 0; i < std::size(m_viewCache); i++)
    {
        if (m_viewCache[i].key != INVALID_SWIZZLE)
            m_viewCache[i].texture->release();
    }

    for (auto& [key, texture] : m_fallbackViewCache)
    {
        texture->release();
    }
}

void LatteTextureViewMtl::RefreshViews()
{
    if (m_rgbaView)
    {
        m_rgbaView->release();
        m_rgbaView = nullptr;
    }
    for (sint32 i = 0; i < std::size(m_viewCache); i++)
    {
        if (m_viewCache[i].key != INVALID_SWIZZLE)
        {
            m_viewCache[i].texture->release();
            m_viewCache[i] = {INVALID_SWIZZLE, nullptr};
        }
    }
    for (auto& [key, texture] : m_fallbackViewCache)
    {
        texture->release();
    }
    m_fallbackViewCache.clear();

    m_rgbaView = CreateUnswizzledView();
}

MTL::Texture* LatteTextureViewMtl::GetSwizzledView(uint32 gpuSamplerSwizzle)
{
    // Mask out
    gpuSamplerSwizzle &= 0x0FFF0000;

    // First, try to find a view in the cache

    // Fast-path: single check against MRU slot 0
    if (m_viewCache[0].key == gpuSamplerSwizzle)
        return m_viewCache[0].texture;

    // Search remaining cache slots and promote on hit
    sint32 freeIndex = (m_viewCache[0].key == INVALID_SWIZZLE) ? 0 : -1;
    for (sint32 i = 1; i < std::size(m_viewCache); i++)
    {
        auto& entry = m_viewCache[i];
        if (entry.key == gpuSamplerSwizzle)
        {
            std::swap(m_viewCache[0], entry);
            return m_viewCache[0].texture;
        }
        else if (entry.key == INVALID_SWIZZLE && freeIndex == -1)
        {
            freeIndex = i;
        }
    }

    // The common path still has a free fixed-cache slot. Do not create an
    // empty fallback-map entry for every first swizzle of every texture view.
    if (freeIndex == -1)
    {
        auto fallback = m_fallbackViewCache.find(gpuSamplerSwizzle);
        if (fallback != m_fallbackViewCache.end())
            return fallback->second;
    }

    MTL::Texture* texture = CreateSwizzledView(gpuSamplerSwizzle);
    if (freeIndex != -1)
        m_viewCache[freeIndex] = {gpuSamplerSwizzle, texture};
    else
        m_fallbackViewCache.emplace(gpuSamplerSwizzle, texture);

    return texture;
}

MTL::Texture* LatteTextureViewMtl::CreateUnswizzledView()
{
	MTL::TextureType textureType;
    switch (dim)
    {
    case Latte::E_DIM::DIM_1D:
        textureType = MTL::TextureType1D;
        break;
    case Latte::E_DIM::DIM_2D:
    case Latte::E_DIM::DIM_2D_MSAA:
        textureType = MTL::TextureType2D;
        break;
    case Latte::E_DIM::DIM_2D_ARRAY:
        textureType = MTL::TextureType2DArray;
        break;
    case Latte::E_DIM::DIM_3D:
        textureType = MTL::TextureType3D;
        break;
    case Latte::E_DIM::DIM_CUBEMAP:
        cemu_assert_debug(this->numSlice % 6 == 0 && "cubemaps must have an array length multiple of 6");

        textureType = MTL::TextureTypeCubeArray;
        break;
    default:
        cemu_assert_unimplemented();
        textureType = MTL::TextureType2D;
        break;
    }

    uint32 baseLevel = firstMip;
    uint32 levelCount = this->numMip;
    uint32 baseLayer = 0;
    uint32 layerCount = 1;
    if (textureType == MTL::TextureType3D)
    {
        cemu_assert_debug(firstMip == 0);
        cemu_assert_debug(this->numSlice == baseTexture->depth);
    }
    else
    {
        baseLayer = firstSlice;
        if (textureType == MTL::TextureTypeCubeArray || textureType == MTL::TextureType2DArray)
            layerCount = this->numSlice;
    }

    auto pixelFormat = GetMtlPixelFormat(format, m_baseTexture->isDepth);
    return NewClampedTextureView(m_baseTexture, pixelFormat, textureType, baseLevel, levelCount, baseLayer, layerCount, nullptr);
}

MTL::Texture* LatteTextureViewMtl::CreateSwizzledView(uint32 gpuSamplerSwizzle)
{
    uint32 compSelR = (gpuSamplerSwizzle >> 16) & 0x7;
	uint32 compSelG = (gpuSamplerSwizzle >> 19) & 0x7;
	uint32 compSelB = (gpuSamplerSwizzle >> 22) & 0x7;
	uint32 compSelA = (gpuSamplerSwizzle >> 25) & 0x7;
	compSelR = LatteTextureMtl_AdjustTextureCompSel(format, compSelR);
	compSelG = LatteTextureMtl_AdjustTextureCompSel(format, compSelG);
	compSelB = LatteTextureMtl_AdjustTextureCompSel(format, compSelB);
	compSelA = LatteTextureMtl_AdjustTextureCompSel(format, compSelA);

	MTL::TextureType textureType;
    switch (dim)
    {
    case Latte::E_DIM::DIM_1D:
        textureType = MTL::TextureType1D;
        break;
    case Latte::E_DIM::DIM_2D:
    case Latte::E_DIM::DIM_2D_MSAA:
        textureType = MTL::TextureType2D;
        break;
    case Latte::E_DIM::DIM_2D_ARRAY:
        textureType = MTL::TextureType2DArray;
        break;
    case Latte::E_DIM::DIM_3D:
        textureType = MTL::TextureType3D;
        break;
    case Latte::E_DIM::DIM_CUBEMAP:
        cemu_assert_debug(this->numSlice % 6 == 0 && "cubemaps must have an array length multiple of 6");

        textureType = MTL::TextureTypeCubeArray;
        break;
    default:
        cemu_assert_unimplemented();
        textureType = MTL::TextureType2D;
        break;
    }

    uint32 baseLevel = firstMip;
    uint32 levelCount = this->numMip;
    uint32 baseLayer = 0;
    uint32 layerCount = 1;
    // TODO: check if base texture is 3D texture as well
    if (textureType == MTL::TextureType3D)
    {
        cemu_assert_debug(firstMip == 0);
        cemu_assert_debug(this->numSlice == baseTexture->depth);
    }
    else
    {
        baseLayer = firstSlice;
        if (textureType == MTL::TextureTypeCubeArray || textureType == MTL::TextureType2DArray)
            layerCount = this->numSlice;
    }

    MTL::TextureSwizzleChannels swizzle;
    swizzle.red = GetMtlTextureSwizzle(compSelR);
    swizzle.green = GetMtlTextureSwizzle(compSelG);
    swizzle.blue = GetMtlTextureSwizzle(compSelB);
    swizzle.alpha = GetMtlTextureSwizzle(compSelA);

    auto pixelFormat = GetMtlPixelFormat(format, m_baseTexture->isDepth);

    bool isIdentitySwizzle = (swizzle.red == MTL::TextureSwizzleRed &&
                              swizzle.green == MTL::TextureSwizzleGreen &&
                              swizzle.blue == MTL::TextureSwizzleBlue &&
                              swizzle.alpha == MTL::TextureSwizzleAlpha);

    return NewClampedTextureView(m_baseTexture, pixelFormat, textureType, baseLevel, levelCount, baseLayer, layerCount, isIdentitySwizzle ? nullptr : &swizzle);
}
