#include "JIT26Support.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <setjmp.h>
#include <vector>
#include <mutex>
#include <chrono>
#include <pthread.h>
#include <mach/mach.h>
#include <libkern/OSCacheControl.h>
#include <sys/sysctl.h>
#include <os/proc.h>
#include "Common/MemoryBudget.h"

extern "C" void CemuBridge_LogJITMilestone(const char* message);
extern "C" bool CemuBridge_IsXcodeDebuggerAttached(void);

#if defined(__aarch64__) || defined(__arm64__)
__attribute__((naked, noinline))
static void* JIT26_ExecuteBrk0xf00d(void* addr, size_t len)
{
    __asm__ volatile(
        "mov x16, #1\n"
        "brk #0xf00d\n"
        "ret\n"
    );
}

__attribute__((naked, noinline))
static void JIT26_ExecuteDetachBrk(void)
{
    __asm__ volatile(
        "mov x16, #0\n"
        "brk #0xf00d\n"
        "ret\n"
    );
}

void* JIT26PrepareRegion(void* addr, size_t len)
{
    // If running attached to Xcode / Desktop Tools, Xcode is the debugger (not StikJIT).
    // Executing brk #0xf00d causes an unhandled EXC_BREAKPOINT in Xcode LLDB.
    if (CemuBridge_IsXcodeDebuggerAttached()) {
        CemuBridge_LogJITMilestone("[JIT26] Xcode LLDB debugger detected - skipping brk #0xf00d (StikJIT cannot attach while Xcode is attached)");
        return nullptr;
    }

    return JIT26_ExecuteBrk0xf00d(addr, len);
}

void JIT26Detach(void)
{
    if (CemuBridge_IsXcodeDebuggerAttached()) {
        return;
    }

    JIT26_ExecuteDetachBrk();
}
#else
void* JIT26PrepareRegion(void* addr, size_t len) { return addr; }
void JIT26Detach(void) {}
#endif

extern "C" void JIT26_QueryAndLogVM(const char* label, const void* ptr)
{
    if (!ptr) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[JIT VM] %s=(null)", label);
        CemuBridge_LogJITMilestone(buf);
        return;
    }

    vm_address_t address = (vm_address_t)ptr;
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name;
    kern_return_t kr = vm_region_64(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object_name);
    if (kr == KERN_SUCCESS) {
        char curProt[8] = "---";
        char maxProt[8] = "---";
        if (info.protection & VM_PROT_READ) curProt[0] = 'r';
        if (info.protection & VM_PROT_WRITE) curProt[1] = 'w';
        if (info.protection & VM_PROT_EXECUTE) curProt[2] = 'x';
        if (info.max_protection & VM_PROT_READ) maxProt[0] = 'r';
        if (info.max_protection & VM_PROT_WRITE) maxProt[1] = 'w';
        if (info.max_protection & VM_PROT_EXECUTE) maxProt[2] = 'x';

        char buf[256];
        snprintf(buf, sizeof(buf), "[JIT VM] %s=%p region=0x%llx size=0x%llx curProt=%s maxProt=%s exec=%s",
                 label, ptr, (unsigned long long)address, (unsigned long long)size,
                 curProt, maxProt, (info.protection & VM_PROT_EXECUTE) ? "YES" : "NO");
        CemuBridge_LogJITMilestone(buf);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "[JIT VM] %s=%p query failed kr=%d", label, ptr, kr);
        CemuBridge_LogJITMilestone(buf);
    }
}

