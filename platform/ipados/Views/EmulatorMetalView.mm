#import "EmulatorMetalView.h"
#import "CemuBridge.h"
#import <os/log.h>

extern "C" void CemuMetal_FrameGenerationDisplayTick(void* layer, double targetSeconds, double displayPeriod);

static void CemuNotifyLayerAttachment(void (^handler)(BOOL), BOOL attached)
{
    if (!handler)
        return;

    // didMoveToWindow can run while SwiftUI is updating UIViewRepresentable.
    // Publish the state on the next main-loop turn to avoid mutating observed
    // SwiftUI state from inside the current view update.
    dispatch_async(dispatch_get_main_queue(), ^{
        handler(attached);
    });
}

static CGSize CemuDisplayDrawableSize(CGSize points, CGFloat scale, BOOL nativeResolution)
{
    CGFloat width = MAX(1.0, points.width * scale);
    CGFloat height = MAX(1.0, points.height * scale);
    // The game is rendered at its own resolution before this presentation copy.
    // A 1920-point cap needlessly shades over 2.5 million pixels on iPad and on
    // external 4K displays. Keep the opt-in native mode, but use a Wii U sized
    // presentation target by default to reduce heat and frame pacing pressure.
    CGFloat factor = nativeResolution ? 1.0 : MIN(1.0, 1280.0 / MAX(width, height));
    return CGSizeMake(round(width * factor), round(height * factor));
}

@interface EmulatorDisplayLinkProxy : NSObject
@property (nonatomic, weak) id target;
@property (nonatomic, assign) SEL action;
- (instancetype)initWithTarget:(id)target action:(SEL)action;
- (void)handleDisplayLink:(CADisplayLink *)link;
@end

@implementation EmulatorDisplayLinkProxy
- (instancetype)initWithTarget:(id)target action:(SEL)action
{
    self = [super init];
    if (self) {
        _target = target;
        _action = action;
    }
    return self;
}
- (void)handleDisplayLink:(CADisplayLink *)link
{
    id strongTarget = _target;
    if (strongTarget && _action) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        [strongTarget performSelector:_action withObject:link];
#pragma clang diagnostic pop
    }
}
@end

@interface EmulatorMetalView ()
@property (nonatomic, strong) CADisplayLink *displayLink;
- (void)startDisplayLink;
- (void)stopDisplayLink;
- (void)updateDisplayLinkFrameRateRange;
- (void)displayLinkDidFire:(CADisplayLink *)link;
@end

@implementation EmulatorMetalView

+ (Class)layerClass
{
    return [CAMetalLayer class];
}

- (CAMetalLayer *)metalLayer
{
    return (CAMetalLayer *)self.layer;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        CAMetalLayer *layer = self.metalLayer;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.maximumDrawableCount = 3;

        // Opaque - no compositing needed for the rendering surface
        self.opaque = YES;
        self.backgroundColor = [UIColor blackColor];
    }
    return self;
}

- (void)didMoveToWindow
{
    [super didMoveToWindow];

    if (self.window) {
        CGFloat scale = self.window.screen.scale;
        self.metalLayer.contentsScale = scale;
        self.contentScaleFactor = scale;

        [self updateDrawableSize];

        if (CemuBridge_IsDiagnosticLoggingEnabled()) {
            os_log(OS_LOG_DEFAULT,
                   "[EmulatorMetalView] %{public}s attached to window=%p (scale=%.1f, bounds=%.0fx%.0f, drawable=%.0fx%.0f)",
                   self.rendersDRC ? "DRC" : "TV",
                   (__bridge void*)self.window,
                   scale,
                   self.bounds.size.width,
                   self.bounds.size.height,
                   self.metalLayer.drawableSize.width,
                   self.metalLayer.drawableSize.height);
        }

        if (self.rendersDRC) {
            // The renderer-owned DRC snapshot is copied from this drawable in
            // the same command buffer. TV remains framebuffer-only.
            self.metalLayer.framebufferOnly = NO;
            CemuBridge_SetDRCMetalLayer(self.metalLayer);
        } else {
            CemuBridge_SetMetalLayer(self.metalLayer);
            [self startDisplayLink];
        }
        [self updateDisplayLinkFrameRateRange];

        CemuNotifyLayerAttachment(self.onLayerAttachedChange, YES);
    } else {
        [self stopDisplayLink];
        os_log(OS_LOG_DEFAULT, "[EmulatorMetalView] %{public}s detached from window",
               self.rendersDRC ? "DRC" : "TV");

        if (self.transferringBetweenWindows) {
            // Reparenting this same CAMetalLayer between the iPad and TV.
        } else if (self.rendersDRC) {
            CemuBridge_ClearDRCMetalLayer();
        } else {
            CemuBridge_ClearMetalLayer();
        }

        CemuNotifyLayerAttachment(self.onLayerAttachedChange, NO);
    }
}

