#!/usr/bin/env python3
"""Stress the production shader thread pool's loading-only workers.

Extracts ShaderMtlThreadPool from RendererShaderMtl.cpp unchanged and runs it
with a stand-in shader type: thousands of loading-screen start/stop cycles
while jobs are queued, including the idle-at-stop case that previously left
join() waiting forever. Every job must compile exactly once and every stop
must return.
"""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.cpp').read_text()
start = source.index('class ShaderMtlThreadPool')
end = source.index('} shaderMtlThreadPool;', start) + len('} shaderMtlThreadPool;')
pool = source[start:end]

fixture = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
using uint32 = uint32_t; using uint64 = uint64_t; using sint64 = int64_t;
#define cemu_assert_debug(x) assert(x)
#include "util/helpers/Semaphore.h"
#define TARGET_OS_IPHONE 1
static void SetThreadName(const char*) {}
static uint32 GetPhysicalCoreCount() { return 6; }
static std::atomic<uint64_t> g_compiled_shaders_async{0};
struct Policy { size_t CompilationWorkers(size_t n) const { return std::min<size_t>(n, 2); } };
static Policy GetMetalMemoryPolicy() { return {}; }
namespace MemoryBudget { inline uint64_t EffectiveAvailableMemory(uint64_t v) { return v; } }
static uint64_t os_proc_available_memory() { return 8ull << 30; }
class RendererShaderMtl {
public:
    enum class COMPILATION_STATE : uint32 { NONE, QUEUED, COMPILING, DONE };
    StateSemaphore<COMPILATION_STATE> m_compilationState{COMPILATION_STATE::NONE};
    std::atomic<int> compiles{0};
    void CompileInternal() { ++compiles; }
    bool ShouldCountCompilation() const { return false; }
};
'''

tests = r'''
static void Enqueue(RendererShaderMtl* job) {
    job->m_compilationState.setValue(RendererShaderMtl::COMPILATION_STATE::QUEUED);
    std::lock_guard lock(shaderMtlThreadPool.s_compilationQueueMutex);
    shaderMtlThreadPool.s_compilationQueue.push_back(job);
    shaderMtlThreadPool.s_compilationQueueCount.increment();
}
int main() {
    shaderMtlThreadPool.StartThreads();
    assert(shaderMtlThreadPool.s_threads.size() == 2);
    std::vector<RendererShaderMtl> jobs(30000);
    size_t next = 0;
    for (int cycle = 0; cycle < 3000; ++cycle) {
        shaderMtlThreadPool.StartLoadingThreads(2);
        // Some cycles queue work, some stop while every worker is idle.
        const int burst = cycle % 3 == 0 ? 0 : 10;
        for (int i = 0; i < burst && next < jobs.size(); ++i) Enqueue(&jobs[next++]);
        if (cycle % 5 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
        const auto t0 = std::chrono::steady_clock::now();
        shaderMtlThreadPool.StopLoadingThreads(); // must always return
        assert(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
        assert(shaderMtlThreadPool.m_loadingThreads.empty());
        // Jobs queued right as loading ends are still compiled by gameplay workers.
        if (cycle % 7 == 0 && next < jobs.size()) Enqueue(&jobs[next++]);
    }
    for (size_t i = 0; i < next; ++i)
        jobs[i].m_compilationState.waitUntilValue(RendererShaderMtl::COMPILATION_STATE::DONE);
    for (size_t i = 0; i < next; ++i) assert(jobs[i].compiles.load() == 1);
    shaderMtlThreadPool.StopThreads();
    printf("PASS: %zu jobs compiled exactly once across 3000 loading start/stop cycles (incl. idle stops); no hang\n", next);
}
'''

with tempfile.TemporaryDirectory(prefix='cemu-shader-loading-') as tmp:
    path = Path(tmp)
    (path / 'test.cpp').write_text(fixture + pool + tests)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O2', '-I' + str(repo / 'src'), str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=120)
