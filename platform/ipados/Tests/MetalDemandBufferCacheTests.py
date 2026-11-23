#!/usr/bin/env python3
"""Production demand-backed cache growth with real queued Metal writes.
Checks byte preservation, bounds/failure rollback, native control and Mach
footprint on this Mac. This is not a target-iPhone game measurement.
"""
from pathlib import Path
import argparse,subprocess,tempfile,json,os
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--mac',action='store_true');a=p.parse_args()
s=(root/'src/Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.cpp').read_text()
def function(sig):
 start=s.index(sig);begin=s.index('{',start);end=begin+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
init=function('void MetalMemoryManager::InitBufferCache(')
init=init[init.index('    m_bufferCacheMaximum = size;'):init.rindex('#ifdef CEMU_DEBUG_ASSERT')]
grow=function('bool MetalMemoryManager::EnsureBufferCacheCapacity(')
fixture=r'''
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <mach/mach.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cassert>
#include "Common/HostUploadMemory.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryPolicy.h"
#undef TARGET_OS_IPHONE
#undef TARGET_OS_IOS
#define TARGET_OS_IPHONE IOS_TEST
#define TARGET_OS_IOS IOS_TEST
#define BOOST_OS_MACOS MAC_TEST
static MetalMemoryPolicy policy;
#define GetMetalMemoryPolicy() policy
static constexpr size_t MiB=1024*1024;
static size_t availableBytes=8ull*1024*1024*1024;
static uint64_t physicalBytes=0;
int sysctlbyname(const char*,void* value,size_t* size,void*,size_t){assert(*size==sizeof(physicalBytes));memcpy(value,&physicalBytes,*size);return 0;}
size_t os_proc_available_memory(){return availableBytes;}
uint64_t footprint(){task_vm_info_data_t v{};mach_msg_type_number_t n=TASK_VM_INFO_COUNT;assert(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&v,&n)==KERN_SUCCESS);return v.phys_footprint;}
enum class MetalBufferCacheMode {DevicePrivate,DeviceShared,Host};
struct Device {
 MTL::Device* real;bool fail=false;
 MTL::Buffer* newBuffer(size_t n,MTL::ResourceOptions o){if(fail){fail=false;return nullptr;}return real->newBuffer(n,o);}
};
struct MetalRenderer {
 Device device;MTL::CommandQueue* queue;MTL::CommandBuffer* pending=nullptr;unsigned waits=0;
 bool loseHeadroomWhileWaiting=false;
 explicit MetalRenderer(MTL::Device* d):device{d},queue(d->newCommandQueue()){}
 ~MetalRenderer(){Flush(true);queue->release();}
 Device* GetDevice(){return &device;}
 MTL::CommandBuffer* GetCommandBuffer(){if(!pending)pending=queue->commandBuffer()->retain();return pending;}
 void Flush(bool wait){if(pending){pending->commit();if(wait){pending->waitUntilCompleted();assert(pending->status()==MTL::CommandBufferStatusCompleted);}pending->release();pending=nullptr;}++waits;}
 void ProcessFinishedCommandBuffers() {if(loseHeadroomWhileWaiting)availableBytes=0;}
 void gpuWrite(MTL::Buffer* b,size_t start,size_t size,unsigned char value){auto* e=GetCommandBuffer()->blitCommandEncoder();e->fillBuffer(b,NS::Range(start,size),value);e->endEncoding();}
 void verify(MTL::Buffer* b,size_t start,size_t size,unsigned char value){auto* out=device.real->newBuffer(size,MTL::ResourceStorageModeShared);auto*e=GetCommandBuffer()->blitCommandEncoder();e->copyFromBuffer(b,start,out,0,size);e->endEncoding();Flush(true);auto*bytes=(unsigned char*)out->contents();for(size_t i=0;i<size;++i)assert(bytes[i]==value);out->release();}
};
struct MetalMemoryManager {
 MetalRenderer* m_mtlr;MTL::Buffer* m_bufferCache=nullptr;size_t m_bufferCacheMaximum=0;bool m_demandBufferCache=false,m_gpuOrderedBufferUpdates=true;
 MetalBufferCacheMode m_metalBufferCacheMode=MetalBufferCacheMode::DeviceShared;
 explicit MetalMemoryManager(MetalRenderer* r):m_mtlr(r){}
 ~MetalMemoryManager(){m_mtlr->Flush(true);if(m_bufferCache)m_bufferCache->release();}
 bool UseHostMemoryForCache()const{return m_metalBufferCacheMode==MetalBufferCacheMode::Host;}
 void InitBufferCache(size_t size){INIT}
 bool EnsureBufferCacheCapacity(size_t requiredEnd);
};
GROW
int main(int argc,char**argv){auto*pool=NS::AutoreleasePool::alloc()->init();auto*d=MTL::CreateSystemDefaultDevice();assert(d);
 bool native=strcmp(argv[1],"native")==0;policy=MakeMetalMemoryPolicy((native?12ull:4ull)*1024*1024*1024);
 physicalBytes=(native?12ull:4ull)*1024*1024*1024;
 setenv("CEMU_MAC_DEMAND_CACHE","1",1);
 MetalRenderer r(d);MetalMemoryManager m(&r);const uint64_t before=footprint();m.InitBufferCache(384*MiB);r.gpuWrite(m.m_bufferCache,0,16*MiB,0);r.Flush(true);const uint64_t allocated=footprint();const uint64_t initialMetal=d->currentAllocatedSize();const size_t initial=m.m_bufferCache->length();
 assert(initial==(native?384:32)*MiB);assert(m.m_bufferCacheMaximum==384*MiB);
 r.gpuWrite(m.m_bufferCache,0,16*MiB,77); // Still queued when growth is requested.
 auto* first=m.m_bufferCache;
 if(!native){availableBytes=64*MiB-1;assert(!m.EnsureBufferCacheCapacity(33*MiB));assert(m.m_bufferCache==first);availableBytes=8ull*1024*1024*1024;r.device.fail=true;assert(!m.EnsureBufferCacheCapacity(33*MiB));assert(m.m_bufferCache==first);r.verify(m.m_bufferCache,0,16*MiB,77);}
#if TARGET_OS_IOS
 if(!native){
  // The first check passes, then headroom disappears during the GPU wait.
  r.loseHeadroomWhileWaiting=true;
  r.device.fail=true;
  assert(!m.EnsureBufferCacheCapacity(33*MiB));
  assert(m.m_bufferCache==first && r.device.fail);
  r.loseHeadroomWhileWaiting=false;
  r.device.fail=false;
  availableBytes=8ull*1024*1024*1024;
 }
#endif
 assert(m.EnsureBufferCacheCapacity(33*MiB));r.verify(m.m_bufferCache,0,16*MiB,77);
 if(native)assert(m.m_bufferCache==first);else assert(m.m_bufferCache->length()==64*MiB);
 r.gpuWrite(m.m_bufferCache,32*MiB,32*MiB,52);r.Flush(true);
 assert(m.EnsureBufferCacheCapacity(61*MiB));
 const uint64_t workingFootprint=footprint();const size_t workingBacking=m.m_bufferCache->length();
 // Reproduce the observed story-mode high-water end (90.44 MiB), with
 // queued data and a failed second growth before the successful retry.
 const size_t storyRequiredEnd=94835712;
 auto* beforeStory=m.m_bufferCache;
 r.gpuWrite(m.m_bufferCache,63*MiB,MiB,101);
 if(!native){availableBytes=128*MiB-1;assert(!m.EnsureBufferCacheCapacity(storyRequiredEnd));assert(m.m_bufferCache==beforeStory);availableBytes=8ull*1024*1024*1024;r.device.fail=true;assert(!m.EnsureBufferCacheCapacity(storyRequiredEnd));assert(m.m_bufferCache==beforeStory);}
 assert(m.EnsureBufferCacheCapacity(storyRequiredEnd));
 assert(m.m_bufferCache->length()==(native?384:128)*MiB);
 r.verify(m.m_bufferCache,63*MiB,MiB,101);r.verify(m.m_bufferCache,0,MiB,77);
 const size_t storyBacking=m.m_bufferCache->length();
 const uint64_t storyFootprint=footprint();
 for(size_t boundary:{129*MiB,257*MiB,384*MiB}){
  size_t previous=m.m_bufferCache->length();r.gpuWrite(m.m_bufferCache,previous-MiB,MiB,93);
  assert(m.EnsureBufferCacheCapacity(boundary));r.verify(m.m_bufferCache,previous-MiB,MiB,93);r.verify(m.m_bufferCache,0,MiB,77);
 }
 assert(m.m_bufferCache->length()==384*MiB);auto* final=m.m_bufferCache;
 assert(!m.EnsureBufferCacheCapacity(384*MiB+1));assert(m.m_bufferCache==final);
 assert(HostUploadMemory::gpuCacheGrowthCount==(native?0:3));
 // Aliases must not double-count root allocation bytes; no Metal ownership changes.
 printf("{\"storyBackingBytes\":%zu,\"storyFootprintBytes\":%llu,\"workingFootprintBytes\":%llu,\"workingBackingBytes\":%zu,\"mode\":\"%s\",\"beforeFootprintBytes\":%llu,\"initialFootprintBytes\":%llu,\"initialBackingBytes\":%zu,\"initialMetalBytes\":%llu,\"finalBackingBytes\":%zu,\"growths\":%llu,\"allQueuedBytesPreserved\":true,\"failedGrowthRollback\":true}\n",storyBacking,(unsigned long long)storyFootprint,(unsigned long long)workingFootprint,workingBacking,argv[1],(unsigned long long)before,(unsigned long long)allocated,initial,(unsigned long long)initialMetal,m.m_bufferCache->length(),(unsigned long long)HostUploadMemory::gpuCacheGrowthCount.load());
 // Keep the autorelease pool alive through manager/queue destruction.
 (void)pool;
}
'''.replace('INIT',init).replace('GROW',grow).replace('IOS_TEST','0' if a.mac else '1').replace('MAC_TEST','1' if a.mac else '0')
if a.mac:
 fixture=fixture.replace('availableBytes=128*MiB-1;assert(!m.EnsureBufferCacheCapacity(storyRequiredEnd));assert(m.m_bufferCache==beforeStory);availableBytes=8ull*1024*1024*1024;','')
 fixture=fixture.replace('availableBytes=64*MiB-1;assert(!m.EnsureBufferCacheCapacity(33*MiB));assert(m.m_bufferCache==first);availableBytes=8ull*1024*1024*1024;','')
with tempfile.TemporaryDirectory(prefix='77u-demand-cache-') as t:
 t=Path(t);(t/'test.cpp').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-O2','-I'+str(root/'src'),'-I'+str(root/'dependencies/metal-cpp'),str(t/'test.cpp'),'-framework','Metal','-framework','Foundation','-o',str(t/'test')],check=True)
 results={mode:[json.loads(subprocess.check_output([str(t/'test'),mode],env=dict(os.environ,MTL_DEBUG_LAYER='1'),text=True)) for _ in range(3)] for mode in ['constrained','native']}
 print(json.dumps(results,indent=2))
 if a.output:a.output.write_text(json.dumps(results,indent=2))
