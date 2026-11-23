#!/usr/bin/env python3
"""Compare production BC expansion through mip/array/cube views on Mac Metal.

The test checks GPU samples, not game image correctness or A14 performance.
"""
from pathlib import Path
import os, subprocess, tempfile

repo = Path(__file__).resolve().parents[3]
fixture = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
struct Format { BCFormat bc; MTLPixelFormat native,expanded; unsigned channels,blockBytes; const char* name; };
int main(){@autoreleasepool{
 id<MTLDevice> device=MTLCreateSystemDefaultDevice();assert(device.supportsBCTextureCompression);
 id<MTLCommandQueue> queue=[device newCommandQueue];unsigned comparisons=0,cases=0;
 Format formats[]={
 {BCFormat::BC1,MTLPixelFormatBC1_RGBA,MTLPixelFormatRGBA8Unorm,4,8,"BC1"},
 {BCFormat::BC1,MTLPixelFormatBC1_RGBA_sRGB,MTLPixelFormatRGBA8Unorm_sRGB,4,8,"BC1 sRGB"},
 {BCFormat::BC2,MTLPixelFormatBC2_RGBA,MTLPixelFormatRGBA8Unorm,4,16,"BC2"},
 {BCFormat::BC3,MTLPixelFormatBC3_RGBA,MTLPixelFormatRGBA8Unorm,4,16,"BC3"},
 {BCFormat::BC3,MTLPixelFormatBC3_RGBA_sRGB,MTLPixelFormatRGBA8Unorm_sRGB,4,16,"BC3 sRGB"},
 {BCFormat::BC4Unorm,MTLPixelFormatBC4_RUnorm,MTLPixelFormatR8Unorm,1,8,"BC4"},
 {BCFormat::BC4Snorm,MTLPixelFormatBC4_RSnorm,MTLPixelFormatR8Snorm,1,8,"BC4 signed"},
 {BCFormat::BC5Unorm,MTLPixelFormatBC5_RGUnorm,MTLPixelFormatRG8Unorm,2,16,"BC5"},
 {BCFormat::BC5Snorm,MTLPixelFormatBC5_RGSnorm,MTLPixelFormatRG8Snorm,2,16,"BC5 signed"}};
 for(bool cube:{false,true}){
  const std::string type=cube?"texturecube_array<float>":"texture2d_array<float>";
  std::string source="#include <metal_stdlib>\nusing namespace metal;\n";
  source+="kernel void compare("+type+" a [[texture(0)]],"+type+" b [[texture(1)]], sampler s [[sampler(0)]],device float4* out [[buffer(0)]],uint i [[thread_position_in_grid]]) {";
  source+="float2 uv=(float2(i%16,(i/16)%16)+0.25)/16.0;uint slice=(i/256)%"+std::string(cube?"6":"2")+";float lod=float(i/(256*"+std::string(cube?"6":"2")+"));";
  if(cube){
   source+="float2 p=2*uv-1;float3 dir;switch(slice){case 0:dir=float3(1,-p.y,-p.x);break;case 1:dir=float3(-1,-p.y,p.x);break;case 2:dir=float3(p.x,1,p.y);break;case 3:dir=float3(p.x,-1,-p.y);break;case 4:dir=float3(p.x,-p.y,1);break;default:dir=float3(-p.x,-p.y,-1);}";
   source+="out[2*i]=a.sample(s,dir,0,level(lod));out[2*i+1]=b.sample(s,dir,0,level(lod));}";
  }else source+="out[2*i]=a.sample(s,uv,slice,level(lod));out[2*i+1]=b.sample(s,uv,slice,level(lod));}";
  NSError* error=nil;id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&error];
  if(!library){NSLog(@"%@",error);return 1;}
  id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"compare"] error:&error];assert(pipeline);
  for(auto f:formats)for(bool formatViewUsage:{true,false}){
   MTLTextureDescriptor* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f.native width:32 height:32 mipmapped:YES];
   desc.textureType=cube?MTLTextureTypeCubeArray:MTLTextureType2DArray;desc.arrayLength=cube?2:4;
   desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
   id<MTLTexture> native=[device newTextureWithDescriptor:desc];assert(native);
   desc.pixelFormat=f.expanded;
   desc.usage=MTLTextureUsageShaderRead|(formatViewUsage?MTLTextureUsagePixelFormatView:0);
   id<MTLTexture> expanded=[device newTextureWithDescriptor:desc];assert(expanded);
   const unsigned slices=cube?12:4;
   for(unsigned slice=0;slice<slices;++slice)for(unsigned mip=0;mip<6;++mip){
    const unsigned width=32>>mip;const unsigned blocks=(width+3)/4;
    std::vector<uint8_t> compressed(blocks*blocks*f.blockBytes),pixels(width*width*f.channels);
    uint32_t random=0x18274635u^(slice*1847u+mip*9391u);
    for(auto& x:compressed){random^=random<<13;random^=random>>17;random^=random<<5;x=uint8_t(random);}
    assert(DecodeBCImage(f.bc,compressed.data(),compressed.size(),width,width,pixels.data(),pixels.size()));
    [native replaceRegion:MTLRegionMake2D(0,0,width,width) mipmapLevel:mip slice:slice withBytes:compressed.data() bytesPerRow:blocks*f.blockBytes bytesPerImage:compressed.size()];
    [expanded replaceRegion:MTLRegionMake2D(0,0,width,width) mipmapLevel:mip slice:slice withBytes:pixels.data() bytesPerRow:width*f.channels bytesPerImage:pixels.size()];
   }
   float maximum=0;
   for(bool srgbView:{false,true})for(unsigned firstMip:{0u,1u})for(bool swizzled:{false,true})for(auto filter:{MTLSamplerMinMagFilterNearest,MTLSamplerMinMagFilterLinear}){
    auto swizzle=swizzled?MTLTextureSwizzleChannelsMake(MTLTextureSwizzleGreen,MTLTextureSwizzleRed,MTLTextureSwizzleOne,MTLTextureSwizzleAlpha):MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleAlpha);
    NSRange levels=NSMakeRange(firstMip,6-firstMip),layers=cube?NSMakeRange(6,6):NSMakeRange(1,2);
    MTLPixelFormat nativeFormat=f.native,expandedFormat=f.expanded;
    if(srgbView){
     switch(nativeFormat){
      case MTLPixelFormatBC1_RGBA:nativeFormat=MTLPixelFormatBC1_RGBA_sRGB;expandedFormat=MTLPixelFormatRGBA8Unorm_sRGB;break;
      case MTLPixelFormatBC2_RGBA:nativeFormat=MTLPixelFormatBC2_RGBA_sRGB;expandedFormat=MTLPixelFormatRGBA8Unorm_sRGB;break;
      case MTLPixelFormatBC3_RGBA:nativeFormat=MTLPixelFormatBC3_RGBA_sRGB;expandedFormat=MTLPixelFormatRGBA8Unorm_sRGB;break;
      case MTLPixelFormatBC1_RGBA_sRGB:nativeFormat=MTLPixelFormatBC1_RGBA;expandedFormat=MTLPixelFormatRGBA8Unorm;break;
      case MTLPixelFormatBC3_RGBA_sRGB:nativeFormat=MTLPixelFormatBC3_RGBA;expandedFormat=MTLPixelFormatRGBA8Unorm;break;
      default:continue;
     }
    }
    id<MTLTexture> a=[native newTextureViewWithPixelFormat:nativeFormat textureType:desc.textureType levels:levels slices:layers swizzle:swizzle];
    id<MTLTexture> b=[expanded newTextureViewWithPixelFormat:expandedFormat textureType:desc.textureType levels:levels slices:layers swizzle:swizzle];assert(a && b);
    assert(a.width==b.width && a.mipmapLevelCount==b.mipmapLevelCount && a.arrayLength==b.arrayLength);
    MTLSamplerDescriptor* sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=filter;sd.sAddressMode=sd.tAddressMode=sd.rAddressMode=MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler=[device newSamplerStateWithDescriptor:sd];assert(sampler);
    const unsigned n=256*(cube?6:2)*(6-firstMip);id<MTLBuffer> output=[device newBufferWithLength:n*8*sizeof(float) options:MTLResourceStorageModeShared];assert(output);
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[cb computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];[encoder setTexture:a atIndex:0];[encoder setTexture:b atIndex:1];[encoder setSamplerState:sampler atIndex:0];[encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(n,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];[encoder endEncoding];[cb commit];[cb waitUntilCompleted];
    if(cb.status!=MTLCommandBufferStatusCompleted){NSLog(@"%@",cb.error);return 2;}
    const bool signedBC=f.bc==BCFormat::BC4Snorm || f.bc==BCFormat::BC5Snorm;const float tolerance=signedBC?0.020f:0.012f;
    const float* values=(const float*)output.contents;
    for(unsigned i=0;i<n;++i)for(unsigned c=0;c<4;++c){float delta=fabsf(values[8*i+c]-values[8*i+4+c]);
     if(!std::isfinite(delta)||delta>tolerance){fprintf(stderr,"FAIL cube=%d %s mip=%u swizzle=%d filter=%lu sample=%u channel=%u native=%g expanded=%g error=%g\n",cube,f.name,firstMip,swizzled,(unsigned long)filter,i,c,values[8*i+c],values[8*i+4+c],delta);return 3;}
     maximum=fmaxf(maximum,delta);++comparisons;
    }++cases;
   }printf("%s %s maxError=%g\n",cube?"cube-array":"2D-array",f.name,maximum);
  }
 }
 printf("PASS: %u mip/array/cube/swizzle/filter cases, %u channel comparisons; quantized fallback, not bit-exact game certification\n",cases,comparisons);
}}
'''
with tempfile.TemporaryDirectory(prefix='77u-bc-views-') as temp:
    p=Path(temp);(p/'test.mm').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-fobjc-arc','-I'+str(repo/'src'),str(p/'test.mm'),
                    str(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.cpp'),
                    '-framework','Metal','-framework','Foundation','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],env=dict(os.environ,MTL_DEBUG_LAYER='1'),check=True)
