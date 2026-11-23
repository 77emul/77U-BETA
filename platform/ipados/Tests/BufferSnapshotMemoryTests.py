#!/usr/bin/env python3
"""Run production snapshot/hash/streamout uploads, with Mach footprint measurement.
The renderer consumes a copy synchronously, as its production upload contract
requires. No guest instructions or target-device FPS are simulated here.
"""
from pathlib import Path
import argparse, json, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline-dir', type=Path)
p.add_argument('--output', type=Path)
p.add_argument('--sanitize', action='store_true')
a=p.parse_args()
source=((a.baseline_dir or root)/'src/Cafe/HW/Latte/Core/LatteBufferCache.cpp').read_text()
def function(signature):
 start=source.index(signature); opening=source.index('{',start); end=opening+1; depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
fixture=r'''
#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mach/mach.h>
#include "Common/HostUploadMemory.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryPolicy.h"
using uint8=uint8_t;using uint32=uint32_t;using uint64=uint64_t;using sint32=int32_t;using MPTR=uint32;
#define CACHE_PAGE_SIZE 0x400
#define cemu_assert_debug(x) assert(x)
#define BOOST_OS_IOS 1
#define ENABLE_METAL 1
MetalMemoryPolicy policy;
#define GetMetalMemoryPolicy() policy
std::vector<uint8> guest;
uint8* memory_getPointerFromPhysicalOffset(uint32 offset){return guest.data()+offset;}
std::vector<uint8> s_pageUploadBuffer;
enum class RendererAPI {Metal,Vulkan};
struct Node;
struct Renderer {
 RendererAPI api=RendererAPI::Metal; Node* node=nullptr; size_t calls=0,bytes=0; bool mutate=false,filtering=false;
 std::vector<uint8> filtered;
 RendererAPI GetType(){return api;}
 void bufferCache_upload(uint8*,uint32,uint32);
} renderer;
auto* g_renderer=&renderer;
struct Node {
 struct Info{uint64 hash=0;};std::vector<Info> m_pageInfo;
 uint32 m_rangeBegin=4096;
 static constexpr uint64 c_streamoutSig0=0xF0F0F0F0155C5B6Aull,c_streamoutSig1=0x8BE6336411814F4Full;
 uint32 getBufferOffset(uint32 address){return address-m_rangeBegin+256;}
 static void UnloadAll(){}
 HASH
 UPLOAD
 FILTERED
};
using BufferCacheNode=Node;
UNLOAD
void Renderer::bufferCache_upload(uint8* data,uint32 size,uint32 offset){
 assert(offset>=256 && size>0);++calls;bytes+=size;
 if(filtering){std::copy_n(data,size,filtered.begin()+offset-256);return;}
 assert(size%CACHE_PAGE_SIZE==0 && (offset-256)%CACHE_PAGE_SIZE==0);
 const uint32 page=(offset-256)/CACHE_PAGE_SIZE;
 if(mutate)memset(guest.data()+node->m_rangeBegin+page*CACHE_PAGE_SIZE,0xe3,size);
 for(uint32 i=0;i<size/CACHE_PAGE_SIZE;++i)
  assert(Node::hashPage(data+i*CACHE_PAGE_SIZE)==node->m_pageInfo[page+i].hash);
}
uint64_t footprint(){task_vm_info_data_t v{};mach_msg_type_number_t n=TASK_VM_INFO_COUNT;
 assert(task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&v),&n)==KERN_SUCCESS);return v.phys_footprint;}
int main(int argc,char** argv){
 const bool constrained=argc>1&&strcmp(argv[1],"small")==0;
 policy=MakeMetalMemoryPolicy(uint64_t(constrained?4:12)*1024*1024*1024);
 constexpr uint32 pages=64*1024,first=13;
 Node node;renderer.node=&node;renderer.mutate=true;
 node.m_pageInfo.resize(pages+first+3);
 guest.resize(node.m_rangeBegin+(pages+first+3)*CACHE_PAGE_SIZE);
 for(size_t i=0;i<guest.size();++i)guest[i]=uint8(i*23+i/53);
 const auto before=footprint();const auto start=std::chrono::steady_clock::now();
 node.uploadPages(first,first+pages);
 const auto duration=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 const auto after=footprint();const auto retained=s_pageUploadBuffer.capacity();const auto calls=renderer.calls;
 assert(renderer.bytes==size_t(pages)*CACHE_PAGE_SIZE);
 assert(node.m_pageInfo[first-1].hash==0 && node.m_pageInfo[first+pages].hash==0);
 LatteBufferCache_UnloadAll();assert(s_pageUploadBuffer.capacity()==EXPECTED_UNLOAD);
 std::vector<uint8>().swap(s_pageUploadBuffer);
 // Tail/one-page ranges and repeated uploads keep every hash paired with its
 // own immutable uploaded bytes even if the guest changes in the callback.
 for(uint32 count:{1u,2u,1023u,1024u,1025u,2049u}){
  std::fill(guest.begin(),guest.end(),0x39);node.uploadPages(first,first+count);
 }
 // The separate streamout filter must leave GPU-owned 16-byte blocks intact.
 renderer.filtering=true;renderer.filtered.assign(CACHE_PAGE_SIZE,0xa7);
 auto* data=guest.data()+node.m_rangeBegin;std::fill_n(data,CACHE_PAGE_SIZE,0x5b);
 for(unsigned block=0;block<CACHE_PAGE_SIZE/16;block+=3){
  memcpy(data+block*16,&Node::c_streamoutSig0,8);memcpy(data+block*16+8,&Node::c_streamoutSig1,8);
 }
 assert(node.uploadPageWithStreamoutFiltered(0));
 for(unsigned i=0;i<CACHE_PAGE_SIZE;++i)assert(renderer.filtered[i]==((i/16)%3==0?0xa7:0x5b));
 assert(node.m_pageInfo[0].hash==Node::hashPage(data));
 printf("{\"constrained\":%s,\"beforeBytes\":%llu,\"afterBytes\":%llu,\"deltaBytes\":%lld,\"snapshotCapacityBytes\":%zu,\"uploadCalls\":%zu,\"cpuMs\":%.3f}\n",
 constrained?"true":"false",(unsigned long long)before,(unsigned long long)after,(long long)after-(long long)before,retained,calls,duration);
}
'''
fixture=fixture.replace('EXPECTED_UNLOAD','0' if 'swap(s_pageUploadBuffer)' in function('void LatteBufferCache_UnloadAll(') else 'retained')
for token,sig in [('HASH','static uint64 hashPage('),('UPLOAD','void uploadPages('),('FILTERED','sint32 uploadPageWithStreamoutFiltered('),('UNLOAD','void LatteBufferCache_UnloadAll(')]:fixture=fixture.replace(token,function(sig))
with tempfile.TemporaryDirectory(prefix='77u-buffer-snapshot-') as tmp:
 tmp=Path(tmp);(tmp/'test.cpp').write_text(fixture)
 cmd=['xcrun','clang++','-std=c++20','-O2','-I'+str(root/'src'),str(tmp/'test.cpp'),'-o',str(tmp/'test')]
 if a.sanitize:cmd+=['-fsanitize=address,undefined']
 subprocess.run(cmd,check=True)
 results={}
 for mode in ('small','native'):
  rows=[json.loads(subprocess.check_output([str(tmp/'test'),mode],text=True)) for _ in range(3 if not a.sanitize else 1)]
  results[mode]=rows
 print(json.dumps(results,indent=2))
 if a.output:a.output.write_text(json.dumps(results,indent=2))
