#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void require(bool condition, NSString* message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message.UTF8String);
        std::exit(1);
    }
}

// Render through every binding, including 16 and 30. Alternating address modes
// make sure textures do not accidentally share the wrong sampler state.
static void testBindings(id<MTLDevice> device, NSUInteger count, bool indirect)
{
    NSMutableString* source = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\n"];
    [source appendString:@"struct CemuSamplerArguments { array<sampler, 31> values [[id(0)]]; };\n"];
    [source appendString:@"vertex float4 vs(uint i [[vertex_id]]) { return float4(i == 2 ? 3.0 : -1.0, i == 1 ? 3.0 : -1.0, 0, 1); }\n"];
    [source appendString:@"fragment float4 ps("];
    for (NSUInteger i = 0; i < count; ++i) {
        if (i) [source appendString:@", "];
        [source appendFormat:@"texture2d<float> tex%lu [[texture(%lu)]]", i, i];
        if (!indirect) [source appendFormat:@", sampler samplr%lu [[sampler(%lu)]]", i, i];
    }
    if (indirect) [source appendString:@", constant CemuSamplerArguments& cemuSamplers [[buffer(4)]]"];
    [source appendString:@") { float4 result = 0;\n"];
    for (NSUInteger i = 0; i < count; ++i) {
        NSString* sampler = indirect ? [NSString stringWithFormat:@"cemuSamplers.values[%lu]", i]
                                     : [NSString stringWithFormat:@"samplr%lu", i];
        [source appendFormat:@"result += tex%lu.sample(%@, float2(1.25, 0.5));\n", i, sampler];
    }
    [source appendFormat:@"return result / %lu.0; }\n", count];
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    require(library != nil, [NSString stringWithFormat:@"Shader compile: %@", error]);
    MTLRenderPipelineDescriptor* pipelineDescriptor = [MTLRenderPipelineDescriptor new];
    pipelineDescriptor.vertexFunction = [library newFunctionWithName:@"vs"];
    pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"ps"];
    pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
    require(pipeline != nil, [NSString stringWithFormat:@"Pipeline: %@", error]);

    MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.supportArgumentBuffers = YES;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> clamp = [device newSamplerStateWithDescriptor:samplerDescriptor];
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    id<MTLSamplerState> repeat = [device newSamplerStateWithDescriptor:samplerDescriptor];
    require(clamp && repeat, @"Sampler allocation failed");
    MTLArgumentDescriptor* argument = [MTLArgumentDescriptor argumentDescriptor];
    argument.dataType = MTLDataTypeSampler;
    argument.index = 0;
    argument.arrayLength = 31;
    id<MTLArgumentEncoder> argumentEncoder = [device newArgumentEncoderWithArguments:@[argument]];
    require(argumentEncoder != nil, @"Argument encoder allocation failed");
    // Exercise a nonzero, aligned ring-allocation offset, as in the renderer.
    NSUInteger offset = MAX((NSUInteger)256, argumentEncoder.alignment);
    id<MTLBuffer> arguments = [device newBufferWithLength:offset + argumentEncoder.encodedLength options:MTLResourceStorageModeShared];
    [argumentEncoder setArgumentBuffer:arguments offset:offset];
    for (NSUInteger i = 0; i < 31; ++i) [argumentEncoder setSamplerState:repeat atIndex:i];

    MTLTextureDescriptor* targetDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    targetDescriptor.storageMode = MTLStorageModeShared;
    targetDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
    [render setRenderPipelineState:pipeline];
    float expected = 0;
    for (NSUInteger i = 0; i < count; ++i) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:2 height:1 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
        float value = float(i + 1) / 31.0f;
        float pixels[8] = {0, 0, 0, 1, value, 0, 0, 1};
        [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:pixels bytesPerRow:sizeof(pixels)];
        [render setFragmentTexture:texture atIndex:i];
        id<MTLSamplerState> sampler = (i % 2) ? repeat : clamp;
        if (indirect) [argumentEncoder setSamplerState:sampler atIndex:i];
        else [render setFragmentSamplerState:sampler atIndex:i];
        if (i % 2 == 0) expected += value / float(count);
    }
    if (indirect) [render setFragmentBuffer:arguments offset:offset atIndex:4];
    [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [render endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, [NSString stringWithFormat:@"GPU: %@", command.error]);
    float result[4] = {};
    [target getBytes:result bytesPerRow:sizeof(result) fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
    require(std::abs(result[0] - expected) < 0.0001f && std::abs(result[3] - 1) < 0.0001f,
            [NSString stringWithFormat:@"Incorrect pixels at %lu bindings: %.6f != %.6f", count, result[0], expected]);
    printf("PASS: %lu %s samplers, correct GPU pixels and independent address modes\n", count, indirect ? "indirect" : "direct");
}

int main(int argc, const char* argv[])
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, @"No Metal GPU available");
        printf("Metal device: %s\n", device.name.UTF8String);
        testBindings(device, 16, false);
        testBindings(device, 17, true);
        testBindings(device, 31, true);
        // Optional captured regression shaders must also compile on real Metal.
        for (int i = 1; i < argc; ++i) {
            NSError* error = nil;
            NSString* source = [NSString stringWithContentsOfFile:@(argv[i]) encoding:NSUTF8StringEncoding error:&error];
            require(source != nil, error.description);
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
            require(library != nil, [NSString stringWithFormat:@"%s: %@", argv[i], error]);
            printf("PASS: compiled %s\n", argv[i]);
        }
    }
}
