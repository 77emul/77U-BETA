#!/usr/bin/env python3
"""Reproduce retiring a staging sync point that owns the last command-buffer reference."""
from pathlib import Path
import subprocess
import tempfile
repo = Path(__file__).resolve().parents[3]
header = (repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.h').read_text()
source = (repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.cpp').read_text()
cls = header[header.index('class MetalSynchronizedRingAllocator\n'):header.index('// heap style allocator')]
methods = source[source.index('void MetalSynchronizedRingAllocator::addUploadBufferSyncPoint'):source.index('/* MetalSynchronizedHeapAllocator */')]
destructor = source[source.index('MetalSynchronizedRingAllocator::~'):source.index('MetalBufferChunkedHeap::~')]
fixture = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include "Common/MemoryBudget.h"
#include <new>
#include <queue>
#include <vector>
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
using uint32=uint32_t; using uint8=uint8_t;
#define BOOST_OS_IOS 1
size_t availableBytes = size_t(8)*1024*1024*1024;
size_t os_proc_available_memory() { return availableBytes; }
unsigned gpuWaits=0;
#define cemu_assert_debug assert
#define cemuLog_log(...) ((void)0)
namespace NS { struct Range { Range(uint32,uint32) {} }; }
namespace MTL {
using ResourceOptions=unsigned; constexpr unsigned ResourceStorageModeManaged=1;
enum CommandBufferStatus { CommandBufferStatusNotEnqueued, CommandBufferStatusCommitted, CommandBufferStatusScheduled, CommandBufferStatusCompleted };
struct CommandBuffer {
    unsigned references=1; bool completed=false,submitted=false;
    CommandBufferStatus status() const { return completed ? CommandBufferStatusCompleted : submitted ? CommandBufferStatusCommitted : CommandBufferStatusNotEnqueued; }
    void waitUntilCompleted() { assert(submitted); completed=true; ++gpuWaits; }
    CommandBuffer* retain() { ++references; return this; }
    void release() { if (!--references) delete this; }
};
struct Buffer {
    std::vector<uint8> bytes;
    explicit Buffer(size_t size):bytes(size) {}
    void* contents() { return bytes.data(); }
    void release() { delete this; }
    void didModifyRange(NS::Range) {}
};
struct Device { bool fail=false; size_t limit=UINT32_MAX;
 size_t maxBufferLength() const {return limit;}
 Buffer* newBuffer(size_t size, ResourceOptions) { return fail ? nullptr : new Buffer(size); } };
}
bool CommandBufferCompleted(MTL::CommandBuffer* cb) { return cb->completed; }
unsigned GetResourceOptions(unsigned value) { return value; }
enum class MetalEncoderType { Render, Blit };
struct MetalRenderer {
    MTL::Device device; MTL::CommandBuffer* current=nullptr;
    MTL::Device* GetDevice() { return &device; }
    MTL::CommandBuffer* GetCommandBuffer() {
        if (!current) current=new MTL::CommandBuffer;
        return current;
    }
    void ProcessFinishedCommandBuffers() {}
    void* GetCommandEncoder() { return nullptr; }
    MetalEncoderType GetEncoderType() { return MetalEncoderType::Blit; }
    void Flush(bool) {}
    void CompleteAndDropOwner() { current->completed=true; current->release(); current=nullptr; }
};
"""
tests=r"""
int main(int argc,char** argv) {
    MetalRenderer renderer;
    MetalSynchronizedRingAllocator ring(&renderer,0,65536);
    auto first=ring.AllocateBufferMemory(128,128);
    first.memPtr[0]=42;
    renderer.CompleteAndDropOwner(); // queued sync point is now the only owner
    if (argc>1) ring.CleanupBuffer(nullptr);
    auto next=ring.AllocateBufferMemory(128,128);
    assert(next.mtlBuffer==first.mtlBuffer);
    renderer.CompleteAndDropOwner();
    ring.CleanupBuffer(nullptr);
    puts("PASS: completed staging command buffers retire safely and ring is reused");
}
"""
with tempfile.TemporaryDirectory(prefix='cemu-staging-life-') as tmp:
    path=Path(tmp); (path/'test.cpp').write_text(fixture+cls+destructor+methods+tests)
    subprocess.run(['xcrun','clang++','-std=c++20','-O1','-fsanitize=address','-g','-I'+str(repo/'src'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    subprocess.run([str(path/'test'),'cleanup'],check=True)
