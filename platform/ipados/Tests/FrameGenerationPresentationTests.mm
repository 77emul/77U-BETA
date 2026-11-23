// From the repository root on a Mac with a window server:
// xcrun clang++ -std=c++20 -fblocks -O1 -Isrc -Idependencies/metal-cpp -DNS_PRIVATE_IMPLEMENTATION -DMTL_PRIVATE_IMPLEMENTATION -DCA_PRIVATE_IMPLEMENTATION -c src/Cafe/HW/Latte/Renderer/Metal/MetalFrameGeneration.cpp -o /tmp/77u-fg.o
// xcrun clang++ -std=c++20 -fobjc-arc -fblocks -O1 -Isrc -Idependencies/metal-cpp platform/ipados/Tests/FrameGenerationPresentationTests.mm /tmp/77u-fg.o -framework AppKit -framework Metal -framework QuartzCore -o /tmp/77u-fg-test
// MTL_DEBUG_LAYER=1 /tmp/77u-fg-test
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include "Cafe/HW/Latte/Renderer/Metal/MetalFrameGeneration.h"
#include <atomic>
#include <cassert>

std::atomic<uint64_t> g_metalPresentCount{0};

int main() { @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    assert(device);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(10, 45, 200, 135)
        styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    CAMetalLayer *layer = [CAMetalLayer layer];
    window.contentView.wantsLayer = YES;
    window.contentView.layer = layer;
    layer.device = device;
    layer.drawableSize = CGSizeMake(1280, 889);
    window.level = NSFloatingWindowLevel;
    [window orderFrontRegardless];
    [CATransaction flush];

    MetalFrameGeneration output((__bridge MTL::Device*)device);
    auto *surface = (__bridge CA::MetalLayer *)layer;
    auto produce = [&](double seconds, bool ticks, double deadlineOffset) {
        const double until = CACurrentMediaTime() + seconds;
        do { @autoreleasepool {
            auto *target = output.Begin(surface, 1280, 889, MTL::PixelFormatBGRA8Unorm);
            assert(target);
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(.1, .3, .4, 1);
            [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
            [cb commit];
            output.Submit((__bridge MTL::CommandBuffer*)cb, 120);
            if (ticks) CemuMetal_FrameGenerationDisplayTick(surface, CACurrentMediaTime() + deadlineOffset, 1.0 / 60);
            [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0 / 60]];
        }} while (CACurrentMediaTime() < until && !output.Failed());
    };

    assert(output.Begin(surface, 1280, 889, MTL::PixelFormatBGRA8Unorm));
    produce(1.2, true, 1.0 / 60);
    auto healthy = output.GetStatistics();
    assert(healthy.original > 0 && !output.Failed());
    produce(.8, true, -.010);
    auto late = output.GetStatistics();
    fprintf(stderr, "120 FPS requested, 60 Hz display: on-time originals %llu, late originals %llu\n",
        (unsigned long long)healthy.original, (unsigned long long)(late.original - healthy.original));
    assert(late.original > healthy.original + 2 && !output.Failed());
    CemuMetal_SetFrameGenerationSuspended(true);
    produce(2.3, false, 0);
    assert(!output.Failed());
    CemuMetal_SetFrameGenerationSuspended(false);
    produce(.6, true, 1.0 / 60);
    assert(!output.Failed() && output.GetStatistics().original > late.original);
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2.2]];
    produce(.4, false, 0);
    assert(!output.Failed()); // A paused source gets a fresh grace period.
    produce(3, false, 0);
    assert(output.Failed());
    output.Reset();
    auto nativePresented = std::make_shared<std::atomic<bool>>(false);
    const double timeout = CACurrentMediaTime() + 2;
    while (!nativePresented->load() && CACurrentMediaTime() < timeout) { @autoreleasepool {
        id<CAMetalDrawable> native = [layer nextDrawable];
        assert(native);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = native.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
        [native addPresentedHandler:^(id<MTLDrawable> shown) {
            if (shown.presentedTime > 0) nativePresented->store(true);
        }];
        [cb presentDrawable:native];
        [cb commit];
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.02]];
    }}
    assert(nativePresented->load());
    [window orderOut:nil];
    puts("PASS: late display ticks still show originals and missing output triggers native fallback");
}}
