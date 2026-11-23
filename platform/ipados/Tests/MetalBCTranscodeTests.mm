// Real-GPU test for MetalBCTranscodeShaderSource.h: transcodes BC1-BC5 blocks
// to ETC2/EAC on the GPU, uploads them into real ETC2/EAC textures, lets the
// GPU sample them and compares against the CPU BC decoder.
// Build: clang++ -std=c++20 -fobjc-arc -I src -I src/Cafe platform/ipados/Tests/MetalBCTranscodeTests.mm
//        src/Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.cpp -framework Metal -framework Foundation
#import <Metal/Metal.h>
#include "Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalBCTranscodeShaderSource.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

struct Case { const char* name; uint32_t format; BCFormat bc; MTLPixelFormat etc; uint32_t srcBlock, dstBlock, channels; bool isSigned; double minPSNR; };

// Smooth, texture-like content: endpoints follow gradients, indices vary.
static std::vector<uint8_t> MakeBlocks(uint32_t bw, uint32_t bh, uint32_t blockBytes, uint32_t format, std::mt19937& rng)
{
    std::vector<uint8_t> data(size_t(bw) * bh * blockBytes);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            uint8_t* b = &data[(size_t(by) * bw + bx) * blockBytes];
            for (uint32_t i = 0; i < blockBytes; ++i) b[i] = uint8_t(rng());
            auto channelBlock = [&](uint8_t* c) {
                const int center = int(40 + 170.0 * (bx + by) / double(bw + bh));
                c[0] = uint8_t(std::clamp(center + int(rng() % 24), 0, 255));
                c[1] = uint8_t(std::clamp(center - int(rng() % 24), 0, 255));
            };
            auto colorBlock = [&](uint8_t* c) {
                const uint32_t r = 4 + (bx * 20) / bw, g = 8 + (by * 40) / bh, bl = 6 + ((bx + by) * 16) / (bw + bh);
                const uint16_t c0 = uint16_t((std::min(31u, r + 3) << 11) | (std::min(63u, g + 6) << 5) | std::min(31u, bl + 3));
                const uint16_t c1 = uint16_t((r << 11) | (g << 5) | bl);
                const bool transparentMode = format == 0 && (rng() % 8) == 0; // some BC1 punch-through blocks
                const uint16_t first = transparentMode ? c1 : c0, second = transparentMode ? c0 : c1;
                c[0] = uint8_t(first); c[1] = uint8_t(first >> 8); c[2] = uint8_t(second); c[3] = uint8_t(second >> 8);
            };
            if (format == 0) colorBlock(b);
            else if (format == 1 || format == 2)
            {
                colorBlock(b + 8);
                if (format == 2) channelBlock(b);
                else // alpha-tested content: transparent texels and nearly opaque texels
                    for (uint32_t p = 0; p < 16; ++p)
                    {
                        const uint8_t nibble = (rng() % 3) == 0 ? 0 : uint8_t(15 - rng() % 3);
                        b[p / 2] = uint8_t((b[p / 2] & ~(0xf << (4 * (p & 1)))) | (nibble << (4 * (p & 1))));
                    }
            }
            else { channelBlock(b); if (format >= 5) channelBlock(b + 8); }
        }
    return data;
}

