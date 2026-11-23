#!/usr/bin/env python3
"""Compare the production fetch expression with the normal GX2 texture view.

Runs on a real Apple Metal GPU. An empty R8 mask must stay empty in every
channel; framebuffer fetch must not replace its replicated alpha with 1.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
emit = (repo / 'src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitMSL.cpp').read_text()
view = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.cpp').read_text()
header = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h').read_text()

def body(text, start):
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[opening + 1:end - 1]

fetch = body(emit, emit.index('if (static_cast<MetalRenderer*>(g_renderer.get())->SupportsFramebufferFetch() && renderTargetIndex != 255)', emit.index('// Do a framebuffer fetch')))
adjust = 'uint32 LatteTextureMtl_AdjustTextureCompSel(Latte::E_GX2SURFFMT format, uint32 compSel) {' + body(view, view.index('uint32 LatteTextureMtl_AdjustTextureCompSel(')) + '\n}'
helper = 'bool MetalFramebufferFetchReplicatesRed(Latte::E_GX2SURFFMT format) {' + body(header, header.index('inline bool MetalFramebufferFetchReplicatesRed(')) + '\n}' if 'inline bool MetalFramebufferFetchReplicatesRed(' in header else ''
fixture = r'''
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <array>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cmath>
using uint8=uint8_t; using uint32=uint32_t;
namespace Latte { enum class E_GX2SURFFMT {
 R8_UNORM,R8_SNORM,A1_B5_G5_R5_UNORM,BC4_UNORM,BC4_SNORM,
 BC5_UNORM,BC5_SNORM,A2_B10_G10_R10_UNORM,X24_G8_UINT,R4_G4_UNORM,RGBA
}; }
struct ContextRegisters { Latte::E_GX2SURFFMT format; };
namespace LatteMRT {
 Latte::E_GX2SURFFMT GetColorBufferFormat(uint32, const ContextRegisters& context) { return context.format; }
}
struct StringBuf {
 std::string str;
 void add(const char* value) { str+=value; }
 template<class T> void addFmt(const char* value, T number) {
  std::string format=value; const auto p=format.find("{}");
  format.replace(p,2,std::to_string(number)); str+=format;
 }
};
struct LatteDecompilerShaderContext { ContextRegisters* contextRegistersNew; };
void _recordUnsafeFramebufferFetch(LatteDecompilerShaderContext*,void*,uint8) {}
HELPER
std::string expression(Latte::E_GX2SURFFMT textureFormat) {
 ContextRegisters regs{textureFormat}; LatteDecompilerShaderContext context{&regs};
 auto* shaderContext=&context; void* texInstruction=nullptr;
 uint8 renderTargetIndex=0; StringBuf source; auto* src=&source;
 FETCH
 return source.str;
}
ADJUST
void require(bool value,const char* message) { if (!value) { puts(message); std::exit(1); } }
std::array<float,4> run(MTL::Device* device,Latte::E_GX2SURFFMT format,uint8 value,bool normalRead) {
 const bool single=format==Latte::E_GX2SURFFMT::R8_UNORM || format==Latte::E_GX2SURFFMT::R8_SNORM;
 const auto pixelFormat=format==Latte::E_GX2SURFFMT::R8_SNORM ? MTL::PixelFormatR8Snorm : (single?MTL::PixelFormatR8Unorm:MTL::PixelFormatRGBA8Unorm);
 auto* d=MTL::TextureDescriptor::texture2DDescriptor(pixelFormat,1,1,false);
 d->setStorageMode(MTL::StorageModeShared);
 d->setUsage(MTL::TextureUsageRenderTarget|MTL::TextureUsageShaderRead|MTL::TextureUsagePixelFormatView);
 auto* input=device->newTexture(d); require(input,"input texture allocation failed");
 std::array<uint8,4> bytes{value,64,128,255};
 input->replaceRegion(MTL::Region::Make2D(0,0,1,1),0,bytes.data(),single?1:4);
 MTL::TextureSwizzle choices[]={MTL::TextureSwizzleRed,MTL::TextureSwizzleGreen,MTL::TextureSwizzleBlue,MTL::TextureSwizzleAlpha};
 MTL::TextureSwizzleChannels swizzle{
  choices[LatteTextureMtl_AdjustTextureCompSel(format,0)],
  choices[LatteTextureMtl_AdjustTextureCompSel(format,1)],
  choices[LatteTextureMtl_AdjustTextureCompSel(format,2)],
  choices[LatteTextureMtl_AdjustTextureCompSel(format,3)]};
 auto* sampled=input->newTextureView(input->pixelFormat(),MTL::TextureType2D,NS::Range::Make(0,1),NS::Range::Make(0,1),swizzle);
 require(sampled,"sample view allocation failed");
 d=MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA32Float,1,1,false);
 d->setStorageMode(MTL::StorageModeShared);d->setUsage(MTL::TextureUsageRenderTarget);
 auto* output=device->newTexture(d);
 std::string msl="#include <metal_stdlib>\nusing namespace metal;\n"
 "vertex float4 vs(uint id [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[id],0,1); }\n"
 "struct Output { float4 value [[color(1)]]; };\n";
 msl += normalRead
  ? "fragment Output ps(texture2d<float> tex [[texture(0)]]) {return Output{tex.read(uint2(0))};}\n"
  : "fragment Output ps(float4 col0 [[color(0), raster_order_group(0)]]) {return Output{" + expression(format) + "};}\n";
 NS::Error* error=nullptr;
 auto* library=device->newLibrary(NS::String::string(msl.c_str(),NS::UTF8StringEncoding),nullptr,&error);
 if(error) puts(error->localizedDescription()->utf8String());
 require(library,"generated MSL did not compile");
 auto* vs=library->newFunction(NS::String::string("vs",NS::UTF8StringEncoding));
 auto* ps=library->newFunction(NS::String::string("ps",NS::UTF8StringEncoding));
 auto* pipelineD=MTL::RenderPipelineDescriptor::alloc()->init();
 pipelineD->setVertexFunction(vs);pipelineD->setFragmentFunction(ps);
 pipelineD->colorAttachments()->object(0)->setPixelFormat(input->pixelFormat());
 pipelineD->colorAttachments()->object(0)->setWriteMask(MTL::ColorWriteMaskNone);
 pipelineD->colorAttachments()->object(1)->setPixelFormat(MTL::PixelFormatRGBA32Float);
 auto* pipeline=device->newRenderPipelineState(pipelineD,&error);
 require(pipeline,"fetch pipeline did not compile");
 auto* pass=MTL::RenderPassDescriptor::renderPassDescriptor();
 pass->colorAttachments()->object(0)->setTexture(input);
 pass->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionLoad);
 pass->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
 pass->colorAttachments()->object(1)->setTexture(output);
 pass->colorAttachments()->object(1)->setLoadAction(MTL::LoadActionDontCare);
 pass->colorAttachments()->object(1)->setStoreAction(MTL::StoreActionStore);
 auto* queue=device->newCommandQueue();auto* cb=queue->commandBuffer();
 auto* encoder=cb->renderCommandEncoder(pass);encoder->setRenderPipelineState(pipeline);
 if(normalRead) encoder->setFragmentTexture(sampled,0);
 encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,NS::UInteger(0),NS::UInteger(3));encoder->endEncoding();
 cb->commit();cb->waitUntilCompleted();require(cb->status()==MTL::CommandBufferStatusCompleted,"GPU failed");
 std::array<float,4> result{};output->getBytes(result.data(),sizeof(result),MTL::Region::Make2D(0,0,1,1),0);
 queue->release();pipeline->release();pipelineD->release();vs->release();ps->release();library->release();
 sampled->release();input->release();output->release();return result;
}
int main() {
 auto* pool=NS::AutoreleasePool::alloc()->init();auto* device=MTL::CreateSystemDefaultDevice();
 require(device,"No Metal GPU");bool passed=true;
 for(auto format:{Latte::E_GX2SURFFMT::R8_UNORM,Latte::E_GX2SURFFMT::R8_SNORM,Latte::E_GX2SURFFMT::RGBA}) {
  for(auto value:{uint8(0),uint8(64),uint8(255)}) {
   const auto expected=run(device,format,value,true),actual=run(device,format,value,false);
   printf("%s mask=%u texture=[%.3f %.3f %.3f %.3f] fetch=[%.3f %.3f %.3f %.3f]\n",
    format==Latte::E_GX2SURFFMT::R8_UNORM?"R8_UNORM":(format==Latte::E_GX2SURFFMT::R8_SNORM?"R8_SNORM":"RGBA"),value,
    expected[0],expected[1],expected[2],expected[3],actual[0],actual[1],actual[2],actual[3]);
   for(int channel=0;channel<4;++channel) passed &= std::fabs(actual[channel]-expected[channel])<0.0001f;
  }
 }
 device->release();pool->release();require(passed,"FAIL: framebuffer fetch changes GX2 mask channels");
 puts("PASS: real GPU fetch equals the GX2 texture-view path for empty and painted masks");
}
'''.replace('FETCH', fetch).replace('ADJUST',adjust).replace('HELPER',helper)
with tempfile.TemporaryDirectory(prefix='cemu-fetch-channels-') as temp:
    p=Path(temp);(p/'test.mm').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O2','-I'+str(repo/'dependencies/metal-cpp'),str(p/'test.mm'),'-framework','Metal','-framework','Foundation','-o',str(p/'test')],check=True)
    env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS='1')
    subprocess.run([str(p/'test')],env=env,check=True)
