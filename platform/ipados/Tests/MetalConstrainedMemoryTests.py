#!/usr/bin/env python3
"""Run production staging allocation under small-memory and legacy policies."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
header = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.h').read_text()
source = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.cpp').read_text()
cls = header[header.index('class MetalSynchronizedRingAllocator\n'):header.index('// heap style allocator')]
methods = source[source.index('void MetalSynchronizedRingAllocator::addUploadBufferSyncPoint'):source.index('/* MetalSynchronizedHeapAllocator */')]
destructor = source[source.index('MetalSynchronizedRingAllocator::~'):source.index('MetalBufferChunkedHeap::~')]
fixture_source = (repo / 'platform/ipados/Tests/MetalStagingLifetimeTests.py').read_text()
fixture = fixture_source.split('fixture = r"""', 1)[1].split('"""', 1)[0]
fixture = fixture.replace('void* GetCommandEncoder() { return nullptr; }',
                          'void* GetCommandEncoder() { return reinterpret_cast<void*>(1); }')
fixture = fixture.replace('MetalEncoderType GetEncoderType() { return MetalEncoderType::Blit; }',
                          'MetalEncoderType GetEncoderType() { return MetalEncoderType::Render; }')
fixture = fixture.replace('void Flush(bool) {}', 'void Flush(bool) { assert(false && "must not flush an active pass"); }')
fixture += '\n#undef TARGET_OS_IPHONE\n#define TARGET_OS_IPHONE 1\n'
fixture += '#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryPolicy.h"\n'

tests = r"""
int main() {
    constexpr uint64_t GiB = uint64_t{1024} * 1024 * 1024;
    constexpr size_t MiB = 1024 * 1024;
    for (uint64_t ram : {4 * GiB, 6 * GiB}) {
        const auto policy = MakeMetalMemoryPolicy(ram);
        assert(policy.constrained);
        assert(policy.stagingChunkBytes == 16 * MiB);
        assert(policy.indexChunkBytes == 4 * MiB);
        assert(policy.CompilationWorkers(8) == 2 && policy.CompilationWorkers(1) == 1);
        MetalRenderer renderer;
        MetalSynchronizedRingAllocator ring(&renderer, 0, policy.stagingChunkBytes, policy.constrained);
        auto small = ring.AllocateBufferMemory(128, 128);
        small.memPtr[0] = 42;
        uint32 count; size_t total, free;
        ring.GetStats(count, total, free);
        assert(count == 1 && total == 16 * MiB);
        auto large = ring.AllocateBufferMemory(20 * MiB, 128);
        large.memPtr[large.size - 1] = 43;
        assert(large.size >= 20 * MiB && small.memPtr[0] == 42);
        ring.GetStats(count, total, free);
        assert(count == 2 && total == 48 * MiB); // whole requests, never truncated
        renderer.CompleteAndDropOwner(); ring.CleanupBuffer(nullptr);
    }
    for (uint64_t ram : {uint64_t{0}, 8 * GiB, 12 * GiB, 16 * GiB}) {
        const auto policy = MakeMetalMemoryPolicy(ram);
        assert(!policy.constrained && policy.stagingChunkBytes == 64 * MiB);
        assert(policy.indexChunkBytes == 16 * MiB && policy.CompilationWorkers(8) == 8);
    }
    uint32 count; size_t total, free;
    // A full constrained ring waits only a submitted batch, then reuses it.
    {
        MetalRenderer renderer;
        MetalSynchronizedRingAllocator ring(&renderer, 0, 65536, true);
        for (unsigned i = 0; i < 16; ++i) {
            auto chunk = ring.AllocateBufferMemory(65536, 128);
            chunk.memPtr[0] = uint8(i);
            renderer.current->submitted = true;
            renderer.current->release(); renderer.current = nullptr;
        }
        const auto waits = gpuWaits;
        ring.AllocateBufferMemory(65536, 128);
        ring.GetStats(count, total, free);
        assert(count == 16 && gpuWaits == waits + 1);
        renderer.CompleteAndDropOwner(); ring.CleanupBuffer(nullptr);
    }
    // Headroom backpressure starts before the cap for 4/6 GB devices only.
    for (bool constrained : {false, true}) {
        MetalRenderer renderer;
        MetalSynchronizedRingAllocator ring(&renderer, 0, 65536, constrained);
        auto old = ring.AllocateBufferMemory(65536, 128);
        renderer.current->submitted = true;
        renderer.current->release(); renderer.current = nullptr;
        availableBytes = 64 * MiB;
        const auto waits = gpuWaits;
        auto next = ring.AllocateBufferMemory(65536, 128);
        assert((next.mtlBuffer == old.mtlBuffer) == constrained);
        assert(gpuWaits == waits + unsigned(constrained));
        renderer.CompleteAndDropOwner(); ring.CleanupBuffer(nullptr);
        availableBytes = 8 * GiB;
    }
    // Newer devices retain their previous growth and never wait at the cap.
    {
        MetalRenderer renderer;
        MetalSynchronizedRingAllocator ring(&renderer, 0, 65536, false);
        const auto waits = gpuWaits;
        for (unsigned i = 0; i < 18; ++i) {
            ring.AllocateBufferMemory(65536, 128);
            renderer.current->submitted = true;
            renderer.current->release(); renderer.current = nullptr;
        }
        ring.GetStats(count, total, free);
        assert(count == 18 && gpuWaits == waits);
    }
    // Unsubmitted decoder / draw bytes survive pressure and growth unchanged.
    {
        MetalRenderer renderer;
        MetalSynchronizedRingAllocator ring(&renderer, 0, 65536, true);
        std::vector<MetalSynchronizedRingAllocator::AllocatorReservation_t> live;
        availableBytes = 64 * MiB;
        const auto waits = gpuWaits;
        for (unsigned i = 0; i < 20; ++i) {
            auto chunk = ring.AllocateBufferMemory(65536, 128);
            chunk.memPtr[0] = uint8(i); live.push_back(chunk);
        }
        assert(gpuWaits == waits && ring.TrimIdleBuffers() == 0);
        for (unsigned i = 0; i < live.size(); ++i) assert(live[i].memPtr[0] == i);
        renderer.CompleteAndDropOwner(); ring.CleanupBuffer(nullptr);
        assert(ring.TrimIdleBuffers() == 19 * 65536);
        availableBytes = 8 * GiB;
    }
    // Failed allocation recycles submitted work even inside an active pass.
    {
        MetalRenderer renderer;
        MetalSynchronizedRingAllocator ring(&renderer, 0, 65536, true);
        auto old = ring.AllocateBufferMemory(65536, 128);
        renderer.current->submitted = true;
        renderer.current->release(); renderer.current = nullptr;
        renderer.device.fail = true;
        assert(ring.AllocateBufferMemory(65536, 128).mtlBuffer == old.mtlBuffer);
        renderer.CompleteAndDropOwner(); ring.CleanupBuffer(nullptr);
    }
    // Neither allocator arithmetic nor the device buffer limit may truncate.
    {
        MetalRenderer renderer;
        MetalSynchronizedRingAllocator ring(&renderer, 0, 65536, true);
        auto live = ring.AllocateBufferMemory(65536, 128);
        live.memPtr[0] = 123;
        renderer.device.fail = true;
        const auto waits = gpuWaits;
        bool rejected = false;
        try { ring.AllocateBufferMemory(65536, 128); } catch (const std::bad_alloc&) { rejected = true; }
        assert(rejected && gpuWaits == waits && live.memPtr[0] == 123);
        renderer.CompleteAndDropOwner(); ring.CleanupBuffer(nullptr);
    }
    {
        MetalRenderer renderer;
        renderer.device.limit = 32768;
        MetalSynchronizedRingAllocator ring(&renderer, 0, 65536, true);
        bool rejected = false;
        try { ring.AllocateBufferMemory(128, 128); } catch (const std::bad_alloc&) { rejected = true; }
        assert(rejected);
        rejected = false;
        try { ring.AllocateBufferMemory(UINT32_MAX, 128); } catch (const std::bad_alloc&) { rejected = true; }
        assert(rejected);
    }
    assert(RuntimePerformance::stagingBufferBytes.load() == 0);
    puts("PASS: 4/6 GB policy, unchanged high-end growth, submitted-only backpressure, live reservation lifetime, large uploads and allocation failure");
}
"""
with tempfile.TemporaryDirectory(prefix='cemu-constrained-memory-') as tmp:
    path = Path(tmp)
    (path / 'test.cpp').write_text(fixture + cls + destructor + methods + tests)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O1', '-fsanitize=address,undefined', '-g',
                    '-I' + str(repo / 'src'), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
