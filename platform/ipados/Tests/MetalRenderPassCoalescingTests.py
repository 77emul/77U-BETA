#!/usr/bin/env python3
"""Replay attachment omission through the production pass policy on Metal.

Check preserved pixels and barriers, then compare the same draw stream with
forced pass boundaries. The host GPU timings are not iPad gameplay timings.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp').read_text()
compiler = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.cpp').read_text()

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

encoder = function(source, 'MTL::RenderCommandEncoder* MetalRenderer::GetRenderCommandEncoder(')
policy = encoder[encoder.index('bool needsNewRenderPass = false;'):encoder.index('attachmentBreak = needsNewRenderPass;')]
helper = function(source, 'bool MetalRenderer::CanReuseRenderPassAttachments(') if 'bool MetalRenderer::CanReuseRenderPassAttachments(' in source else ''
mask_start = compiler.index('// Disable writes if not in the active FBO')
mask_end = compiler.index('// Blending', mask_start)
write_masks = compiler[mask_start:mask_end]
reuse_state = function(encoder, 'if (!needsNewRenderPass)\n').replace('return (MTL::RenderCommandEncoder*)m_commandEncoder;', 'return true;')

fixture = r'''
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using uint8=uint8_t;using uint32=uint32_t;using sint32=int32_t;
constexpr int LATTE_NUM_COLOR_TARGET=8;
constexpr int LATTE_CEMU_VS_TEX_UNIT_BASE=32,LATTE_CEMU_PS_TEX_UNIT_BASE=0,LATTE_CEMU_GS_TEX_UNIT_BASE=64;
namespace Latte {
 enum class E_GX2SURFFMT { INVALID_FORMAT, RGBA };
}
namespace LatteConst { enum class ShaderType { Vertex, Pixel, Geometry }; }
struct LatteTexture {};
struct LatteTextureView { LatteTexture* baseTexture; int firstMip=0, firstSlice=0; };
struct LatteTextureViewMtl : LatteTextureView {
 MTL::Texture* metal;
 MTL::Texture* GetRGBAView() const { return metal; }
};
uint32 g_attach_break_size=0,g_attach_break_depth=0,g_attach_break_color=0,g_attach_break_hazard=0;
struct FrameExtent { int x=512,y=512;bool operator==(const FrameExtent&)const=default; };
struct CachedFBOMtl {
 struct { LatteTextureView* texture=nullptr; } colorBuffer[8];
 struct { LatteTextureView* texture=nullptr;bool hasStencil=false; } depthBuffer;
 FrameExtent m_size;
};
struct Mapping {
 std::vector<int> units;
 int getTextureCount() const { return int(units.size()); }
 int getRelativeTextureUnitFromRelativeBindingPoint(int i) const {return units[i];}
};
struct LatteDecompilerShader { LatteConst::ShaderType shaderType; Mapping resourceMapping;
 uint8 textureUnitListCount=1;std::array<uint8,32> textureUnitList{};
 std::array<uint8,32> textureRenderTargetIndex=[] {std::array<uint8,32> a;a.fill(255);return a;}(); };
LatteDecompilerShader* vs=nullptr;LatteDecompilerShader* ps=nullptr;LatteDecompilerShader* gs=nullptr;
LatteDecompilerShader* LatteSHRC_GetActiveVertexShader() {return vs;}
LatteDecompilerShader* LatteSHRC_GetActivePixelShader() {return ps;}
LatteDecompilerShader* LatteSHRC_GetActiveGeometryShader() {return gs;}
struct Counter {void fetch_add(unsigned,std::memory_order) {}};
struct { struct { Counter metalEncoderReusesAfterFBOChange; } telemetry; } LatteGPUState;
struct MetalRenderer {
 bool m_supportsFramebufferFetch=true;
 struct {
  struct {CachedFBOMtl* m_fbo=nullptr;} m_activeFBO,m_lastUsedFBO;
  LatteTextureViewMtl* m_textures[96]{};
 } m_state;
 bool CanReuseRenderPassAttachments();
 void TrackRetainedAttachmentsForReusedEncoder() {}
 bool reuse(CachedFBOMtl* held,CachedFBOMtl* active,bool changed=true) {
  m_state.m_activeFBO.m_fbo=active;m_state.m_lastUsedFBO.m_fbo=held;
  bool fboChanged=changed;
  POLICY
  REUSE_STATE
  return false;
 }
};
HELPER
struct Attachments {std::array<Latte::E_GX2SURFFMT,8> colorFormats;};
MTL::ColorWriteMask GetMtlColorWriteMask(uint32 value) {return MTL::ColorWriteMask(value);}
void setWriteMasks(MTL::RenderPipelineDescriptor* desc,const Attachments& activeAttachmentsInfo) {
 uint32 renderTargetMask=0xffffffff;
 for(uint8 i=0;i<2;++i) {
  auto* colorAttachment=desc->colorAttachments()->object(i);
  WRITE_MASKS
 }
}
void require(bool value,const char* message) {if(!value){puts(message);std::exit(1);}}
MTL::Texture* texture(MTL::Device* device) {
 auto* d=MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm,512,512,false);
 d->setStorageMode(MTL::StorageModeShared);d->setUsage(MTL::TextureUsageRenderTarget|MTL::TextureUsageShaderRead);
 return device->newTexture(d);
}
struct Result {unsigned passes;double gpuMs;std::array<uint8,4> a,b;};
Result replay(MTL::Device* device,MTL::Library* library,bool forceSplits) {
 auto* a=texture(device);auto* b=texture(device);require(a&&b,"texture allocation");
 LatteTexture baseA,baseB;LatteTextureViewMtl va{{&baseA},a},vb{{&baseB},b};
 CachedFBOMtl both,onlyA;both.colorBuffer[0].texture=&va;both.colorBuffer[1].texture=&vb;
 onlyA.colorBuffer[0].texture=&va;
 std::array<MTL::RenderPipelineState*,2> pipelines{};
 auto* vertex=library->newFunction(NS::String::string("vs",NS::UTF8StringEncoding));
 auto* fragment=library->newFunction(NS::String::string("ps",NS::UTF8StringEncoding));
 for(int kind=0;kind<2;++kind) {
  auto* desc=MTL::RenderPipelineDescriptor::alloc()->init();desc->setVertexFunction(vertex);desc->setFragmentFunction(fragment);
  for(int slot=0;slot<2;++slot) desc->colorAttachments()->object(slot)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  Attachments active{};active.colorFormats.fill(Latte::E_GX2SURFFMT::INVALID_FORMAT);
  active.colorFormats[0]=Latte::E_GX2SURFFMT::RGBA;
  if(kind==0) active.colorFormats[1]=Latte::E_GX2SURFFMT::RGBA;
  setWriteMasks(desc,active);NS::Error* error=nullptr;
  pipelines[kind]=device->newRenderPipelineState(desc,&error);require(pipelines[kind],"pipeline allocation");desc->release();
 }
 auto* queue=device->newCommandQueue();auto* cb=queue->commandBuffer();
 MetalRenderer renderer;MTL::RenderCommandEncoder* enc=nullptr;CachedFBOMtl* held=nullptr;unsigned passes=0;
 for(int draw=0;draw<1002;++draw) {
  auto* active=draw%2?&onlyA:&both;
  const bool canReuse=enc&&renderer.reuse(held,active);
  if(canReuse) held=renderer.m_state.m_lastUsedFBO.m_fbo;
  if(!enc||forceSplits||!canReuse) {
   if(enc) enc->endEncoding();
   // The physical pass remains a superset. Production write masks above
   // keep omitted targets unchanged; logical and physical FBOs stay separate.
   auto* pass=MTL::RenderPassDescriptor::renderPassDescriptor();
   for(int slot=0;slot<2;++slot) {
    auto* att=pass->colorAttachments()->object(slot);att->setTexture(slot?b:a);
    att->setLoadAction(passes?MTL::LoadActionLoad:MTL::LoadActionClear);
    att->setStoreAction(MTL::StoreActionStore);
    att->setClearColor(MTL::ClearColor(0,0,0,0));
   }
   enc=cb->renderCommandEncoder(pass);held=&both;++passes;
  }
  enc->setRenderPipelineState(pipelines[draw%2]);
  const std::array<float,4> color=draw%2?std::array<float,4>{0,1,0,1}:std::array<float,4>{1,0,0,1};
  enc->setFragmentBytes(color.data(),sizeof(color),0);
  enc->drawPrimitives(MTL::PrimitiveTypeTriangle,NS::UInteger(0),NS::UInteger(3));
 }
 enc->endEncoding();cb->commit();cb->waitUntilCompleted();require(cb->status()==MTL::CommandBufferStatusCompleted,"GPU error");
 Result out{passes,(cb->GPUEndTime()-cb->GPUStartTime())*1000,{},{}};
 a->getBytes(out.a.data(),4,MTL::Region::Make2D(16,16,1,1),0);
 b->getBytes(out.b.data(),4,MTL::Region::Make2D(16,16,1,1),0);
 for(auto* p:pipelines)p->release();vertex->release();fragment->release();queue->release();a->release();b->release();return out;
}
int main() {
 auto* pool=NS::AutoreleasePool::alloc()->init();auto* device=MTL::CreateSystemDefaultDevice();require(device,"No Metal GPU");
 NS::Error* error=nullptr;
 const char* msl="#include <metal_stdlib>\nusing namespace metal;\n"
 "vertex float4 vs(uint id [[vertex_id]]) {float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[id],0,1);}\n"
 "struct O {float4 a [[color(0)]];float4 b [[color(1)]];};\n"
 "fragment O ps(constant float4& color [[buffer(0)]]){return O{color,color};}\n";
 auto* library=device->newLibrary(NS::String::string(msl,NS::UTF8StringEncoding),nullptr,&error);require(library,"MSL error");
 auto baseline=replay(device,library,true),reused=replay(device,library,false);
 printf("forced: %u passes %.3fms GPU; production policy: %u passes %.3fms GPU\n",baseline.passes,baseline.gpuMs,reused.passes,reused.gpuMs);
 require(baseline.a==reused.a&&baseline.b==reused.b,"coalescing changed pixels");
 require(reused.a==std::array<uint8,4>{0,255,0,255}&&reused.b==std::array<uint8,4>{255,0,0,255},"inactive target overwritten");
 require(reused.passes==1,"FAIL: omission splits every render pass");
 LatteTexture ba,bb,bc;auto* a=texture(device);auto* b=texture(device);auto* c=texture(device);
 LatteTextureViewMtl va{{&ba},a},vb{{&bb},b},vc{{&bc},c},alias{{&ba},a};
 CachedFBOMtl both,onlyA;both.colorBuffer[0].texture=&va;both.colorBuffer[1].texture=&vb;onlyA.colorBuffer[0].texture=&va;
 MetalRenderer renderer;
 for(auto stage:{LatteConst::ShaderType::Vertex,LatteConst::ShaderType::Pixel,LatteConst::ShaderType::Geometry}) {
  LatteDecompilerShader shader{stage,{{0}}};
  vs=stage==LatteConst::ShaderType::Vertex?&shader:nullptr;
  ps=stage==LatteConst::ShaderType::Pixel?&shader:nullptr;
  gs=stage==LatteConst::ShaderType::Geometry?&shader:nullptr;
  int unit=stage==LatteConst::ShaderType::Vertex?32:(stage==LatteConst::ShaderType::Geometry?64:0);
  renderer.m_state.m_textures[unit]=&vb;
  require(!renderer.reuse(&both,&onlyA),"sampled omitted target must end encoder");
  require(!renderer.reuse(&both,&onlyA,false),"later shader sampling without an FBO change must split");
  renderer.m_state.m_textures[unit]=nullptr;
 }
 vs=ps=gs=nullptr;
 LatteDecompilerShader readShader{LatteConst::ShaderType::Pixel,{{0}}};ps=&readShader;
 renderer.m_state.m_textures[0]=&vb;
 require(!renderer.reuse(&both,&both),"reactivated color sampled as texture must split");
 readShader.textureRenderTargetIndex[0]=1;
 require(renderer.reuse(&both,&both),"valid active framebuffer fetch should remain in pass");
 require(!renderer.reuse(&both,&onlyA),"fetch of omitted target cannot bypass barrier");
 renderer.m_state.m_textures[0]=nullptr;ps=nullptr;
 onlyA.colorBuffer[0].texture=&alias;require(renderer.reuse(&both,&onlyA),"identical Metal view alias should reuse");
 onlyA.colorBuffer[0].texture=&vc;require(!renderer.reuse(&both,&onlyA),"changed color target must split");
 onlyA.colorBuffer[0].texture=&va;onlyA.m_size.x=256;require(!renderer.reuse(&both,&onlyA),"changed extent must split");
 onlyA.m_size=both.m_size;onlyA.depthBuffer.texture=&vc;require(!renderer.reuse(&both,&onlyA),"changed depth must split");
 onlyA.depthBuffer.texture=nullptr;onlyA.depthBuffer.hasStencil=true;require(!renderer.reuse(&both,&onlyA),"changed stencil must split");
 onlyA.depthBuffer.hasStencil=false;both.depthBuffer.texture=&vc;
 require(renderer.reuse(&both,&onlyA),"omitted depth target should reuse pass");
 renderer.m_state.m_textures[0]=&vc;
 LatteDecompilerShader depthReadShader{LatteConst::ShaderType::Pixel,{{0}}};ps=&depthReadShader;
 require(!renderer.reuse(&both,&onlyA),"sampled omitted depth target must end encoder");
 ps=nullptr;renderer.m_state.m_textures[0]=nullptr;both.depthBuffer.texture=nullptr;
 a->release();b->release();c->release();library->release();device->release();pool->release();
 puts("PASS: inactive pixels preserved; VS/PS/GS sampling, replacement, extent, depth and stencil still split");
}
'''.replace('POLICY', policy).replace('HELPER', helper).replace('WRITE_MASKS', write_masks).replace('REUSE_STATE', reuse_state)
with tempfile.TemporaryDirectory(prefix='cemu-pass-coalescing-') as temporary:
    p = Path(temporary)
    (p/'test.mm').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O2','-I'+str(repo/'dependencies/metal-cpp'),str(p/'test.mm'),'-framework','Metal','-framework','Foundation','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS='1'),check=True)
