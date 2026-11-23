#pragma once
#include <cstdint>
namespace TextureCacheRetention {
// Unsigned elapsed values retain the cache's existing tick/frame wrap behavior.
// These are cache reuse lifetimes, not process budgets or iOS memory limits.
constexpr bool ShouldEvictRAMCopy(bool constrained, bool independentRAMCopy,
                                  uint32_t idleMs, uint32_t idleFrames) {
    return constrained && independentRAMCopy
        ? idleMs >= 15000 && idleFrames >= 450 // 4/6 GiB: reconstructable copies go sooner
        : idleMs >= 120000 && idleFrames >= 2000;
}
}
