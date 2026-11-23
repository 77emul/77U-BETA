#!/usr/bin/env python3
"""Exercise the actual GX2 depth-copy path on a Metal GPU.

The source depth attachment must be stored before sampling, and must not be
reattached to the utility color-only pipeline. Verify copied pixels, including
real depth-array slices and mipmaps, rather than accepting a submitted command buffer.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp').read_text()
common = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalCommon.h').read_text()
cleanup = common[common.index('__attribute__((unused)) static inline void StackAutoRelease'):
                 common.index('// Cast from const char*')]

def method(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

fixture = r'''
#define NS_PRIVATE_IMPLEMENTATION
#include <Foundation/NSAutoreleasePool.hpp>
#undef NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include "Cafe/HW/Latte/Renderer/Metal/UtilityShaderSource.h"
using sint32 = int32_t;
using uint32 = uint32_t;
#define GET_HELPER_BUFFER_BINDING(index) (28 + index)
#define GET_HELPER_TEXTURE_BINDING(index) (29 + index)
#define METAL_SHADER_TYPE_FRAGMENT 1
enum class LogType { Force };
template<class... T> void cemuLog_log(LogType, const char* text, T...) { puts(text); }
void require(bool b, const char* text) { if (!b) { puts(text); std::exit(1); } }
namespace Latte {
    enum class E_DIM { DIM_2D, DIM_2D_ARRAY };
    using E_GX2SURFFMT = MTL::PixelFormat;
}
struct LatteTexture;
struct LatteTextureView {
    LatteTexture* baseTexture = nullptr;
    sint32 firstMip = 0, firstSlice = 0, numMip = 1, numSlice = 1;
    Latte::E_DIM dim = Latte::E_DIM::DIM_2D;
    Latte::E_GX2SURFFMT format = MTL::PixelFormatInvalid;
};
struct LatteTextureViewMtl : LatteTextureView {
    MTL::Texture* texture = nullptr;
    MTL::Texture* GetRGBAView() { return texture; }
};
struct LatteTexture {
    bool isDepth = false;
    Latte::E_DIM dim = Latte::E_DIM::DIM_2D;
    Latte::E_GX2SURFFMT format = MTL::PixelFormatInvalid;
    MTL::Texture* texture = nullptr;
    std::vector<LatteTextureView*> views;
    ~LatteTexture() {
        for (auto* view : views) {
            static_cast<LatteTextureViewMtl*>(view)->texture->release();
            delete static_cast<LatteTextureViewMtl*>(view);
        }
        if (texture) texture->release();
    }
    LatteTextureView* CreateView(Latte::E_DIM viewDim, Latte::E_GX2SURFFMT viewFormat,
                                int mip, int mipCount, int slice, int sliceCount) {
        auto* v = new LatteTextureViewMtl;
        v->baseTexture = this; v->firstMip = mip; v->firstSlice = slice;
        v->numMip = mipCount; v->numSlice = sliceCount; v->dim = viewDim; v->format = viewFormat;
        v->texture = texture->newTextureView(viewFormat,
            viewDim == Latte::E_DIM::DIM_2D ? MTL::TextureType2D : MTL::TextureType2DArray,
            NS::Range::Make(mip,mipCount), NS::Range::Make(slice,sliceCount));
        require(v->texture, "View creation failed");
        views.push_back(v); return v;
    }
    // Actual cache lookup overloads from LatteTexture.h are inserted below.
    VIEW_METHODS
};
void LatteTexture_scaleToEffectiveSize(LatteTexture*, sint32*, sint32*, int) {}
namespace LatteMRT {
    LatteTextureView* color = nullptr;
    LatteTextureView* depth = nullptr;
    void UpdateCurrentFBO() {}
    LatteTextureView* GetColorAttachment(int) { return color; }
    LatteTextureView* GetDepthAttachment() { return depth; }
    void GetVirtualViewportDimensions(sint32& w, sint32& h) { w = 8; h = 8; }
}
void LatteRenderTarget_updateViewport() {}
struct MetalRenderer {
    MTL::Device* m_device;
    MTL::CommandQueue* queue;
    MTL::CommandBuffer* commands;
    MTL::RenderCommandEncoder* encoder = nullptr;
    MTL::RenderPipelineDescriptor* m_copyDepthToColorDesc;
    std::map<MTL::PixelFormat, MTL::RenderPipelineState*> m_copyDepthToColorPipelines;
    MTL::RenderPipelineDescriptor* m_copyColorToDepthDesc;
    std::map<MTL::PixelFormat, MTL::RenderPipelineState*> m_copyColorToDepthPipelines;
    MTL::DepthStencilState* m_copyColorToDepthState;

    struct {
        struct {
            MTL::RenderPipelineState* m_renderPipelineState = nullptr;
            std::array<std::array<std::array<void*,1>,31>,2> m_buffers{};
        } m_encoderState;
    } m_state;
    MetalRenderer() {
        m_device = MTL::CreateSystemDefaultDevice(); require(m_device, "No Metal GPU");
        queue = m_device->newCommandQueue(); commands = queue->commandBuffer()->retain();
        NS::Error* error = nullptr;
        auto* library = m_device->newLibrary(NS::String::string(utilityShaderSource, NS::UTF8StringEncoding), nullptr, &error);
        require(library, "Utility shader compile failed");
        m_copyDepthToColorDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        auto* vs = library->newFunction(NS::String::string("vertexFullscreen", NS::UTF8StringEncoding));
        auto* ps = library->newFunction(NS::String::string("fragmentCopyDepthToColor", NS::UTF8StringEncoding));
        m_copyDepthToColorDesc->setVertexFunction(vs); m_copyDepthToColorDesc->setFragmentFunction(ps);
        auto* reverse = library->newFunction(NS::String::string("fragmentCopyColorToDepth",NS::UTF8StringEncoding));
        m_copyColorToDepthDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        m_copyColorToDepthDesc->setVertexFunction(vs); m_copyColorToDepthDesc->setFragmentFunction(reverse);
        auto* ds = MTL::DepthStencilDescriptor::alloc()->init();
        ds->setDepthCompareFunction(MTL::CompareFunctionAlways); ds->setDepthWriteEnabled(true);
        m_copyColorToDepthState = m_device->newDepthStencilState(ds);
        ds->release(); reverse->release(); vs->release(); ps->release(); library->release();
    }
    void EndEncoding() { if (encoder) { encoder->endEncoding(); encoder->release(); encoder = nullptr; } }
    void FlushPendingTextureClears(LatteTexture*, uint32, uint32, uint32, uint32) {}
    void SetTexture(MTL::RenderCommandEncoder* enc, int, MTL::Texture* tex, int slot) { enc->setFragmentTexture(tex, slot); }
    MTL::RenderCommandEncoder* GetTemporaryRenderCommandEncoder(MTL::RenderPassDescriptor* rpd) {
        EndEncoding(); encoder = commands->renderCommandEncoder(rpd)->retain(); return encoder;
    }
    MTL::RenderCommandEncoder* GetRenderCommandEncoder() {
        auto* rpd = MTL::RenderPassDescriptor::renderPassDescriptor();
        rpd->colorAttachments()->object(0)->setTexture(static_cast<LatteTextureViewMtl*>(LatteMRT::color)->texture);
        rpd->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionLoad);
        rpd->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
        rpd->depthAttachment()->setTexture(static_cast<LatteTextureViewMtl*>(LatteMRT::depth)->texture);
        rpd->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
        rpd->depthAttachment()->setStoreAction(MTL::StoreActionStore);
        return GetTemporaryRenderCommandEncoder(rpd);
    }
    void texture_copyImageSubData(LatteTexture*, int, int, int, int, LatteTexture*, int, int, int, int, int, int, int) {
        require(false, "Depth/color conversion fell through to incompatible blit");
    }
    void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture*, sint32, sint32, LatteTexture*, sint32, sint32, sint32, sint32);
    void draw_handleSpecialState5();
};
'''

# Use the real cache lookup methods; the fixture only supplies Metal-backed
# texture creation in place of the full emulator resource registry.
header = (repo / 'src/Cafe/HW/Latte/Core/LatteTexture.h').read_text()
start = header.index('\tLatteTextureView* GetOrCreateView(')
end = header.index('\n\tbool IsCompressedFormat()', start)
fixture = fixture.replace('VIEW_METHODS', header[start:end])

tests = r'''
int main() {
    auto* pool = NS::AutoreleasePool::alloc()->init();
    MetalRenderer renderer;
    // Include the exact single-slice 2DArray binding from the M4 capture.
    // Distinct values and real mip/slice storage catch wrong slice routing.
    for (int variant = 0; variant < 3; ++variant) {
        LatteTexture source, destination;
        source.isDepth = true;
        const bool array = variant != 0;
        const int mip = variant == 2 ? 1 : 0, slice = array ? 1 : 0;
        const int baseSize = mip ? 16 : 8;
        const float expected = array ? 0.625f : 0.25f;
        source.dim = destination.dim = array ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D;
        source.format = array ? MTL::PixelFormatDepth16Unorm : MTL::PixelFormatDepth32Float;
        destination.format = MTL::PixelFormatR32Float;
        auto* dd = MTL::TextureDescriptor::texture2DDescriptor(source.format, baseSize, baseSize, mip != 0);
        dd->setTextureType(array ? MTL::TextureType2DArray : MTL::TextureType2D);
        dd->setArrayLength(array ? 2 : 1); dd->setMipmapLevelCount(mip + 1);
        dd->setStorageMode(MTL::StorageModePrivate);
        dd->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView);
        source.texture = renderer.m_device->newTexture(dd); require(source.texture, "Depth texture allocation");
        auto* cd = MTL::TextureDescriptor::texture2DDescriptor(destination.format, baseSize, baseSize, mip != 0);
        cd->setTextureType(array ? MTL::TextureType2DArray : MTL::TextureType2D);
        cd->setArrayLength(array ? 2 : 1); cd->setMipmapLevelCount(mip + 1);
        cd->setStorageMode(MTL::StorageModeShared);
        cd->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsagePixelFormatView);
        destination.texture = renderer.m_device->newTexture(cd); require(destination.texture, "Color texture allocation");
        std::array<float,64> seed; seed.fill(0.9f);
        for (int layer=0; layer<(array ? 2 : 1); ++layer) {
            destination.texture->replaceRegion(MTL::Region(0,0,8,8), mip, layer, seed.data(), 32, 256);
            auto* clear = MTL::RenderPassDescriptor::renderPassDescriptor();
            clear->depthAttachment()->setTexture(source.texture);
            clear->depthAttachment()->setLevel(mip); clear->depthAttachment()->setSlice(layer);
            clear->depthAttachment()->setLoadAction(MTL::LoadActionClear);
            clear->depthAttachment()->setStoreAction(MTL::StoreActionStore);
            clear->depthAttachment()->setClearDepth(layer == slice ? expected : 0.125f);
            renderer.GetTemporaryRenderCommandEncoder(clear); renderer.EndEncoding();
        }
        LatteMRT::depth = source.GetOrCreateView(mip,1,slice,1);
        LatteMRT::color = destination.GetOrCreateView(mip,1,slice,1);
        renderer.draw_handleSpecialState5();
        renderer.EndEncoding(); renderer.commands->commit(); renderer.commands->waitUntilCompleted();
        require(renderer.commands->status() == MTL::CommandBufferStatusCompleted, "Depth copy GPU command failed");
        std::array<float,64> pixels{};
        destination.texture->getBytes(pixels.data(), 32, 256, MTL::Region(0,0,8,8), mip, slice);
        for (float pixel : pixels) require(std::abs(pixel - expected) < 0.00002f, "Depth copy pixels not equal to stored source depth");
        if (array) {
            destination.texture->getBytes(pixels.data(),32,256,MTL::Region(0,0,8,8),mip,0);
            for (float pixel : pixels) require(pixel == 0.9f, "Depth copy overwrote another array slice");
        }
        printf("PASS: actual GX2 depth copy, %s, physical mip/slice %d/%d, 64 copied pixels and untouched neighbour\n", array ? "Depth16 array" : "Depth32 2D", mip, slice);
        renderer.commands->release(); renderer.commands = renderer.queue->commandBuffer()->retain();
    }

    // Splatoon copies its color shadow alias back to depth for sample_compare.
    // Read the result through the production depth-to-color path, not status.
    for (auto depthFormat : {MTL::PixelFormatDepth16Unorm, MTL::PixelFormatDepth32Float,
                             MTL::PixelFormatDepth32Float_Stencil8}) {
        for (int copySize : {8, 4}) {
            LatteTexture color, depth, readback;
            color.dim = depth.dim = readback.dim = Latte::E_DIM::DIM_2D_ARRAY;
            color.format = depthFormat == MTL::PixelFormatDepth16Unorm ? MTL::PixelFormatR16Unorm : MTL::PixelFormatR32Float;
            depth.format = depthFormat; depth.isDepth = true;
            readback.format = MTL::PixelFormatR32Float;
            const bool stencil = depthFormat == MTL::PixelFormatDepth32Float_Stencil8;
            auto make = [&](LatteTexture& texture, MTL::StorageMode storage) {
                auto* desc = MTL::TextureDescriptor::texture2DDescriptor(texture.format,16,16,true);
                desc->setTextureType(MTL::TextureType2DArray); desc->setArrayLength(2);
                desc->setStorageMode(storage);
                desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView);
                texture.texture = renderer.m_device->newTexture(desc);
                require(texture.texture, "Color-to-depth test allocation failed");
            };
            make(color,MTL::StorageModeShared); make(depth,MTL::StorageModePrivate); make(readback,MTL::StorageModeShared);
            std::array<float,64> floats; floats.fill(0.625f);
            std::array<uint16_t,64> unorm; unorm.fill(40959);
            for(int layer=0;layer<2;++layer) {
                color.texture->replaceRegion(MTL::Region(0,0,8,8),1,layer,
                    color.format == MTL::PixelFormatR16Unorm ? static_cast<const void*>(unorm.data()) : floats.data(),
                    color.format == MTL::PixelFormatR16Unorm ? 16 : 32, 0);
                auto* clear = MTL::RenderPassDescriptor::renderPassDescriptor();
                clear->depthAttachment()->setTexture(depth.texture);
                clear->depthAttachment()->setLevel(1); clear->depthAttachment()->setSlice(layer);
                clear->depthAttachment()->setLoadAction(MTL::LoadActionClear);
                clear->depthAttachment()->setStoreAction(MTL::StoreActionStore);
                clear->depthAttachment()->setClearDepth(0.9);
                if(stencil) {
                    clear->stencilAttachment()->setTexture(depth.texture);
                    clear->stencilAttachment()->setLevel(1); clear->stencilAttachment()->setSlice(layer);
                    clear->stencilAttachment()->setLoadAction(MTL::LoadActionClear);
                    clear->stencilAttachment()->setStoreAction(MTL::StoreActionStore);
                    clear->stencilAttachment()->setClearStencil(0x37);
                }
                renderer.GetTemporaryRenderCommandEncoder(clear); renderer.EndEncoding();
            }
            renderer.surfaceCopy_copySurfaceWithFormatConversion(&color,1,1,&depth,1,1,copySize,copySize);
            for(int layer=0;layer<2;++layer)
                renderer.surfaceCopy_copySurfaceWithFormatConversion(&depth,1,layer,&readback,1,layer,8,8);
            MTL::Buffer* stencilBytes = nullptr;
            if(stencil) {
                stencilBytes = renderer.m_device->newBuffer(2048,MTL::ResourceStorageModeShared);
                auto* blit = renderer.commands->blitCommandEncoder();
                blit->copyFromTexture(depth.texture,1,1,MTL::Origin(0,0,0),MTL::Size(8,8,1),stencilBytes,0,256,2048,MTL::BlitOptionStencilFromDepthStencil);
                blit->endEncoding();
            }
            renderer.commands->commit(); renderer.commands->waitUntilCompleted();
            require(renderer.commands->status() == MTL::CommandBufferStatusCompleted,"Color-to-depth GPU command failed");
            for(int layer=0;layer<2;++layer) {
                std::array<float,64> pixels;
                readback.texture->getBytes(pixels.data(),32,256,MTL::Region(0,0,8,8),1,layer);
                for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
                    const float expected = layer == 1 && x<copySize && y<copySize
                        ? (color.format == MTL::PixelFormatR16Unorm ? 40959.f/65535.f : 0.625f) : 0.9f;
                    require(std::abs(pixels[y*8+x]-expected)<0.00002f,"Color-to-depth pixels wrong or uncopied pixels changed");
                }
            }
            if(stencilBytes) {
                auto* bytes = static_cast<uint8_t*>(stencilBytes->contents());
                for(int y=0;y<8;++y) for(int x=0;x<8;++x)
                    require(bytes[y*256+x]==0x37,"Color-to-depth copy damaged stencil");
                stencilBytes->release();
            }
            printf("PASS: color-to-depth format %lu, %dx%d, array mip/slice 1/1 and neighbour/stencil preserved\n",(unsigned long)depthFormat,copySize,copySize);
            renderer.commands->release(); renderer.commands = renderer.queue->commandBuffer()->retain();
        }
    }
    pool->release();
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-depth-copy-') as tmp:
    path = Path(tmp)
    production = '\n'.join(method(s) for s in [
        'void MetalRenderer::surfaceCopy_copySurfaceWithFormatConversion(',
        'void MetalRenderer::draw_handleSpecialState5()',
    ])
    (path / 'test.cpp').write_text(fixture + cleanup + production + tests)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O3', '-DNDEBUG',
                    '-I'+str(repo/'dependencies/metal-cpp'), '-I'+str(repo/'src'),
                    str(path/'test.cpp'), '-framework', 'Metal', '-framework', 'Foundation',
                    '-o', str(path/'test')], check=True)
    env = dict(os.environ, MTL_DEBUG_LAYER='1')
    subprocess.run([str(path/'test')], env=env, check=True)