- (void)dealloc
{
    [self stopDisplayLink];
    os_log(OS_LOG_DEFAULT, "[EmulatorMetalView] %{public}s dealloc",
           self.rendersDRC ? "DRC" : "TV");
    if (self.transferringBetweenWindows) {
        return;
    } else if (self.rendersDRC) {
        CemuBridge_ClearDRCMetalLayer();
    } else {
        CemuBridge_ClearMetalLayer();
    }
    CemuNotifyLayerAttachment(self.onLayerAttachedChange, NO);
}

- (void)startDisplayLink
{
    if (self.displayLink) return;
    EmulatorDisplayLinkProxy *proxy = [[EmulatorDisplayLinkProxy alloc] initWithTarget:self action:@selector(displayLinkDidFire:)];
    self.displayLink = [CADisplayLink displayLinkWithTarget:proxy selector:@selector(handleDisplayLink:)];
    [self updateDisplayLinkFrameRateRange];
    [self.displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)updateDisplayLinkFrameRateRange
{
    if (@available(iOS 15.0, *)) {
        float targetFps = CemuBridge_GetTargetFrameRate();
        // Request the configured console rate. iOS can still select a different
        // cadence for power, accessibility or thermal reasons. Record the period
        // reported by the callback instead of assuming this hint is enforced.
        CAFrameRateRange range;
        if (targetFps > 60.0f) {
            range = CAFrameRateRangeMake(60.0, targetFps, targetFps);
        } else {
            range = CAFrameRateRangeMake(60.0, 60.0, 60.0);
        }
        if (self.displayLink) {
            const CAFrameRateRange current = self.displayLink.preferredFrameRateRange;
            if (current.minimum != range.minimum || current.maximum != range.maximum || current.preferred != range.preferred)
                self.displayLink.preferredFrameRateRange = range;
        }
    }
}

- (void)stopDisplayLink
{
    if (!self.displayLink) return;
    [self.displayLink invalidate];
    self.displayLink = nil;
}

- (void)displayLinkDidFire:(CADisplayLink *)link
{
    // Presentation-only notification. The normal renderer and guest VSync are
    // unchanged; the FG controller returns immediately while disabled.
    CemuMetal_FrameGenerationDisplayTick((__bridge void*)self.metalLayer, link.targetTimestamp, link.targetTimestamp - link.timestamp);
    if (CemuBridge_IsDiagnosticLoggingEnabled())
        CemuBridge_RecordDisplayLinkPeriod(link.targetTimestamp - link.timestamp);
    static uint32_t s_displayLinkTickCounter = 0;
    if ((++s_displayLinkTickCounter % 60) == 0) {
        [self updateDisplayLinkFrameRateRange];
    }
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    [self updateDrawableSize];
}

- (void)setNativeResolution:(BOOL)nativeResolution
{
    if (_nativeResolution == nativeResolution) return;
    _nativeResolution = nativeResolution;
    [self updateDrawableSize];
}

- (void)updateDrawableSize
{
    CGFloat scale = self.metalLayer.contentsScale;
    CGSize drawableSize = self.rendersDRC
        ? CGSizeMake(854.0, 480.0)
        : CemuDisplayDrawableSize(self.bounds.size, scale, self.nativeResolution);

    // Only update if size actually changed (avoid unnecessary
    // drawable invalidation on redundant layout passes)
    CGSize current = self.metalLayer.drawableSize;
    if (drawableSize.width != current.width || drawableSize.height != current.height) {
        self.metalLayer.drawableSize = drawableSize;
        if (self.rendersDRC) {
            CemuBridge_UpdateDRCDrawableSize((float)drawableSize.width, (float)drawableSize.height);
        } else {
            CemuBridge_UpdateDrawableSize((float)drawableSize.width, (float)drawableSize.height);
        }
    }
}

@end


@interface DRCPreviewMetalView ()

@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property (nonatomic, strong) id<MTLSamplerState> samplerState;
@property (nonatomic) MTLPixelFormat pipelinePixelFormat;
@property (nonatomic, strong) id<MTLCommandBuffer> inFlightCommandBuffer0;
@property (nonatomic, strong) id<MTLCommandBuffer> inFlightCommandBuffer1;
@property (nonatomic, strong) id<MTLCommandBuffer> inFlightCommandBuffer2;
@property (nonatomic) NSUInteger nextInFlightSlot;
@property (nonatomic) uint64_t lastDisplayedGeneration;
@property (nonatomic) CGSize lastDisplayedSize;
@property (nonatomic, strong, nullable) UITouch *activeDRCTouch;

- (void)displayLatestTexture;

@end


static NSHashTable<DRCPreviewMetalView *> *CemuDRCPreviewViews(void)
{
    static NSHashTable<DRCPreviewMetalView *> *views;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{ views = [NSHashTable weakObjectsHashTable]; });
    return views;
}

