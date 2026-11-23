#!/usr/bin/env python3
"""Exercise production staging retirement with deterministic owner-thread time."""
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
fixture = fixture_source.split('fixture = r"""', 1)[1].split('"""', 1)[0]
tests = r'''
int main() {
 constexpr uint64_t second=1000000000ULL;
 constexpr uint32 chunk=16*1024*1024;
 MetalRenderer host;
 MetalSynchronizedRingAllocator ring(&host,0,chunk,true);
 for(int i=0;i<8;++i)ring.AllocateBufferMemory(chunk,128);
 assert(ring.TrimLongIdleBuffers(second)==0);
 assert(ring.TrimLongIdleBuffers(40*second)==0); // Unsubmitted reservations cannot retire.
 host.CompleteAndDropOwner();
 assert(ring.TrimLongIdleBuffers(41*second)==0); // Grace starts only after completion.
 assert(ring.TrimLongIdleBuffers(70*second)==0);
 assert(ring.TrimLongIdleBuffers(71*second)==6*size_t(chunk));
 uint32 count;size_t capacity,free;ring.GetStats(count,capacity,free);
 assert(count==2 && capacity==2*size_t(chunk));
 assert(ring.TrimLongIdleBuffers(120*second)==0); // Keep two warm working chunks.
 const auto retained=ring.AllocateBufferMemory(chunk,128);
 retained.memPtr[0]=71;
 const auto other=ring.AllocateBufferMemory(chunk,128);
 other.memPtr[0]=72;
 const auto grown=ring.AllocateBufferMemory(chunk,128);
 grown.memPtr[0]=73; // A vacant slot regrows without moving active indices.
 assert(ring.TrimLongIdleBuffers(160*second)==0);
 assert(retained.memPtr[0]==71 && other.memPtr[0]==72 && grown.memPtr[0]==73);
 host.CompleteAndDropOwner();
 assert(ring.TrimLongIdleBuffers(161*second)==0);
 assert(ring.TrimLongIdleBuffers(190*second)==0);
 assert(ring.TrimLongIdleBuffers(191*second)==chunk);

 // Reusing a formerly idle chunk resets its grace period.
 MetalRenderer reused;
 MetalSynchronizedRingAllocator reset(&reused,0,65536,true);
 for(int i=0;i<3;++i)reset.AllocateBufferMemory(65536,128);
 reused.CompleteAndDropOwner();reset.TrimLongIdleBuffers(second);
 reset.AllocateBufferMemory(65536,128); // preferred chunk
 auto a=reset.AllocateBufferMemory(65536,128); // formerly idle chunk
 a.memPtr[0]=44;reused.CompleteAndDropOwner();
 auto pending=reset.AllocateBufferMemory(65536,128); // use preferred chunk again
 assert(reset.TrimLongIdleBuffers(30*second)==0);
 assert(reset.TrimLongIdleBuffers(31*second)==65536); // Only the untouched old spare can retire.
 assert(a.memPtr[0]==44 && pending.mtlBuffer==a.mtlBuffer);
 reused.CompleteAndDropOwner();

 // Regression of the test clock cannot underflow into immediate retirement.
 MetalRenderer clock;
 MetalSynchronizedRingAllocator regressed(&clock,0,65536,true);
 for(int i=0;i<3;++i)regressed.AllocateBufferMemory(65536,128);
 clock.CompleteAndDropOwner();regressed.TrimLongIdleBuffers(100*second);
 assert(regressed.TrimLongIdleBuffers(50*second)==0);
 assert(regressed.TrimLongIdleBuffers(79*second)==0);
 assert(regressed.TrimLongIdleBuffers(80*second)==65536);

 MetalRenderer native;
 MetalSynchronizedRingAllocator unrestricted(&native,0,65536,false);
 for(int i=0;i<3;++i)unrestricted.AllocateBufferMemory(65536,128);
 native.CompleteAndDropOwner();
 assert(unrestricted.TrimLongIdleBuffers(second)==0);
 assert(unrestricted.TrimLongIdleBuffers(1000*second)==0);
 unrestricted.GetStats(count,capacity,free);assert(count==3 && capacity==3*65536);
 puts("PASS: constrained completed staging 128 -> 32 MiB after 30-second grace; two warm chunks retained; pending uploads, regrowth, clock rollback and Native peak reuse preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='77u-staging-aging-') as temp:
    p = Path(temp)
    (p/'test.cpp').write_text(fixture+cls+destructor+methods+tests)
    subprocess.run(['xcrun','clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined',
                    '-I'+str(repo/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
