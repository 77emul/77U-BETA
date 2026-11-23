#pragma once

#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
//#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Common/precompiled.h"
#include "HW/Latte/Core/LatteTextureLoader.h"

struct Uvec2 {
    uint32 x;
    uint32 y;
};

enum class MetalDataType
{
    NONE,
    INT,
    UINT,
    FLOAT,
};

struct MetalPixelFormatInfo {
    MTL::PixelFormat pixelFormat;
    MetalDataType dataType;
    size_t bytesPerBlock;
    Uvec2 blockTexelSize = {1, 1};
    bool hasStencil = false;
    TextureDecoder* textureDecoder = nullptr;
};

void CheckForPixelFormatSupport(const MetalPixelFormatSupport& support);

const MetalPixelFormatInfo GetMtlPixelFormatInfo(Latte::E_GX2SURFFMT format, bool isDepth);

MTL::PixelFormat GetMtlPixelFormat(Latte::E_GX2SURFFMT format, bool isDepth);

inline bool IsAnyDepthOrStencilFormat(MTL::PixelFormat format)
{
	return format == MTL::PixelFormatDepth16Unorm ||
	       format == MTL::PixelFormatDepth32Float ||
	       format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
	       format == MTL::PixelFormatDepth32Float_Stencil8 ||
	       format == MTL::PixelFormatStencil8 ||
	       format == MTL::PixelFormatX32_Stencil8 ||
	       format == MTL::PixelFormatX24_Stencil8;
}

inline bool ArePixelFormatsViewCompatible(MTL::PixelFormat a, MTL::PixelFormat b)
{
	if (a == b)
		return true;
	auto toLinear = [](MTL::PixelFormat f) -> MTL::PixelFormat {
		switch (f) {
		case MTL::PixelFormatRGBA8Unorm_sRGB: return MTL::PixelFormatRGBA8Unorm;
		case MTL::PixelFormatBGRA8Unorm_sRGB: return MTL::PixelFormatBGRA8Unorm;
		case MTL::PixelFormatBC1_RGBA_sRGB: return MTL::PixelFormatBC1_RGBA;
		case MTL::PixelFormatBC2_RGBA_sRGB: return MTL::PixelFormatBC2_RGBA;
		case MTL::PixelFormatBC3_RGBA_sRGB: return MTL::PixelFormatBC3_RGBA;
		case MTL::PixelFormatBC7_RGBAUnorm_sRGB: return MTL::PixelFormatBC7_RGBAUnorm;
		case MTL::PixelFormatEAC_RGBA8_sRGB: return MTL::PixelFormatEAC_RGBA8;
		default: return f;
		}
	};
	return toLinear(a) == toLinear(b);
}

inline uint32 GetMetalTextureSliceCount(MTL::Texture* tex)
{
	if (!tex)
		return 1;
	switch (tex->textureType())
	{
	case MTL::TextureTypeCube:
		return 6;
	case MTL::TextureTypeCubeArray:
		return 6 * (uint32)tex->arrayLength();
	case MTL::TextureType2DArray:
	case MTL::TextureType1DArray:
	case MTL::TextureType2DMultisampleArray:
		return (uint32)tex->arrayLength();
	default:
		return 1;
	}
}

inline MetalDataType GetColorBufferDataType(const uint32 index, const LatteContextRegister& lcr)
{
    auto format = LatteMRT::GetColorBufferFormat(index, lcr);
    return GetMtlPixelFormatInfo(format, false).dataType;
}

inline bool MetalFramebufferFetchReplicatesRed(Latte::E_GX2SURFFMT format)
{
	// Match LatteTextureMtl_AdjustTextureCompSel: GX2 R8 normalized texture
	// reads replicate red into G/B/A; raw Metal attachment reads do not.
	return format == Latte::E_GX2SURFFMT::R8_UNORM || format == Latte::E_GX2SURFFMT::R8_SNORM;
}

inline const char* GetDataTypeStr(MetalDataType dataType)
{
	switch (dataType)
	{
	case MetalDataType::INT:
	    return "int4";
	case MetalDataType::UINT:
	    return "uint4";
	case MetalDataType::FLOAT:
	    return "float4";
	default:
	    cemu_assert_suspicious();
		return "INVALID";
	}
}

size_t GetMtlTextureBytesPerRow(Latte::E_GX2SURFFMT format, bool isDepth, uint32 width);

size_t GetMtlTextureBytesPerImage(Latte::E_GX2SURFFMT format, bool isDepth, uint32 height, size_t bytesPerRow);

MTL::PrimitiveType GetMtlPrimitiveType(LattePrimitiveMode primitiveMode);

MTL::VertexFormat GetMtlVertexFormat(uint8 format);

uint32 GetMtlVertexFormatSize(uint8 format);

MTL::IndexType GetMtlIndexType(Renderer::INDEX_TYPE indexType);

MTL::BlendOperation GetMtlBlendOp(Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC combineFunc);

MTL::BlendFactor GetMtlBlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor);

MTL::CompareFunction GetMtlCompareFunc(Latte::E_COMPAREFUNC func);

MTL::SamplerAddressMode GetMtlSamplerAddressMode(Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clamp);

MTL::TextureSwizzle GetMtlTextureSwizzle(uint32 swizzle);

MTL::StencilOperation GetMtlStencilOp(Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION action);

MTL::ColorWriteMask GetMtlColorWriteMask(uint8 mask);

inline MTL::BlitOption GetMtlBlitOption(MTL::PixelFormat pixelFormat)
{
	switch (pixelFormat)
	{
	case MTL::PixelFormatDepth16Unorm:
	case MTL::PixelFormatDepth32Float:
		return MTL::BlitOptionDepthFromDepthStencil;
	case MTL::PixelFormatStencil8:
		return MTL::BlitOptionStencilFromDepthStencil;
	case MTL::PixelFormatDepth24Unorm_Stencil8:
	case MTL::PixelFormatDepth32Float_Stencil8:
		return MTL::BlitOptionDepthFromDepthStencil;
	default:
		return MTL::BlitOptionNone;
	}
}
