#!/usr/bin/env python3
"""Run production enqueue, worker and drain with a blocked compiler destructor."""
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[3]
s=(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp').read_text()
def function(name):
    a=s.index(name); opening=s.index('{',a); end=opening+1; depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}'); end+=1
    return s[a:end]
worker=function('static void compileThreadFunc(')
queue=function('static void queuePipeline(')
drain=function('void MetalPipelineCache::WaitForPendingCompilations()')
fixture=r"""
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <pthread.h>
#include <pthread/qos.h>
using uint32=unsigned; using sint32=int;
void SetThreadName(const char*) {}
namespace RuntimePerformance { std::atomic<unsigned long> pendingPipelines{0}; }
std::mutex stageMutex;
std::condition_variable stageCV;
bool compileStarted=false,compileRelease=false,destructorStarted=false,destructorRelease=false;
std::atomic<bool> drainFinished{false},destroyed{false};
struct PipelineObject {};
struct MetalPipelineCompiler {
    bool Compile(bool,bool,bool) {
        std::unique_lock lock(stageMutex);
        compileStarted=true; stageCV.notify_all();
        stageCV.wait(lock,[]{return compileRelease;});
        return true;
    }
    ~MetalPipelineCompiler() {
        std::unique_lock lock(stageMutex);
        destructorStarted=true; stageCV.notify_all();
        stageCV.wait(lock,[]{return destructorRelease;});
        destroyed=true;
    }
};
struct PendingPipelineCompile { MetalPipelineCompiler* compiler; PipelineObject* object; };
std::mutex g_compilePipelineMutex;
std::condition_variable g_compilePipelineCondVar,g_compilePipelineDrained;
uint32 g_pendingPipelineCompiles=0;
std::deque<PendingPipelineCompile> g_compilePipelineRequests;
struct MetalPipelineCache { void EndLoading() {} void WaitForPendingCompilations(); };
""" + worker + queue + drain + r"""
int main() {
    MetalPipelineCache cache; PipelineObject object;
    queuePipeline(new MetalPipelineCompiler,&object);
    {
        std::lock_guard lock(g_compilePipelineMutex);
        if(g_pendingPipelineCompiles!=1 || RuntimePerformance::pendingPipelines!=1) return 1;
    }
    std::thread(compileThreadFunc,0).detach();
    std::thread shutdown([&]{cache.WaitForPendingCompilations();drainFinished=true;});
    {
        std::unique_lock lock(stageMutex);
        if(!stageCV.wait_for(lock,std::chrono::seconds(2),[]{return compileStarted;})) std::_Exit(2);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if(drainFinished) std::_Exit(3);
        compileRelease=true; stageCV.notify_all();
        if(!stageCV.wait_for(lock,std::chrono::seconds(2),[]{return destructorStarted;})) std::_Exit(4);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if(drainFinished || destroyed) std::_Exit(5);
        destructorRelease=true; stageCV.notify_all();
    }
    shutdown.join();
    {
        std::lock_guard lock(g_compilePipelineMutex);
        if(!destroyed || g_pendingPipelineCompiles!=0 || RuntimePerformance::pendingPipelines!=0) std::_Exit(6);
    }
    puts("PASS: real worker drain waits through compiler deletion, not just Metal compilation");
    fflush(stdout);
    // Production workers are process-lifetime detached threads. End this test
    // process without destroying the queue while that worker is idle on it.
    std::_Exit(0);
}
"""
with tempfile.TemporaryDirectory(prefix='cemu-pipeline-worker-') as tmp:
    p=Path(tmp); (p/'test.cpp').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O3','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