static void CemuUpdateDRCPreviewPublicationHandler(void)
{
    BOOL needsPreviewFrames = NO;
    for (DRCPreviewMetalView *view in CemuDRCPreviewViews().allObjects) {
        if (view.window && view.previewActive) {
            needsPreviewFrames = YES;
            break;
        }
    }

    if (!needsPreviewFrames) {
        CemuBridge_SetDRCPreviewPublicationHandler(nil);
        return;
    }

    CemuBridge_SetDRCPreviewPublicationHandler(^{
        for (DRCPreviewMetalView *view in CemuDRCPreviewViews().allObjects) {
            if (view.window && view.previewActive) [view displayLatestTexture];
        }
    });
}

@implementation DRCPreviewMetalView

+ (Class)layerClass
{
    return [CAMetalLayer class];
}

- (CAMetalLayer *)metalLayer
{
    return (CAMetalLayer *)self.layer;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        self.opaque = YES;
        self.backgroundColor = UIColor.blackColor;
        self.multipleTouchEnabled = NO;
        self.previewActive = YES;
        self.metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        self.metalLayer.framebufferOnly = YES;
    }
    return self;
}

- (void)didMoveToWindow
{
    [super didMoveToWindow];

    if (self.window) {
        self.metalLayer.contentsScale = self.window.screen.scale;
        [self updateDrawableSize];
        [CemuDRCPreviewViews() addObject:self];
        CemuUpdateDRCPreviewPublicationHandler();
        [self displayLatestTexture];
    } else {
        [CemuDRCPreviewViews() removeObject:self];
        CemuUpdateDRCPreviewPublicationHandler();
        [self clearDRCTouch];
    }
}

- (void)dealloc
{
    [CemuDRCPreviewViews() removeObject:self];
    CemuUpdateDRCPreviewPublicationHandler();
    CemuBridge_UpdateDRCTouch(false, 0.0f, 0.0f);
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    [self updateDrawableSize];
}

