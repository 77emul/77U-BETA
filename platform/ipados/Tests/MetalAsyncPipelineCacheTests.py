#!/usr/bin/env python3
"""Production cache + resolver retain draw-local policy across every cache path."""
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[3]
s=(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp').read_text()
r=(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp').read_text()
def function(source,name):
    a=source.index(name); opening=source.index('{',a); end=opening+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[a:end],source[a:opening].strip()
get,signature=function(s,'PipelineObject* MetalPipelineCache::GetRenderPipelineState(')
resolver,_=function(r,'static MTL::RenderPipelineState* ResolvePipelineForDraw(')
fixture=r"""
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#define NDEBUG 1
#undef TARGET_OS_IOS
#define TARGET_OS_IOS 1
using uint64=uint64_t; using uint32=uint32_t; using FSpinlock=std::mutex;
struct LatteFetchShader {};
struct LatteDecompilerShader { uint64 baseHash=0, auxHash=0; };
namespace Latte { enum class E_GX2SURFFMT { INVALID_FORMAT=0, Color=37, Depth=19 }; }
struct MetalAttachmentsInfo { std::array<Latte::E_GX2SURFFMT,8> colorFormats{}; Latte::E_GX2SURFFMT depthFormat{}; };
constexpr int mmVGT_STRMOUT_EN=0;
struct LatteContextRegister {
    uint64 hash=1; bool rasterization=true; std::array<uint32,1> registers{};
    bool IsRasterizationEnabled() const { return rasterization; }
    const uint32* GetRawView() const { return registers.data(); }
};
struct Config { bool async_compile=true; };
Config& GetConfig() { static Config config; return config; }
struct MetalRenderer {
    struct Context { struct Size { int x=1920, y=1080; } size; bool queryActive=false,readbackAttachment=false; } context;
    int diagnosticGets=0;
    Context GetPipelineDiagnosticContext() { ++diagnosticGets; return context; }
};
namespace MTL { struct RenderPipelineState {}; }
bool diagnosticsEnabled=false;
bool CemuBridge_IsDiagnosticLoggingEnabled() { return diagnosticsEnabled; }
MTL::RenderPipelineState compiled;
struct PipelineObject {
    MTL::RenderPipelineState* pipeline=nullptr;
    int waits=0;
    MTL::RenderPipelineState* GetPipeline() const { return pipeline; }
    void WaitForCompilation() { ++waits; pipeline=&compiled; }
};
struct MetalPipelineCompiler {
    PipelineObject& object;
    MetalPipelineCompiler(void*,PipelineObject& p,bool=false):object(p) {}
    RuntimePerformance::PipelineCompileTimings GetCompileTimings() const { return {1000000,2000000,7000000}; }
    bool IsMeshPipeline() const { return false; }
    void InitFromState(const LatteFetchShader*,const LatteDecompilerShader*,const LatteDecompilerShader*,const LatteDecompilerShader*,const MetalAttachmentsInfo&,const MetalAttachmentsInfo&,const LatteContextRegister&) {}
    bool Compile(bool,bool,bool) { object.pipeline=&compiled; return true; }
};
std::vector<MetalPipelineCompiler*> jobs;
bool g_compilePipelineThreadInit=true;
void initCompileThread() {}
void queuePipeline(MetalPipelineCompiler* compiler,PipelineObject*) { jobs.push_back(compiler); }
struct MetalPipelineCache {
    MetalRenderer renderer;
    MetalRenderer* m_mtlr=&renderer;
    std::map<uint64,PipelineObject*> m_pipelineCache;
    FSpinlock m_pipelineCacheLock;
    uint64 m_lastPipelineHash=0;
    PipelineObject* m_lastPipelineObj=nullptr;
    struct Entry { uint64 hash=0; PipelineObject* object=nullptr; };
    std::array<Entry,1024> m_fastPipelineCache{};
    int priorities=0;
    static MetalPipelineCache* instance;
    static MetalPipelineCache& GetInstance() { return *instance; }
    void PrioritizePendingPipeline(PipelineObject*) { ++priorities; }
    uint64 CalculatePipelineHash(const LatteFetchShader*,const LatteDecompilerShader*,const LatteDecompilerShader*,const LatteDecompilerShader*,const MetalAttachmentsInfo&,const MetalAttachmentsInfo&,const LatteContextRegister& lcr) { return lcr.hash; }
    void AddCurrentStateToCache(uint64,const MetalAttachmentsInfo&) {}
    """+signature.replace('MetalPipelineCache::','')+r""";
};
MetalPipelineCache* MetalPipelineCache::instance=nullptr;
""" + get + resolver + r"""
int main() {
    MetalPipelineCache cache; MetalPipelineCache::instance=&cache;
    MetalAttachmentsInfo attachments; LatteContextRegister context;
    LatteDecompilerShader vertex{1,2},geometry{3,4},pixel{5,6};
    auto request=[&](bool defer) {
        auto* object=cache.GetRenderPipelineState(nullptr,&vertex,&geometry,&pixel,
            attachments,attachments,context,defer);
        return std::pair{object,ResolvePipelineForDraw(object,defer)};
    };
    auto [one,notReady]=request(true);
    if(notReady || one->waits || jobs.size()!=1) return 1;
    auto [same,pending]=request(true); // last-hit
    if(same!=one || pending || one->waits || jobs.size()!=1) return 2;
    auto [required,ready]=request(false);
    if(required!=one || ready!=&compiled || one->waits!=1 || cache.priorities!=1) return 3;
    context.hash=2;
    auto [two,pendingTwo]=request(true);
    if(pendingTwo || two->waits || jobs.size()!=2) return 4;
    context.hash=1; one->pipeline=nullptr;
    request(false); // direct-cache hit (last-hit now points at key 2)
    if(one->waits!=2 || cache.priorities!=2) return 5;
    cache.m_lastPipelineHash=0; cache.m_lastPipelineObj=nullptr;
    cache.m_fastPipelineCache={}; one->pipeline=nullptr;
    request(false); // map hit
    if(one->waits!=3 || cache.priorities!=3 || jobs.size()!=2) return 6;
    context.hash=3;
    auto [sync,readySync]=request(false); // new required compile stays synchronous
    if(readySync!=&compiled || sync->waits || jobs.size()!=2) return 7;
    if(cache.renderer.diagnosticGets!=0 || RuntimePerformance::pipelineSyncRecorder.takeReport(1).count) return 8;
    diagnosticsEnabled=true; GetConfig().async_compile=false;
    cache.renderer.context={{1600,1600},true,true};
    context.rasterization=false; context.registers[0]=1;
    attachments.colorFormats[0]=Latte::E_GX2SURFFMT::Color;
    context.hash=4; request(false);
#if TARGET_OS_IOS
    auto report=RuntimePerformance::pipelineSyncRecorder.takeReport(1000000001);
    if(cache.renderer.diagnosticGets!=1 || report.count!=1 || report.strongest.reasons!=127 ||
       report.strongest.key!=4 || report.strongest.vsBase!=1 || report.strongest.vsAux!=2 ||
       report.strongest.gsBase!=3 || report.strongest.gsAux!=4 || report.strongest.psBase!=5 ||
       report.strongest.psAux!=6 || report.strongest.width!=1600 || report.strongest.height!=1600 ||
       report.strongest.colorFormats[0]!=37 || !report.strongest.success) return 9;
    request(false); // Diagnostic mode still does no context collection on cache hits.
    if(cache.renderer.diagnosticGets!=1) return 10;
    context.hash=1; request(false); // Direct cache hit with diagnostics enabled.
    cache.m_lastPipelineHash=0; cache.m_lastPipelineObj=nullptr; cache.m_fastPipelineCache={};
    request(false); // Map hit with diagnostics enabled.
    context.hash=5; request(true); // New asynchronous pipeline also has no required-miss probe.
    if(cache.renderer.diagnosticGets!=1) return 12;
    // A depth-only miss and a caller-required scene miss must stay distinguishable.
    GetConfig().async_compile=true; cache.renderer.context={{1920,1080},false,false};
    context.rasterization=true; context.registers[0]=0; attachments.colorFormats={};
    attachments.depthFormat=Latte::E_GX2SURFFMT::Depth;
    context.hash=6; request(false);
    report=RuntimePerformance::pipelineSyncRecorder.takeReport(2000000001);
    if(report.count!=1 || report.strongest.reasons!=128 || report.strongest.depthFormat!=19 ||
       report.strongest.activeDepthFormat!=19 || report.reasonCounts[7]!=1) return 13;
    attachments.colorFormats[0]=Latte::E_GX2SURFFMT::Color;
    context.hash=7; request(false);
    report=RuntimePerformance::pipelineSyncRecorder.takeReport(3000000001);
    if(report.count!=1 || report.strongest.reasons!=256 || report.reasonCounts[8]!=1) return 14;
#else
    if(cache.renderer.diagnosticGets!=0 || RuntimePerformance::pipelineSyncRecorder.takeReport(1000000001).count) return 11;
#endif
    for(auto* job:jobs) { job->Compile(true,false,true); delete job; }
    for(auto [hash,object]:cache.m_pipelineCache) delete object;
    puts("PASS: last-hit, direct-cache, map-hit and new required draws all honor current policy");
}
"""
with tempfile.TemporaryDirectory(prefix='cemu-async-cache-') as tmp:
    p=Path(tmp)
    for ios in (1,0):
        (p/'test.cpp').write_text(fixture.replace('#define TARGET_OS_IOS 1',f'#define TARGET_OS_IOS {ios}'))
        subprocess.run(['xcrun','clang++','-std=c++20','-O3','-fsanitize=address,undefined','-I'+str(repo/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
