#!/usr/bin/env python3
"""Measure production upload storage before/after on this Mac's real Metal GPU.
Mach phys_footprint is measured; Metal bytes are reported separately, never
added. These workloads are not full games or target-iPhone measurements.
"""
from pathlib import Path
import argparse,json,os,subprocess,tempfile
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline-dir',type=Path)
p.add_argument('--output',type=Path)
a=p.parse_args()
base=a.baseline_dir or root
manager=(base/'src/Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.cpp').read_text()
renderer=(base/'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp').read_text()
def function(source,sig):
 start=source.index(sig);opening=source.index('{',start);end=opening+1;depth=1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
rh=(root/'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.h').read_text()
rs=(root/'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.cpp').read_text()
source=(root/'platform/ipados/Tests/MetalDirectTextureUploadTests.py').read_text()
fixture=source.split("fixture = r'''",1)[1].split("'''.replace(",1)[0]
fixture=fixture[:fixture.index('int main(int argc')]
fixture=fixture.replace('#undef TARGET_OS_IPHONE','#include <mach/mach.h>\nstatic MetalMemoryPolicy testPolicy;\n#define GetMetalMemoryPolicy() testPolicy\n#undef TARGET_OS_IPHONE',1)
new_api='bool decodedMatchesUpload' in manager
if not new_api:fixture=fixture.replace('AcquireTextureUploadBuffer(size_t,bool=false)','AcquireTextureUploadBuffer(size_t)')
load=function(renderer,'void MetalRenderer::texture_loadSlice(')
fixture=fixture.replace('UPLOAD',load[load.index('// Allocate a temporary buffer'):load.index('if (mipIndex >= 0')].replace('memcpy(','countedCopy('))
for token,sig in [('ACQUIRE','void* MetalMemoryManager::AcquireTextureUploadBuffer('),('RELEASE','void MetalMemoryManager::ReleaseTextureUploadBuffer('),('RESOLVE','bool MetalMemoryManager::ResolveTextureUploadBuffer(')]:fixture=fixture.replace(token,function(manager,sig))
fixture=fixture.replace('RING_CLASS',rh[rh.index('class MetalSynchronizedRingAllocator\n'):rh.index('// heap style allocator')]).replace('RING_DESTRUCTOR',rs[rs.index('MetalSynchronizedRingAllocator::~'):rs.index('MetalBufferChunkedHeap::~')]).replace('RING_METHODS',rs[rs.index('void MetalSynchronizedRingAllocator::addUploadBufferSyncPoint'):rs.index('/* MetalSynchronizedHeapAllocator */')])
fixture+=r'''
uint64_t footprint(){task_vm_info_data_t v{};mach_msg_type_number_t n=TASK_VM_INFO_COUNT;
 require(task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&v),&n)==KERN_SUCCESS,"Mach footprint");return v.phys_footprint;}
int main(int argc,char** argv){
 auto* pool=NS::AutoreleasePool::alloc()->init();auto* d=MTL::CreateSystemDefaultDevice();require(d,"Metal device");
 bool native=strcmp(argv[1],"native")==0,bc=strcmp(argv[1],"bc")==0;
 testPolicy=MakeMetalMemoryPolicy(uint64_t(native?12:4)*1024*1024*1024);
 UploadHarness h(d);h.memory.m_useDirectTextureUploads=native;
 constexpr unsigned width=4096,height=2048,outputBytes=width*height*4;
 constexpr unsigned inputBC=width*height/2;
 const unsigned inputBytes=bc?inputBC:outputBytes;
 const auto before=footprint();const auto beforeMetal=d->currentAllocatedSize();
 const auto start=std::chrono::steady_clock::now();
 auto* input=static_cast<uint8*>(ACQUIRE_CALL);
 memset(input,bc?0:0x4d,inputBytes);
 if(bc)for(unsigned i=0;i<inputBytes;i+=8){input[i]=0x00;input[i+1]=0xf8;input[i+2]=0xe0;input[i+3]=0x07;}
 auto allocation=h.upload(input,inputBytes,width,height,bc);
 auto* desc=MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm,width,height,false);
 desc->setStorageMode(MTL::StorageModePrivate);desc->setUsage(MTL::TextureUsageShaderRead);
 auto* texture=d->newTexture(desc);require(texture,"private texture");
 auto* cb=h.renderer.GetCommandBuffer();auto* encoder=cb->blitCommandEncoder();
 encoder->copyFromBuffer(allocation.mtlBuffer,allocation.bufferOffset,width*4,0,MTL::Size(width,height,1),texture,0,0,MTL::Origin(0,0,0));
 encoder->endEncoding();
 const auto peak=footprint();const auto scratchBeforeRelease=h.memory.m_textureUploadBuffer.capacity();
 h.memory.ReleaseTextureUploadBuffer(input);
 const auto released=footprint();const auto scratchAfterRelease=h.memory.m_textureUploadBuffer.capacity();
 cb->commit();cb->waitUntilCompleted();require(cb->status()==MTL::CommandBufferStatusCompleted,"GPU upload completed");
 const auto end=std::chrono::steady_clock::now();const auto completed=footprint();const auto metal=d->currentAllocatedSize();
 // Readback allocated only AFTER memory measurements. Verify every channel;
 // releasing CPU scratch must not change the encoded GPU copy.
 auto* readback=d->newBuffer(outputBytes,MTL::ResourceStorageModeShared);require(readback,"readback");
 auto* verify=h.renderer.queue->commandBuffer();auto* e=verify->blitCommandEncoder();
 e->copyFromTexture(texture,0,0,MTL::Origin(0,0,0),MTL::Size(width,height,1),readback,0,width*4,0);e->endEncoding();
 verify->commit();verify->waitUntilCompleted();require(verify->status()==MTL::CommandBufferStatusCompleted,"readback completed");
 auto* pixels=static_cast<uint8*>(readback->contents());
 for(unsigned i=0;i<outputBytes;++i)require(pixels[i]==(bc?(i%4==0||i%4==3?255:0):0x4d),"GPU pixels changed");
 printf("{\"mode\":\"%s\",\"beforeBytes\":%llu,\"preparedBytes\":%llu,\"releasedBytes\":%llu,\"completedBytes\":%llu,\"scratchBeforeRelease\":%zu,\"scratchAfterRelease\":%zu,\"metalBeforeBytes\":%zu,\"metalAfterBytes\":%zu,\"cpuCopyBytes\":%zu,\"prepareAndGPUms\":%.3f,\"gpuMs\":%.3f}\n",argv[1],(unsigned long long)before,(unsigned long long)peak,(unsigned long long)released,(unsigned long long)completed,scratchBeforeRelease,scratchAfterRelease,beforeMetal,metal,cpuCopyBytes,std::chrono::duration<double,std::milli>(end-start).count(),(cb->GPUEndTime()-cb->GPUStartTime())*1000);
 readback->release();texture->release();d->release();pool->release();
}
'''
fixture=fixture.replace('ACQUIRE_CALL','h.memory.AcquireTextureUploadBuffer(inputBytes,!bc)' if new_api else 'h.memory.AcquireTextureUploadBuffer(inputBytes)')
with tempfile.TemporaryDirectory(prefix='77u-upload-footprint-') as tmp:
 tmp=Path(tmp);(tmp/'test.mm').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG','-I'+str(root/'src'),'-I'+str(root/'dependencies/metal-cpp'),str(tmp/'test.mm'),str(root/'src/Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.cpp'),'-framework','Metal','-framework','Foundation','-o',str(tmp/'test')],check=True)
 results={}
 for mode in ('legacy-uncompressed','bc','native'):
  rows=[]
  for _ in range(3):
   output=subprocess.check_output([str(tmp/'test'),mode],env=dict(os.environ,MTL_DEBUG_LAYER='1'),text=True)
   rows.append(json.loads(output))
  results[mode]=rows
 print(json.dumps(results,indent=2))
 if a.output:a.output.write_text(json.dumps(results,indent=2))