- (void)setPreviewActive:(BOOL)previewActive
{
    // SwiftUI may keep this view alive while the renderer is recreated for a
    // title launch. Re-register even when the value did not change so the new
    // renderer cannot inherit a missing publication callback.
    if (_previewActive == previewActive) {
        CemuUpdateDRCPreviewPublicationHandler();
        if (previewActive) [self displayLatestTexture];
        return;
    }
    _previewActive = previewActive;
    CemuUpdateDRCPreviewPublicationHandler();
    if (!previewActive) [self clearDRCTouch];
    else [self displayLatestTexture];
}

- (void)setNativeResolution:(BOOL)nativeResolution
{
    if (_nativeResolution == nativeResolution) return;
    _nativeResolution = nativeResolution;
    [self updateDrawableSize];
}

- (void)updateDrawableSize
{
    CGSize size = CemuDisplayDrawableSize(self.bounds.size, self.metalLayer.contentsScale, self.nativeResolution);
    if (!CGSizeEqualToSize(size, self.metalLayer.drawableSize)) {
        self.metalLayer.drawableSize = size;
        [self displayLatestTexture];
    }
}

- (void)updateDRCTouch:(UITouch *)touch active:(BOOL)active
{
    const CGSize size = self.bounds.size;
    if (size.width <= 0.0 || size.height <= 0.0) {
        CemuBridge_UpdateDRCTouch(false, 0.0f, 0.0f);
        return;
    }

    const CGPoint point = [touch locationInView:self];
    const float normalizedX = (float)(point.x / size.width);
    const float normalizedY = (float)(point.y / size.height);
    CemuBridge_UpdateDRCTouch(active, normalizedX, normalizedY);
}

- (void)clearDRCTouch
{
    CemuBridge_UpdateDRCTouch(false, 0.0f, 0.0f);
    self.activeDRCTouch = nil;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [super touchesBegan:touches withEvent:event];
    if (!self.activeDRCTouch) {
        self.activeDRCTouch = touches.anyObject;
        if (self.activeDRCTouch)
            [self updateDRCTouch:self.activeDRCTouch active:YES];
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [super touchesMoved:touches withEvent:event];
    if (self.activeDRCTouch && [touches containsObject:self.activeDRCTouch])
        [self updateDRCTouch:self.activeDRCTouch active:YES];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [super touchesEnded:touches withEvent:event];
    if (self.activeDRCTouch && [touches containsObject:self.activeDRCTouch]) {
        [self updateDRCTouch:self.activeDRCTouch active:NO];
        self.activeDRCTouch = nil;
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [super touchesCancelled:touches withEvent:event];
    if (self.activeDRCTouch && [touches containsObject:self.activeDRCTouch])
        [self clearDRCTouch];
}

- (id<MTLCommandBuffer>)commandBufferForSlot:(NSUInteger)slot
{
    switch (slot) {
        case 0: return self.inFlightCommandBuffer0;
        case 1: return self.inFlightCommandBuffer1;
        default: return self.inFlightCommandBuffer2;
    }
}

- (void)setCommandBuffer:(id<MTLCommandBuffer>)commandBuffer forSlot:(NSUInteger)slot
{
    switch (slot) {
        case 0: self.inFlightCommandBuffer0 = commandBuffer; break;
        case 1: self.inFlightCommandBuffer1 = commandBuffer; break;
        default: self.inFlightCommandBuffer2 = commandBuffer; break;
    }
}

- (BOOL)prepareMetalForTexture:(id<MTLTexture>)sourceTexture
{
    id<MTLDevice> device = sourceTexture.device;
    id<MTLCommandQueue> rendererCommandQueue = CemuBridge_GetDRCPreviewCommandQueue();
    if (!device || !rendererCommandQueue || rendererCommandQueue.device != device) {
        return NO;
    }
    MTLPixelFormat outputFormat = sourceTexture.pixelFormat;
    if (outputFormat != MTLPixelFormatBGRA8Unorm && outputFormat != MTLPixelFormatBGRA8Unorm_sRGB)
        outputFormat = MTLPixelFormatBGRA8Unorm;
    if (self.pipelineState && self.metalLayer.device == device &&
        self.commandQueue == rendererCommandQueue && self.pipelinePixelFormat == outputFormat) {
        return YES;
    }

    self.metalLayer.device = device;
    self.metalLayer.pixelFormat = outputFormat;
    // Use the renderer queue. Command-buffer commit order then provides the
    // resource dependency without a CPU wait or a second independent GPU queue.
    self.commandQueue = rendererCommandQueue;

    static NSString *shaderSource =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct PreviewOut { float4 position [[position]]; float2 uv; };\n"
         "vertex PreviewOut previewVertex(uint id [[vertex_id]]) {\n"
         "  const float2 positions[3] = { float2(-1.0, -3.0), float2(-1.0, 1.0), float2(3.0, 1.0) };\n"
         "  PreviewOut out; out.position = float4(positions[id], 0.0, 1.0);\n"
         "  out.uv = positions[id] * 0.5 + 0.5; out.uv.y = 1.0 - out.uv.y; return out;\n"
         "}\n"
         "fragment float4 previewFragment(PreviewOut in [[stage_in]], texture2d<float> source [[texture(0)]], sampler textureSampler [[sampler(0)]]) {\n"
         "  return source.sample(textureSampler, in.uv);\n"
         "}\n";

    NSError *libraryError = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:shaderSource options:nil error:&libraryError];
    if (!library) {
        os_log_error(OS_LOG_DEFAULT, "[DRC Preview] shader compilation failed: %{public}@", libraryError);
        return NO;
    }

    MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.vertexFunction = [library newFunctionWithName:@"previewVertex"];
    pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"previewFragment"];
    pipelineDescriptor.colorAttachments[0].pixelFormat = outputFormat;

    NSError *pipelineError = nil;
    self.pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&pipelineError];
    if (!self.pipelineState) {
        os_log_error(OS_LOG_DEFAULT, "[DRC Preview] pipeline creation failed: %{public}@", pipelineError);
        return NO;
    }
    self.pipelinePixelFormat = outputFormat;

    MTLSamplerDescriptor *samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    self.samplerState = [device newSamplerStateWithDescriptor:samplerDescriptor];
    return self.commandQueue != nil && self.samplerState != nil;
}