int main()
{
    @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { puts("no Metal device"); return 1; }
    printf("GPU: %s\n", device.name.UTF8String);
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@(bcTranscodeShaderSource) options:nil error:&error];
    if (!library) { printf("FAIL: compile: %s\n", error.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> transcode = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"transcodeBCToETC"] error:&error];
    id<MTLLibrary> readLib = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void readTex(texture2d<float, access::read> t [[texture(0)]], device float4* o [[buffer(0)]], constant uint& w [[buffer(1)]], uint2 id [[thread_position_in_grid]]) {"
        " if (id.x < t.get_width() && id.y < t.get_height()) o[id.y * w + id.x] = t.read(id); }" options:nil error:&error];
    id<MTLComputePipelineState> readback = [device newComputePipelineStateWithFunction:[readLib newFunctionWithName:@"readTex"] error:&error];
    id<MTLCommandQueue> queue = [device newCommandQueue];

    const Case cases[] = {
        {"BC1->EAC_RGBA8", 0, BCFormat::BC1, MTLPixelFormatEAC_RGBA8, 8, 16, 4, false, 32.0},
        {"BC2->EAC_RGBA8", 1, BCFormat::BC2, MTLPixelFormatEAC_RGBA8, 16, 16, 4, false, 32.0},
        {"BC3->EAC_RGBA8", 2, BCFormat::BC3, MTLPixelFormatEAC_RGBA8, 16, 16, 4, false, 32.0},
        {"BC4U->EAC_R11", 3, BCFormat::BC4Unorm, MTLPixelFormatEAC_R11Unorm, 8, 8, 1, false, 38.0},
        {"BC4S->EAC_R11S", 4, BCFormat::BC4Snorm, MTLPixelFormatEAC_R11Snorm, 8, 8, 1, true, 38.0},
        {"BC5U->EAC_RG11", 5, BCFormat::BC5Unorm, MTLPixelFormatEAC_RG11Unorm, 16, 16, 2, false, 38.0},
        {"BC5S->EAC_RG11S", 6, BCFormat::BC5Snorm, MTLPixelFormatEAC_RG11Snorm, 16, 16, 2, true, 38.0},
    };
    std::mt19937 rng(1234);
    int failures = 0;
    for (const Case& c : cases)
    for (uint32_t size : {64u, 6u, 1u}) // full blocks, partial blocks, 1x1 mip tail
    {
        const uint32_t w = size, h = size == 6 ? 5 : size;
        const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        auto blocks = MakeBlocks(bw, bh, c.srcBlock, c.format, rng);
        std::vector<uint8_t> reference(size_t(w) * h * c.channels);
        if (!DecodeBCImage(c.bc, blocks.data(), blocks.size(), w, h, reference.data(), reference.size()))
        { printf("FAIL %s: CPU decode\n", c.name); ++failures; continue; }

        id<MTLBuffer> src = [device newBufferWithBytes:blocks.data() length:blocks.size() options:MTLResourceStorageModeShared];
        id<MTLBuffer> dst = [device newBufferWithLength:size_t(bw) * bh * c.dstBlock options:MTLResourceStorageModeShared];
        id<MTLBuffer> out = [device newBufferWithLength:size_t(w) * h * 16 options:MTLResourceStorageModeShared];
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.etc width:w height:h mipmapped:NO];
        d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [device newTextureWithDescriptor:d];
        if (!tex) { printf("FAIL %s: texture format unsupported\n", c.name); ++failures; continue; }
        uint32_t params[8] = {bw, bh, c.format, w, h, 0, 0, 0};

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:transcode];
        [ce setBuffer:src offset:0 atIndex:0];
        [ce setBuffer:dst offset:0 atIndex:1];
        [ce setBytes:params length:sizeof(params) atIndex:2];
        [ce dispatchThreads:MTLSizeMake(bw, bh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [ce endEncoding];
        // Same call shape as MetalRenderer::texture_loadSlice for compressed destinations.
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromBuffer:dst sourceOffset:0 sourceBytesPerRow:bw * c.dstBlock sourceBytesPerImage:0 sourceSize:MTLSizeMake(w, h, 1)
                 toTexture:tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [be endEncoding];
        id<MTLComputeCommandEncoder> re = [cb computeCommandEncoder];
        [re setComputePipelineState:readback];
        [re setTexture:tex atIndex:0];
        [re setBuffer:out offset:0 atIndex:0];
        [re setBytes:&w length:4 atIndex:1];
        [re dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(std::min(w, 8u), std::min(h, 8u), 1)];
        [re endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { printf("FAIL %s: GPU error %s\n", c.name, cb.error.localizedDescription.UTF8String); ++failures; continue; }

        const float* gpu = static_cast<const float*>(out.contents);
        double sq = 0; double maxErr = 0; size_t n = 0; int alphaFlips = 0;
        for (uint32_t i = 0; i < w * h; ++i)
            for (uint32_t ch = 0; ch < c.channels; ++ch)
            {
                const uint8_t raw = reference[i * c.channels + ch];
                const double expected = c.isSigned ? std::max(-1.0, int8_t(raw) / 127.0) : raw / 255.0;
                const double got = gpu[i * 4 + ch];
                const double err = (got - expected) * (c.isSigned ? 127.5 : 255.0);
                if (c.channels == 4 && ch == 3 && (raw == 0) != (got < 0.02)) ++alphaFlips;
                sq += err * err; maxErr = std::max(maxErr, std::fabs(err)); ++n;
            }
        const double mse = sq / n;
        const double psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99.0;
        const bool ok = psnr >= c.minPSNR && alphaFlips == 0;
        printf("%s %-16s %2ux%-2u PSNR %5.1f dB  max err %5.1f  alpha flips %d\n", ok ? "PASS" : "FAIL", c.name, w, h, psnr, maxErr, alphaFlips);
        failures += !ok;
    }

    // Throughput: 1024x1024 BC3 (65536 blocks).
    {
        const uint32_t bw = 256, bh = 256;
        auto blocks = MakeBlocks(bw, bh, 16, 2, rng);
        id<MTLBuffer> src = [device newBufferWithBytes:blocks.data() length:blocks.size() options:MTLResourceStorageModeShared];
        id<MTLBuffer> dst = [device newBufferWithLength:bw * bh * 16 options:MTLResourceStorageModePrivate];
        uint32_t params[8] = {bw, bh, 2, 1024, 1024, 0, 0, 0};
        double best = 1e9;
        for (int r = 0; r < 5; ++r)
        {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:transcode];
            [ce setBuffer:src offset:0 atIndex:0]; [ce setBuffer:dst offset:0 atIndex:1];
            [ce setBytes:params length:sizeof(params) atIndex:2];
            [ce dispatchThreads:MTLSizeMake(bw, bh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
            best = std::min(best, (cb.GPUEndTime - cb.GPUStartTime) * 1000.0);
        }
        printf("GPU transcode 1024x1024 BC3 -> EAC_RGBA8: %.2f ms\n", best);
    }
    if (failures) { printf("FAIL: %d case(s)\n", failures); return 1; }
    puts("PASS: GPU BC1-BC5 -> ETC2/EAC transcode matches the CPU BC decoder within ETC2/EAC precision");
    return 0;
    }
}