class JIT26Arena
{
private:
    std::mutex m_lock;
    struct DualChunk {
        uint8_t* rxBase = nullptr;
        uint8_t* rwBase = nullptr;
        size_t size = 0;
        size_t used = 0;
    };
    std::vector<DualChunk> m_chunks;
    // StikDebug services the allocation breakpoint during startup, but not while
    // a title is already running. Reserve enough executable space in that one
    // startup allocation so normal code-cache growth needs no later BRK.
    // StikDebug's prepare_memory_region dirties every page of the chunk, so
    // the whole reservation counts toward the process footprint. Size: see
    // JITCodeReservationBytes. Splatoon needs ~37 MiB of code and BotW
    // ~105 MiB. If it ever fills up, new functions stay interpreted (slower,
    // never a crash).
    static size_t ChunkSizeForDevice()
    {
        uint64_t physical = 0;
        size_t length = sizeof(physical);
        if (sysctlbyname("hw.memsize", &physical, &length, nullptr, 0) != 0 || length != sizeof(physical))
            physical = 0;
        // Real iOS allowance; 0 (unknown) keeps the RAM-class choice.
        uint64_t allowance = 0;
        task_vm_info_data_t vmInfo{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        const size_t available = os_proc_available_memory();
        if (available && task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmInfo, &count) == KERN_SUCCESS)
            allowance = vmInfo.phys_footprint + available;
        return (size_t)MemoryBudget::JITCodeReservationBytes(physical, allowance);
    }
    static constexpr size_t SUBALLOC_ALIGNMENT = 64; // 64-byte alignment for code blocks

    uint64_t m_allocationCount = 0;
    uint64_t m_chunkCount = 0;
    uint64_t m_requestedBytes = 0;
    uint64_t m_allocatedBytes = 0;
    uint64_t m_largestRequest = 0;

    struct ChunkRange {
        uintptr_t rxStart;
        uintptr_t rxEnd;
        uintptr_t rwStart;
        uintptr_t rwEnd;
    };
    static constexpr size_t MAX_ASYNC_CHUNKS = 16;
    ChunkRange m_asyncChunks[MAX_ASYNC_CHUNKS]{};
    std::atomic<uint32_t> m_asyncChunkCount{0};

