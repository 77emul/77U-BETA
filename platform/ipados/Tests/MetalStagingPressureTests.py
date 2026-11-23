#!/usr/bin/env python3
"""Exercise production ring reclamation with completed and in-flight upload batches."""
from pathlib import Path
import subprocess
import tempfile
repo = Path(__file__).resolve().parents[3]
header = (repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.h').read_text()
source = (repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.cpp').read_text()
cls = header[header.index('class MetalSynchronizedRingAllocator\n'):header.index('// heap style allocator')]
methods = source[source.index('void MetalSynchronizedRingAllocator::addUploadBufferSyncPoint'):source.index('/* MetalSynchronizedHeapAllocator */')]
destructor = source[source.index('MetalSynchronizedRingAllocator::~'):source.index('MetalBufferChunkedHeap::~')]
fixture_source = (repo/'platform/ipados/Tests/MetalStagingLifetimeTests.py').read_text()
fixture = fixture_source.split('fixture = r"""',1)[1].split('"""',1)[0]
fixture += '\n#undef TARGET_OS_IPHONE\n#define TARGET_OS_IPHONE 1\n'
if 'TrimIdleBuffers(' not in cls:
    cls = cls.replace('public:', 'public:\n size_t TrimIdleBuffers() { return 0; }', 1)
tests = r"""
int main() {
    constexpr size_t MiB=1024*1024;
    MetalRenderer renderer;
    MetalSynchronizedRingAllocator ring(&renderer,0,64*MiB);
    for (int i=0;i<3;++i) ring.AllocateBufferMemory(64*MiB,128);
    uint32 count; size_t total,free;
    ring.GetStats(count,total,free);
    assert(total==192*MiB);
    assert(ring.TrimIdleBuffers()==0); // no submitted/completed GPU work yet
    ring.GetStats(count,total,free); assert(total==192*MiB);
    renderer.CompleteAndDropOwner();
    ring.CleanupBuffer(nullptr);
    const size_t released=ring.TrimIdleBuffers();
    ring.GetStats(count,total,free);
    assert(released==128*MiB && count==1 && total==64*MiB);
    assert(RuntimePerformance::stagingBufferBytes.load()==64*MiB);
    auto reused=ring.AllocateBufferMemory(128,128);
    reused.memPtr[0]=77;
    assert(ring.GetBufferByIndex(reused.bufferIndex)==reused.mtlBuffer);
    auto active=ring.AllocateBufferMemory(64*MiB,128);
    active.memPtr[0]=88;
    assert(ring.TrimIdleBuffers()==0);
    assert(reused.memPtr[0]==77 && active.memPtr[0]==88);
    renderer.CompleteAndDropOwner();
    ring.CleanupBuffer(nullptr);
    assert(ring.TrimIdleBuffers()==64*MiB);
    // A currently busy last chunk must not pin completed earlier chunks.
    MetalRenderer streaming;
    MetalSynchronizedRingAllocator burst(&streaming,0,64*MiB);
    for (int i=0;i<3;++i) burst.AllocateBufferMemory(64*MiB,128);
    streaming.CompleteAndDropOwner();
    auto pending=burst.AllocateBufferMemory(128,128); // preferred last chunk
    pending.memPtr[0]=99;
    assert(burst.TrimIdleBuffers()==128*MiB);
    assert(burst.GetBufferByIndex(pending.bufferIndex)==pending.mtlBuffer);
    assert(pending.memPtr[0]==99);
    auto regrown=burst.AllocateBufferMemory(64*MiB,128);
    assert(regrown.bufferIndex!=pending.bufferIndex);
    assert(burst.GetBufferByIndex(pending.bufferIndex)==pending.mtlBuffer);
    streaming.CompleteAndDropOwner();
    burst.CleanupBuffer(nullptr);
    // A full ring recycles submitted work instead of growing indefinitely.
    MetalRenderer bounded;
    MetalSynchronizedRingAllocator queue(&bounded,0,65536,true);
    for(unsigned i=0;i<16;++i) {
        auto chunk=queue.AllocateBufferMemory(65536,128);
        chunk.memPtr[0]=uint8(i);
        bounded.current->submitted=true;
        bounded.current->release();bounded.current=nullptr;
    }
    queue.GetStats(count,total,free);assert(count==16);
    const unsigned waitsBefore=gpuWaits;
    queue.AllocateBufferMemory(65536,128);
    queue.GetStats(count,total,free);
    assert(count==16 && gpuWaits==waitsBefore+1);
    bounded.CompleteAndDropOwner();queue.CleanupBuffer(nullptr);

    // Low headroom reclaims an older submitted batch before reaching the cap.
    MetalRenderer low;
    MetalSynchronizedRingAllocator lowRing(&low,0,65536,true);
    auto prior=lowRing.AllocateBufferMemory(65536,128);
    low.current->submitted=true;low.current->release();low.current=nullptr;
    availableBytes=64*MiB;
    auto recycled=lowRing.AllocateBufferMemory(65536,128);
    assert(recycled.mtlBuffer==prior.mtlBuffer);
    low.CompleteAndDropOwner();lowRing.CleanupBuffer(nullptr);
    availableBytes=size_t(8)*1024*MiB;

    // Allocation failure may also reuse submitted work, without a global flush.
    MetalRenderer failed;
    MetalSynchronizedRingAllocator recovery(&failed,0,65536);
    auto old=recovery.AllocateBufferMemory(65536,128);
    failed.current->submitted=true;failed.current->release();failed.current=nullptr;
    failed.device.fail=true;
    assert(recovery.AllocateBufferMemory(65536,128).mtlBuffer==old.mtlBuffer);
    failed.CompleteAndDropOwner();recovery.CleanupBuffer(nullptr);

    // Live unsubmitted reservations survive growth and trimming unchanged.
    MetalRenderer live;
    MetalSynchronizedRingAllocator growing(&live,0,65536);
    std::vector<MetalSynchronizedRingAllocator::AllocatorReservation_t> pendingReservations;
    const unsigned noWaits=gpuWaits;
    for(unsigned i=0;i<18;++i) {auto r=growing.AllocateBufferMemory(65536,128);r.memPtr[0]=uint8(i);pendingReservations.push_back(r);}
    assert(gpuWaits==noWaits);
    for(unsigned i=0;i<pendingReservations.size();++i)assert(pendingReservations[i].memPtr[0]==i);
    assert(growing.TrimIdleBuffers()==0);
    live.CompleteAndDropOwner();growing.CleanupBuffer(nullptr);

    bool rejected=false;
    try { growing.AllocateBufferMemory(UINT32_MAX,128); } catch(const std::bad_alloc&) {rejected=true;}
    assert(rejected);
    MetalRenderer limited;limited.device.limit=32768;
    MetalSynchronizedRingAllocator deviceLimit(&limited,0,65536);
    rejected=false;try {deviceLimit.AllocateBufferMemory(128,128);}catch(const std::bad_alloc&) {rejected=true;}
    assert(rejected);
    puts("PASS: full/low-headroom/allocation-failure rings reclaim only submitted work; live uploads survive; overflow/device limits rejected");
    puts("PASS: 192 -> 64 MiB idle staging; in-flight data survives; trimmed ring regrows safely");
}
"""
with tempfile.TemporaryDirectory(prefix='cemu-staging-pressure-') as tmp:
    path=Path(tmp)
    (path/'test.cpp').write_text(fixture+cls+destructor+methods+tests)
    subprocess.run(['xcrun','clang++','-std=c++20','-O1','-fsanitize=address,undefined','-g','-I'+str(repo/'src'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
