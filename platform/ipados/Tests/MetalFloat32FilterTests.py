#!/usr/bin/env python3
"""Compare the production float32 gather fallback against native Metal filtering.
Runs on the Mac GPU; this does not certify A14 hardware or game rendering.
"""
from pathlib import Path
import subprocess,tempfile,os
r=Path(__file__).resolve().parents[3]
fixture=r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Cafe/HW/Latte/Renderer/Metal/MetalFloat32Filter.h"
struct V {float x,y,z,w;};
int main(){@autoreleasepool{
 id<MTLDevice> d=MTLCreateSystemDefaultDevice();assert(d && d.supports32BitFloatFiltering);
 id<MTLCommandQueue> q=[d newCommandQueue];unsigned cases=0,comparisons=0;float maxError=0;
 for(bool array:{false,true})for(bool depth:{false,true}){
 std::string type=depth?"depth2d":"texture2d";if(array)type+="_array";type+="<float>";
 std::string args="uv";if(array)args+=", uint(i%2)";if(depth)args+=", 0.46875";
 std::string source="#include <metal_stdlib>\nusing namespace metal;\n";
 source+=MetalFloat32FilterSource;
 source+="kernel void compare("+type+" tex [[texture(0)]], sampler s [[sampler(0)]], device float4* out [[buffer(0)]], uint i [[thread_position_in_grid]]) {";
 source+="float2 uv=float2(float(int(i%31)-8)/16.0, float(int(i/31)-4)/16.0);";
 source+="out[2*i]="+std::string(depth?"float4(":"")+"tex."+(depth?"sample_compare":"sample")+"(s,"+args+",level(0))"+(depth?")":"")+";";
 source+="out[2*i+1]=cemuBilinear32(tex,s,"+args+");}";
 NSError* e=nil;MTLCompileOptions* opt=[MTLCompileOptions new];opt.languageVersion=MTLLanguageVersion3_0;opt.mathMode=MTLMathModeSafe;
 id<MTLLibrary> l=[d newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:opt error:&e];if(!l){NSLog(@"%@",e);return 1;}
 id<MTLComputePipelineState> ps=[d newComputePipelineStateWithFunction:[l newFunctionWithName:@"compare"] error:&e];assert(ps);
 for(MTLPixelFormat format:{MTLPixelFormatR32Float,MTLPixelFormatRG32Float,MTLPixelFormatRGBA32Float}){
  if(depth && format!=MTLPixelFormatR32Float)continue;
  MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth?MTLPixelFormatDepth32Float:format width:4 height:4 mipmapped:NO];
  td.textureType=array?MTLTextureType2DArray:MTLTextureType2D;td.arrayLength=array?2:1;td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
  id<MTLTexture> tex=[d newTextureWithDescriptor:td];assert(tex);
  unsigned components=depth||format==MTLPixelFormatR32Float?1:format==MTLPixelFormatRG32Float?2:4;
  std::vector<float> values(16*components);
  for(unsigned slice=0;slice<td.arrayLength;++slice){for(unsigned i=0;i<values.size();++i)values[i]=float((i*7+slice*11)%29)/32.0;
   [tex replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 slice:slice withBytes:values.data() bytesPerRow:4*components*4 bytesPerImage:16*components*4];}
  for(unsigned variant=0;variant<(depth?1u:2u);++variant){
  id<MTLTexture> bound=tex;
  if(variant)bound=[tex newTextureViewWithPixelFormat:td.pixelFormat textureType:td.textureType levels:NSMakeRange(0,1) slices:NSMakeRange(0,td.arrayLength) swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleGreen,MTLTextureSwizzleRed,MTLTextureSwizzleOne,MTLTextureSwizzleZero)];assert(bound);
  for(MTLCompareFunction compare:{MTLCompareFunctionNever,MTLCompareFunctionLess,MTLCompareFunctionEqual,MTLCompareFunctionLessEqual,MTLCompareFunctionGreater,MTLCompareFunctionNotEqual,MTLCompareFunctionGreaterEqual,MTLCompareFunctionAlways}){if(!depth&&compare!=MTLCompareFunctionLessEqual)continue;
  for(MTLSamplerAddressMode mode:{MTLSamplerAddressModeClampToEdge,MTLSamplerAddressModeRepeat,MTLSamplerAddressModeMirrorRepeat,MTLSamplerAddressModeClampToBorderColor})for(MTLSamplerBorderColor border:{MTLSamplerBorderColorTransparentBlack,MTLSamplerBorderColorOpaqueBlack,MTLSamplerBorderColorOpaqueWhite}){
   MTLSamplerDescriptor* sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterLinear;sd.sAddressMode=sd.tAddressMode=mode;sd.borderColor=border;sd.compareFunction=compare;
   id<MTLSamplerState> sm=[d newSamplerStateWithDescriptor:sd];assert(sm);
   const unsigned n=31*21;id<MTLBuffer> out=[d newBufferWithLength:n*2*sizeof(V) options:MTLResourceStorageModeShared];
   id<MTLCommandBuffer> cb=[q commandBuffer];id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];[enc setComputePipelineState:ps];[enc setTexture:bound atIndex:0];[enc setSamplerState:sm atIndex:0];[enc setBuffer:out offset:0 atIndex:0];[enc dispatchThreads:MTLSizeMake(n,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[enc endEncoding];[cb commit];[cb waitUntilCompleted];if(cb.status!=MTLCommandBufferStatusCompleted){NSLog(@"%@",cb.error);return 2;}
   const float* bytes=(const float*)out.contents;for(unsigned i=0;i<n;++i)for(unsigned c=0;c<4;++c){float expected=bytes[8*i+c],actual=bytes[8*i+4+c];float delta=fabsf(expected-actual);if(!std::isfinite(actual)||delta>0.00001){fprintf(stderr,"mismatch array=%d depth=%d format=%lu mode=%lu border=%lu i=%u channel=%u expected=%g actual=%g\n",array,depth,(unsigned long)format,(unsigned long)mode,(unsigned long)border,i,c,expected,actual);return 3;}maxError=fmaxf(maxError,delta);++comparisons;}
   ++cases;
  }
  }
  }
 }
 }
 printf("PASS: %u Metal cases, %u channel comparisons; 2D/array, R32/RG32/RGBA32, depth PCF/all comparison functions, swizzles, edge/repeat/mirror/borders; maxError=%g\n",cases,comparisons,maxError);
}}
'''
with tempfile.TemporaryDirectory(prefix='77u-float32-filter-') as t:
 p=Path(t);(p/'test.mm').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-fobjc-arc','-I'+str(r/'src'),str(p/'test.mm'),'-framework','Metal','-framework','Foundation','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],env=dict(os.environ,MTL_DEBUG_LAYER='1'),check=True)