    void UpdateBounds(const DualChunk& c)
    {
        uintptr_t rwStart = (uintptr_t)c.rwBase;
        uintptr_t rwEnd = rwStart + c.size;
        uintptr_t rxStart = (uintptr_t)c.rxBase;
        uintptr_t rxEnd = rxStart + c.size;
        uint32_t idx = m_asyncChunkCount.load(std::memory_order_relaxed);
        if (idx < MAX_ASYNC_CHUNKS) {
            m_asyncChunks[idx] = {rxStart, rxEnd, rwStart, rwEnd};
            m_asyncChunkCount.store(idx + 1, std::memory_order_release);
        }
    }

public:
    bool Prepare()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_chunks.empty()) return true;
        DualChunk chunk = AllocateNewDualChunk(ChunkSizeForDevice());
        if (!chunk.rwBase) return false;
        UpdateBounds(chunk);
        m_chunks.push_back(chunk);
        return true;
    }

    void* Allocate(size_t size)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_allocationCount++;
        m_requestedBytes += size;
        if (size > m_largestRequest)
            m_largestRequest = size;

        // Align requested size to 64 bytes for ARM64 cache line / instruction alignment
        size_t allocSize = (size + SUBALLOC_ALIGNMENT - 1) & ~(SUBALLOC_ALIGNMENT - 1);
        m_allocatedBytes += allocSize;

        // If arena is not yet created, initialize the startup dual-mapped chunk.
        if (m_chunks.empty()) {
            DualChunk initialChunk = AllocateNewDualChunk(ChunkSizeForDevice());
            if (!initialChunk.rwBase) return nullptr;
            UpdateBounds(initialChunk);
            initialChunk.used = allocSize;
            m_chunks.push_back(initialChunk);
            return initialChunk.rwBase;
        }

        // Suballocate from the current dual-mapped chunk.
        DualChunk& current = m_chunks.back();
        if (current.used + allocSize <= current.size) {
            void* rwPtr = current.rwBase + current.used;
            current.used += allocSize;
            return rwPtr;
        }

        // A runtime JIT26PrepareRegion() would raise an unhandled SIGTRAP once a
        // title is running, so never attempt to grow the arena here.
        char errBuf[256];
        snprintf(errBuf, sizeof(errBuf),
                 "[JIT26 ARENA EXHAUSTED] requested=0x%zx used=0x%zx capacity=0x%zx",
                 allocSize, current.used, current.size);
        CemuBridge_LogJITMilestone(errBuf);
        return nullptr;
    }

    void* RWToRX(const void* rwPtr)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        const uint8_t* p = (const uint8_t*)rwPtr;
        for (const auto& c : m_chunks) {
            if (p >= c.rwBase && p < c.rwBase + c.size) {
                size_t offset = p - c.rwBase;
                return c.rxBase + offset;
            }
        }
        return (void*)rwPtr;
    }

    void* RXToRW(const void* rxPtr)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        const uint8_t* p = (const uint8_t*)rxPtr;
        for (const auto& c : m_chunks) {
            if (p >= c.rxBase && p < c.rxBase + c.size) {
                size_t offset = p - c.rxBase;
                return c.rwBase + offset;
            }
        }
        return (void*)rxPtr;
    }

    bool IsRWAddress(const void* ptr)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        const uint8_t* p = (const uint8_t*)ptr;
        for (const auto& c : m_chunks) {
            if (p >= c.rwBase && p < c.rwBase + c.size)
                return true;
        }
        return false;
    }

    bool IsRXAddress(const void* ptr)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        const uint8_t* p = (const uint8_t*)ptr;
        for (const auto& c : m_chunks) {
            if (p >= c.rxBase && p < c.rxBase + c.size)
                return true;
        }
        return false;
    }

    bool IsAddressInArena(const void* ptr)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        const uint8_t* p = (const uint8_t*)ptr;
        for (const auto& c : m_chunks) {
            if ((p >= c.rwBase && p < c.rwBase + c.size) ||
                (p >= c.rxBase && p < c.rxBase + c.size))
                return true;
        }
        return false;
    }

    bool IsAddressInArenaAsyncSafe(const void* ptr) const
    {
        if (!ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uint32_t count = m_asyncChunkCount.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count && i < MAX_ASYNC_CHUNKS; ++i) {
            if ((p >= m_asyncChunks[i].rxStart && p < m_asyncChunks[i].rxEnd) ||
                (p >= m_asyncChunks[i].rwStart && p < m_asyncChunks[i].rwEnd)) {
                return true;
            }
        }
        return false;
    }

    void GetStats(size_t* outBlocks, size_t* outUsedMB)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (outBlocks) *outBlocks = (size_t)m_allocationCount;
        if (outUsedMB) {
            size_t totalUsedBytes = 0;
            for (const auto& c : m_chunks) totalUsedBytes += c.used;
            *outUsedMB = totalUsedBytes / (1024 * 1024);
        }
    }

    void GetMemoryStats(JIT26MemoryStats* outStats)
    {
        if (!outStats) return;
        std::lock_guard<std::mutex> lock(m_lock);
        outStats->allocationCount = m_allocationCount;
        outStats->requestedBytes = m_requestedBytes;
        outStats->allocatedBytes = m_allocatedBytes;
        outStats->largestRequest = m_largestRequest;
        size_t totalCapacity = 0;
        size_t totalUsed = 0;
        for (const auto& c : m_chunks) {
            totalCapacity += c.size;
            totalUsed += c.used;
        }
        outStats->arenaCapacityMB = totalCapacity / (1024 * 1024);
        outStats->arenaUsedMB = totalUsed / (1024 * 1024);
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto& c : m_chunks) {
            c.used = 0;
        }
        m_allocationCount = 0;
        m_requestedBytes = 0;
        m_allocatedBytes = 0;
        m_largestRequest = 0;
        CemuBridge_LogJITMilestone("[JIT26 ARENA] Reset for new title - reused existing dual-mapped chunks");
    }

