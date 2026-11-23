#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalTextureShape.h"
#include "Cemu/Logging/CemuLogging.h"

LatteTextureMtl::LatteTextureMtl(class MetalRenderer* mtlRenderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle,
	Latte::E_HWTILEMODE tileMode, bool isDepth)
	: LatteTexture(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth), m_mtlr(mtlRenderer)
{
    NS_STACK_SCOPED MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
    desc->setStorageMode(MTL::StorageModePrivate);
    //desc->setCpuCacheMode(MTL::CPUCacheModeWriteCombined);

	sint32 effectiveBaseWidth = width;
	sint32 effectiveBaseHeight = height;
	sint32 effectiveBaseDepth = depth;
	if (overwriteInfo.hasResolutionOverwrite)
	{
		effectiveBaseWidth = overwriteInfo.width;
		effectiveBaseHeight = overwriteInfo.height;
		effectiveBaseDepth = overwriteInfo.depth;
	}
	effectiveBaseWidth = std::max(1, effectiveBaseWidth);
	effectiveBaseHeight = std::max(1, effectiveBaseHeight);
	effectiveBaseDepth = std::max(1, effectiveBaseDepth);

	MTL::TextureType textureType;
	switch (dim)
    {
    case Latte::E_DIM::DIM_1D:
        textureType = MTL::TextureType1D;
        effectiveBaseHeight = 1;
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
        cemu_assert_debug(effectiveBaseDepth % 6 == 0 && "cubemaps must have an array length multiple of 6");

        textureType = MTL::TextureTypeCubeArray;
        break;
    default:
        cemu_assert_unimplemented();
        textureType = MTL::TextureType2D;
        break;
    }
    desc->setTextureType(textureType);

    // Clamp mip levels
    mipLevels = std::min(mipLevels, (uint32)maxPossibleMipLevels);
    mipLevels = std::max(mipLevels, (uint32)1);

	MetalTextureShape shape{textureType, GetMtlPixelFormat(format, isDepth), (uint32)effectiveBaseWidth, (uint32)effectiveBaseHeight,
		(uint32)effectiveBaseDepth, 1, mipLevels};
	if (textureType == MTL::TextureTypeCubeArray)
		shape.arrayLength = effectiveBaseDepth / 6;
	else if (textureType == MTL::TextureType2DArray)
		shape.arrayLength = effectiveBaseDepth;
	// texture_loadSlice/copy follow the stored Metal format, so a decoded ETC fallback stays consistent.
	if (SanitizeMetalTextureShape(shape))
		cemuLog_logOnce(LogType::Force, "Metal texture descriptor adjusted: type {} fmt {}->{} {}x{}x{} layers {} mips {}->{}",
			(uint32)textureType, (uint32)GetMtlPixelFormat(format, isDepth), (uint32)shape.format, shape.width, shape.height,
			shape.depth, shape.arrayLength, mipLevels, shape.mipLevels);

	desc->setWidth(shape.width);
	desc->setHeight(shape.height);
	desc->setMipmapLevelCount(shape.mipLevels);
	if (textureType == MTL::TextureType3D)
		desc->setDepth(shape.depth);
	else if (textureType == MTL::TextureTypeCubeArray || textureType == MTL::TextureType2DArray)
		desc->setArrayLength(shape.arrayLength);

	const auto pixelFormat = shape.format;
	desc->setPixelFormat(pixelFormat);

	MTL::TextureUsage usage = MTL::TextureUsageShaderRead;
	if (!isDepth)
		usage |= MTL::TextureUsagePixelFormatView;
	if (FormatIsRenderable(format) && textureType != MTL::TextureType3D)
		usage |= MTL::TextureUsageRenderTarget;
	desc->setUsage(usage);

	// Reuse existing Metal texture if another descriptor exists with identical specs at the same physical address
	if (physAddress != 0)
	{
		for (auto* existingTex : LatteTexture::GetAllTextures())
		{
			if (existingTex && existingTex != this && existingTex->physAddress == physAddress &&
			    existingTex->width == width && existingTex->height == height &&
			    existingTex->format == format && existingTex->dim == dim &&
			    existingTex->depth == depth && existingTex->mipLevels == mipLevels &&
			    existingTex->isDepth == isDepth)
			{
				auto* existingMtl = static_cast<LatteTextureMtl*>(existingTex);
				if (existingMtl->GetTexture())
				{
					m_texture = existingMtl->GetTexture()->retain();
					break;
				}
			}
		}
	}

	if (!m_texture)
		m_texture = mtlRenderer->GetDevice()->newTexture(desc);
	if (!m_texture)
	{
		cemuLog_logOnce(LogType::Force, "Metal texture allocation failed ({}x{} fmt {}), using a 1x1 fallback", width, height, (uint32)format);
		desc->setTextureType(MTL::TextureType2D);
		desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
		desc->setWidth(1);
		desc->setHeight(1);
		desc->setMipmapLevelCount(1);
		desc->setUsage(MTL::TextureUsageShaderRead);
		m_texture = mtlRenderer->GetDevice()->newTexture(desc);
	}
	if (MetalResourceCrashDiagnostics_ShouldRecordTextureEvent(MetalResourceCrashEventType::TextureCreate))
	{
	MetalResourceCrashEvent event{};
	event.type = MetalResourceCrashEventType::TextureCreate;
	event.texturePtr = reinterpret_cast<uint64>(m_texture);
	event.ownerPtr = reinterpret_cast<uint64>(this);
	event.ownerObjectId = hostTextureObjectId;
	event.guestAddress = physAddress;
	event.width = width;
	event.height = height;
	event.pitch = pitch;
	event.pixelFormat = static_cast<uint32>(pixelFormat);
	event.tileMode = static_cast<uint32>(tileMode);
	event.flags = (isDepth ? 1u : 0u) | (FormatIsRenderable(format) ? 2u : 0u);
	if (mtlRenderer->IsCommandBufferActive())
	{
		auto commandBuffer = mtlRenderer->GetCurrentCommandBuffer();
		event.commandBufferPtr = reinterpret_cast<uint64>(commandBuffer);
		event.commandBufferStatus = static_cast<uint32>(commandBuffer->status());
	}
	MetalResourceCrashDiagnostics_Record(event);
	}
}

