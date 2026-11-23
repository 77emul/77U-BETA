#pragma once

#include <Metal/MTLPixelFormat.hpp>
#include <Metal/MTLTexture.hpp>

#include <algorithm>
#include <cstdint>

// newTexture aborts the process (not nil, not catchable) on descriptors Metal
// rejects. iPad A16: BotW 3D BC textures transcoded to EAC killed LatteThread.

inline bool IsETCPixelFormat(MTL::PixelFormat format)
{
	return format >= MTL::PixelFormatEAC_R11Unorm && format <= MTL::PixelFormatETC2_RGB8A1_sRGB;
}

// Uncompressed layout of the CPU BC decoder output for each ETC2/EAC transcode target.
inline MTL::PixelFormat GetETCDecodedPixelFormat(MTL::PixelFormat format, uint32_t& bytesPerPixel)
{
	switch (format)
	{
	case MTL::PixelFormatEAC_R11Unorm: bytesPerPixel = 1; return MTL::PixelFormatR8Unorm;
	case MTL::PixelFormatEAC_R11Snorm: bytesPerPixel = 1; return MTL::PixelFormatR8Snorm;
	case MTL::PixelFormatEAC_RG11Unorm: bytesPerPixel = 2; return MTL::PixelFormatRG8Unorm;
	case MTL::PixelFormatEAC_RG11Snorm: bytesPerPixel = 2; return MTL::PixelFormatRG8Snorm;
	case MTL::PixelFormatEAC_RGBA8_sRGB: bytesPerPixel = 4; return MTL::PixelFormatRGBA8Unorm_sRGB;
	default: bytesPerPixel = 4; return MTL::PixelFormatRGBA8Unorm;
	}
}

struct MetalTextureShape
{
	MTL::TextureType type;
	MTL::PixelFormat format;
	uint32_t width, height, depth, arrayLength, mipLevels;
};

// Bring a descriptor inside Metal's limits on Apple GPUs. Returns true if anything changed.
inline bool SanitizeMetalTextureShape(MetalTextureShape& s)
{
	const MetalTextureShape before = s;
	const bool is3D = s.type == MTL::TextureType3D;
	const bool is1D = s.type == MTL::TextureType1D || s.type == MTL::TextureType1DArray;
	// ETC2/EAC exist only as 2D, 2D array and cube textures.
	if ((is3D || is1D) && IsETCPixelFormat(s.format))
	{
		uint32_t bytesPerPixel;
		s.format = GetETCDecodedPixelFormat(s.format, bytesPerPixel);
	}
	const uint32_t maxSize = is3D ? 2048u : 16384u;
	s.width = std::clamp<uint32_t>(s.width, 1, maxSize);
	s.height = is1D ? 1u : std::clamp<uint32_t>(s.height, 1, maxSize);
	s.depth = is3D ? std::clamp<uint32_t>(s.depth, 1, maxSize) : 1u;
	s.arrayLength = std::clamp<uint32_t>(s.arrayLength, 1, s.type == MTL::TextureTypeCubeArray ? 2048 / 6 : 2048);
	uint32_t largest = std::max({s.width, s.height, s.depth});
	uint32_t maxMips = 1;
	while (largest >>= 1)
		++maxMips;
	s.mipLevels = is1D ? 1u : std::clamp<uint32_t>(s.mipLevels, 1, maxMips);
	return s.format != before.format || s.width != before.width || s.height != before.height ||
		s.depth != before.depth || s.arrayLength != before.arrayLength || s.mipLevels != before.mipLevels;
}
