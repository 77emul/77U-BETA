#!/usr/bin/env python3
"""Real GPU upload burst through production BC decoder, ring and batch boundary.
Reports Mach footprint separately from Metal resources. No target FPS claim.
"""
from pathlib import Path
import argparse,json,os,subprocess,tempfile
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--baseline-dir',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
metal='src/Cafe/HW/Latte/Renderer/Metal/'
def fn(source,sig):
 start=source.index(sig);opening=source.index('{',start);end=opening+1;depth=1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
source=(root/'platform/ipados/Tests/MetalDirectTextureUploadTests.py').read_text()
fixture=source.split("fixture = r'''",1)[1].split("'''.replace(",1)[0]
fixture=fixture[:fixture.index('int main(int argc')]
fixture=fixture.replace('#undef TARGET_OS_IPHONE','#include <mach/mach.h>\n#include <deque>\n#include <functional>\nstatic MetalMemoryPolicy testPolicy;\n#define GetMetalMemoryPolicy() testPolicy\n#undef TARGET_OS_IPHONE',1)
start=fixture.index('struct MetalRenderer {');end=fixture.index('RING_CLASS',start)
fixture=fixture[:start]+r'''
struct MetalRenderer {
 MTL::Device* device;MTL::CommandQueue* queue;MTL::CommandBuffer* cb=nullptr;MTL::BlitCommandEncoder* encoder=nullptr;
 struct{bool m_supportsBCTextureCompression=false;}m_pixelFormatSupport;
 struct{size_t m_textureUploadBytes=0;}m_currentCommandBuffer;
 std::deque<MTL::CommandBuffer*> m_executingCommandBuffers;
 std::function<void(MTL::CommandBuffer*)> cleanup;
 unsigned commits=0;double gpuMs=0;
 explicit MetalRenderer(MTL::Device* d):device(d),queue(d->newCommandQueue()){}
 ~MetalRenderer(){require(!cb&&m_executingCommandBuffers.empty(),"all GPU work drained before allocator destruction");queue->release();}
 MTL::Device* GetDevice(){return device;}
 MTL::CommandBuffer* GetCommandBuffer(){if(!cb)cb=queue->commandBuffer()->retain();return cb;}
 void* GetCommandEncoder(){return encoder;}
 MetalEncoderType GetEncoderType(){return MetalEncoderType::Blit;}
 MTL::BlitCommandEncoder* GetBlit(){if(!encoder)encoder=GetCommandBuffer()->blitCommandEncoder();return encoder;}
 void CommitCommandBuffer(){if(!cb)return;if(encoder){encoder->endEncoding();encoder=nullptr;}
  cb->commit();m_executingCommandBuffers.push_back(cb);cb=nullptr;m_currentCommandBuffer.m_textureUploadBytes=0;++commits;}
 void ProcessFinishedCommandBuffers(){while(!m_executingCommandBuffers.empty()&&CommandBufferCompleted(m_executingCommandBuffers.front())){
  auto* done=m_executingCommandBuffers.front();require(done->status()==MTL::CommandBufferStatusCompleted,"GPU command buffer");
  gpuMs+=(done->GPUEndTime()-done->GPUStartTime())*1000;if(cleanup)cleanup(done);done->release();m_executingCommandBuffers.pop_front();}}
 void Flush(bool wait){CommitCommandBuffer();if(wait&&!m_executingCommandBuffers.empty())m_executingCommandBuffers.back()->waitUntilCompleted();ProcessFinishedCommandBuffers();}
 void FinishTextureUpload(size_t);
};
'''+fixture[end:]
manager=((a.baseline_dir or root)/metal/'MetalMemoryManager.cpp').read_text()
renderer=((a.baseline_dir or root)/metal/'MetalRenderer.cpp').read_text()
if 'bool decodedMatchesUpload' not in manager:fixture=fixture.replace('AcquireTextureUploadBuffer(size_t,bool=false)','AcquireTextureUploadBuffer(size_t)')
load=fn(renderer,'void MetalRenderer::texture_loadSlice(')
fixture=fixture.replace('UPLOAD',load[load.index('// Allocate a temporary buffer'):load.index('if (mipIndex >= 0')].replace('memcpy(','countedCopy('))
for token,sig in [('ACQUIRE','void* MetalMemoryManager::AcquireTextureUploadBuffer('),('RELEASE','void MetalMemoryManager::ReleaseTextureUploadBuffer('),('RESOLVE','bool MetalMemoryManager::ResolveTextureUploadBuffer(')]:fixture=fixture.replace(token,fn(manager,sig))
rh=(root/metal/'MetalBufferAllocator.h').read_text();rs=(root/metal/'MetalBufferAllocator.cpp').read_text()
fixture=fixture.replace('RING_CLASS',rh[rh.index('class MetalSynchronizedRingAllocator\n'):rh.index('// heap style allocator')]).replace('RING_DESTRUCTOR',rs[rs.index('MetalSynchronizedRingAllocator::~'):rs.index('MetalBufferChunkedHeap::~')]).replace('RING_METHODS',rs[rs.index('void MetalSynchronizedRingAllocator::addUploadBufferSyncPoint'):rs.index('/* MetalSynchronizedHeapAllocator */')])
fixture=fixture.replace('allocator(&renderer,MTL::ResourceStorageModeShared,1024*1024)','allocator(&renderer,MTL::ResourceStorageModeShared,static_cast<uint32>(GetMetalMemoryPolicy().stagingChunkBytes),GetMetalMemoryPolicy().constrained)')
fixture+=fn(renderer,'void MetalRenderer::FinishTextureUpload(')
fixture+=r'''
uint64_t footprint(){task_vm_info_data_t v{};mach_msg_type_number_t n=TASK_VM_INFO_COUNT;
 require(task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&v),&n)==KERN_SUCCESS,"Mach footprint");return v.phys_footprint;}
int main(int argc,char** argv){
 auto* pool=NS::AutoreleasePool::alloc()->init();auto* d=MTL::CreateSystemDefaultDevice();require(d,"Metal");
 testPolicy=MakeMetalMemoryPolicy(uint64_t(atoi(argv[1]))*1024*1024*1024);
 UploadHarness h(d);h.memory.m_useDirectTextureUploads=false;
 h.renderer.cleanup=[&](auto* cb){h.allocator.CleanupBuffer(cb);};
 constexpr unsigned width=1024,height=1024,inputBytes=width*height/2,outputBytes=width*height*4,count=64;
 auto* desc=MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm,width,height,false);desc->setStorageMode(MTL::StorageModePrivate);
 auto* texture=d->newTexture(desc);require(texture,"texture");
 const auto before=footprint();auto peak=before;uint64_t peakStaging=0;
 const auto start=std::chrono::steady_clock::now();
 for(unsigned iteration=0;iteration<count;++iteration){
  auto* input=static_cast<uint8*>(h.memory.AcquireTextureUploadBuffer(inputBytes));
  memset(input,0,inputBytes);for(unsigned i=0;i<inputBytes;i+=8){input[i+1]=0xf8;input[i+2]=0xe0;input[i+3]=0x07;}
  auto allocation=h.upload(input,inputBytes,width,height,true);
  h.renderer.GetBlit()->copyFromBuffer(allocation.mtlBuffer,allocation.bufferOffset,width*4,0,MTL::Size(width,height,1),texture,0,0,MTL::Origin(0,0,0));
  h.memory.ReleaseTextureUploadBuffer(input);h.renderer.FinishTextureUpload(outputBytes);
  peak=std::max(peak,footprint());peakStaging=std::max(peakStaging,RuntimePerformance::stagingBufferBytes.load());
 }
 h.renderer.Flush(true);const auto finish=std::chrono::steady_clock::now();
 const auto completed=footprint();const auto metalBytes=d->currentAllocatedSize();
 auto* read=d->newBuffer(outputBytes,MTL::ResourceStorageModeShared);auto* cb=h.renderer.queue->commandBuffer();auto* e=cb->blitCommandEncoder();
 e->copyFromTexture(texture,0,0,MTL::Origin(0,0,0),MTL::Size(width,height,1),read,0,width*4,0);e->endEncoding();cb->commit();cb->waitUntilCompleted();
 require(cb->status()==MTL::CommandBufferStatusCompleted,"readback");auto* bytes=static_cast<uint8*>(read->contents());
 for(unsigned i=0;i<outputBytes;++i)require(bytes[i]==(i%4==0||i%4==3?255:0),"BC upload bytes");
 printf("{\"ramGiB\":%d,\"beforeBytes\":%llu,\"peakBytes\":%llu,\"completedBytes\":%llu,\"peakStagingBytes\":%llu,\"metalBytes\":%zu,\"commits\":%u,\"cpuAndGPUms\":%.3f,\"GPUms\":%.3f}\n",atoi(argv[1]),(unsigned long long)before,(unsigned long long)peak,(unsigned long long)completed,(unsigned long long)peakStaging,metalBytes,h.renderer.commits,std::chrono::duration<double,std::milli>(finish-start).count(),h.renderer.gpuMs);
 read->release();texture->release();d->release();pool->release();
}
'''
with tempfile.TemporaryDirectory(prefix='77u-bc-burst-') as tmp:
 tmp=Path(tmp);(tmp/'test.mm').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG','-I'+str(root/'src'),'-I'+str(root/'dependencies/metal-cpp'),str(tmp/'test.mm'),str(root/metal/'MetalBCFallback.cpp'),'-framework','Metal','-framework','Foundation','-o',str(tmp/'test')],check=True)
 results={}
 for ram in ('4','6','12'):
  results[ram]=[json.loads(subprocess.check_output([str(tmp/'test'),ram],env=dict(os.environ,MTL_DEBUG_LAYER='1'),text=True)) for _ in range(3)]
 print(json.dumps(results,indent=2))
 if a.output:a.output.write_text(json.dumps(results,indent=2))
