#!/usr/bin/env python3
"""Exercise production PM4/readback logic with real Metal texture-to-buffer copies.

The boundary is CPU-visible query data, not the number of wait calls. This
reproduces skipped mandatory completion and waiting for an unsubmitted buffer.
The tiny test textures are fixtures, not a claim about Splatoon's ink encoding.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
core = (repo / 'src/Cafe/HW/Latte/Core/LatteTextureReadback.cpp').read_text()
metal = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/LatteTextureReadbackMtl.cpp').read_text()
processor = (repo / 'src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp').read_text()
common = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalCommon.h').read_text()
renderer = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp').read_text()
header = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h').read_text()


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


handlers = []
position = 0
for _ in range(2):
    position = processor.index('case IT_HLE_SYNC_ASYNC_OPERATIONS:', position)
    body = function(processor[position:], 'case IT_HLE_SYNC_ASYNC_OPERATIONS:')
    handlers.append(body[body.index('{') + 1:-1])
    position += len(body)

fixture = r'''
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <vector>
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#undef TARGET_OS_IPHONE
#undef TARGET_OS_IOS
#define TARGET_OS_IPHONE 1
#define TARGET_OS_IOS 1
using uint8=uint8_t; using uint32=uint32_t; using uint64_t=std::uint64_t;
using HRTick=std::chrono::steady_clock::time_point;
void require(bool value,const char* message) {if(!value){puts(message);std::exit(1);}}
#define cemu_assert(x) require(x,#x)
#define cemu_assert_debug(x) require(x,#x)
#define cemuLog_log(...) ((void)0)
struct HighResolutionTimer {
 HighResolutionTimer now() {return {};}
 HRTick getTick() {return std::chrono::steady_clock::now();}
};
struct Measurement {void beginMeasuring() {} void endMeasuring() {}};
struct {Measurement gpuTime_waitForAsync;} performanceMonitor;
struct {uint32 drawCallCounter=0;} LatteGPUState;
namespace Latte {enum class E_DIM {DIM_2D,DIM_3D};}
struct LatteTextureDefinition {
 uint32 physAddress=0,width=0,height=0,pitch=0,format=0,tileMode=0;
 LatteTextureDefinition()=default;
 LatteTextureDefinition(const LatteTextureDefinition* p):LatteTextureDefinition(*p) {}
};
struct LatteTextureMtl:LatteTextureDefinition {
 MTL::Texture* metal=nullptr; bool isDepth=false; Latte::E_DIM dim=Latte::E_DIM::DIM_2D;
 struct {bool hasResolutionOverwrite=false;} overwriteInfo;
 MTL::Texture* GetTexture() {return metal;}
};
using LatteTexture=LatteTextureMtl;
struct LatteTextureView {LatteTexture* baseTexture;uint32 firstSlice=0,firstMip=0;};
struct LatteTextureReadbackInfo {
 LatteTextureDefinition hostTextureCopy;LatteTextureView* m_textureView;
 HRTick transferStartTime{},waitStartTime{};bool forceFinish=false;
 LatteTextureReadbackInfo(LatteTextureView* v):hostTextureCopy(v->baseTexture),m_textureView(v) {}
 virtual ~LatteTextureReadbackInfo()=default;
 virtual void StartTransfer()=0;virtual void ForceFinish()=0;virtual bool IsFinished()=0;
 virtual uint8* GetData()=0;virtual void ReleaseData() {}
};
struct MetalRenderer;
struct LatteTextureReadbackInfoMtl:LatteTextureReadbackInfo {
 MetalRenderer* m_mtlr;MTL::CommandBuffer* m_commandBuffer=nullptr;uint32 m_bufferOffset;
 LatteTextureReadbackInfoMtl(MetalRenderer* r,LatteTextureView* v,uint32 o):
  LatteTextureReadbackInfo(v),m_mtlr(r),m_bufferOffset(o) {}
 ~LatteTextureReadbackInfoMtl();
 void StartTransfer() override;void ForceFinish() override;bool IsFinished() override;
 uint8* GetData() override;
};
struct MetalCommandBuffer {
 MTL::CommandBuffer* m_commandBuffer=nullptr;bool m_commited=false;bool m_readbackPending=false;
};
struct MetalRenderer {
 MetalCommandBuffer m_currentCommandBuffer;
 MTL::CommandQueue* queue;MTL::Buffer* data;
 MTL::CommandBuffer*& current=m_currentCommandBuffer.m_commandBuffer;MTL::BlitCommandEncoder* blit=nullptr;
 bool& committed=m_currentCommandBuffer.m_commited;bool activeRender=false;unsigned submissions=0,offset=0;
 unsigned m_idleReadbackSubmissions=0;
 explicit MetalRenderer(MTL::Device* d):queue(d->newCommandQueue()),data(d->newBuffer(2*1024*1024,MTL::ResourceStorageModeShared)) {
  memset(data->contents(),0xcd,2*1024*1024);
 }
 ~MetalRenderer() {if(current)current->release();data->release();queue->release();}
 bool HasActiveRenderEncoder() {return activeRender;}
 bool accurateBarriers=false,earlyReadbacks=false;
 bool ShouldSubmitReadbacksEarly() const {return earlyReadbacks;}
 unsigned m_recordedDrawcalls=0,m_commitTreshold=4096;
 enum class MetalEncoderType {Render};MetalEncoderType m_encoderType=MetalEncoderType::Render;
 struct {bool m_sequenceRasterized=false;}m_state;
 void TrackActiveAttachmentWritesForCurrentEncoder(){}
 void draw_endSequence();
 bool RequiresAccurateBarriers() const {return accurateBarriers;}
 bool RequiresAccurateQueryResults() const {return RequiresAccurateBarriers();}
 bool IsCommandBufferActive() {return current&&!committed;}
 MTL::CommandBuffer* GetCurrentCommandBuffer() {return current;}
 MTL::Buffer* GetTextureReadbackBuffer() {return data;}
 void FlushPendingTextureClears(LatteTexture*,int,int,int,int) {}
 void NotifyLatteCommandProcessorIdle();
 /*MARK_READBACK*/
 void EndEncoding() {if(blit){blit->endEncoding();blit=nullptr;}activeRender=false;}
 MTL::BlitCommandEncoder* GetBlitCommandEncoder() {
  if(!current||committed) {
   if(current)current->release();m_currentCommandBuffer={queue->commandBuffer()->retain()};
  }
  if(!blit)blit=current->blitCommandEncoder();activeRender=false;return blit;
 }
 void CommitCommandBuffer(bool = false, const char* = nullptr) {
  if(blit){blit->endEncoding();blit=nullptr;}
  if(IsCommandBufferActive()){current->commit();committed=true;++submissions;}
 }
 LatteTextureReadbackInfo* texture_createReadback(LatteTextureView* v) {
  unsigned start=offset;offset+=v->baseTexture->width*v->baseTexture->height*4;
  return new LatteTextureReadbackInfoMtl(this,v,start);
 }
};
struct LatteDecompilerShader {};
LatteDecompilerShader* LatteSHRC_GetActivePixelShader(){return nullptr;}
void LatteRenderTarget_trackUpdates(){}
MetalRenderer* g_renderer=nullptr;
bool LatteTextureReadback_Update(bool forceStart=false);
size_t GetMtlTextureBytesPerRow(uint32,bool,uint32 width){return width*4;}
size_t GetMtlTextureBytesPerImage(uint32,bool,uint32 height,size_t row){return height*row;}
MTL::BlitOption GetMtlBlitOption(MTL::PixelFormat){return MTL::BlitOptionNone;}
bool CemuBridge_IsDiagnosticLoggingEnabled(){return false;}
void RecordSlowReadback(const LatteTextureReadbackInfoMtl&,MTL::CommandBuffer*,uint64_t,HRTick) {}
template<typename T> void vectorRemoveByIndex(std::vector<T>& v,size_t i){v.erase(v.begin()+i);}
struct LatteTextureViewLookupCache {
 static LatteTextureView* lookupSlice(uint32,uint32,uint32,uint32,int,int,uint32){return nullptr;}
};
void LatteTC_ResetTextureChangeTracker(LatteTexture*,bool) {}
std::array<std::vector<uint8>,4> guestRAM;
unsigned published=0,querySyncs=0;
void LatteTextureLoader_writeReadbackTextureToMemory(LatteTextureDefinition* t,int,int,uint8* bytes) {
 guestRAM[t->physAddress].assign(bytes,bytes+t->width*t->height*4);++published;
}
void LatteQuery_UpdateFinishedQueries(){++querySyncs;}
void LatteQuery_UpdateFinishedQueriesForceFinishAll(){++querySyncs;}
'''

declarations = core[core.index('struct LatteTextureReadbackQueueEntry'):core.index('void LatteTextureReadback_StartTransfer(')]
functions = '\n'.join(function(core, signature) for signature in [
    'void LatteTextureReadback_StartTransfer(', 'bool LatteTextureReadback_Update(',
    'void LatteTextureReadback_Initate(', 'void LatteTextureReadback_UpdateFinishedTransfers('])
functions += '\n' + '\n'.join(function(metal, signature) for signature in [
    'LatteTextureReadbackInfoMtl::~', 'void LatteTextureReadbackInfoMtl::StartTransfer(',
    'bool LatteTextureReadbackInfoMtl::IsFinished(', 'void LatteTextureReadbackInfoMtl::ForceFinish(',
    'uint8* LatteTextureReadbackInfoMtl::GetData('])
functions += '\n' + function(common, 'inline bool CommandBufferCompleted(')
# This forward declaration is needed by the production Metal methods above.
fixture = fixture.replace('/*MARK_READBACK*/', function(header, 'void MarkReadbackPendingSubmission('))
fixture += '\nbool CommandBufferCompleted(MTL::CommandBuffer*);\n'
fixture += '\nbool LatteTextureReadback_HasEligibleTransfers();\n'
if 'bool LatteTextureReadback_HasEligibleTransfers(' in core:
    functions += '\n' + function(core, 'bool LatteTextureReadback_HasEligibleTransfers(')
functions += '\n' + function(renderer, 'void MetalRenderer::NotifyLatteCommandProcessorIdle(')
functions += '\n' + function(renderer, 'void MetalRenderer::draw_endSequence(')
fixture += declarations + functions
for index, body in enumerate(handlers):
    fixture += f'\nvoid sync{index}() {{switch(1) {{case 1:{{{body}}}}}}}\n'

fixture += r'''
LatteTextureMtl texture(MTL::Device* d,unsigned id,unsigned width,unsigned height,uint8 value) {
 auto* desc=MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm,width,height,false);
 desc->setStorageMode(MTL::StorageModeShared);desc->setUsage(MTL::TextureUsageShaderRead);
 LatteTextureMtl result;result.physAddress=id;result.width=width;result.height=height;result.pitch=width;
 result.metal=d->newTexture(desc);require(result.metal,"allocate query texture");
 std::vector<uint8> pixels(width*height*4,value);
 result.metal->replaceRegion(MTL::Region(0,0,width,height),0,pixels.data(),width*4);
 guestRAM[id].assign(width*height*4,0xcd);return result;
}
void check(unsigned id,uint8 expected) {
 require(std::all_of(guestRAM[id].begin(),guestRAM[id].end(),[&](auto b){return b==expected;}),
  "FAIL: mandatory GPU sync returned stale CPU query bytes");
}
int main(int argc,char** argv) {
 auto* pool=NS::AutoreleasePool::alloc()->init();auto* d=MTL::CreateSystemDefaultDevice();
 require(d,"Metal device");const char* mode=argc>1?argv[1]:"pm4-buffer";
 MetalRenderer r(d);g_renderer=&r;auto t=texture(d,0,64,8,0);LatteTextureView v{&t};
 if(strncmp(mode,"pm4-",4)==0) {
  r.accurateBarriers=strstr(mode,"relaxed")==nullptr;
  auto own=texture(d,1,32,16,0x35),enemy=texture(d,2,64,8,0xe2);
  LatteTextureView ownView{&own},enemyView{&enemy};r.activeRender=true;
  LatteTextureReadback_Initate(&v);LatteTextureReadback_Initate(&ownView);LatteTextureReadback_Initate(&enemyView);
  require(!LatteTextureReadback_Update(false)&&published==0,"ordinary polls remain asynchronous");
  if(strstr(mode,"ring"))sync1();else sync0();
  check(0,0);check(1,0x35);check(2,0xe2);
  require(published==3&&sTextureScheduledReadbacks.empty()&&sTextureActiveReadbackQueue.empty(),"drain scheduled and active queries");
  require(r.submissions==1,"batch all query copies in one command buffer");
  require(querySyncs==1,"preserve occlusion-query completion");
  own.metal->release();enemy.metal->release();
 } else if(strcmp(mode,"early-small")==0||strcmp(mode,"early-large")==0) {
  r.earlyReadbacks=true;bool large=strcmp(mode,"early-large")==0;
  auto own=texture(d,1,large?512:32,large?512:16,0x35),enemy=texture(d,2,large?256:64,large?256:8,0xe2);
  LatteTextureView ownView{&own},enemyView{&enemy};r.activeRender=true;
  LatteTextureReadback_Initate(&v);LatteTextureReadback_Initate(&ownView);LatteTextureReadback_Initate(&enemyView);
  r.draw_endSequence();require(r.submissions==0&&r.activeRender,"recent writes remain queued");
  LatteGPUState.drawCallCounter=5;r.draw_endSequence();
  require(r.submissions==1&&published==0&&sTextureScheduledReadbacks.empty(),"submit one stable batch before CPU dependency without waiting or publishing");
  r.GetBlitCommandEncoder(); // Later independent rendering must not join this readback's wait.
  LatteTextureReadback_UpdateFinishedTransfers(true);
  check(0,0);check(1,0x35);check(2,0xe2);
  require(r.submissions==1&&published==3&&r.IsCommandBufferActive(),"accurate completion uses submitted batch without committing unrelated work");
  r.CommitCommandBuffer();r.current->waitUntilCompleted();own.metal->release();enemy.metal->release();
 } else if(strcmp(mode,"force-small")==0||strcmp(mode,"force-large")==0) {
  if(strcmp(mode,"force-large")==0){t.metal->release();t=texture(d,0,128,16,0x35);}
  LatteTextureReadbackInfoMtl info(&r,&v,0);info.StartTransfer();
  require(r.submissions==0,"StartTransfer must not submit each query on iOS");
  info.ForceFinish();require(info.IsFinished(),"FAIL: ForceFinish skipped completion of small query");
  require(r.submissions==1,"forced read submits its unsubmitted command buffer");
  uint8 expected=strcmp(mode,"force-large")==0?0x35:0;
  require(info.GetData()[0]==expected,"blocking read sees completed GPU bytes");
 } else if(strcmp(mode,"already-submitted")==0) {
  LatteTextureReadbackInfoMtl info(&r,&v,0);info.StartTransfer();r.CommitCommandBuffer();
  auto* unrelated=r.GetBlitCommandEncoder();(void)unrelated;
  info.ForceFinish();require(info.IsFinished(),"complete retained submitted buffer");
  require(r.submissions==1,"do not commit an unrelated current buffer");
  r.CommitCommandBuffer();r.current->waitUntilCompleted();
 } else if(strcmp(mode,"idle-copies")==0) {
  r.accurateBarriers=true;
  LatteTextureReadbackInfoMtl info(&r,&v,0);info.StartTransfer();
  require(r.submissions==0,"readbacks batch before idle");
  r.NotifyLatteCommandProcessorIdle();
  require(r.submissions==1,"FAIL: encoded readback stayed unsubmitted while command processor was idle");
  for(int i=0;i<1000;++i)r.NotifyLatteCommandProcessorIdle();
  require(r.submissions==1,"idle polls must not resubmit a completed batch");
  info.ForceFinish();require(info.GetData()[0]==0,"idle submission preserves accurate GPU bytes");
  r.GetBlitCommandEncoder();r.NotifyLatteCommandProcessorIdle();
  require(r.submissions==1,"old readback flag must not submit unrelated subsequent work");
  r.CommitCommandBuffer();r.current->waitUntilCompleted();
 } else if(strcmp(mode,"idle-queued")==0||strcmp(mode,"idle-large")==0) {
  r.accurateBarriers=true;
  bool large=strcmp(mode,"idle-large")==0;
  if(large){t.metal->release();t=texture(d,0,512,512,0);}
  auto own=texture(d,1,large?256:32,large?256:16,0x35),enemy=texture(d,2,64,8,0xe2);
  LatteTextureView ownView{&own},enemyView{&enemy};r.activeRender=true;
  LatteTextureReadback_Initate(&v);LatteTextureReadback_Initate(&ownView);LatteTextureReadback_Initate(&enemyView);
  r.NotifyLatteCommandProcessorIdle();
  require(r.submissions==0&&r.activeRender,"recent writes preserve five-draw eligibility rule");
  LatteGPUState.drawCallCounter=5;
  require(!LatteTextureReadback_Update(false),"active pass delays scheduled copies");
  r.NotifyLatteCommandProcessorIdle();
  require(r.submissions==1,"FAIL: eligible readback batch must start at command processor idle boundary");
  require(published==0,"idle submission does not force or publish GPU completion");
  for(int i=0;i<1000;++i)r.NotifyLatteCommandProcessorIdle();
  require(r.submissions==1&&r.m_idleReadbackSubmissions==1,"one submission for the complete idle batch");
  r.current->waitUntilCompleted();LatteTextureReadback_UpdateFinishedTransfers(false);
  check(0,0);check(1,0x35);check(2,0xe2);require(published==3,"idle publishes completed current queries");
  own.metal->release();enemy.metal->release();
 } else if(strcmp(mode,"idle-relaxed")==0) {
  // The relaxed iOS path batches eligible copies with the frame. It must not
  // introduce a second submit at each transient command-stream idle boundary.
  auto own=texture(d,1,32,16,0x35),enemy=texture(d,2,64,8,0xe2);
  LatteTextureView ownView{&own},enemyView{&enemy};r.activeRender=true;
  LatteTextureReadback_Initate(&v);LatteTextureReadback_Initate(&ownView);LatteTextureReadback_Initate(&enemyView);
  r.NotifyLatteCommandProcessorIdle();
  require(r.activeRender&&r.submissions==0,"relaxed idle preserves active writes until eligible");
  LatteGPUState.drawCallCounter=5;r.NotifyLatteCommandProcessorIdle();
  require(sTextureScheduledReadbacks.empty()&&sTextureActiveReadbackQueue.size()==3,
   "relaxed idle starts the eligible batch");
  for(int i=0;i<1000;++i)r.NotifyLatteCommandProcessorIdle();
  require(r.submissions==0&&r.m_idleReadbackSubmissions==0&&published==0,
   "relaxed idle must not force submission or publish incomplete data");
  // A real CPU dependency must still submit and drain that same retained
  // command buffer, even though the optional idle flush was suppressed.
  LatteTextureReadback_UpdateFinishedTransfers(true);
  check(0,0);check(1,0x35);check(2,0xe2);
  require(r.submissions==1&&published==3&&sTextureActiveReadbackQueue.empty(),
   "explicit completion submits one relaxed batch and publishes accurate bytes");
  own.metal->release();enemy.metal->release();
 } else if(strcmp(mode,"idle-empty")==0) {
  r.activeRender=true;
  for(int i=0;i<1000;++i)r.NotifyLatteCommandProcessorIdle();
  require(r.submissions==0&&r.activeRender,"idle without readbacks must not split passes or create buffers");
 } else if(strcmp(mode,"async")==0) {
  r.activeRender=true;LatteTextureReadback_Initate(&v);LatteGPUState.drawCallCounter=10;
  for(int i=0;i<1000;++i){LatteTextureReadback_Update(false);LatteTextureReadback_UpdateFinishedTransfers(false);}
  require(r.submissions==0&&published==0,"ordinary frames do not force sync or submit");
  r.activeRender=false;require(LatteTextureReadback_Update(false),"start after pass boundary");
  require(r.submissions==0,"query copies remain batched before normal commit");
  LatteTextureReadback_UpdateFinishedTransfers(false);require(published==0,"do not publish unfinished GPU data");
  r.CommitCommandBuffer();r.current->waitUntilCompleted();LatteTextureReadback_UpdateFinishedTransfers(false);
  check(0,0);require(published==1,"publish naturally completed query without forced submission");
 }
 t.metal->release();printf("PASS: %s; submissions=%u, published=%u\n",mode,r.submissions,published);
 d->release();pool->release();
}
'''

with tempfile.TemporaryDirectory(prefix='cemu-readback-sync-') as temp:
    p = Path(temp)
    (p / 'test.mm').write_text(fixture)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O3', '-DNDEBUG',
                    '-I'+str(repo/'src'), '-I'+str(repo/'dependencies/metal-cpp'),
                    str(p/'test.mm'), '-framework', 'Metal', '-framework', 'Foundation',
                    '-o', str(p/'test')], check=True)
    env = dict(os.environ, MTL_DEBUG_LAYER='1')
    failures = []
    for mode in ['pm4-buffer', 'pm4-ring', 'pm4-buffer-relaxed', 'pm4-ring-relaxed', 'early-small', 'early-large', 'force-small', 'force-large', 'already-submitted', 'async', 'idle-copies', 'idle-queued', 'idle-large', 'idle-relaxed', 'idle-empty']:
        result = subprocess.run([str(p/'test'), mode], env=env, timeout=15)
        if result.returncode:
            failures.append((mode, result.returncode))
    if failures:
        raise SystemExit(f'FAIL: {failures}')
