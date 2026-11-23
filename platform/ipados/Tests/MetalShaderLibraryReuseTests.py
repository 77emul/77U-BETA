#!/usr/bin/env python3
"""Test actual library compilation with reuse and the bounded concurrent cache."""
from pathlib import Path
import subprocess, tempfile
repo=Path(__file__).resolve().parents[3]
source=(repo/'src/Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.cpp').read_text()
start=source.index('MTL::Library* RendererShaderMtl::LibraryFromSource()')
end=source.index('void RendererShaderMtl::CompileInternal()',start)
production=source[start:end]
fixture=r'''

#include <cassert>
#include <atomic>
#include <memory>
#include <thread>
#include <barrier>
#include <stdexcept>
#include <iostream>
#if __has_include("Cafe/HW/Latte/Renderer/Metal/MetalShaderLibraryCache.h")
#include "Cafe/HW/Latte/Renderer/Metal/MetalShaderLibraryCache.h"
#endif
#include <string>
#include <vector>
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#define NS_STACK_SCOPED
#define cemuLog_log(...) ((void)0)
namespace NS {
struct Error {
    Error* localizedDescription() { return this; }
    const char* utf8String() { return "compiler diagnostic"; }
};
}
namespace MTL {
struct Library { Library* retain() { return this; } void release() {} };
struct CompileOptions {
    static CompileOptions* alloc() { static CompileOptions value; return &value; }
    CompileOptions* init() { return this; }
    void setFastMathEnabled(bool) {}
    void setPreserveInvariance(bool) {}
};
struct Device {
    Library* result = nullptr;
    NS::Error* diagnostic = nullptr;
    std::atomic<int> compiles{0};
    std::vector<std::string> compiledSources;
    Library* newLibrary(const std::string& source, CompileOptions*, NS::Error** error) {
        compiledSources.push_back(source);
        ++compiles;
        *error = diagnostic;
        return result;
    }
};
}
struct Profile { bool GetShaderFastMath() { return true; } } profile;
auto* g_current_game_profile = &profile;
const std::string& ToNSString(const std::string& value) { return value; }
struct Renderer {
    MTL::Device device;
    bool GetPositionInvariance() { return false; }
    MTL::Device* GetDevice() { return &device; }
};
struct RendererShaderMtl {
    Renderer* m_mtlr;
    std::string m_mslCode;
    uint64_t m_baseHash=0,m_auxHash=0;
    uint32_t m_type=0;
    MTL::Library* LibraryFromSource();
};
#if __has_include("Cafe/HW/Latte/Renderer/Metal/MetalShaderLibraryCache.h")
MetalShaderLibraryCache<MTL::Library> s_shaderLibraryCache;
#endif
'''
tests=r'''

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    Renderer renderer;
    MTL::Library library;
    renderer.device.result = &library;
    RendererShaderMtl a{&renderer, "same generated MSL"}, b{&renderer, "same generated MSL"};
    check(a.LibraryFromSource() == &library, "first library missing");
    check(b.LibraryFromSource() == &library, "shared library missing");
    check(renderer.device.compiles == 1, "identical MSL compiled twice");
    RendererShaderMtl different{&renderer, "different generated MSL"};
    different.LibraryFromSource();
    check(renderer.device.compiles == 2, "different source incorrectly reused");
    // Real captured Splatoon shader libraries have identical program bytes
    // but different leading guest-hash comments. Metadata is not program state.
    const int beforeMetadata = renderer.device.compiles;
    RendererShaderMtl metadataA{&renderer, "// shader 6ba0be290412b07c\r\nidentical MSL program\r\n"};
    RendererShaderMtl metadataB{&renderer, "// shader 3a4c8cf84a04b328\r\nidentical MSL program\r\n"};
    RendererShaderMtl metadataC{&renderer, "// shader dc3ae5af6384b163\r\nidentical MSL program\r\n"};
    metadataA.LibraryFromSource(); metadataB.LibraryFromSource(); metadataC.LibraryFromSource();
    check(renderer.device.compiles == beforeMetadata + 1, "guest-hash comments force redundant library compiles");
    RendererShaderMtl changedProgram{&renderer, "// shader 6ba0be290412b07c\r\ndifferent MSL program\r\n"};
    changedProgram.LibraryFromSource();
    check(renderer.device.compiles == beforeMetadata + 2, "different program incorrectly reused");
    RendererShaderMtl arbitraryComment{&renderer, "// arbitrary comment\r\nidentical MSL program\r\n"};
    arbitraryComment.LibraryFromSource();
    check(renderer.device.compiles == beforeMetadata + 3, "unrecognized source prefix incorrectly stripped");
    // The Apple compiler must receive the canonical bytes, too. An in-memory
    // cache hit alone hides the bug until a title restart or bounded eviction.
    check(renderer.device.compiledSources[beforeMetadata] == "identical MSL program\r\n",
          "Apple compiler received guest-hash metadata that defeats driver-cache reuse");
    s_shaderLibraryCache.Clear();
    metadataB.LibraryFromSource();
    check(renderer.device.compiledSources.back() == "identical MSL program\r\n",
          "title restart changed Apple compiler input for an identical program");
    arbitraryComment.LibraryFromSource();
    check(renderer.device.compiledSources.back() == "// arbitrary comment\r\nidentical MSL program\r\n",
          "compiler input lost a non-metadata comment");
#if __has_include("Cafe/HW/Latte/Renderer/Metal/MetalShaderLibraryCache.h")
    check(MetalShaderLibrarySourceKey("// shader 000000000000000g\r\nprogram") == "// shader 000000000000000g\r\nprogram", "malformed hash stripped");
    check(MetalShaderLibrarySourceKey("// shader 0000000000000000 extra\r\nprogram") == "// shader 0000000000000000 extra\r\nprogram", "non-metadata header stripped");
    check(MetalShaderLibrarySourceKey("// shader 0000000000000000\nprogram") == "program", "LF metadata key incorrect");
    MetalShaderLibraryCache<int> cache;
    std::atomic<int> compiles{0};
    auto compile = [&] { ++compiles; return std::make_shared<int>(123); };
    auto first = cache.GetOrCompile(&renderer, 0, "source", compile);
    check(first == cache.GetOrCompile(&renderer, 0, "source", compile), "not reused");
    cache.GetOrCompile(&renderer, 1, "source", compile); // fast math/invariance differ
    cache.GetOrCompile(&library, 0, "source", compile); // different Metal device
    check(compiles == 3, "device/compiler options mixed");
    std::barrier start(8);
    std::thread threads[8];
    std::shared_ptr<int> results[8];
    for (int i=0; i<8; ++i) threads[i]=std::thread([&,i] {
        start.arrive_and_wait();
        results[i]=cache.GetOrCompile(&renderer, 0, "parallel", compile);
    });
    for (auto& thread : threads) thread.join();
    check(compiles == 4, "parallel source compiled more than once");
    for (auto& result : results) check(result == results[0], "parallel publication failed");
    int failures=0;
    auto fail = [&] { ++failures; return std::shared_ptr<int>{}; };
    check(!cache.GetOrCompile(&renderer, 0, "failure", fail), "failure became success");
    check(!cache.GetOrCompile(&renderer, 0, "failure", fail) && failures==2, "failure cached permanently");
    auto stable = cache.GetOrCompile(&renderer, 0, "stable", compile);
    cache.Clear();
    check(*stable == 123, "cache clearing destroyed a caller's library");
    auto after = cache.GetOrCompile(&renderer, 0, "stable", compile);
    check(stable != after, "clear kept entries from an old game");
    check(cache.RetainedSourceBytes()>0, "source accounting missing");
    check(cache.TrimCompleted()>0 && cache.RetainedSourceBytes()==0, "completed cache retention not trimmed");
    check(*stable==123 && *after==123, "pressure destroyed an active shader reference");
    std::atomic<bool> entered{false}, release{false};
    std::thread inFlight([&] { cache.GetOrCompile(&renderer,0,"in-flight",[&] {
        entered=true; while(!release.load()) std::this_thread::yield(); return std::make_shared<int>(456);
    }); });
    while(!entered.load()) std::this_thread::yield();
    check(cache.TrimCompleted()==0 && cache.RetainedSourceBytes()==9, "pressure removed a compiling entry");
    release=true; inFlight.join(); check(cache.TrimCompleted()==9, "completed job was not reclaimable");
    MetalShaderLibraryCache<int> bounded(2, 8);
    auto one=bounded.GetOrCompile(&renderer,0,"one",compile);
    bounded.GetOrCompile(&renderer,0,"two",compile);
    bounded.GetOrCompile(&renderer,0,"tri",compile);
    check(*one==123, "eviction destroyed retained library");
    check(bounded.GetOrCompile(&renderer,0,"one",compile)!=one, "source budget failed to evict");
#endif
    std::cout << "PASS: identical source reuse, distinct options/devices, concurrent publication, retry and bounded lifetime\n";
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-shader-reuse-') as tmp:
 p=Path(tmp); (p/'test.cpp').write_text(fixture+production+tests)
 subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG','-pthread','-I'+str(repo/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
