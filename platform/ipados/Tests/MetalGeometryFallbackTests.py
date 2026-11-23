#!/usr/bin/env python3
"""Real Metal image/numerical comparisons; actual rectangle emitter + fallback adapter.
Runs on this Mac's GPU, not an iPhone 11 hardware certification.
"""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[3]
compiler = (root/'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.cpp').read_text()
rect = compiler[compiler.index('static void rectsEmulationGS_outputSingleVertex'):compiler.index('#define INVALID_TITLE_ID')]
fixture = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cassert>
#include <iostream>
#include <sstream>
#include <vector>
#include "Cafe/HW/Latte/Renderer/Metal/MetalGeometryFallback.h"
using sint32=int; using uint32=unsigned;
namespace fmt {
template<class T> std::string value(T v) { std::ostringstream s; s<<v; return s.str(); }
template<class... T> std::string format(std::string s,T... v) {
    for (auto x: std::vector<std::string>{value(v)...}) { auto p=s.find("{}"); assert(p!=std::string::npos); s.replace(p,2,x); } return s;
}}
struct Import {bool isFlat=false, isNoPerspective=false;};
struct LatteContextRegister { const unsigned* GetRawView() const {return nullptr;} };
struct LatteDecompilerShader { unsigned outputParameterMask=3; };
struct LatteShaderPSInputTable {
    Import inputs[2]{{true,false},{false,true}};
    int getVertexShaderOutParamSemanticId(const unsigned*,unsigned i) {return i;}
    bool hasPSImportForSemanticId(int i) {return i>=0&&i<2;}
    Import* getPSImportBySemanticId(int i) {return hasPSImportForSemanticId(i)?&inputs[i]:nullptr;}
    int getPSImportLocationBySemanticId(int i) {return i;}
};
void LatteShader_CreatePSInputTable(LatteShaderPSInputTable*,const unsigned*) {}
struct MetalRenderer {bool mesh;bool SupportsMeshShaders()const{return mesh;}};
struct RendererShader {enum class ShaderType{kGeometry};};
struct RendererShaderMtl {std::string source;
RendererShaderMtl(MetalRenderer*,RendererShader::ShaderType,int,int,bool,bool,const std::string& s):source(s){} };
'''
fixture += rect
fixture += r'''
static id<MTLLibrary> Library(id<MTLDevice> d,std::string s) {
 NSError* error=nil; MTLCompileOptions* o=[MTLCompileOptions new];o.languageVersion=MTLLanguageVersion3_0;o.mathMode=MTLMathModeSafe;
 id<MTLLibrary> l=[d newLibraryWithSource:[NSString stringWithUTF8String:s.c_str()] options:o error:&error];
 if(!l) {std::cerr<<s<<"\n"; NSLog(@"%@",error);abort();} return l;
}
const char* capture=R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VertexOut {float4 position;float4 passParameterSem0;float4 passParameterSem1;};
struct ObjectPayload {VertexOut vertexOut[3];};
vertex void main0(device ObjectPayload* out [[buffer(28)]],const device ObjectPayload* in [[buffer(0)]],uint v [[vertex_id]]) {
 out[v/3].vertexOut[v%3]=in[v/3].vertexOut[v%3];
}
)MSL";
const char* object=R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VertexOut {float4 position;float4 passParameterSem0;float4 passParameterSem1;};
struct ObjectPayload {VertexOut vertexOut[3];};
[[object,max_total_threads_per_threadgroup(3),max_total_threadgroups_per_mesh_grid(1)]]
void main0(object_data ObjectPayload& out [[payload]],mesh_grid_properties grid,
 const device ObjectPayload* in [[buffer(0)]],uint p [[threadgroup_position_in_grid]],uint v [[thread_index_in_threadgroup]]) {
 out.vertexOut[v]=in[p].vertexOut[v]; if(v==0)grid.set_threadgroups_per_grid(uint3(1));
}
)MSL";
const char* fragment=R"MSL(
#include <metal_stdlib>
using namespace metal;
struct In {float4 position [[position]];float4 color [[user(locn0),flat]];float4 uv [[user(locn1),center_no_perspective]];};
fragment float4 main0(In in [[stage_in]]) {return float4(in.color.xy,in.uv.x,in.position.z);}
)MSL";
struct V {float p[4],color[4],uv[4];};struct Payload{V v[3];};
static std::vector<unsigned char> Image(id<MTLDevice> d,id<MTLBuffer> input,id<MTLFunction> cap,id<MTLFunction> obj,
 id<MTLFunction> gs,id<MTLFunction> fs,bool fallback,unsigned vertices,unsigned topology,unsigned requested=4) {
 NSError* error=nil;
 id<MTLRenderPipelineState> pipeline;
 MTLRenderPipelineDescriptor* desc=[MTLRenderPipelineDescriptor new];
 desc.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
 desc.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;desc.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
 if(fallback){desc.vertexFunction=gs;desc.fragmentFunction=fs;pipeline=[d newRenderPipelineStateWithDescriptor:desc error:&error];}
 else {MTLMeshRenderPipelineDescriptor* m=[MTLMeshRenderPipelineDescriptor new];m.objectFunction=obj;m.meshFunction=gs;m.fragmentFunction=fs;
 m.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;m.depthAttachmentPixelFormat=desc.depthAttachmentPixelFormat;m.stencilAttachmentPixelFormat=desc.stencilAttachmentPixelFormat;
 pipeline=[d newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionNone reflection:nil error:&error];}
 if(!pipeline){NSLog(@"%@",error);abort();}
 desc.vertexFunction=cap;desc.fragmentFunction=nil;desc.rasterizationEnabled=NO;
 id<MTLRenderPipelineState> capturePipeline=[d newRenderPipelineStateWithDescriptor:desc error:&error];assert(capturePipeline);
 MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
 td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModeShared;id<MTLTexture> color=[d newTextureWithDescriptor:td];
 td.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;td.storageMode=MTLStorageModePrivate;id<MTLTexture> depth=[d newTextureWithDescriptor:td];
 MTLRenderPassDescriptor* pass=[MTLRenderPassDescriptor new];pass.colorAttachments[0].texture=color;
 pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
 pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=1;
 pass.stencilAttachment.texture=depth;pass.stencilAttachment.loadAction=MTLLoadActionClear;pass.stencilAttachment.storeAction=MTLStoreActionStore;
 MTLDepthStencilDescriptor* ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionLess;ds.depthWriteEnabled=YES;
 MTLStencilDescriptor* stencil=[MTLStencilDescriptor new];stencil.stencilCompareFunction=MTLCompareFunctionAlways;stencil.depthStencilPassOperation=MTLStencilOperationReplace;
 ds.frontFaceStencil=stencil;ds.backFaceStencil=stencil;
 auto visibility=[d newBufferWithLength:8 options:MTLResourceStorageModeShared];memset(visibility.contents,0,8);pass.visibilityResultBuffer=visibility;
 auto q=[d newCommandQueue];auto cb=[q commandBuffer];auto e=[cb renderCommandEncoderWithDescriptor:pass];
 [e setDepthClipMode:MTLDepthClipModeClamp];
 [e setDepthStencilState:[d newDepthStencilStateWithDescriptor:ds]];[e setStencilReferenceValue:7];
 // Exercise culling, depth, stencil and a nontrivial viewport/scissor identically.
 [e setFrontFacingWinding:MTLWindingCounterClockwise];[e setCullMode:MTLCullModeBack];
 [e setViewport:MTLViewport{1,2,62,60,0,1}];[e setScissorRect:MTLScissorRect{2,3,59,58}];
 auto payload=[d newBufferWithLength:sizeof(Payload) options:MTLResourceStorageModePrivate];
 if(fallback){[e setRenderPipelineState:capturePipeline];[e setVertexBuffer:input offset:0 atIndex:0];[e setVertexBuffer:payload offset:0 atIndex:28];
 [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:3];[e memoryBarrierWithScope:MTLBarrierScopeBuffers afterStages:MTLRenderStageVertex beforeStages:MTLRenderStageVertex];
 [e setRenderPipelineState:pipeline];[e setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0];[e setVertexBuffer:payload offset:0 atIndex:28];[e setVertexBytes:&requested length:4 atIndex:0];
 [e drawPrimitives:topology==0?MTLPrimitiveTypePoint:topology==1?MTLPrimitiveTypeLine:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertices];}
 else {[e setRenderPipelineState:pipeline];[e setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0];[e setObjectBuffer:input offset:0 atIndex:0];[e setMeshBytes:&requested length:4 atIndex:0];[e drawMeshThreadgroups:MTLSizeMake(1,1,1) threadsPerObjectThreadgroup:MTLSizeMake(3,1,1) threadsPerMeshThreadgroup:MTLSizeMake(1,1,1)];}
 [e endEncoding];
 id<MTLBuffer> numerical=[d newBufferWithLength:sizeof(Payload) options:MTLResourceStorageModeShared];
 if(fallback){auto b=[cb blitCommandEncoder];[b copyFromBuffer:payload sourceOffset:0 toBuffer:numerical destinationOffset:0 size:sizeof(Payload)];[b endEncoding];}
 [cb commit];[cb waitUntilCompleted];if(cb.status!=MTLCommandBufferStatusCompleted){NSLog(@"%@",cb.error);abort();}
 if(fallback)assert(memcmp(input.contents,numerical.contents,sizeof(Payload))==0);
 std::vector<unsigned char> pixels(64*64*4);[color getBytes:pixels.data() bytesPerRow:64*4 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
 auto count=(unsigned char*)visibility.contents;pixels.insert(pixels.end(),count,count+8);return pixels;
}

static void CaptureInputsTest(id<MTLDevice> d) {
 std::string base=R"MSL(
#include <metal_stdlib>
using namespace metal;
#define VERTICES_PER_VERTEX_PRIMITIVE 3
#define VERTEX_BUFFERS
struct VertexOut{float4 position;float4 passParameterSem0;float4 passParameterSem1;};
struct ObjectPayload{VertexOut vertexOut[VERTICES_PER_VERTEX_PRIMITIVE];};
struct VertexIn{float4 a;};
struct LabDraw{int baseVertex;uint baseInstance;uint verticesPerInstance;uint primitivesPerInstance;};
VertexIn fetchVertex(thread uint& vid,uint iid,device uint* indexBuffer,uchar indexType,int labBaseVertex){
 if(indexType==1)vid=((device ushort*)indexBuffer)[vid];else if(indexType==2)vid=indexBuffer[vid];
 vid+=labBaseVertex;VertexIn in;in.a=float4(float(vid),float(iid),float(vid+iid),1.0);return in;
}
vertex void main0(device ObjectPayload* labPayloads [[buffer(28)]],constant LabDraw& labDraw [[buffer(29)]],uint labVertex [[vertex_id]],device uint* indexBuffer [[buffer(0)]],constant uchar& indexType [[buffer(1)]]) {
)MSL";
 for(bool connected:{false,true})for(uint8_t kind:{0,1,2}) {
  auto prefix=base;const uint32_t inputVertices=connected?2:3;
  MetalGeometryFallback::Replace(prefix,"#define VERTICES_PER_VERTEX_PRIMITIVE 3","#define VERTICES_PER_VERTEX_PRIMITIVE "+std::to_string(inputVertices));
  auto source=prefix+MetalGeometryFallback::CapturePrelude(connected)+"out.position=in.a;out.passParameterSem0=float4(vid);out.passParameterSem1=float4(iid);}\n";
  auto entry=[Library(d,source) newFunctionWithName:@"main0"];
  auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=entry;desc.rasterizationEnabled=NO;desc.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
  NSError* error=nil;auto pipeline=[d newRenderPipelineStateWithDescriptor:desc error:&error];assert(pipeline);
  const uint32_t count=connected?5:6,per=connected?4:2,total=per*2;
  MetalGeometryFallback::Draw draw{kind?-1:2,3,count,per};
  uint32_t index32[8]={5,2,7,1,9,6,8,3};uint16_t index16[8]={5,2,7,1,9,6,8,3};
  auto index=[d newBufferWithBytes:kind==1?(void*)index16:(void*)index32 length:kind==1?sizeof(index16):sizeof(index32) options:MTLResourceStorageModeShared];
  auto output=[d newBufferWithLength:total*inputVertices*sizeof(V) options:MTLResourceStorageModeShared];
  auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;
  auto pass=[MTLRenderPassDescriptor new];pass.colorAttachments[0].texture=[d newTextureWithDescriptor:td];
  auto cb=[[d newCommandQueue] commandBuffer];auto e=[cb renderCommandEncoderWithDescriptor:pass];[e setRenderPipelineState:pipeline];
  [e setVertexBuffer:output offset:0 atIndex:28];[e setVertexBytes:&draw length:sizeof(draw) atIndex:29];[e setVertexBuffer:index offset:0 atIndex:0];[e setVertexBytes:&kind length:1 atIndex:1];
  [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:total*inputVertices];[e endEncoding];[cb commit];[cb waitUntilCompleted];assert(cb.status==MTLCommandBufferStatusCompleted);
  auto payload=(V*)output.contents;
  for(uint32_t primitive=0;primitive<total;++primitive)for(uint32_t tid=0;tid<inputVertices;++tid){
   uint32_t local=primitive%per,vid=connected?local+tid:local*inputVertices+tid;
   uint32_t expected=(kind?index32[vid]:vid)+draw.baseVertex;uint32_t instance=primitive/per+draw.baseInstance;
   assert(payload[primitive*inputVertices+tid].p[0]==float(expected));assert(payload[primitive*inputVertices+tid].p[1]==float(instance));
  }
 }
}

int main(){@autoreleasepool{
 auto d=MTLCreateSystemDefaultDevice();assert(d && [d supportsFamily:MTLGPUFamilyApple7]);
 CaptureInputsTest(d);
 auto cap=[Library(d,capture) newFunctionWithName:@"main0"],obj=[Library(d,object) newFunctionWithName:@"main0"],fs=[Library(d,fragment) newFunctionWithName:@"main0"];
 LatteDecompilerShader vs;LatteContextRegister regs;MetalRenderer native{true},legacy{false};
 auto ng=std::unique_ptr<RendererShaderMtl>(rectsEmulationGS_generate(&native,&vs,regs));
 auto fg=std::unique_ptr<RendererShaderMtl>(rectsEmulationGS_generate(&legacy,&vs,regs));
 auto n=[Library(d,ng->source) newFunctionWithName:@"main0"],f=[Library(d,fg->source) newFunctionWithName:@"main0"];
 unsigned cases=0;
 // Every rectangle diagonal branch, both orientations and non-unit clip W.
 for(unsigned diagonal=0;diagonal<3;++diagonal)for(unsigned flip=0;flip<2;++flip)for(float w:{1.f,2.f}){
  Payload p{};float positions[3][2]={{-.8f,-.7f},{.8f,-.7f},{-.8f,.7f}};
  for(unsigned v=0;v<3;++v){auto i=(v+diagonal)%3;p.v[v].p[0]=positions[i][0]*w;p.v[v].p[1]=positions[i][1]*w*(flip?-1:1);p.v[v].p[2]=.4f*w;p.v[v].p[3]=w;
   p.v[v].color[0]=.2f+.2f*v;p.v[v].color[1]=.7f;p.v[v].uv[0]=float(i)/2;}
  auto in=[d newBufferWithBytes:&p length:sizeof(p) options:MTLResourceStorageModeShared];
  auto a=Image(d,in,cap,obj,n,fs,false,6,2),b=Image(d,in,cap,obj,f,fs,true,6,2);assert(a==b);++cases;
 }

 // Data-dependent point, line and triangle geometry outputs, including no output.
 // Input, transform feedback and raster output stay separate from game timing.
 for(unsigned topology=0;topology<3;++topology){
  std::string src=R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VertexOut {float4 position;float4 passParameterSem0;float4 passParameterSem1;};
struct ObjectPayload {VertexOut vertexOut[3];};
struct GeometryOut {float4 position [[position]];float4 passParameterSem0 [[user(locn0),flat]];float4 passParameterSem1 [[user(locn1),center_no_perspective]];float pointSize [[point_size]];};
)MSL";
  if(topology!=0){MetalGeometryFallback::Replace(src,"float pointSize [[point_size]];","");}
  const auto primitive=topology==0?"point":topology==1?"line":"triangle";
  const auto divisor=topology+1;
  src+="using MeshType = mesh<GeometryOut, void, 32, 32, topology::"+std::string(primitive)+">;\n";
  src+=R"MSL(
[[mesh, max_total_threads_per_threadgroup(1)]]
void main0(MeshType mesh, const object_data ObjectPayload& objectPayload [[payload]],constant uint& requested [[buffer(0)]]) {
 GeometryOut out{};uint vertexIndex=0;
 for(uint i=0;i<min(requested,4u);++i){
  out.position=objectPayload.vertexOut[i%3].position;
  if(i==3)out.position=objectPayload.vertexOut[1].position+objectPayload.vertexOut[2].position-objectPayload.vertexOut[0].position;
  out.passParameterSem0=objectPayload.vertexOut[i%3].passParameterSem0;
  out.passParameterSem1=float4(float(i)/4);out.pointSize=3;
  mesh.set_vertex(vertexIndex,out);++vertexIndex;
 }
)MSL";
  src+="uint primitives = vertexIndex > "+std::to_string(topology)+"u ? vertexIndex - "+std::to_string(topology)+"u : 0u;\nmesh.set_primitive_count(primitives);\n";
  if(topology==2)src+="for(uint i=0;i<primitives*3;++i)mesh.set_index(i,(i/3)+((i/3)%2&&i%3!=0?3-i%3:i%3));\n";
  else if(topology==1)src+="for(uint i=0;i<primitives*2;++i)mesh.set_index(i,(i/2)+i%2);\n";
  else src+="for(uint i=0;i<vertexIndex;++i)mesh.set_index(i,i);\n";
  src+="}\n";
  if(topology!=0)MetalGeometryFallback::Replace(src,"out.pointSize=3;","");
  auto nativeGS=[Library(d,src) newFunctionWithName:@"main0"];
  auto fallbackGS=[Library(d,MetalGeometryFallback::GeometrySource(src,topology)) newFunctionWithName:@"main0"];
  Payload p{};float coordinates[3][2]={{-.8f,-.7f},{.8f,-.7f},{-.8f,.7f}};
  for(unsigned v=0;v<3;++v){p.v[v].p[0]=coordinates[v][0];p.v[v].p[1]=coordinates[v][1];p.v[v].p[2]=.4;p.v[v].p[3]=1;p.v[v].color[0]=.3+.2*v;p.v[v].color[1]=.7;}
  auto in=[d newBufferWithBytes:&p length:sizeof(p) options:MTLResourceStorageModeShared];
  for(unsigned requested=0;requested<=4;++requested){
   auto a=Image(d,in,cap,obj,nativeGS,fs,false,0,topology,requested);
   auto b=Image(d,in,cap,obj,fallbackGS,fs,true,MetalGeometryFallback::ExpandedVertices(topology),topology,requested);
   assert(a==b);++cases;
  }
 }
 bool rejected=false;try{MetalGeometryFallback::GeometrySource("bad",2);}catch(...){rejected=true;}assert(rejected);
 assert(MetalGeometryFallback::ExpandedVertices(0)==32&&MetalGeometryFallback::ExpandedVertices(1)==62&&MetalGeometryFallback::ExpandedVertices(2)==90);
 std::cout<<"PASS: "<<cases<<" production rectangle and data-dependent GS mesh/vertex images byte-identical; exact captured payload; production capture prologue indexed/nonindexed, base vertex/instance and connected primitives; flat/nonperspective attributes, clip W, culling, depth clamp/stencil, occlusion counts, scissor, malformed-source rejection\n";
}}
'''
with tempfile.TemporaryDirectory(prefix='77u-geometry-') as tmp:
    tmp=Path(tmp); (tmp/'test.mm').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O1','-fobjc-arc','-I'+str(root/'src'),str(tmp/'test.mm'),'-framework','Foundation','-framework','Metal','-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True,env={**os.environ,'MTL_DEBUG_LAYER':'1'},timeout=120)