private:
    DualChunk AllocateNewDualChunk(size_t chunkSize)
    {
        m_chunkCount++;
        uint64_t chunkIdx = m_chunkCount;
        uint64_t tid = 0;
        pthread_threadid_np(NULL, &tid);

        char logBuf[384];
        snprintf(logBuf, sizeof(logBuf),
                 "[JIT26 CHUNK #%llu] BEGIN size=0x%zx (%.1f MB, thread=0x%llx)",
                 (unsigned long long)chunkIdx, chunkSize, (double)chunkSize / (1024.0 * 1024.0), (unsigned long long)tid);
        CemuBridge_LogJITMilestone(logBuf);

        // RX mapping via StikDebug _M<size>,rx + prepare_memory_region
        snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] PREPARE_BEGIN", (unsigned long long)chunkIdx);
        CemuBridge_LogJITMilestone(logBuf);

        auto tPrepareStart = std::chrono::steady_clock::now();
        void* rxPtr = JIT26PrepareRegion(nullptr, chunkSize);
        auto tPrepareEnd = std::chrono::steady_clock::now();
        auto prepareMs = std::chrono::duration_cast<std::chrono::milliseconds>(tPrepareEnd - tPrepareStart).count();

        snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] PREPARE_END rx=%p elapsedMs=%lld",
                 (unsigned long long)chunkIdx, rxPtr, (long long)prepareMs);
        CemuBridge_LogJITMilestone(logBuf);

        if (rxPtr == nullptr || rxPtr == (void*)-1) {
            snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] FAILED: JIT26PrepareRegion(nullptr) returned error", (unsigned long long)chunkIdx);
            CemuBridge_LogJITMilestone(logBuf);
            return DualChunk{};
        }

        vm_address_t rxAddress = (vm_address_t)rxPtr;
        vm_size_t rxSize = 0;
        vm_region_basic_info_data_64_t rxInfo;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object_name;
        kern_return_t kr_rx = vm_region_64(mach_task_self(), &rxAddress, &rxSize, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&rxInfo, &count, &object_name);

        char rxCurProt[8] = "---";
        if (kr_rx == KERN_SUCCESS) {
            if (rxInfo.protection & VM_PROT_READ) rxCurProt[0] = 'r';
            if (rxInfo.protection & VM_PROT_WRITE) rxCurProt[1] = 'w';
            if (rxInfo.protection & VM_PROT_EXECUTE) rxCurProt[2] = 'x';
        }

        // RW alias onto the same physical pages via vm_remap
        snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] REMAP_BEGIN", (unsigned long long)chunkIdx);
        CemuBridge_LogJITMilestone(logBuf);

        vm_address_t rwAddress = 0;
        vm_prot_t cur_protection = 0;
        vm_prot_t max_protection = 0;
        auto tRemapStart = std::chrono::steady_clock::now();
        kern_return_t kr_remap = vm_remap(
            mach_task_self(),
            &rwAddress,
            chunkSize,
            0,
            VM_FLAGS_ANYWHERE,
            mach_task_self(),
            (vm_address_t)rxPtr,
            FALSE, // shared physical memory
            &cur_protection,
            &max_protection,
            VM_INHERIT_NONE
        );
        auto tRemapEnd = std::chrono::steady_clock::now();
        auto remapMs = std::chrono::duration_cast<std::chrono::milliseconds>(tRemapEnd - tRemapStart).count();

        snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] REMAP_END kr=%d rw=%p elapsedMs=%lld",
                 (unsigned long long)chunkIdx, kr_remap, (void*)rwAddress, (long long)remapMs);
        CemuBridge_LogJITMilestone(logBuf);

        if (kr_remap != KERN_SUCCESS || rwAddress == 0) {
            snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] FAILED: vm_remap returned kr=%d", (unsigned long long)chunkIdx, kr_remap);
            CemuBridge_LogJITMilestone(logBuf);
            return DualChunk{};
        }

        snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] PROTECT_RW_BEGIN", (unsigned long long)chunkIdx);
        CemuBridge_LogJITMilestone(logBuf);

        auto tProtectStart = std::chrono::steady_clock::now();
        kern_return_t kr_rw = vm_protect(mach_task_self(), rwAddress, chunkSize, FALSE, VM_PROT_READ | VM_PROT_WRITE);
        auto tProtectEnd = std::chrono::steady_clock::now();
        auto protectMs = std::chrono::duration_cast<std::chrono::milliseconds>(tProtectEnd - tProtectStart).count();

        snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] PROTECT_RW_END kr=%d elapsedMs=%lld",
                 (unsigned long long)chunkIdx, kr_rw, (long long)protectMs);
        CemuBridge_LogJITMilestone(logBuf);

        // Verify the alias protection
        vm_address_t qAddr = rwAddress;
        vm_size_t qSize = 0;
        vm_region_basic_info_data_64_t rwInfo;
        count = VM_REGION_BASIC_INFO_COUNT_64;
        kern_return_t kr_rw_query = vm_region_64(mach_task_self(), &qAddr, &qSize, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&rwInfo, &count, &object_name);

        char rwCurProt[8] = "---";
        if (kr_rw_query == KERN_SUCCESS) {
            if (rwInfo.protection & VM_PROT_READ) rwCurProt[0] = 'r';
            if (rwInfo.protection & VM_PROT_WRITE) rwCurProt[1] = 'w';
            if (rwInfo.protection & VM_PROT_EXECUTE) rwCurProt[2] = 'x';
        }

        snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] VERIFY rxProt=%s rwProt=%s",
                 (unsigned long long)chunkIdx, rxCurProt, rwCurProt);
        CemuBridge_LogJITMilestone(logBuf);

        size_t totalReservedMB = (m_chunks.size() + 1) * (chunkSize / (1024 * 1024));
        snprintf(logBuf, sizeof(logBuf), "[JIT26 CHUNK #%llu] READY totalChunks=%zu totalRXReservedMB=%zu",
                 (unsigned long long)chunkIdx, m_chunks.size() + 1, totalReservedMB);
        CemuBridge_LogJITMilestone(logBuf);

        DualChunk chunk;
        chunk.rxBase = (uint8_t*)rxPtr;
        chunk.rwBase = (uint8_t*)rwAddress;
        chunk.size = chunkSize;
        chunk.used = 0;
        return chunk;
    }
};

