#!/usr/bin/env python3
"""Exercise production shader results and startup without persistent archive access."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.cpp').read_text()
start = source.index('MTL::Library* RendererShaderMtl::LibraryFromSource()')
production = source[start:source.index('void RendererShaderMtl::FinishCompilation()', start)]
start = source.index('void RendererShaderMtl::ShaderCacheLoading_begin(')
startup = source[start:source.index('size_t RendererShaderMtl::TrimReuseCaches()', start)]
fixture = r'''
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "Cafe/HW/Latte/Renderer/Metal/MetalShaderLibraryCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#define NS_STACK_SCOPED
#define cemuLog_log(...) ((void)0)
using uint64 = uint64_t;
namespace NS {
struct AutoreleasePool { static AutoreleasePool* alloc() { static AutoreleasePool p; return &p; } AutoreleasePool* init() { return this; } };
struct Error {
    Error* localizedDescription() { return this; }
    const char* utf8String() { return "compiler diagnostic"; }
};
}
namespace MTL {
struct Function {
    int retains = 1;
    Function* retain() { assert(retains > 0); ++retains; return this; }
    void release() { assert(retains > 0); --retains; }
};
struct Library {
    int retains = 0, functionCalls = 0;
    bool failFunction = false;
    std::string lastName;
    std::vector<std::unique_ptr<Function>> functions;
    Library* retain() { ++retains; return this; }
    void release() { assert(retains > 0); --retains; }
    Function* newFunction(const std::string& name) {
        ++functionCalls; lastName = name;
        if (failFunction) return nullptr;
        functions.push_back(std::make_unique<Function>());
        return functions.back().get();
    }
};
struct CompileOptions {
    static CompileOptions* alloc() { static CompileOptions value; return &value; }
    CompileOptions* init() { return this; }
    void setFastMathEnabled(bool) {}
    void setPreserveInvariance(bool) {}
};
struct Device {
    Library* result = nullptr;
    NS::Error* diagnostic = nullptr;
    int compiles = 0, bulkLibraryLoads = 0;
    std::string lastSource;
    Library* newLibrary(const std::string& text, CompileOptions*, NS::Error** error) {
        ++compiles; lastSource = text; *error = diagnostic;
        return result ? result->retain() : nullptr;
    }
    template<class... Args> Library* newLibrary(Args&&...) {
        ++bulkLibraryLoads; std::abort();
    }
};
}
struct Profile { bool GetShaderFastMath() { return true; } } profile;
auto* g_current_game_profile = &profile;
const std::string& ToNSString(const std::string& value) { return value; }
struct Policy { bool constrained = false; size_t LoadingWorkers(size_t n) const { return n; } } policy;
struct { size_t m_legacyWorkerCount = 2; std::vector<int> s_threads{0, 0}; int loadingStarts = 0;
    void StartLoadingThreads(size_t) { ++loadingStarts; } void StopLoadingThreads() {} } shaderMtlThreadPool;
const Policy& GetMetalMemoryPolicy() { return policy; }
namespace ActiveSettings {
int pathRequests = 0;
template<class... Args> std::filesystem::path GetCachePath(Args&&...) {
    ++pathRequests; std::abort();
}
}
struct Archive {
    int accesses = 0;
    template<class... Args> void Open(Args&&...) { ++accesses; std::abort(); }
    MTL::Function* FindShader(const std::string&) { ++accesses; std::abort(); }
    void Close() { ++accesses; std::abort(); }
};
struct MetalRenderer {
    MTL::Device device;
    Archive archive;
    bool maximizing = false;
    int maximizeCalls = 0;
    bool GetPositionInvariance() { return false; }
    MTL::Device* GetDevice() { return &device; }
    Archive& GetShaderArchive() { ++archive.accesses; std::abort(); }
    void SetShouldMaximizeConcurrentCompilation(bool value) { maximizing = value; ++maximizeCalls; }
};
std::unique_ptr<MetalRenderer> g_renderer;
bool s_isLoadingShadersMtl = false;
struct RendererShaderMtl {
    MetalRenderer* m_mtlr;
    std::string m_mslCode;
    uint64_t m_baseHash = 0, m_auxHash = 0;
    uint32_t m_type = 0;
    MTL::Function* m_function = nullptr;
    MTL::Library* LibraryFromSource();
    bool ShouldCountCompilation() { return false; }
    void CompileInternal();
    void FinishCompilation() { m_mslCode.clear(); }
    static void ShaderCacheLoading_begin(uint64);
    static void ShaderCacheLoading_end();
    static void ShaderCacheLoading_Close();
};
MetalShaderLibraryCache<MTL::Library> s_shaderLibraryCache;
MetalShaderLibraryCache<MTL::Function> s_shaderFunctionCache;
std::atomic_int g_compiled_shaders_total{0};
'''
tests = r'''
int main(int argc, char** argv) {
    assert(argc == 2 && std::filesystem::exists(argv[1]));
    g_renderer = std::make_unique<MetalRenderer>();
    auto& renderer = *g_renderer;
    MTL::Library library;
    NS::Error warning;
    RendererShaderMtl shader{&renderer, "source"};
    renderer.device.result = &library;
    renderer.device.diagnostic = &warning;
    auto* result = shader.LibraryFromSource();
    assert(result == &library && renderer.device.compiles == 1);
    result->release();
    result = shader.LibraryFromSource();
    assert(result == &library && renderer.device.compiles == 1);
    result->release();
    s_shaderLibraryCache.Clear();
    assert(library.retains == 0);
    renderer.device.result = nullptr;
    assert(shader.LibraryFromSource() == nullptr);
    renderer.device.diagnostic = nullptr;
    assert(shader.LibraryFromSource() == nullptr);
    assert(renderer.device.compiles == 3 && s_shaderLibraryCache.GetStatistics().entries == 0);
    renderer.device.result = &library;
    result = shader.LibraryFromSource();
    assert(result == &library && renderer.device.compiles == 4);
    result->release();
    s_shaderLibraryCache.Clear();

    // Invoke the real startup body under both policies with populated reuse caches.
    for (bool constrained : {false, true}) {
        policy.constrained = constrained;
        RendererShaderMtl seeded{&renderer, "seeded program"};
        seeded.CompileInternal();
        assert(seeded.m_function && seeded.m_function->retains == 2);
        assert(s_shaderLibraryCache.GetStatistics().entries == 1);
        assert(s_shaderFunctionCache.GetStatistics().entries == 1);
        const int before = renderer.device.compiles;
        RendererShaderMtl::ShaderCacheLoading_begin(0x0005000010176a00);
        assert(s_isLoadingShadersMtl && renderer.maximizing);
        assert(s_shaderLibraryCache.GetStatistics().entries == 0);
        assert(s_shaderFunctionCache.GetStatistics().entries == 0);
        assert(s_shaderLibraryCache.RetainedSourceBytes() == 0);
        assert(s_shaderFunctionCache.RetainedSourceBytes() == 0);
        assert(renderer.device.compiles == before && renderer.device.bulkLibraryLoads == 0);
        assert(renderer.archive.accesses == 0 && ActiveSettings::pathRequests == 0);
        assert(seeded.m_function->retains == 1 && library.retains == 0);
        seeded.m_function->release();
        RendererShaderMtl::ShaderCacheLoading_end();
        RendererShaderMtl::ShaderCacheLoading_Close();
        assert(!s_isLoadingShadersMtl && !renderer.maximizing);
    }
    assert(renderer.maximizeCalls == 4);

    // Both policies preserve main0 and byte-exact canonical source reuse.
    std::vector<MTL::Function*> owned;
    for (bool constrained : {false, true}) {
        policy.constrained = constrained;
        const int before = renderer.device.compiles;
        const int functionsBefore = library.functionCalls;
        RendererShaderMtl first{&renderer, "// shader 0123456789abcdef\nvertex void main0() {}"};
        RendererShaderMtl repeated{&renderer, "// shader fedcba9876543210\nvertex void main0() {}"};
        first.CompileInternal(); repeated.CompileInternal();
        assert(first.m_function && first.m_function == repeated.m_function);
        assert(first.m_function->retains == 3);
        assert(renderer.device.compiles == before + 1 && library.functionCalls == functionsBefore + 1);
        assert(renderer.device.lastSource == "vertex void main0() {}" && library.lastName == "main0");
        assert(first.m_mslCode.empty() && repeated.m_mslCode.empty());
        s_shaderFunctionCache.Clear(); s_shaderLibraryCache.Clear();
        assert(first.m_function->retains == 2 && library.retains == 0);
        owned.push_back(first.m_function); owned.push_back(repeated.m_function);
    }

    // Source failure and function-creation failure stay retryable and publish no function.
    renderer.device.result = nullptr;
    RendererShaderMtl failure{&renderer, "bad program"}; failure.CompileInternal();
    assert(!failure.m_function && failure.m_mslCode.empty());
    renderer.device.result = &library;
    RendererShaderMtl retry{&renderer, "bad program"}; retry.CompileInternal();
    assert(retry.m_function); owned.push_back(retry.m_function);
    library.failFunction = true;
    RendererShaderMtl badFunction{&renderer, "function program"}; badFunction.CompileInternal();
    assert(!badFunction.m_function);
    library.failFunction = false;
    const int before = renderer.device.compiles;
    RendererShaderMtl retryFunction{&renderer, "function program"}; retryFunction.CompileInternal();
    assert(retryFunction.m_function && renderer.device.compiles == before);
    owned.push_back(retryFunction.m_function);
    s_shaderFunctionCache.Clear(); s_shaderLibraryCache.Clear();
    assert(library.retains == 0);
    for (auto* function : owned) function->release();
    for (const auto& function : library.functions) assert(function->retains == 0);
    assert(renderer.archive.accesses == 0 && ActiveSettings::pathRequests == 0);
    assert(renderer.device.bulkLibraryLoads == 0);
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-metal-result-') as temporary:
    path = Path(temporary)
    archive = path / 'functions-v1.metallib'
    sentinel = b'preexisting archive must not be opened or modified\n'
    archive.write_bytes(sentinel)
    (path / 'test.cpp').write_text(fixture + startup + production + tests)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-I'+str(repo/'src'), str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test'), str(archive)], check=True)
    assert archive.read_bytes() == sentinel
print('PASS: production warning/failure/retry, retained function lifetime, main0 source reuse, '
      'and Native/constrained startup with archive/path/bulk-library traps')
