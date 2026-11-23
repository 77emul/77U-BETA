#!/usr/bin/env python3
"""Exercise production query publication using real Metal visibility counters.

Catch premature fabricated hits, mandatory flushes that never submit their
buffer, and stale data published to GX2. The 4x4 fixtures are sample-count
tests, not a claim about Splatoon's encoding of own/enemy ink.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
query = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalQuery.cpp').read_text()
renderer = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp').read_text()
header = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h').read_text()
processor = (repo / 'src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp').read_text()
core = (repo / 'src/Cafe/HW/Latte/Core/LatteQuery.cpp').read_text()
common = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalCommon.h').read_text()


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


fixture = r'''
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#undef TARGET_OS_IPHONE
#undef TARGET_OS_IOS
#define TARGET_OS_IPHONE 1
#define TARGET_OS_IOS 1
using uint32=uint32_t;using sint32=int32_t;using uint64=uint64_t;using MPTR=uint32_t;
constexpr uint32 INVALID_UINT32=std::numeric_limits<uint32>::max();
struct MetalQueryRange {uint32 begin,end;};
struct {bool vk_accurate_barriers=false;} config;
auto& GetConfig(){return config;}
namespace CafeSystem {
 uint64 foregroundTitle=0x0005000010176a00ULL;
 uint64 GetForegroundTitleId(){return foregroundTitle;}
}
void require(bool value,const char* message){if(!value){puts(message);std::exit(1);}}
#define cemu_assert_debug(x) require(x,#x)
bool CommandBufferCompleted(MTL::CommandBuffer*);
'''
fixture += (repo / 'src/Cafe/HW/Latte/Core/LatteQueryObject.h').read_text().replace('#pragma once', '')
query_header = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalQuery.h').read_text()
fixture += '\n'.join(line for line in query_header.splitlines() if not line.startswith('#'))
fixture += r'''
struct MetalRenderer {
 static constexpr uint32 OCCLUSION_QUERY_POOL_SIZE=1024;
 MTL::Device* device;MTL::CommandQueue* queue;MTL::RenderPipelineState* pipeline;
 MTL::Texture* color;MTL::RenderCommandEncoder* m_commandEncoder=nullptr;
 enum class MetalEncoderType {None,Render};MetalEncoderType m_encoderType=MetalEncoderType::None;
 struct {MTL::Buffer* m_resultBuffer;uint64* m_resultsPtr;uint32 m_currentIndex=0;
  bool m_active=false;MTL::CommandBuffer* m_lastCommandBuffer=nullptr;} m_occlusionQuery;
 struct {MTL::CommandBuffer* m_commandBuffer=nullptr;bool m_commited=false;} m_currentCommandBuffer;
 std::vector<MTL::CommandBuffer*> m_executingCommandBuffers;
 uint32 m_recordedDrawcalls=0,m_commitTreshold=512;
 unsigned submissions=0;
 explicit MetalRenderer(MTL::Device* d):device(d),queue(d->newCommandQueue()) {
  m_occlusionQuery.m_resultBuffer=d->newBuffer(8192,MTL::ResourceStorageModeShared);
  m_occlusionQuery.m_resultsPtr=(uint64*)m_occlusionQuery.m_resultBuffer->contents();
  memset(m_occlusionQuery.m_resultsPtr,0,8192);
  auto* desc=MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm,4,4,false);
  desc->setStorageMode(MTL::StorageModeShared);desc->setUsage(MTL::TextureUsageRenderTarget);
  color=d->newTexture(desc);
  const char* msl=R"(
   #include <metal_stdlib>
   using namespace metal;
   vertex float4 vs(uint v [[vertex_id]]) {
    float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[v],0,1);
   }
   fragment float4 ps(constant uint& hit [[buffer(0)]]) {
    if(!hit)discard_fragment();return float4(1);
   }
  )";
  NS::Error* error=nullptr;auto* lib=d->newLibrary(NS::String::string(msl,NS::UTF8StringEncoding),nullptr,&error);
  require(lib,"query fixture shader");auto* v=lib->newFunction(NS::String::string("vs",NS::UTF8StringEncoding));
  auto* f=lib->newFunction(NS::String::string("ps",NS::UTF8StringEncoding));
  auto* pd=MTL::RenderPipelineDescriptor::alloc()->init();pd->setVertexFunction(v);pd->setFragmentFunction(f);
  pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  pipeline=d->newRenderPipelineState(pd,&error);require(pipeline,"query fixture pipeline");
  pd->release();f->release();v->release();lib->release();newBuffer();
 }
 ~MetalRenderer(){
  CommitCommandBuffer();m_currentCommandBuffer.m_commandBuffer->waitUntilCompleted();
  if(m_occlusionQuery.m_lastCommandBuffer)m_occlusionQuery.m_lastCommandBuffer->release();
  for(auto* b:m_executingCommandBuffers)b->release();
  if(!m_currentCommandBuffer.m_commited)m_currentCommandBuffer.m_commandBuffer->release();
  pipeline->release();color->release();m_occlusionQuery.m_resultBuffer->release();queue->release();
 }
 void newBuffer(){m_currentCommandBuffer={queue->commandBuffer()->retain(),false};m_recordedDrawcalls=0;}
 bool IsCommandBufferActive()const{return m_currentCommandBuffer.m_commandBuffer&&!m_currentCommandBuffer.m_commited;}
 MTL::CommandBuffer* GetCurrentCommandBuffer()const{return m_currentCommandBuffer.m_commandBuffer;}
 MTL::CommandBuffer* GetAndRetainCurrentCommandBufferIfNotCompleted()const;
 uint64* GetOcclusionQueryResultsPtr(){return m_occlusionQuery.m_resultsPtr;}
 uint32 GetOcclusionQueryIndex(){return m_occlusionQuery.m_currentIndex;}
 void BeginOcclusionQuery(){m_occlusionQuery.m_active=true;}
 void EndOcclusionQuery();
 void RequestSoonCommit();
 bool RequiresAccurateBarriers() const {return config.vk_accurate_barriers;}
 bool RequiresAccurateQueryResults() const;
 void occlusionQuery_flush();
 void occlusionQuery_updateState(){}
 void occlusionQuery_destroy(LatteQueryObject* q){delete static_cast<LatteQueryObjectMtl*>(q);}
 void CommitCommandBuffer(){
  if(m_commandEncoder){m_commandEncoder->endEncoding();m_commandEncoder=nullptr;m_encoderType=MetalEncoderType::None;}
  if(IsCommandBufferActive()){
   GetCurrentCommandBuffer()->commit();m_currentCommandBuffer.m_commited=true;
   m_executingCommandBuffers.push_back(GetCurrentCommandBuffer());++submissions;
  }
 }
 void draw(uint32 hit){
  if(m_currentCommandBuffer.m_commited)newBuffer();
  if(!m_commandEncoder){
   auto* p=MTL::RenderPassDescriptor::renderPassDescriptor();auto* a=p->colorAttachments()->object(0);
   a->setTexture(color);a->setLoadAction(MTL::LoadActionClear);a->setStoreAction(MTL::StoreActionStore);
   p->setVisibilityResultBuffer(m_occlusionQuery.m_resultBuffer);
   m_commandEncoder=GetCurrentCommandBuffer()->renderCommandEncoder(p);m_encoderType=MetalEncoderType::Render;
   m_commandEncoder->setRenderPipelineState(pipeline);
  }
  m_commandEncoder->setVisibilityResultMode(MTL::VisibilityResultModeCounting,m_occlusionQuery.m_currentIndex*8);
  m_commandEncoder->setFragmentBytes(&hit,4,0);m_commandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle,NS::UInteger(0),NS::UInteger(3));
  m_occlusionQuery.m_currentIndex=(m_occlusionQuery.m_currentIndex+1)%OCCLUSION_QUERY_POOL_SIZE;++m_recordedDrawcalls;
 }
};
MetalRenderer* g_renderer;
alignas(8) std::array<uint64,64> guestRAM;
void* memory_getPointerFromVirtualOffset(MPTR address){return (char*)guestRAM.data()+address;}
'''
for signature in ['MTL::CommandBuffer* GetAndRetainCurrentCommandBufferIfNotCompleted()',
                  'void EndOcclusionQuery()', 'void RequestSoonCommit()']:
    body = function(header, signature)
    fixture += '\n' + body.replace(signature, signature.replace(' ', ' MetalRenderer::', 1) if signature.startswith('void ') else 'MTL::CommandBuffer* MetalRenderer::GetAndRetainCurrentCommandBufferIfNotCompleted()')
fixture += '\n' + function(common, 'inline bool CommandBufferCompleted(')
fixture += '\n' + '\n'.join(function(query, signature) for signature in [
    'bool LatteQueryObjectMtl::getResult(', 'LatteQueryObjectMtl::~',
    'void LatteQueryObjectMtl::begin(', 'void LatteQueryObjectMtl::end('])
if 'bool MetalRenderer::RequiresAccurateQueryResults(' in renderer:
    fixture += '\n' + function(renderer, 'bool MetalRenderer::RequiresAccurateQueryResults(')
else:
    fixture += '\nbool MetalRenderer::RequiresAccurateQueryResults() const {return RequiresAccurateBarriers();}\n'
fixture += '\n' + function(renderer, 'void MetalRenderer::occlusionQuery_flush(')
fixture += '\n' + core[core.index('uint64 queryEventCounter'):core.index('uint64 LatteQuery_getNextEventId')]
fixture += '\n' + '\n'.join(function(core, signature) for signature in [
    'void LatteQuery_finishGX2Query(', 'void LatteQuery_UpdateFinishedQueries(',
    'void LatteQuery_UpdateFinishedQueriesForceFinishAll('])
fixture += '\nvoid LatteTextureReadback_UpdateFinishedTransfers(bool) {}\n'
position = 0
for index in range(2):
    position = processor.index('case IT_HLE_SYNC_ASYNC_OPERATIONS:', position)
    handler = function(processor[position:], 'case IT_HLE_SYNC_ASYNC_OPERATIONS:')
    fixture += f'\nvoid sync{index}() {{switch(1) {{case 1:{{{handler[handler.index("{")+1:-1]}}}}}}}\n'
    position += len(handler)
fixture += r'''
void addGX2Query(MetalRenderer& r,uint32 address,uint32 hit,unsigned draws=1){
 auto* binding=(LatteGX2QueryInformation*)calloc(1,sizeof(LatteGX2QueryInformation));
 binding->queryMPTR=address;binding->queryEventStart=++queryEventCounter;
 auto* q=new LatteQueryObjectMtl(&r);q->queryEventStart=binding->queryEventStart;q->begin();
 for(unsigned i=0;i<draws;++i)r.draw(hit);
 q->queryEnded=true;q->queryEventEnd=++queryEventCounter;binding->queryEnded=true;
 binding->queryEventEnd=q->queryEventEnd;q->end();
 list_queriesInFlight.push_back(q);list_activeGX2Queries2.push_back(binding);
 *(uint64*)((char*)guestRAM.data()+address+32)=0x4f435055;
}
int main(int argc,char** argv){
 auto* pool=NS::AutoreleasePool::alloc()->init();auto* d=MTL::CreateSystemDefaultDevice();require(d,"real Metal device");
 const char* mode=argc>1?argv[1]:"pending-off";config.vk_accurate_barriers=strstr(mode,"-on")!=nullptr;
 {
  MetalRenderer r(d);g_renderer=&r;guestRAM.fill(0);
  // The title exception must not re-enable expensive synchronization in Zelda.
  if(strstr(mode,"zelda")) {
   CafeSystem::foregroundTitle=0x00050000101c9500ULL;
   require(!r.RequiresAccurateQueryResults(),"Zelda keeps the v2 query policy with barriers disabled");
   r.occlusionQuery_flush();require(r.submissions==0,"Zelda query policy adds no forced submission");
   puts("PASS: Zelda keeps v2 nonblocking query policy");return 0;
  }
  for(uint64 title : {0x0005000010176900ULL,0x0005000010176a00ULL,0x0005000010162b00ULL,0x00050002101ce400ULL}) {
   CafeSystem::foregroundTitle=title;
   require(r.RequiresAccurateQueryResults(),"All Splatoon regions require real query results");
  }
  CafeSystem::foregroundTitle=0x0005000010176a00ULL;
  if(strstr(mode,"wrap"))r.m_occlusionQuery.m_currentIndex=1023;
  addGX2Query(r,0,0);addGX2Query(r,64,0);addGX2Query(r,128,1,strstr(mode,"wrap")?2:1);
  require(r.submissions==0,"query endings must not immediately submit each draw");
  if(strstr(mode,"pending")){
   const auto t=std::chrono::steady_clock::now();
   for(unsigned i=0;i<1000;++i)LatteQuery_UpdateFinishedQueries();
   auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-t).count();
   printf("pending polls: %lld us; ready=%zu, guest0=%llu\n",(long long)us,
    size_t(3-list_queriesInFlight.size()),(unsigned long long)guestRAM[1]);
   require(list_queriesInFlight.size()==3&&guestRAM[4]==0x4f435055,"FAIL: unfinished GPU query was published as a fabricated hit");
   require(us<500000&&r.submissions==0,"ordinary query polling must be nonblocking");
  }
  if(strstr(mode,"submitted")){
   r.CommitCommandBuffer();r.GetCurrentCommandBuffer()->waitUntilCompleted();LatteQuery_UpdateFinishedQueries();
  }else if(strstr(mode,"pm4-ring")) sync1();
  else if(strstr(mode,"pm4-buffer")) sync0();
  else LatteQuery_UpdateFinishedQueriesForceFinishAll();
  printf("samples: %llu, %llu, %llu; submissions=%u\n",(unsigned long long)guestRAM[1],
   (unsigned long long)guestRAM[9],(unsigned long long)guestRAM[17],r.submissions);
  require(guestRAM[1]==0&&guestRAM[9]==0,"FAIL: zero-sample query became a false positive");
  require(guestRAM[17]==(strstr(mode,"wrap")?32:16),"FAIL: positive query lost its exact sample count");
  require(guestRAM[4]==0&&guestRAM[12]==0&&guestRAM[20]==0&&list_queriesInFlight.empty(),"publish completed GX2 result flags");
  require(r.submissions==1,"batch query completion in one submission");
  r.occlusionQuery_flush();require(r.submissions==1,"completed-query flush must not resubmit");
 }
 d->release();pool->release();printf("PASS: %s\n",mode);
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-occlusion-result-') as temp:
    p = Path(temp)
    (p / 'test.mm').write_text(fixture)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O3', '-DNDEBUG',
                    '-I'+str(repo/'dependencies/metal-cpp'), str(p/'test.mm'),
                    '-framework', 'Metal', '-framework', 'Foundation', '-o', str(p/'test')], check=True)
    env = dict(os.environ, MTL_DEBUG_LAYER='1')
    failures = []
    for mode in ['pending-off', 'pending-on', 'flush-off', 'flush-on',
                 'submitted-off', 'submitted-on', 'wrap-off', 'wrap-on',
                 'pm4-buffer-off', 'pm4-ring-off', 'zelda-off']:
        try:
            result = subprocess.run([str(p/'test'), mode], env=env, timeout=10)
            if result.returncode:
                failures.append((mode, result.returncode))
        except subprocess.TimeoutExpired:
            failures.append((mode, 'unsubmitted-buffer wait timed out'))
    if failures:
        raise SystemExit(f'FAIL: {failures}')
