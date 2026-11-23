#!/usr/bin/env python3
"""Compare CPU-expanded BC samples to native BC on the Mac Metal GPU.

This isolates decode/format/filter behavior; it does not certify game rendering
or any actual Apple7 device. Byte quantization of BC4/5 interpolation permits a
small numerical difference, which must not be mistaken for a material change.
"""
from pathlib import Path
import subprocess, tempfile, os
repo = Path(__file__).resolve().parents[3]
fixture = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <cstdint>
struct Format { BCFormat bc; MTLPixelFormat native,expanded; unsigned channels,blockBytes; const char* name; };
int main(){@autoreleasepool{
 id<MTLDevice> d=MTLCreateSystemDefaultDevice();assert(d && d.supportsBCTextureCompression);
 id<MTLCommandQueue> q=[d newCommandQueue];NSError* e=nil;
 NSString* src=@"#include <metal_stdlib>\nusing namespace metal;\n"
 "kernel void compare(texture2d<float> a [[texture(0)]],texture2d<float> b [[texture(1)]],"
 "sampler s [[sampler(0)]],device float4* o [[buffer(0)]],uint i [[thread_position_in_grid]]) {"
 "float2 uv=(float2(i%64,i/64)+0.5)/64.0;o[2*i]=a.sample(s,uv,level(0));o[2*i+1]=b.sample(s,uv,level(0));}";
 id<MTLLibrary> lib=[d newLibraryWithSource:src options:nil error:&e];if(!lib){NSLog(@"%@",e);return 1;}
 id<MTLComputePipelineState> pipeline=[d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"compare"] error:&e];assert(pipeline);
 Format formats[]={
 {BCFormat::BC1,MTLPixelFormatBC1_RGBA,MTLPixelFormatRGBA8Unorm,4,8,"BC1"},
 {BCFormat::BC1,MTLPixelFormatBC1_RGBA_sRGB,MTLPixelFormatRGBA8Unorm_sRGB,4,8,"BC1 sRGB"},
 {BCFormat::BC2,MTLPixelFormatBC2_RGBA,MTLPixelFormatRGBA8Unorm,4,16,"BC2"},
 {BCFormat::BC2,MTLPixelFormatBC2_RGBA_sRGB,MTLPixelFormatRGBA8Unorm_sRGB,4,16,"BC2 sRGB"},
 {BCFormat::BC3,MTLPixelFormatBC3_RGBA,MTLPixelFormatRGBA8Unorm,4,16,"BC3"},
 {BCFormat::BC3,MTLPixelFormatBC3_RGBA_sRGB,MTLPixelFormatRGBA8Unorm_sRGB,4,16,"BC3 sRGB"},
 {BCFormat::BC4Unorm,MTLPixelFormatBC4_RUnorm,MTLPixelFormatR8Unorm,1,8,"BC4 UNORM"},
 {BCFormat::BC4Snorm,MTLPixelFormatBC4_RSnorm,MTLPixelFormatR8Snorm,1,8,"BC4 SNORM"},
 {BCFormat::BC5Unorm,MTLPixelFormatBC5_RGUnorm,MTLPixelFormatRG8Unorm,2,16,"BC5 UNORM"},
 {BCFormat::BC5Snorm,MTLPixelFormatBC5_RGSnorm,MTLPixelFormatRG8Snorm,2,16,"BC5 SNORM"}};
 unsigned comparisons=0;float maximum=0;
 for(auto f:formats){
  // Every block includes arbitrary endpoints and all 2/3/4-bit indices.
  std::vector<uint8_t> blocks(8*8*f.blockBytes),pixels(32*32*f.channels);
  uint32_t random=0x12345678;
  for(auto& x:blocks){random^=random<<13;random^=random>>17;random^=random<<5;x=uint8_t(random);}
  assert(DecodeBCImage(f.bc,blocks.data(),blocks.size(),32,32,pixels.data(),pixels.size()));
  MTLTextureDescriptor* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f.native width:32 height:32 mipmapped:NO];
  desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
  id<MTLTexture> native=[d newTextureWithDescriptor:desc];assert(native);
  [native replaceRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0 withBytes:blocks.data() bytesPerRow:8*f.blockBytes];
  desc.pixelFormat=f.expanded;id<MTLTexture> expanded=[d newTextureWithDescriptor:desc];assert(expanded);
  [expanded replaceRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:32*f.channels];
  float formatError=0;
  for(auto filter:{MTLSamplerMinMagFilterNearest,MTLSamplerMinMagFilterLinear}){
   MTLSamplerDescriptor* sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=filter;sd.sAddressMode=sd.tAddressMode=MTLSamplerAddressModeRepeat;
   id<MTLSamplerState> sampler=[d newSamplerStateWithDescriptor:sd];assert(sampler);
   id<MTLBuffer> output=[d newBufferWithLength:64*64*8*sizeof(float) options:MTLResourceStorageModeShared];assert(output);
   id<MTLCommandBuffer> cb=[q commandBuffer];id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
   [enc setComputePipelineState:pipeline];[enc setTexture:native atIndex:0];[enc setTexture:expanded atIndex:1];
   [enc setSamplerState:sampler atIndex:0];[enc setBuffer:output offset:0 atIndex:0];
   [enc dispatchThreads:MTLSizeMake(64*64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];[enc endEncoding];[cb commit];[cb waitUntilCompleted];
   if(cb.status!=MTLCommandBufferStatusCompleted){NSLog(@"%@",cb.error);return 2;}
   const float* values=(const float*)output.contents;
   for(unsigned i=0;i<64*64;++i)for(unsigned c=0;c<4;++c){
    float delta=fabsf(values[8*i+c]-values[8*i+4+c]);
    const bool signedBC=f.bc==BCFormat::BC4Snorm || f.bc==BCFormat::BC5Snorm;
    // Native BC interpolation precision is implementation-dependent; decoded
    // SNORM additionally quantizes each tap to 1/127 before filtering.
    const float tolerance=signedBC?0.020f:0.012f;
    if(!std::isfinite(delta)||delta>tolerance){fprintf(stderr,"FAIL %s filter=%lu sample=%u channel=%u native=%g expanded=%g error=%g\n",f.name,(unsigned long)filter,i,c,values[8*i+c],values[8*i+4+c],delta);return 3;}
    formatError=fmaxf(formatError,delta);++comparisons;
   }
  }
  maximum=fmaxf(maximum,formatError);printf("%s max sample error=%g\n",f.name,formatError);
 }
 printf("PASS: %u native/CPU-expanded BC channel comparisons across 10 formats and nearest/linear sampling; maximum=%g\n",comparisons,maximum);
}}
'''
with tempfile.TemporaryDirectory(prefix='77u-bc-sampling-') as temp:
    p=Path(temp);(p/'test.mm').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-fobjc-arc','-I'+str(repo/'src'),
                    str(p/'test.mm'),str(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.cpp'),
                    '-framework','Metal','-framework','Foundation','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],env=dict(os.environ,MTL_DEBUG_LAYER='1'),check=True)
