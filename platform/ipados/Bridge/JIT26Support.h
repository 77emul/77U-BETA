#pragma once
#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/// Low-level StikDebug / Universal iOS 26 breakpoint helpers.
/// Triggers BRK #0xf00d with x16=1 (JIT26PrepareRegion).
void* JIT26PrepareRegion(void* addr, size_t len);

/// Triggers BRK #0xf00d with x16=0 (JIT26Detach).
void JIT26Detach(void);

/// Generic iPadOS JIT code region allocator for AArch64 recompiler.
/// Prepares a unified RWX/executable memory buffer for the JIT code cache.
void* JIT26_AllocateCodeRegion(size_t size);

/// Live JIT26 memory allocation statistics
struct JIT26MemoryStats {
    uint64_t allocationCount;
    uint64_t requestedBytes;
    uint64_t allocatedBytes;
    uint64_t largestRequest;
    size_t arenaCapacityMB;
    size_t arenaUsedMB;
};

/// Get live arena allocation statistics (aggregate memory metrics).
void JIT26_GetMemoryStats(struct JIT26MemoryStats* outStats);

/// Get live arena allocation statistics (block count and MB used).
void JIT26_GetArenaStats(size_t* outBlocks, size_t* outUsedMB);

/// Query and log Mach VM region protection information for a pointer.
void JIT26_QueryAndLogVM(const char* label, const void* ptr);

/// Returns true if the pointer lies within an allocated JIT arena chunk (RW or RX).
bool JIT26_IsAddressInArena(const void* ptr);

/// Async-signal-safe check without mutex for exception/signal handlers.
bool JIT26_IsAddressInArenaAsyncSafe(const void* ptr);

/// Returns true if the pointer is within the writable RW alias of any JIT chunk.
bool JIT26_IsRWAddress(const void* ptr);

/// Returns true if the pointer is within the executable RX mapping of any JIT chunk.
bool JIT26_IsRXAddress(const void* ptr);

/// Translate a writable RW alias pointer to its corresponding executable RX pointer.
void* JIT26_RWToRX(const void* rwPtr);

/// Translate an executable RX pointer to its corresponding writable RW alias pointer.
void* JIT26_RXToRW(const void* rxPtr);

/// Finalize dynamic code emitted in the RW alias: flushes RX icache and returns RX pointer.
void* JIT26_FinalizeCode(void* rwCode, size_t size);

/// Reset the arena offset so that the existing pre-allocated dual-mapped chunk(s)
/// can be cleanly reused from offset 0 for a new title without making new BRKs.
void JIT26_ResetArena(void);

#ifdef __cplusplus
}
#endif
