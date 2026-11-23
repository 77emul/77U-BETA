// Real-GPU test for MetalTextureShape.h: every sanitized descriptor must be
// accepted by newTexture. Metal aborts the process on a rejected descriptor,
// so a regression shows up as a crash of this test.
// Build: clang++ -std=c++20 -fobjc-arc -I src -I dependencies/metal-cpp platform/ipados/Tests/MetalTextureShapeTests.mm
//        -framework Metal -framework Foundation
#import <Metal/Metal.h>
#include "Cafe/HW/Latte/Renderer/Metal/MetalTextureShape.h"
#include <cassert>
#include <cstdio>

int main()
{
	@autoreleasepool
	{
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		assert(device && [device supportsFamily:MTLGPUFamilyApple1]);

		// The iPad A16 BotW crash: BC 3D texture transcoded to EAC.
		MetalTextureShape crash{MTL::TextureType3D, MTL::PixelFormatEAC_RGBA8, 64, 64, 16, 1, 7};
		assert(SanitizeMetalTextureShape(crash) && crash.format == MTL::PixelFormatRGBA8Unorm && crash.mipLevels == 7);
		MetalTextureShape srgb{MTL::TextureType3D, MTL::PixelFormatEAC_RGBA8_sRGB, 8, 8, 8, 1, 1};
		assert(SanitizeMetalTextureShape(srgb) && srgb.format == MTL::PixelFormatRGBA8Unorm_sRGB);
		MetalTextureShape oneD{MTL::TextureType1D, MTL::PixelFormatRGBA8Unorm, 256, 1, 1, 1, 9};
		assert(SanitizeMetalTextureShape(oneD) && oneD.mipLevels == 1);
		MetalTextureShape valid{MTL::TextureType2D, MTL::PixelFormatEAC_RGBA8, 256, 128, 1, 1, 9};
		assert(!SanitizeMetalTextureShape(valid));
		uint32_t bytesPerPixel = 0;
		assert(GetETCDecodedPixelFormat(MTL::PixelFormatEAC_RG11Snorm, bytesPerPixel) == MTL::PixelFormatRG8Snorm && bytesPerPixel == 2);

		const MTL::TextureType types[] = {MTL::TextureType1D, MTL::TextureType2D, MTL::TextureType2DArray, MTL::TextureType3D, MTL::TextureTypeCubeArray};
		const MTL::PixelFormat formats[] = {MTL::PixelFormatEAC_RGBA8, MTL::PixelFormatEAC_RGBA8_sRGB, MTL::PixelFormatEAC_R11Unorm,
			MTL::PixelFormatEAC_R11Snorm, MTL::PixelFormatEAC_RG11Unorm, MTL::PixelFormatEAC_RG11Snorm, MTL::PixelFormatRGBA8Unorm};
		const uint32_t sizes[][3] = {{64, 64, 6}, {1, 1, 1}, {0, 0, 0}, {300, 7, 12}, {20000, 4, 3000}};
		const uint32_t mips[] = {1, 4, 30};
		unsigned created = 0;
		for (auto type : types)
			for (auto format : formats)
				for (auto& size : sizes)
					for (auto mip : mips)
					{
						MetalTextureShape s{type, format, size[0], size[1], size[2], type == MTL::TextureTypeCubeArray ? size[2] / 6 : size[2], mip};
						SanitizeMetalTextureShape(s);
						if (type == MTL::TextureTypeCubeArray && s.width != s.height)
							s.height = s.width; // cube faces are square in the caller (guest constraint)
						MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
						desc.textureType = (MTLTextureType)s.type;
						desc.pixelFormat = (MTLPixelFormat)s.format;
						desc.width = s.width;
						desc.height = s.height;
						desc.mipmapLevelCount = s.mipLevels;
						if (type == MTL::TextureType3D)
							desc.depth = s.depth;
						else if (type == MTL::TextureType2DArray || type == MTL::TextureTypeCubeArray)
							desc.arrayLength = s.arrayLength;
						desc.storageMode = MTLStorageModePrivate;
						desc.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
						// Huge sizes may legitimately fail allocation (nil); they must never abort.
						if ([device newTextureWithDescriptor:desc])
							++created;
					}
		assert(created > 0);
		std::printf("MetalTextureShapeTests passed (%u textures created)\n", created);
	}
	return 0;
}
