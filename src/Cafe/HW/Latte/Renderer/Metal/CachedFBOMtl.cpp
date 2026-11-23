#include "Cafe/HW/Latte/Renderer/Metal/CachedFBOMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Cafe/HW/Latte/Core/Latte.h"

CachedFBOMtl::CachedFBOMtl(class MetalRenderer* metalRenderer, uint64 key) : LatteCachedFBO(key)
{
	m_renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();

	bool hasAttachment = false;
	for (int i = 0; i < 8; ++i)
	{
		const auto& buffer = colorBuffer[i];
		auto textureView = (LatteTextureViewMtl*)buffer.texture;
		if (!textureView)
		{
			continue;
		}
		auto rgbaView = textureView ? textureView->GetRGBAView() : nullptr;
		if (!rgbaView)
		{
			continue;
		}
		auto colorAttachment = m_renderPassDescriptor->colorAttachments()->object(i);
		colorAttachment->setTexture(rgbaView);
		colorAttachment->setLoadAction(MTL::LoadActionLoad);
		colorAttachment->setStoreAction(MTL::StoreActionStore);
		LatteGPUState.telemetry.loadActions.fetch_add(1, std::memory_order_relaxed);
		LatteGPUState.telemetry.storeActions.fetch_add(1, std::memory_order_relaxed);
		Latte_RecordRTFirstPass(textureView->baseTexture, 1 /* MTL::LoadActionLoad */, false, false, 0xF, textureView->baseTexture->width, textureView->baseTexture->height);

		const auto info = GetMtlPixelFormatInfo(textureView->format, false);
		const uint32 bx = (rgbaView->width() + info.blockTexelSize.x - 1) / info.blockTexelSize.x;
		const uint32 by = (rgbaView->height() + info.blockTexelSize.y - 1) / info.blockTexelSize.y;
		const uint64 attBytes = uint64(bx) * by * info.bytesPerBlock;
		m_estimatedColorBytesAttachment[i] = attBytes;
		m_estimatedColorBytes += attBytes;

		hasAttachment = true;
	}

	// setup depth attachment
	if (depthBuffer.texture)
	{
		auto textureView = static_cast<LatteTextureViewMtl*>(depthBuffer.texture);
		auto rgbaView = textureView ? textureView->GetRGBAView() : nullptr;
		if (rgbaView)
		{
			auto depthAttachment = m_renderPassDescriptor->depthAttachment();
			depthAttachment->setTexture(rgbaView);
			depthAttachment->setLoadAction(MTL::LoadActionLoad);
			depthAttachment->setStoreAction(MTL::StoreActionStore);
			LatteGPUState.telemetry.loadActions.fetch_add(1, std::memory_order_relaxed);
			LatteGPUState.telemetry.storeActions.fetch_add(1, std::memory_order_relaxed);

			const auto info = GetMtlPixelFormatInfo(depthBuffer.texture->format, true);
			const uint32 bx = (rgbaView->width() + info.blockTexelSize.x - 1) / info.blockTexelSize.x;
			const uint32 by = (rgbaView->height() + info.blockTexelSize.y - 1) / info.blockTexelSize.y;
			m_estimatedDepthBytes += uint64(bx) * by * info.bytesPerBlock;

			// setup stencil attachment
			if (depthBuffer.hasStencil && GetMtlPixelFormatInfo(depthBuffer.texture->format, true).hasStencil)
			{
				auto stencilAttachment = m_renderPassDescriptor->stencilAttachment();
				stencilAttachment->setTexture(rgbaView);
				stencilAttachment->setLoadAction(MTL::LoadActionLoad);
				stencilAttachment->setStoreAction(MTL::StoreActionStore);
				LatteGPUState.telemetry.loadActions.fetch_add(1, std::memory_order_relaxed);
				LatteGPUState.telemetry.storeActions.fetch_add(1, std::memory_order_relaxed);
			}

			hasAttachment = true;
		}
	}

	// HACK: setup a dummy color attachment to prevent Metal from discarding draws for stremout draws in Super Smash Bros. for Wii U (works fine on MoltenVK without this hack though)
	if (!hasAttachment)
	{
        auto colorAttachment = m_renderPassDescriptor->colorAttachments()->object(0);
    	colorAttachment->setTexture(metalRenderer->GetNullTexture2D());
    	colorAttachment->setLoadAction(MTL::LoadActionDontCare);
    	colorAttachment->setStoreAction(MTL::StoreActionDontCare);
	}

	// Visibility buffer
	m_renderPassDescriptor->setVisibilityResultBuffer(metalRenderer->GetOcclusionQueryResultBuffer());
}

CachedFBOMtl::~CachedFBOMtl()
{
	m_renderPassDescriptor->release();
}