static JIT26Arena s_jitArena;

extern "C" bool JIT26_PrepareArena(void)
{
    return s_jitArena.Prepare();
}

extern "C" void* JIT26_AllocateCodeRegion(size_t size)
{
    return s_jitArena.Allocate(size);
}

extern "C" void JIT26_GetMemoryStats(struct JIT26MemoryStats* outStats)
{
    s_jitArena.GetMemoryStats(outStats);
}

extern "C" void JIT26_GetArenaStats(size_t* outBlocks, size_t* outUsedMB)
{
    s_jitArena.GetStats(outBlocks, outUsedMB);
}

extern "C" bool JIT26_IsAddressInArena(const void* ptr)
{
    return s_jitArena.IsAddressInArena(ptr);
}

extern "C" bool JIT26_IsAddressInArenaAsyncSafe(const void* ptr)
{
    return s_jitArena.IsAddressInArenaAsyncSafe(ptr);
}

extern "C" bool JIT26_IsRWAddress(const void* ptr)
{
    return s_jitArena.IsRWAddress(ptr);
}

extern "C" bool JIT26_IsRXAddress(const void* ptr)
{
    return s_jitArena.IsRXAddress(ptr);
}

extern "C" void* JIT26_RWToRX(const void* rwPtr)
{
    return s_jitArena.RWToRX(rwPtr);
}

extern "C" void* JIT26_RXToRW(const void* rxPtr)
{
    return s_jitArena.RXToRW(rxPtr);
}

extern "C" void* JIT26_FinalizeCode(void* rwCode, size_t size)
{
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    void* rxCode = JIT26_RWToRX(rwCode);
    if (rxCode != rwCode)
    {
        // Clean the writable alias to the Point of Coherency
        sys_dcache_flush(rwCode, size);
        asm volatile("dsb ish" ::: "memory");
        // Invalidate the instruction cache for the executable alias
        sys_icache_invalidate(rxCode, size);
        // Wait for the invalidation, then re-fetch instructions
        asm volatile("dsb ish\n\tisb" ::: "memory");
        return rxCode;
    }
#elif defined(__APPLE__) && defined(__aarch64__)
    sys_dcache_flush(rwCode, size);
    asm volatile("dsb ish" ::: "memory");
    sys_icache_invalidate(rwCode, size);
    asm volatile("dsb ish\n\tisb" ::: "memory");
#endif
    return rwCode;
}

extern "C" void JIT26_ResetArena(void)
{
    s_jitArena.Reset();
}
