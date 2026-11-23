#include "Common/MemoryBudget.h"
#include <cassert>
#include <cstdio>

using namespace MemoryBudget;

static uint64_t s_footprint = 0;
static uint64_t ReadFootprint() { return s_footprint; }

int main()
{
    footprintProvider = ReadFootprint;
    assert(EffectiveAvailableMemory(8 * GiB) == 8 * GiB); // target off: OS headroom unchanged

    processTargetEnabled = true; // as the renderer sets it on 4/6 GiB devices
    assert(EffectiveAvailableMemory(2 * GiB) == 2 * GiB - TargetMarginBytes);
    // iPhone 12 (limit ~2048 MiB): 1500 MiB used, 548 MiB left -> target 1920 MiB.
    assert(TargetProcessBudget(1500 * MiB, 548 * MiB) == 1920 * MiB);
    s_footprint = 1500 * MiB;
    assert(EffectiveAvailableMemory(548 * MiB) == 420 * MiB);
    // 8 GiB allowance: the 2.5 GiB target applies and never wraps below zero.
    assert(TargetProcessBudget(3000 * MiB, 5000 * MiB) == TargetProcessBytes);
    s_footprint = 3000 * MiB;
    assert(EffectiveAvailableMemory(5000 * MiB) == 0);

    assert(JITCodeReservationBytes(4 * GiB, 0) == 160 * MiB);
    assert(JITCodeReservationBytes(12 * GiB, 2 * GiB) == 160 * MiB); // no increased-memory entitlement
    assert(JITCodeReservationBytes(6 * GiB, 0) == 256 * MiB);
    assert(JITCodeReservationBytes(12 * GiB, 0) == 512 * MiB);
    puts("PASS: memory budget and JIT reservation policy");
}
