#!/usr/bin/env python3
"""Exercise production shader compilation with real Cocoa autorelease semantics."""
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[3]
source=(repo/'src/Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.cpp').read_text()
production=source[source.index('MTL::Library* RendererShaderMtl::LibraryFromSource()'):]
common=(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalCommon.h').read_text()
cleanup=common[common.index('__attribute__((unused)) static inline void StackAutoRelease'):common.index('// Cast from const char*')]
fixture=r"""
#include <Foundation/Foundation.hpp>
#import <Foundation/Foundation.h>
#include <atomic>
#include <cassert>
#include <string>
#include <memory>
#include "Cafe/HW/Latte/Renderer/Metal/MetalShaderLibraryCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
static std::atomic<int> liveTemporaryObjects{0};
static std::atomic<int> functionCreates{0};
static std::atomic<bool> failFunction{false};
@interface CompilerTemporary : NSObject
@end
@implementation CompilerTemporary
- (instancetype)init { self=[super init]; if (self) ++liveTemporaryObjects; return self; }
- (void)dealloc { --liveTemporaryObjects; [super dealloc]; }
@end
#define cemuLog_log(...) ((void)0)
std::atomic<int> g_compiled_shaders_total{0};
namespace MTL {
struct Function { int marker=77; int references=1; Function* retain() { ++references; return this; } void release() { if (--references==0) delete this; } };
struct Library {
    int references=1;
    Library* retain() { ++references; return this; }
    Function* newFunction(NS::String*) {
        ++functionCreates; [[[CompilerTemporary alloc] init] autorelease]; return failFunction ? nullptr : new Function;
    }
    void release() { if (--references==0) delete this; }
};
struct CompileOptions : NS::Object {
    static CompileOptions* alloc() { return reinterpret_cast<CompileOptions*>([NSObject alloc]); }
    CompileOptions* init() { return reinterpret_cast<CompileOptions*>([(id)this init]); }
    void setFastMathEnabled(bool) {} void setPreserveInvariance(bool) {}
};
struct Device {
    bool fail=false;
    Library* newLibrary(NS::String*,CompileOptions*,NS::Error**) {
        [[[CompilerTemporary alloc] init] autorelease];
        return fail ? nullptr : new Library;
    }
};
}
struct Profile { bool GetShaderFastMath() { return true; } } profile;
auto* g_current_game_profile=&profile;
NS::String* ToNSString(const std::string& s) { return NS::String::string(s.c_str(),NS::ASCIIStringEncoding); }
NS::String* ToNSString(const char* s) { return NS::String::string(s,NS::ASCIIStringEncoding); }
struct Renderer { MTL::Device device; bool GetPositionInvariance() { return false; } MTL::Device* GetDevice() { return &device; } };
struct RendererShaderMtl {
    Renderer* m_mtlr;
    std::string m_mslCode;
    uint64_t m_baseHash=1,m_auxHash=2; unsigned m_type=0;
    MTL::Function* m_function=nullptr;
    bool ShouldCountCompilation() { return false; }
    MTL::Library* LibraryFromSource(); void CompileInternal(); void FinishCompilation();
};
"""
fixture += "\nMetalShaderLibraryCache<MTL::Library> s_shaderLibraryCache;\nMetalShaderLibraryCache<MTL::Function> s_shaderFunctionCache;\n"
tests=r"""
int main() {
    std::thread worker([] {
        auto outerPool=NS::AutoreleasePool::alloc()->init();
        Renderer renderer;
        for (int i=0;i<1000;++i) {
            RendererShaderMtl shader{&renderer,std::string(12000,'x')};
            shader.CompileInternal();
            assert(liveTemporaryObjects==0 && "compiler temporaries survived the job");
            assert(shader.m_function && shader.m_function->marker==77);
            shader.m_function->release();
        }
        assert(functionCreates==1 && "identical program repeatedly created main0");
        s_shaderFunctionCache.Clear();
        RendererShaderMtl afterClear{&renderer,std::string(12000,'x')};
        afterClear.CompileInternal();
        assert(functionCreates==2 && afterClear.m_function);
        s_shaderFunctionCache.Clear();
        assert(afterClear.m_function->marker==77 && "cache clear destroyed retained function");
        afterClear.m_function->release();
        failFunction=true;
        RendererShaderMtl failedEntry{&renderer,"missing entry"};
        failedEntry.CompileInternal();
        assert(!failedEntry.m_function);
        failFunction=false;
        RendererShaderMtl retryEntry{&renderer,"missing entry"};
        retryEntry.CompileInternal();
        assert(retryEntry.m_function && "missing function must be retryable");
        retryEntry.m_function->release();
        renderer.device.fail=true;
        RendererShaderMtl failed{&renderer,"invalid source"};
        failed.CompileInternal();
        assert(!failed.m_function);
        assert(liveTemporaryObjects==0 && "failed compiler temporaries survived the job");
        outerPool->release();
    });
    worker.join();
    puts("PASS: 1000 compilation jobs drain temporaries; owned shader functions survive; failure drains too");
}
"""
with tempfile.TemporaryDirectory(prefix='cemu-compiler-life-') as tmp:
    path=Path(tmp); (path/'test.mm').write_text(fixture+cleanup+production+tests)
    (path/'foundation.cpp').write_text('#define NS_PRIVATE_IMPLEMENTATION\n#include <Foundation/NSAutoreleasePool.hpp>\n#include <Foundation/NSString.hpp>\n#include <Foundation/NSError.hpp>\n')
    subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG=1','-UNDEBUG','-fno-objc-arc','-I'+str(repo/'dependencies/metal-cpp'),'-I'+str(repo/'src'),str(path/'test.mm'),str(path/'foundation.cpp'),'-framework','Foundation','-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
