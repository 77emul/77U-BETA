#!/usr/bin/env python3
"""Exercise the production startup-frame path against a real Metal texture.

A drawable that was never written retains the magenta sentinel. The actual
BeginFrame/DrawEmptyFrame/clear implementations must encode opaque black before
presentation, including reused drawables and shader-cache loading frames.
"""
from pathlib import Path
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

production = '\n'.join(method(signature) for signature in [
    'bool MetalRenderer::BeginFrame(',
    'void MetalRenderer::DrawEmptyFrame(',
    'void MetalRenderer::ClearColorbuffer(bool padView, float',
    'void MetalRenderer::ClearColorTextureInternal(',
])

fixture = r'''
#define NS_PRIVATE_IMPLEMENTATION
#include <Foundation/NSAutoreleasePool.hpp>
#undef NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdint>
#undef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 1
#undef TARGET_OS_IOS
#define TARGET_OS_IOS 1
using sint32 = int32_t;
using uint32 = uint32_t;
struct Drawable {
    MTL::Texture* image = nullptr;
    MTL::Texture* texture() { return image; }
};
struct Layer {
    struct FGView { struct Size { double width=8,height=8; }; Size drawableSize() {return {};} };
    Drawable drawable;
    FGView view;
    Drawable* GetDrawable() { return &drawable; }
    FGView* GetLayer() { return &view; }
};
struct MetalRenderer {
    MTL::Device* device;
    MTL::CommandQueue* queue;
    MTL::CommandBuffer* commands = nullptr;
    MTL::RenderCommandEncoder* encoder = nullptr;
    Layer main, pad;
    Layer& m_mainLayer = main;
    bool available = true, padVisible = false;
    bool m_mainDrawableContentsDefined = false, m_padDrawableContentsDefined = false;
    unsigned acquires = 0, presents = 0;
    struct { unsigned m_clears = 0; } m_performanceMonitor;
    struct { bool m_usesSRGB = false; } m_state;
    MTL::Texture* m_frameGenTarget = nullptr;
    bool m_frameGenRendered = false, frameGeneration = false;
    int ActiveFrameGenerationTarget() const { return frameGeneration ? 60 : 0; }
    void EnsureFrameGenTextures(uint32,uint32,MTL::PixelFormat,bool=true) {
        assert(frameGeneration); m_frameGenTarget = main.drawable.image;
    }
    MetalRenderer() {
        device = MTL::CreateSystemDefaultDevice(); assert(device);
        queue = device->newCommandQueue(); assert(queue);
        for (auto* layer : {&main, &pad}) {
            auto* desc = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, 8, 8, false);
            desc->setUsage(MTL::TextureUsageRenderTarget);
            desc->setStorageMode(MTL::StorageModeShared);
            layer->drawable.image = device->newTexture(desc); assert(layer->drawable.image);
        }
    }
    ~MetalRenderer() {
        main.drawable.image->release(); pad.drawable.image->release();
        queue->release(); device->release();
    }
    bool ShouldDrawPadDisplay() { return padVisible; }
    bool AcquireDrawable(bool) { ++acquires; return available; }
    Layer& GetLayer(bool mainWindow) { return mainWindow ? main : pad; }
    MTL::CommandBuffer* GetCommandBuffer() {
        if (!commands) commands = queue->commandBuffer()->retain();
        return commands;
    }
    MTL::RenderCommandEncoder* GetTemporaryRenderCommandEncoder(MTL::RenderPassDescriptor* desc) {
        EndEncoding();
        encoder = GetCommandBuffer()->renderCommandEncoder(desc)->retain();
        return encoder;
    }
    void EndEncoding() {
        if (encoder) { encoder->endEncoding(); encoder->release(); encoder = nullptr; }
    }
    void Finish() {
        EndEncoding();
        GetCommandBuffer()->commit();
        // Only the host GPU test waits to read the pixel. Production BeginFrame
        // and DrawEmptyFrame do not introduce a completion wait.
        commands->waitUntilCompleted();
        assert(commands->status() == MTL::CommandBufferStatusCompleted);
        commands->release(); commands = nullptr;
    }
    void SwapBuffers(bool, bool) { ++presents; Finish(); }
    bool BeginFrame(bool);
    void DrawEmptyFrame(bool);
    void ClearColorbuffer(bool, float, float, float, float);
    void ClearColorTextureInternal(MTL::Texture*, sint32, sint32, float, float, float, float);
    void SeedMagenta(bool mainWindow) {
        std::array<uint8_t, 256> pixels{};
        for (size_t i=0; i<pixels.size(); i+=4) { pixels[i]=255; pixels[i+2]=255; pixels[i+3]=255; }
        GetLayer(mainWindow).drawable.image->replaceRegion(MTL::Region(0, 0, 8, 8), 0, pixels.data(), 32);
    }
    void CheckBlack(bool mainWindow) {
        std::array<uint8_t, 256> pixels{};
        GetLayer(mainWindow).drawable.image->getBytes(pixels.data(), 32, MTL::Region(0, 0, 8, 8), 0);
        for (size_t i=0; i<pixels.size(); i+=4)
            assert(pixels[i]==0 && pixels[i+1]==0 && pixels[i+2]==0 && pixels[i+3]==255);
    }
};
'''
tests = r'''
int main() {
    auto* pool = NS::AutoreleasePool::alloc()->init();
    {
        MetalRenderer renderer;
        for (int i=0; i<3; ++i) {
            renderer.SeedMagenta(true);
            renderer.DrawEmptyFrame(true);
            renderer.CheckBlack(true);
            assert(renderer.m_mainDrawableContentsDefined);
        }
        renderer.SeedMagenta(true);
        assert(renderer.BeginFrame(true)); // shader-cache loading also uses BeginFrame
        renderer.Finish(); renderer.CheckBlack(true);
        const auto clears = renderer.m_performanceMonitor.m_clears;
        const auto presents = renderer.presents;
        renderer.available = false;
        renderer.DrawEmptyFrame(true);
        assert(renderer.m_performanceMonitor.m_clears==clears && renderer.presents==presents);
        renderer.available = true;
        const auto acquired = renderer.acquires;
        renderer.DrawEmptyFrame(false);
        assert(renderer.acquires==acquired && renderer.presents==presents);
        renderer.padVisible = true;
        renderer.SeedMagenta(false);
        renderer.DrawEmptyFrame(false);
        renderer.CheckBlack(false);
        assert(renderer.m_padDrawableContentsDefined);
        renderer.frameGeneration = true;
        renderer.SeedMagenta(true);
        const auto nativeAcquires = renderer.acquires;
        assert(renderer.BeginFrame(true));
        renderer.Finish(); renderer.CheckBlack(true);
        assert(renderer.m_frameGenRendered && renderer.acquires == nativeAcquires);
        renderer.frameGeneration = false;
        renderer.m_frameGenTarget = nullptr;
        renderer.SeedMagenta(true); renderer.DrawEmptyFrame(true); renderer.CheckBlack(true);
    }
    pool->release();
    puts("PASS: real Metal startup and cache-loading frames are opaque black; reused, unavailable and hidden-pad drawables handled");
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-startup-frame-') as tmp:
    path = Path(tmp)
    (path / 'test.cpp').write_text(fixture + cleanup + production + tests)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O3',
                    '-I'+str(repo/'dependencies/metal-cpp'), str(path/'test.cpp'),
                    '-framework', 'Metal', '-framework', 'Foundation',
                    '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
