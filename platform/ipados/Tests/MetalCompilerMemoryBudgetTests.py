#!/usr/bin/env python3
"""Execute production worker and preload sizing decisions for simulated devices.

Only the hardware query and thread creation boundary are replaced. This runs
all three production compiler pool sizing paths without starting worker loops.
"""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
metal = repo / "src/Cafe/HW/Latte/Renderer/Metal"
shader = (metal / "RendererShaderMtl.cpp").read_text()
pipeline = (metal / "MetalPipelineCache.cpp").read_text()
loading = (repo / "src/Cafe/HW/Latte/Core/LatteShaderCache.cpp").read_text()

start = shader.index("// Create thread pool")
shader_sizing = shader[start:shader.index("for (uint32 i = 0; i < threadCount;", start)]
start = pipeline.index("static void initCompileThread()")
start = pipeline.index("{", start) + 1
pipeline_sizing = pipeline[start:pipeline.index("for (uint32 i = 0; i < numCompileThreads;", start)]
start = pipeline.index("uint32 MetalPipelineCache::BeginLoading(")
start = pipeline.index("uint32 cpuCoreCount", start)
loader_sizing = pipeline[start:pipeline.index("m_compilationThreads.reserve", start)]
start = loading.index("sint32 compileQueueLimit =")
shader_queue = loading[start:loading.index("auto LoadShadersUpdate", start)]

fixture = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryPolicy.h"
#undef TARGET_OS_IPHONE
#undef TARGET_OS_IOS
#define TARGET_OS_IPHONE TEST_IOS
#define TARGET_OS_IOS TEST_IOS
#define ENABLE_METAL 1
#define SHADER_CACHE_COMPILE_QUEUE_SIZE (TEST_IOS ? 128 : 32)
using uint32 = uint32_t; using sint32 = int32_t;
static uint32 cores;
static size_t m_legacyWorkerCount = 0;
static MetalMemoryPolicy testPolicy;
#define GetMetalMemoryPolicy() testPolicy
uint32 GetPhysicalCoreCount() { return cores; }
enum class RendererAPI { Metal, Vulkan };
struct Renderer { RendererAPI api=RendererAPI::Metal; RendererAPI GetType() { return api; } } renderer;
auto* g_renderer = &renderer;
uint32 shaderWorkers() { SHADER return threadCount; }
uint32 pipelineWorkers() { PIPELINE return numCompileThreads; }
struct Loader {
 size_t m_loadingQueueLimit = 128;
 uint32 workers() { LOADER return numCompilationThreads; }
};
sint32 shaderQueueLimit() { QUEUE return compileQueueLimit; }
int main() {
 constexpr uint64_t GiB=uint64_t{1024}*1024*1024;
 for (auto bytes : {uint64_t{0},4*GiB,6*GiB,8*GiB,12*GiB,16*GiB}) {
  testPolicy=MakeMetalMemoryPolicy(bytes);
  for (auto count : {2u,4u,6u,8u,10u}) {
   cores=count;
   const uint32 normalShader=TEST_IOS ? (count>=8 ? std::min(8u,count-2) : count>=6 ? 4 : 2) : 2;
   uint32 normalPipeline=std::min(count<=2 ? 1 : count-1,8u);
   if (TEST_IOS) normalPipeline=count>=8 ? std::clamp(count-2,1u,8u) : std::clamp(count>=6 ? 4u : 2u,1u,4u);
   const uint32 normalLoader=std::clamp(count,1u,8u);
   const bool small=TEST_IOS && bytes && bytes<=6*GiB;
   assert(shaderWorkers()==(small ? std::min(normalShader,2u) : normalShader));
   assert(pipelineWorkers()==(small ? std::min(normalPipeline,2u) : normalPipeline));
   Loader loader;
   // Loading-screen-only threads may use up to four on small devices.
   assert(loader.workers()==(small ? std::min(normalLoader,4u) : normalLoader));
   assert(loader.m_loadingQueueLimit==(small ? 32 : 128));
   renderer.api=RendererAPI::Metal;
   assert(shaderQueueLimit()==(small ? 32 : SHADER_CACHE_COMPILE_QUEUE_SIZE));
   renderer.api=RendererAPI::Vulkan;
   assert(shaderQueueLimit()==SHADER_CACHE_COMPILE_QUEUE_SIZE);
  }
 }
 puts("PASS: production shader/gameplay/preload compiler sizing, 4/6/8/12/16 GiB, all core counts; high-end and non-iOS unchanged");
}
"""
fixture = fixture.replace("SHADER return", shader_sizing + " return").replace("PIPELINE return", pipeline_sizing + " return").replace("LOADER return", loader_sizing + " return").replace("QUEUE return", shader_queue + " return")
with tempfile.TemporaryDirectory(prefix="cemu-compiler-memory-") as temp:
    temp=Path(temp)
    (temp/"test.cpp").write_text(fixture)
    for ios in (0,1):
        binary=temp/f"test-{ios}"
        subprocess.run(["xcrun","clang++","-std=c++20","-O1","-fsanitize=address,undefined",f"-DTEST_IOS={ios}","-I"+str(repo/"src"),str(temp/"test.cpp"),"-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
