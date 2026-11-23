#!/usr/bin/env python3
"""Check production texture-upload storage with real in-flight Metal blits.

Catch reusable scratch being overwritten while the GPU still consumes it.
Measure redundant CPU copy bytes separately from host GPU timings.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', help='Read the upload implementation from this Git revision for a before/after regression check')
args = parser.parse_args()


def production_source(relative):
    if args.baseline:
        return subprocess.check_output(['git', 'show', f'{args.baseline}:{relative}'], cwd=repo, text=True)
    return (repo/relative).read_text()


manager = production_source('src/Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.cpp')
renderer = production_source('src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp')
ring_header = (repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.h').read_text()
ring_source = (repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.cpp').read_text()
ring_class = ring_header[ring_header.index('class MetalSynchronizedRingAllocator\n'):ring_header.index('// heap style allocator')]
ring_methods = ring_source[ring_source.index('void MetalSynchronizedRingAllocator::addUploadBufferSyncPoint'):ring_source.index('/* MetalSynchronizedHeapAllocator */')]
ring_destructor = ring_source[ring_source.index('MetalSynchronizedRingAllocator::~'):ring_source.index('MetalBufferChunkedHeap::~')]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


acquire = function(manager, 'void* MetalMemoryManager::AcquireTextureUploadBuffer(')
release = function(manager, 'void MetalMemoryManager::ReleaseTextureUploadBuffer(')
resolve = (function(manager, 'bool MetalMemoryManager::ResolveTextureUploadBuffer(')
           if 'bool MetalMemoryManager::ResolveTextureUploadBuffer(' in manager else
           'bool MetalMemoryManager::ResolveTextureUploadBuffer(const void*,size_t,MetalSynchronizedRingAllocator::AllocatorReservation_t&) const {return false;}')
load = function(renderer, 'void MetalRenderer::texture_loadSlice(')
upload = load[load.index('// Allocate a temporary buffer'):load.index('if (mipIndex >= 0')]

fixture = r'''
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <queue>
#include <vector>
#include "Common/MemoryBudget.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryPolicy.h"
#include "Common/HostUploadMemory.h"
#undef TARGET_OS_IPHONE
#undef TARGET_OS_IOS
#define TARGET_OS_IPHONE 1
#define TARGET_OS_IOS 1
#define BOOST_OS_IOS 1
using uint8=uint8_t;using uint32=uint32_t;
void require(bool value,const char* message){if(!value){puts(message);std::exit(1);}}
#define cemu_assert_debug(x) require(x,#x)
#define cemuLog_logOnce(...) ((void)0)
#define cemuLog_log(...) ((void)0)
size_t os_proc_available_memory(){return size_t(8)*1024*1024*1024;}
size_t cpuCopyBytes=0;
void* countedCopy(void* dst,const void* src,size_t size){cpuCopyBytes+=size;return std::memcpy(dst,src,size);}
bool CommandBufferCompleted(MTL::CommandBuffer* b){return b->status()==MTL::CommandBufferStatusCompleted||b->status()==MTL::CommandBufferStatusError;}
enum class MetalEncoderType {Blit,Render};
MTL::ResourceOptions GetResourceOptions(MTL::ResourceOptions value){return value|MTL::ResourceCPUCacheModeWriteCombined;}
struct MetalRenderer {
 MTL::Device* device;MTL::CommandQueue* queue;MTL::CommandBuffer* cb=nullptr;
 explicit MetalRenderer(MTL::Device* d):device(d),queue(d->newCommandQueue()){}
 ~MetalRenderer(){if(cb)cb->release();queue->release();}
 MTL::Device* GetDevice(){return device;}
 MTL::CommandBuffer* GetCommandBuffer(){if(!cb)cb=queue->commandBuffer()->retain();return cb;}
 void* GetCommandEncoder(){return nullptr;}
 MetalEncoderType GetEncoderType(){return MetalEncoderType::Blit;}
 void ProcessFinishedCommandBuffers(){}
 void Flush(bool){require(false,"fixture must not exhaust staging capacity");}
};
RING_CLASS
RING_DESTRUCTOR
RING_METHODS
struct MetalMemoryManager {
 MetalRenderer* m_mtlr;std::vector<uint8> m_textureUploadBuffer;
 MetalSynchronizedRingAllocator& m_stagingAllocator;
 MetalSynchronizedRingAllocator::AllocatorReservation_t m_textureUploadReservation{};
 bool m_useDirectTextureUploads=true;
 MetalMemoryManager(MetalRenderer* r,MetalSynchronizedRingAllocator& a):m_mtlr(r),m_stagingAllocator(a){}
 void* AcquireTextureUploadBuffer(size_t,bool=false);void ReleaseTextureUploadBuffer(uint8*);
 bool ResolveTextureUploadBuffer(const void*,size_t,MetalSynchronizedRingAllocator::AllocatorReservation_t&) const;
 MetalSynchronizedRingAllocator& GetStagingAllocator(){return m_stagingAllocator;}
};
struct UploadHarness {
 MetalRenderer renderer;MetalSynchronizedRingAllocator allocator;MetalMemoryManager memory;
 MetalMemoryManager* m_memoryManager=&memory;
 explicit UploadHarness(MTL::Device* d):renderer(d),allocator(&renderer,MTL::ResourceStorageModeShared,1024*1024),memory(&renderer,allocator){}
 auto upload(void* pixelData,size_t compressedImageSize,unsigned width,unsigned height,bool decodeBC=false) {
  BCFormat bcFormat=BCFormat::BC1;
  size_t bytesPerRow=width*4;
  struct {uint32 format=0;} tex;auto* textureMtl=&tex;
  UPLOAD
  return allocation;
 }
};
ACQUIRE
RELEASE
RESOLVE
int main(int argc,char** argv) {
 auto* pool=NS::AutoreleasePool::alloc()->init();auto* d=MTL::CreateSystemDefaultDevice();require(d,"Metal device");
 UploadHarness r(d);bool fallback=argc>1&&strcmp(argv[1],"native")!=0;
 bool directUncompressed=argc>1&&strcmp(argv[1],"legacy-uncompressed")==0;
 r.memory.m_useDirectTextureUploads=!fallback;
 constexpr unsigned side=128,bytes=side*side*4,count=256;
 auto* cb=r.renderer.GetCommandBuffer();auto* enc=cb->blitCommandEncoder();
 std::vector<MTL::Texture*> textures;std::vector<uint8> external(bytes,0x6e);
 auto start=std::chrono::steady_clock::now();
 for(unsigned index=0;index<count;++index) {
  auto* data=(uint8*)r.memory.AcquireTextureUploadBuffer(bytes,directUncompressed);
  memset(data,uint8(index),bytes);
  auto allocation=r.upload(data,bytes,side,side);
  auto* desc=MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm,side,side,false);
  desc->setStorageMode(MTL::StorageModeShared);desc->setUsage(MTL::TextureUsageShaderRead);
  auto* texture=d->newTexture(desc);require(texture,"fixture texture");textures.push_back(texture);
  enc->copyFromBuffer(allocation.mtlBuffer,allocation.bufferOffset,side*4,0,MTL::Size(side,side,1),texture,0,0,MTL::Origin(0,0,0));
  r.memory.ReleaseTextureUploadBuffer(data);
 }
 // Non-decoder callers keep the copy fallback, even on a native-BC GPU.
 auto externalAllocation=r.upload(external.data(),bytes,side,side);
 require(externalAllocation.memPtr[0]==0x6e,"external upload fallback bytes");
 if(fallback) {
  // Unsupported BC devices still need a distinct expanded allocation.
  const std::array<uint8,8> redBlock={0x00,0xf8,0xe0,0x07,0,0,0,0};
  auto* data=(uint8*)r.memory.AcquireTextureUploadBuffer(redBlock.size());
  std::copy(redBlock.begin(),redBlock.end(),data);
  auto expanded=r.upload(data,redBlock.size(),4,4,true);
  require(expanded.memPtr!=data,"compressed input must not alias expanded output");
  for(unsigned pixel=0;pixel<16;++pixel)
   require(expanded.memPtr[pixel*4]==255 && expanded.memPtr[pixel*4+1]==0 &&
           expanded.memPtr[pixel*4+2]==0 && expanded.memPtr[pixel*4+3]==255,"BC expansion changed pixels");
  require(std::equal(redBlock.begin(),redBlock.end(),data),"BC expansion overwrote compressed input");
  r.memory.ReleaseTextureUploadBuffer(data);
 }
 enc->endEncoding();auto encoded=std::chrono::steady_clock::now();cb->commit();cb->waitUntilCompleted();
 std::vector<uint8> pixels(bytes);
 for(unsigned index=0;index<count;++index) {
  textures[index]->getBytes(pixels.data(),side*4,MTL::Region(0,0,side,side),0);
  require(std::all_of(pixels.begin(),pixels.end(),[&](auto b){return b==uint8(index);}),"in-flight decoder upload bytes were overwritten");
  textures[index]->release();
 }
 const size_t wanted=(fallback&&!directUncompressed)?(count+1)*bytes:bytes;
 printf("CPU-copy bytes: %zu; desired: %zu; CPU prepare+encode: %.3fms; GPU: %.3fms\n",cpuCopyBytes,wanted,
  std::chrono::duration<double,std::milli>(encoded-start).count(),(cb->GPUEndTime()-cb->GPUStartTime())*1000);
 require(cpuCopyBytes==wanted,"FAIL: decoder upload repeats a full CPU image copy");
 d->release();pool->release();puts("PASS: 256 in-flight textures through the production ring allocator preserve every byte");
}
'''.replace('UPLOAD', upload.replace('memcpy(', 'countedCopy(')).replace('ACQUIRE', acquire).replace('RELEASE', release).replace('RESOLVE', resolve).replace('RING_CLASS',ring_class).replace('RING_DESTRUCTOR',ring_destructor).replace('RING_METHODS',ring_methods)

with tempfile.TemporaryDirectory(prefix='cemu-direct-upload-') as temp:
    p=Path(temp)
    (p/'test.mm').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG',
                    '-I'+str(repo/'src'),'-I'+str(repo/'dependencies/metal-cpp'),str(p/'test.mm'),
                    str(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.cpp'),
                    '-framework','Metal','-framework','Foundation','-o',str(p/'test')],check=True)
    env=dict(os.environ,MTL_DEBUG_LAYER='1')
    results=[subprocess.run([str(p/'test'),mode],env=env).returncode for mode in ['native','fallback','legacy-uncompressed']]
    if any(results):
        raise SystemExit(f'FAIL: native/fallback exit codes {results}')
