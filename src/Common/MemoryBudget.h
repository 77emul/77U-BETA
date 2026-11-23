#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

// Process memory policy for iOS devices. All figures describe what jetsam
// enforces in practice, not documented Apple limits.
namespace MemoryBudget
{
inline constexpr uint64_t MiB = 1024 * 1024;
inline constexpr uint64_t GiB = 1024 * MiB;

// Whole-process target on 4/6 GiB devices: 2.5 GiB, but never above what iOS
// grants this process (footprint + os_proc_available_memory, minus a margin),
// so on 4 GiB phones the real limit wins. Cleanup starts before jetsam does.
// Larger devices keep their previous behavior: their staging reuse was never
// validated for headroom-driven release (it faulted the GPU on an iPhone 17 Pro).
inline constexpr uint64_t TargetProcessBytes = 2560 * MiB;
inline constexpr uint64_t TargetMarginBytes = 128 * MiB;

constexpr uint64_t TargetProcessBudget(uint64_t footprint, uint64_t actualAvailable)
{
    uint64_t budget = TargetProcessBytes;
    if (actualAvailable)
    {
        const uint64_t osLimit = footprint + actualAvailable;
        budget = std::min(budget, osLimit > TargetMarginBytes ? osLimit - TargetMarginBytes : 0);
    }
    return budget;
}

// Startup JIT code reservation. Every page is dirtied when StikDebug blesses it,
// so it counts fully against the iOS limit. Size by the smaller of the RAM class
// and the real process allowance (footprint + os_proc_available_memory, 0 if
// unknown): a 12 GiB iPhone without the increased-memory entitlement gets only
// ~3.3 GiB and was jetsam-killed in BotW with 512 MiB reserved for ~105 MiB of code.
constexpr uint64_t JITCodeReservationBytes(uint64_t physicalBytes, uint64_t processAllowanceBytes)
{
    const auto atMost = [&](uint64_t physicalLimit, uint64_t allowanceLimit) {
        return (physicalBytes && physicalBytes <= physicalLimit) ||
            (processAllowanceBytes && processAllowanceBytes <= allowanceLimit);
    };
    if (atMost(4 * GiB, 2560 * MiB))
        return 160 * MiB; // iPhone 12/13 class: ~2 GiB jetsam limit
    if (atMost(6 * GiB, 4 * GiB))
        return 256 * MiB;
    return 512 * MiB;
}

constexpr uint64_t LimitHeadroom(uint64_t actualAvailable, uint64_t footprint, uint64_t budget)
{
    return budget ? std::min(actualAvailable, footprint < budget ? budget - footprint : 0) : actualAvailable;
}

inline std::atomic<bool> processTargetEnabled{false}; // set by the renderer for constrained devices
inline uint64_t (*footprintProvider)() = nullptr;

// Headroom against the process target where it is enabled.
inline uint64_t EffectiveAvailableMemory(uint64_t actualAvailable)
{
    if (!footprintProvider || !processTargetEnabled.load(std::memory_order_relaxed))
        return actualAvailable;
    const uint64_t footprint = footprintProvider();
    return LimitHeadroom(actualAvailable, footprint, TargetProcessBudget(footprint, actualAvailable));
}
}
