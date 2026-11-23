#!/usr/bin/env python3
"""Replay production cache loading: descriptor aliases compile only once, including parallel workers."""
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[3]
s=(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp').read_text()
a=s.index('void MetalPipelineCache::LoadPipelineFromCache(')
b=s.index('\nvoid MetalPipelineCache::AddCurrentStateToCache(',a)
production=s[a:b]
fixture=r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>
using uint8=uint8_t; using uint64=uint64_t; using sint32=int32_t;
using FSpinlock=std::mutex;
#define cemuLog_log(...) ((void)0)
#define cemu_assert_debug(...) ((void)0)
struct LatteContextRegister {};
struct MetalAttachmentsInfo { MetalAttachmentsInfo()=default; MetalAttachmentsInfo(const LatteContextRegister&,void*) {} };
struct ShaderHash { bool isPresent=false; uint64 baseHash=0,auxHash=0; };
struct CachedPipeline { ShaderHash vsHash{true,1,0},gsHash{},psHash{true,2,0}; MetalAttachmentsInfo lastUsedAttachmentsInfo; int gpuState=0; };
namespace Latte { void LoadGPURegisterState(LatteContextRegister&,int) {} }
struct MemStreamReader { const uint8* data; MemStreamReader(const uint8* p,sint32):data(p) {} };
struct LatteDecompilerShader { void* compatibleFetchShader=nullptr; } vertex,pixel;
LatteDecompilerShader* LatteSHRC_FindVertexShader(uint64,uint64) { return &vertex; }
LatteDecompilerShader* LatteSHRC_FindGeometryShader(uint64,uint64) { return nullptr; }
LatteDecompilerShader* LatteSHRC_FindPixelShader(uint64,uint64) { return &pixel; }
struct Pipeline { void release() {} };
struct PipelineObject { Pipeline* pipeline=nullptr; Pipeline* GetPipeline() { return pipeline; } };
std::atomic<int> compileCount{0}; std::atomic<bool> failNext{false};
struct MetalPipelineCompiler {
 PipelineObject& object;
 MetalPipelineCompiler(void*,PipelineObject& p):object(p) {}
 void InitFromState(void*,LatteDecompilerShader*,LatteDecompilerShader*,LatteDecompilerShader*,const MetalAttachmentsInfo&,const MetalAttachmentsInfo&,LatteContextRegister&) {}
 bool Compile(bool,bool,bool) {
  ++compileCount;
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if(failNext.exchange(false)) return false;
  static Pipeline value; object.pipeline=&value; return true;
 }
};
struct MetalPipelineCache {
 void* m_mtlr=nullptr;
 std::map<uint64,PipelineObject*> m_pipelineCache;
 FSpinlock m_pipelineCacheLock;
 ~MetalPipelineCache() { for(auto [key,p]:m_pipelineCache) delete p; }
 bool DeserializePipeline(MemStreamReader& r,CachedPipeline&) { return r.data[0]!=0; }
 uint64 CalculatePipelineHash(void*,LatteDecompilerShader*,LatteDecompilerShader*,LatteDecompilerShader*,const MetalAttachmentsInfo&,const MetalAttachmentsInfo&,LatteContextRegister&) { return 55; }
 void LoadPipelineFromCache(std::span<uint8>);
};
''' + production + r'''
int main() {
 uint8 a=1,b=2,bad=0;
 {
  MetalPipelineCache cache;
  cache.LoadPipelineFromCache({&a,1});
  cache.LoadPipelineFromCache({&b,1});
  if(compileCount!=1 || cache.m_pipelineCache.size()!=1) {
   puts("FAIL: aliases are discarded only after redundant Metal compilation"); return 1;
  }
 }
 compileCount=0;
 {
  MetalPipelineCache cache;
  std::vector<std::thread> threads;
  for(int i=0;i<8;++i) threads.emplace_back([&]{cache.LoadPipelineFromCache({&a,1});});
  for(auto& t:threads)t.join();
  if(compileCount!=1 || cache.m_pipelineCache.size()!=1 || !cache.m_pipelineCache.at(55)->GetPipeline()) return 2;
 }
 compileCount=0;
 {
  MetalPipelineCache cache;
  failNext=true;
  cache.LoadPipelineFromCache({&a,1});
  if(!cache.m_pipelineCache.empty()) return 3;
  cache.LoadPipelineFromCache({&b,1});
  if(compileCount!=2 || cache.m_pipelineCache.size()!=1) return 4;
  cache.LoadPipelineFromCache({&bad,1});
  if(compileCount!=2) return 5;
 }
 puts("PASS: serialized aliases compile once; parallel loads coalesce; failures retry without stale entries");
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-pipeline-load-') as temp:
 p=Path(temp);(p/'test.cpp').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-O3','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