- (void)displayLatestTexture
{
    if (!self.window || !self.previewActive || UIApplication.sharedApplication.applicationState != UIApplicationStateActive) {
        return;
    }

    const NSUInteger slot = self.nextInFlightSlot;
    id<MTLCommandBuffer> previousCommandBuffer = [self commandBufferForSlot:slot];
    if (previousCommandBuffer) {
        MTLCommandBufferStatus status = previousCommandBuffer.status;
        if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError) {
            return;
        }
        [self setCommandBuffer:nil forSlot:slot];
    }

    id<MTLTexture> sourceTexture = CemuBridge_GetDRCPreviewTexture();
    if (!sourceTexture || sourceTexture.device == nil ||
        sourceTexture.width == 0 || sourceTexture.height == 0 ||
        ![self prepareMetalForTexture:sourceTexture]) {
        return;
    }

    uint64_t generation = CemuBridge_GetDRCPreviewGeneration();
    CGSize displaySize = self.metalLayer.drawableSize;
    if (generation == self.lastDisplayedGeneration && CGSizeEqualToSize(displaySize, self.lastDisplayedSize)) return;

    id<CAMetalDrawable> drawable = [self.metalLayer nextDrawable];
    id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
    if (!drawable || !commandBuffer) {
        return;
    }

    MTLRenderPassDescriptor *renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
    renderPass.colorAttachments[0].texture = drawable.texture;
    renderPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    renderPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    renderPass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
    [encoder setRenderPipelineState:self.pipelineState];
    [encoder setFragmentTexture:sourceTexture atIndex:0];
    [encoder setFragmentSamplerState:self.samplerState atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];

    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
    [self setCommandBuffer:commandBuffer forSlot:slot];
    self.nextInFlightSlot = (slot + 1) % 3;
    self.lastDisplayedGeneration = generation;
    self.lastDisplayedSize = displaySize;
}

@end
