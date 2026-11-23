#!/usr/bin/env python3
"""Exercise the actual FG handoff when submission retires the final CB owner."""
from pathlib import Path
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[3]
source=(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp').read_text()
start=source.index('void MetalRenderer::SwapBuffers(bool')
end=source.index('    static auto s_lastFrameTime',start)
handoff=source[start:end]+'\n#endif\n}\n'
fixture=r'''
#undef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 1
#include <atomic>
#include <memory>
#include <stdexcept>
#include <cassert>
#include <cstdio>
using uint64=uint64_t;
#define cemuLog_log(...) ((void)0)
namespace MTL {
struct CommandBuffer {
 static inline unsigned live=0;
 static inline unsigned waits=0;
 bool submitted=false;
 unsigned refs=1;
 CommandBuffer(){++live;}
 ~CommandBuffer(){--live;}
 CommandBuffer* retain(){++refs;return this;}
 void release(){if(!--refs)delete this;}
 void waitUntilCompleted(){assert(submitted);++waits;}
 double GPUStartTime(){return 0;} double GPUEndTime(){return 0;}
};
}
struct FG {
 bool fail=false;
 MTL::CommandBuffer* received=nullptr;
 ~FG(){if(received)received->release();}
 void Submit(MTL::CommandBuffer* source,int,bool){
  assert(source && source->refs>0);
  if(fail)throw std::runtime_error("injected queue failure");
  received=source->retain();
 }
 struct Stats{double sourceFPS=0,displayedFPS=0,gpuMs=0;uint64_t generated=0,original=0,repeated=0,dropped=0;};
 Stats GetStatistics(){return {};}
};
struct{unsigned drawCallCounter=0;}LatteGPUState;
static bool CemuBridge_GetRunningGameFrameGenerationFast(){return false;}
static bool CemuBridge_IsDiagnosticLoggingEnabled(){return false;}
struct MetalRenderer {
 int configured=60;
 double m_lastCompletedGpuMs=0;
 struct{bool Recording()const{return false;}void EndFrame(MTL::CommandBuffer*,unsigned){}void BeginFrame(bool,unsigned){}}m_passProfiler;
 unsigned commandGets=0,commits=0;
 bool committed=false,m_frameGenRendered=true;
 void* m_frameGenTarget=nullptr;
 struct{void Fail(){}}m_frameGenMode;
 std::unique_ptr<FG> m_frameGeneration=std::make_unique<FG>();
 MTL::CommandBuffer* current=new MTL::CommandBuffer;
 ~MetalRenderer(){if(current&&!committed)current->release();}
 int ActiveFrameGenerationTarget(){return configured;}
 void SwapBuffer(bool){}
 MTL::CommandBuffer* GetCommandBuffer(){++commandGets;if(committed||!current){current=new MTL::CommandBuffer;committed=false;}return current;}
 void CommitCommandBuffer(bool){++commits;if(current&&!committed){current->submitted=true;current->release();committed=true;}}
 void SwapBuffers(bool,bool);
};
std::atomic<uint64_t> g_metalSwapCount{0};
'''
tests=r'''
int main(){
 {MetalRenderer r;r.SwapBuffers(true,false);assert(r.m_frameGeneration->received);assert(MTL::CommandBuffer::live==1);}
 assert(MTL::CommandBuffer::live==0);
 // Simulate a render prefix which completed and retired before SwapBuffers.
 {MetalRenderer r;r.current->release();r.committed=true;r.SwapBuffers(true,false);assert(r.commandGets==1&&r.m_frameGeneration->received);}
 assert(MTL::CommandBuffer::live==0);
 {MetalRenderer r;r.configured=0;r.m_frameGeneration.reset();r.SwapBuffers(true,false);assert(r.commandGets==0);}
 assert(MTL::CommandBuffer::live==0);
 {MetalRenderer r;r.m_frameGeneration->fail=true;r.SwapBuffers(true,false);assert(!r.m_frameGeneration);}
 assert(MTL::CommandBuffer::live==0);
 // A pad-only flip must not destroy a live TV frame-generation controller.
 {MetalRenderer r;r.SwapBuffers(false,true);assert(r.m_frameGeneration&&r.commandGets==0);}
 assert(MTL::CommandBuffer::live==0);
 assert(MTL::CommandBuffer::waits==0);
 puts("PASS: actual SwapBuffers handoff, native/off and pad-only paths");
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-fg-source-') as temp:
    p=Path(temp);(p/'test.cpp').write_text(fixture+handoff+tests)
    subprocess.run(['xcrun','clang++','-std=c++20','-fsanitize=address,undefined','-g','-I'+str(repo/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