LatteTextureMtl::~LatteTextureMtl()
{
	if (MetalResourceCrashDiagnostics_ShouldRecordTextureEvent(MetalResourceCrashEventType::TextureDestroy))
	{
	MetalResourceCrashEvent event{};
	event.type = MetalResourceCrashEventType::TextureDestroy;
	event.texturePtr = reinterpret_cast<uint64>(m_texture);
	event.ownerPtr = reinterpret_cast<uint64>(this);
	event.ownerObjectId = hostTextureObjectId;
	event.guestAddress = physAddress;
	event.width = width;
	event.height = height;
	event.pitch = pitch;
	event.pixelFormat = m_texture ? static_cast<uint32>(m_texture->pixelFormat()) : 0;
	event.tileMode = static_cast<uint32>(tileMode);
	event.flags = (isDepth ? 1u : 0u) | (FormatIsRenderable(format) ? 2u : 0u);
	if (m_mtlr->IsCommandBufferActive())
	{
		auto commandBuffer = m_mtlr->GetCurrentCommandBuffer();
		event.commandBufferPtr = reinterpret_cast<uint64>(commandBuffer);
		event.commandBufferStatus = static_cast<uint32>(commandBuffer->status());
	}
	MetalResourceCrashDiagnostics_Record(event);
	}
	if (m_mtlr)
	{
		m_mtlr->DropPendingTextureClears(this);
	}
	if (m_texture)
		m_texture->release();
}

void LatteTextureMtl::AdoptSharedTexture(MTL::Texture* newTexture)
{
	if (m_texture == newTexture || !newTexture)
		return;
	if (m_texture)
		m_texture->release();
	m_texture = newTexture->retain();
	for (auto* view : views)
	{
		auto* viewMtl = dynamic_cast<LatteTextureViewMtl*>(view);
		if (viewMtl)
			viewMtl->RefreshViews();
	}
}

LatteTextureView* LatteTextureMtl::CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
{
	cemu_assert_debug(mipCount > 0);
	cemu_assert_debug(sliceCount > 0);
	cemu_assert_debug((firstMip + mipCount) <= this->mipLevels);
	cemu_assert_debug((firstSlice + sliceCount) <= this->depth);

	return new LatteTextureViewMtl(m_mtlr, this, dim, format, firstMip, mipCount, firstSlice, sliceCount);
}

// TODO: lazy allocation?
void LatteTextureMtl::AllocateOnHost()
{
	// The texture is already allocated
}
