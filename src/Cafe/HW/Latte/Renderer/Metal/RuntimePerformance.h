#pragma once
#ifndef CEMU_ENABLE_DETAILED_METAL_PERF
#define CEMU_ENABLE_DETAILED_METAL_PERF 0
#endif
#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace RuntimePerformance
{
// Detailed counters are useful while investigating a renderer bottleneck, but
// several of them are touched once per draw, encoder, or buffer upload. Keep
// their storage and atomic behavior in Debug builds (or explicit profiling
// builds) and compile them to no-ops in the normal Release app.
class DetailCounter
{
public:
    uint64_t fetch_add(uint64_t value, std::memory_order order = std::memory_order_relaxed) noexcept
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        return m_value.fetch_add(value, order);
#else
        (void)value; (void)order;
        return 0;
#endif
    }

    uint64_t load(std::memory_order order = std::memory_order_relaxed) const noexcept
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        return m_value.load(order);
#else
        (void)order;
        return 0;
#endif
    }

    void store(uint64_t value, std::memory_order order = std::memory_order_relaxed) noexcept
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        m_value.store(value, order);
#else
        (void)value; (void)order;
#endif
    }

    bool compare_exchange_weak(uint64_t& expected, uint64_t desired,
        std::memory_order order = std::memory_order_relaxed) noexcept
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        return m_value.compare_exchange_weak(expected, desired, order);
#else
        (void)expected; (void)desired; (void)order;
        return false;
#endif
    }

private:
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
    std::atomic<uint64_t> m_value{0};
#endif
};

// Keep only the coarse A/B metrics in production. The detailed per-draw
// counters and sampled trackers above remain opt-in.
class BasicCounter
{
public:
    uint64_t fetch_add(uint64_t value, std::memory_order order = std::memory_order_relaxed) noexcept
    {
        return m_value.fetch_add(value, order);
    }
    uint64_t load(std::memory_order order = std::memory_order_relaxed) const noexcept
    {
        return m_value.load(order);
    }
private:
    std::atomic<uint64_t> m_value{0};
};

inline uint64_t nowNs()
{
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    return 0;
#endif
}
// One producer (LatteThread), one watchdog consumer. Packed sequence + duration
// keeps each sample coherent without locks or allocations on the render thread.
class FrameIntervals
{
public:
    static constexpr size_t capacity = 4096;
    void record(uint64_t now)
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        if (m_previous && now > m_previous)
        {
            const auto us = std::min<uint64_t>((now - m_previous) / 1000, UINT32_MAX);
            const uint32_t sequence = ++m_sequence;
            m_samples[sequence % capacity].store((uint64_t(sequence) << 32) | us, std::memory_order_release);
            m_published.store(sequence, std::memory_order_release);
        }
        m_previous = now;
#else
        (void)now;
#endif
    }
    struct Window { size_t count = 0, over50ms = 0, lost = 0; double meanMs = 0, low1FPS = 0, maxMs = 0; };
    Window read(uint32_t& cursor) const
    {
        Window result;
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        const uint32_t end = m_published.load(std::memory_order_acquire);
        const uint32_t available = end - cursor;
        const uint32_t count = std::min<uint32_t>(available, capacity);
        result.lost = available - count;
        std::vector<uint32_t> durations; durations.reserve(count);
        for (uint32_t i = count; i > 0; --i)
        {
            const uint32_t sequence = end - i + 1;
            const uint64_t sample = m_samples[sequence % capacity].load(std::memory_order_acquire);
            if (uint32_t(sample >> 32) != sequence) { ++result.lost; continue; }
            durations.push_back(uint32_t(sample));
        }
        cursor = end;
        result.count = durations.size();
        if (durations.empty()) return result;
        uint64_t sum = 0;
        for (const auto us : durations) { sum += us; if (us > 50000) ++result.over50ms; }
        result.meanMs = double(sum) / durations.size() / 1000.0;
        std::sort(durations.begin(), durations.end(), std::greater<uint32_t>());
        result.maxMs = durations.front() / 1000.0;
        const size_t slowCount = (durations.size() + 99) / 100;
        uint64_t slowSum = 0;
        for (size_t i = 0; i < slowCount; ++i) slowSum += durations[i];
        result.low1FPS = slowSum ? 1.0e6 * slowCount / slowSum : 0.0;
#else
        (void)cursor;
#endif
        return result;
    }
private:
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
    uint64_t m_previous = 0;
    uint32_t m_sequence = 0;
    std::atomic<uint32_t> m_published{0};
    std::array<std::atomic<uint64_t>, capacity> m_samples{};
#endif
};
inline FrameIntervals tvSubmitIntervals;
inline BasicCounter commandBuffersSubmitted;
inline DetailCounter commandBuffersPeak;
inline DetailCounter commandBufferFrameEnd, commandBufferThreshold;
inline DetailCounter commandBufferReadback, commandBufferFlush;
inline DetailCounter commandBufferBlockingReadback, commandBufferOther;
inline DetailCounter soonRequestReadback, soonRequestQuery;
inline DetailCounter soonBufferReadback, soonBufferQuery, soonBufferBoth;
inline DetailCounter soonCommitAtFrameEnd, soonCommitDelayDraws, soonCommitDelayDrawsMax;
inline DetailCounter blockingReadbackWaitNs, blockingReadbackWaitCount;
inline DetailCounter stagingBytes, stagingBuffers, indexBytes, metalBytes;
inline DetailCounter tvCopies, drcCopies, textureUploads, textureUploadBytes;
inline DetailCounter textureCreates;
inline BasicCounter renderEncodersCount, blitEncodersCount;
inline DetailCounter computeEncodersCount;
inline BasicCounter renderPassInterrupts;
inline BasicCounter renderPassFlushSelfDependency;
inline DetailCounter renderPassInterruptBufferUpload, renderPassInterruptBufferCopy;
inline DetailCounter renderPassInterruptStreamout, renderPassInterruptTextureUpload;
inline DetailCounter renderPassInterruptTextureCopy, renderPassInterruptReadback;
inline DetailCounter renderPassInterruptDisplayCopy, renderPassInterruptScreenshot;
inline DetailCounter renderPassInterruptOther;
inline DetailCounter bufferUploads, bufferUploadBytes;
inline DetailCounter bufferUploadsLE256, bufferUploadsLE1K, bufferUploadsLE4K, bufferUploadsGT4K;
inline DetailCounter bufferCopies, streamoutCopies;

enum class BufferSyncSource
{
    Unknown,
    VertexAttribute,
    UniformVS,
    UniformPS,
    UniformGS
};
inline thread_local BufferSyncSource currentBufferSyncSource{BufferSyncSource::Unknown};
inline DetailCounter bufferUploadsUniformVS, bufferUploadsUniformPS, bufferUploadsUniformGS, bufferUploadsVertex;
inline DetailCounter bufferUploadBytesUniformVS, bufferUploadBytesUniformPS, bufferUploadBytesUniformGS, bufferUploadBytesVertex;
inline DetailCounter directUniformUploads, directUniformUploadBytes;
inline DetailCounter streamoutDrawsTotal, streamoutDrawsRasterDisabled, streamoutDrawsRasterEnabled;

inline DetailCounter textureCopyDataCalls, textureCopySliceCalls, surfaceCopyGPUCalls;
inline DetailCounter sameAddrMismatchAvoided;

// 1-second detailed telemetry counters
inline DetailCounter colorAttachmentLoads, colorAttachmentStores;
inline DetailCounter depthLoads, depthStores;
inline DetailCounter estimatedColorStoreBytes, estimatedDepthStoreBytes;
inline DetailCounter estimatedColorLoadBytes, estimatedDepthLoadBytes;

inline DetailCounter vertexUploadCalls, vertexUploadBytes;
inline DetailCounter textureCopyCalls, textureCopyBytes;

inline DetailCounter pipelineChanges, textureBindings, drawCalls;
// nextDrawable() is timed already; retain its once-per-frame total in Release
// so a hitch can be attributed to presentation without enabling per-draw probes.
inline BasicCounter drawableWaitUs;
// CADisplayLink-reported period, not the panel maximum or measured GPU time.
inline std::atomic<uint32_t> displayLinkPeriodUs{0};
inline BasicCounter commandBufferWaitUs;

inline BasicCounter selfDepColor, selfDepDepth;
inline DetailCounter selfDepForcedAccurateBarrier, selfDepNormalCollision;
inline DetailCounter selfDepExactMatch, selfDepBaseAlias;
inline std::array<DetailCounter, 32> selfDepSlotCounts{};

inline DetailCounter feedbackBatchDep;
inline DetailCounter feedbackBatchIndep;
inline DetailCounter feedbackBatchUnk;
inline DetailCounter feedbackBatchedPassesSaved;

inline DetailCounter snapshotsTaken;
inline DetailCounter selfDepSnapshotBypassed;

inline DetailCounter feedbackRun2;
inline DetailCounter feedbackRun3_5;
inline DetailCounter feedbackRun6_10;
inline DetailCounter feedbackRun11_20;
inline DetailCounter feedbackRun20Plus;

inline void recordFeedbackRun(uint32_t run)
{
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
    if (run == 2) feedbackRun2.fetch_add(1, std::memory_order_relaxed);
    else if (run >= 3 && run <= 5) feedbackRun3_5.fetch_add(1, std::memory_order_relaxed);
    else if (run >= 6 && run <= 10) feedbackRun6_10.fetch_add(1, std::memory_order_relaxed);
    else if (run >= 11 && run <= 20) feedbackRun11_20.fetch_add(1, std::memory_order_relaxed);
    else if (run > 20) feedbackRun20Plus.fetch_add(1, std::memory_order_relaxed);
#else
    (void)run;
#endif
}

struct SelfDepEntry
{
    uint32_t physAddress = 0;
    uint32_t slot = 0;
    bool isDepth = false;
    uint32_t count = 0;
};

class SelfDepTracker
{
public:
    static constexpr size_t MAX_TRACKED = 16;

    void record(uint32_t physAddress, uint32_t slot, bool isDepth)
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        static std::atomic<uint32_t> s_sampleCounter{0};
        if ((s_sampleCounter.fetch_add(1, std::memory_order_relaxed) & 63) != 0)
            return;

        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < m_count; ++i)
        {
            if (m_entries[i].physAddress == physAddress && m_entries[i].slot == slot && m_entries[i].isDepth == isDepth)
            {
                m_entries[i].count += 64;
                return;
            }
        }
        if (m_count < MAX_TRACKED)
        {
            m_entries[m_count++] = {physAddress, slot, isDepth, 64};
        }
        else
        {
            size_t minIdx = 0;
            for (size_t i = 1; i < MAX_TRACKED; ++i)
            {
                if (m_entries[i].count < m_entries[minIdx].count)
                    minIdx = i;
            }
            m_entries[minIdx] = {physAddress, slot, isDepth, m_entries[minIdx].count + 64};
        }
#else
        (void)physAddress; (void)slot; (void)isDepth;
#endif
    }

    std::vector<SelfDepEntry> snapshotAndReset()
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<SelfDepEntry> result(m_entries.begin(), m_entries.begin() + m_count);
        std::sort(result.begin(), result.end(), [](const SelfDepEntry& a, const SelfDepEntry& b) {
            return a.count > b.count;
        });
        m_count = 0;
        return result;
#else
        return {};
#endif
    }

private:
    std::mutex m_mutex;
    size_t m_count = 0;
    std::array<SelfDepEntry, MAX_TRACKED> m_entries{};
};

inline SelfDepTracker selfDepTracker;

inline DetailCounter selfDepTotalBreaks;
inline DetailCounter selfDepCurrentDrawWritesTarget;
inline DetailCounter selfDepCurrentDrawDoesNotWriteTarget;

struct SelfDepBreakDiag
{
    uint8_t feedbackTargetSlot = 0;
    uint32_t feedbackTexture = 0;
    uint64_t currentPixelShader = 0;
    uint32_t currentDrawColorTargetMask = 0;
    bool currentDrawActuallyWritesFeedbackTarget = false;
    uint8_t currentDrawWriteMaskForFeedbackTarget = 0;
    bool pixelShaderExportsFeedbackTarget = false;
    bool blendEnabledForFeedbackTarget = false;
    bool depthWriteEnabled = false;
    bool attachmentBound = false;
    bool attachmentActuallyRequiredByThisDraw = false;
    uint32_t count = 0;
};

class SelfDepBreakTracker
{
public:
    static constexpr size_t MAX_TRACKED = 24;

    void record(uint8_t slot, uint32_t texture, uint64_t psHash, uint32_t targetMask,
                bool actuallyWrites, uint8_t writeMask, bool exports, bool blend,
                bool depthWrite, bool bound, bool required)
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        static std::atomic<uint32_t> s_counter{0};
        if ((s_counter.fetch_add(1, std::memory_order_relaxed) & 31) != 0)
            return;

        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < m_count; ++i)
        {
            if (m_entries[i].feedbackTargetSlot == slot &&
                m_entries[i].feedbackTexture == texture &&
                m_entries[i].currentPixelShader == psHash &&
                m_entries[i].currentDrawColorTargetMask == targetMask &&
                m_entries[i].currentDrawActuallyWritesFeedbackTarget == actuallyWrites)
            {
                m_entries[i].count += 32;
                return;
            }
        }
        if (m_count < MAX_TRACKED)
        {
            auto& e = m_entries[m_count++];
            e = {slot, texture, psHash, targetMask, actuallyWrites, writeMask, exports, blend, depthWrite, bound, required, 32};
        }
        else
        {
            size_t minIdx = 0;
            for (size_t i = 1; i < MAX_TRACKED; ++i)
            {
                if (m_entries[i].count < m_entries[minIdx].count)
                    minIdx = i;
            }
            m_entries[minIdx] = {slot, texture, psHash, targetMask, actuallyWrites, writeMask, exports, blend, depthWrite, bound, required, m_entries[minIdx].count + 32};
        }
#else
        (void)slot; (void)texture; (void)psHash; (void)targetMask;
        (void)actuallyWrites; (void)writeMask; (void)exports; (void)blend;
        (void)depthWrite; (void)bound; (void)required;
#endif
    }

    std::vector<SelfDepBreakDiag> snapshotAndReset()
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<SelfDepBreakDiag> result(m_entries.begin(), m_entries.begin() + m_count);
        std::sort(result.begin(), result.end(), [](const SelfDepBreakDiag& a, const SelfDepBreakDiag& b) {
            return a.count > b.count;
        });
        m_count = 0;
        return result;
#else
        return {};
#endif
    }

private:
    std::mutex m_mutex;
    size_t m_count = 0;
    std::array<SelfDepBreakDiag, MAX_TRACKED> m_entries{};
};

inline SelfDepBreakTracker selfDepBreakTracker;

struct FeedbackPairRecord
{
    uint64_t writerBase = 0;
    uint64_t writerAux = 0;
    uint64_t readerBase = 0;
    uint64_t readerAux = 0;
    uint32_t physAddress = 0;
    uint8_t targetSlot = 0;
    uint8_t readerSlot = 0;
    uint8_t readerStage = 0; // 0: VS, 1: PS, 2: GS
    // Texture properties
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint16_t tileMode = 0;
    uint8_t mip = 0;
    uint8_t slice = 0;
    // Sampler properties
    uint8_t filterMin = 0;
    uint8_t filterMag = 0;
    uint8_t filterMip = 0;
    uint8_t clampS = 0;
    uint8_t clampT = 0;
    uint8_t clampR = 0;
    uint16_t minLod = 0;
    uint16_t maxLod = 0;
    int16_t lodBias = 0;
    bool hasSampler = false;
    // Counter
    uint32_t count = 0;
};

class FeedbackPairTracker
{
public:
    static constexpr size_t MAX_PAIRS = 64;

    void record(const FeedbackPairRecord& entry)
    {
        // Provenance complete - disable recording to eliminate lock contention
        (void)entry;
    }

    std::vector<FeedbackPairRecord> snapshotAndReset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<FeedbackPairRecord> result(m_entries.begin(), m_entries.begin() + m_count);
        std::sort(result.begin(), result.end(), [](const FeedbackPairRecord& a, const FeedbackPairRecord& b) {
            return a.count > b.count;
        });
        m_count = 0;
        return result;
    }

private:
    std::mutex m_mutex;
    size_t m_count = 0;
    std::array<FeedbackPairRecord, MAX_PAIRS> m_entries{};
};

inline FeedbackPairTracker feedbackPairTracker;

enum class NextAccessClass : uint8_t
{
    A_Sampled = 0,        // A = sampled/read before any overwrite
    B_Blend,              // B = blend/read-modify-write requiring existing contents
    C_DrawWrite,          // C = partial/unknown draw write
    D_Clear,              // D = guaranteed full clear
    E_FullOverwrite,      // E = guaranteed complete overwrite
    G_Unknown,            // G = unknown / timed out
    Count
};

inline const char* NextAccessToString(NextAccessClass c)
{
    switch (c)
    {
    case NextAccessClass::A_Sampled: return "A(sample)";
    case NextAccessClass::B_Blend: return "B(blend)";
    case NextAccessClass::C_DrawWrite: return "C(draw)";
    case NextAccessClass::D_Clear: return "D(clear)";
    case NextAccessClass::E_FullOverwrite: return "E(ovrwt)";
    default: return "G(unkn)";
    }
}

struct AttachmentLivenessEntry
{
    uint32_t feedbackSource = 0;
    uint8_t targetSlot = 0; // 0..7 color, 8 depth
    uint32_t physAddress = 0;
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t sizeBytes = 0;

    uint32_t totalBreaks = 0;
    uint32_t countA_Sampled = 0;
    uint32_t countB_Blend = 0;
    uint32_t countC_DrawWrite = 0;
    uint32_t countD_Clear = 0;
    uint32_t countE_FullOverwrite = 0;
    uint32_t countG_Unknown = 0;

    static constexpr size_t SAMPLE_SIZE = 64;
    std::array<uint16_t, SAMPLE_SIZE> drawSamples{};
    size_t sampleCount = 0;

    void addDrawSample(uint32_t draws)
    {
        drawSamples[sampleCount % SAMPLE_SIZE] = (uint16_t)std::min<uint32_t>(draws, 65535);
        sampleCount++;
    }

    uint32_t getMedianDraws() const
    {
        if (sampleCount == 0) return 0;
        size_t n = std::min(sampleCount, SAMPLE_SIZE);
        std::array<uint16_t, SAMPLE_SIZE> sorted;
        std::copy_n(drawSamples.begin(), n, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + n);
        return sorted[n / 2];
    }

    uint32_t getP95Draws() const
    {
        if (sampleCount == 0) return 0;
        size_t n = std::min(sampleCount, SAMPLE_SIZE);
        std::array<uint16_t, SAMPLE_SIZE> sorted;
        std::copy_n(drawSamples.begin(), n, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + n);
        size_t idx = (n * 95) / 100;
        if (idx >= n) idx = n - 1;
        return sorted[idx];
    }
};

struct PendingLivenessItem
{
    uint32_t feedbackSource = 0;
    uint8_t slot = 0;
    uint32_t physAddress = 0;
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t sizeBytes = 0;
    uint32_t startDraw = 0;
    uint32_t startPass = 0;
    bool active = false;
};

class AttachmentLivenessTracker
{
public:
    static constexpr size_t MAX_PENDING = 256;
    static constexpr size_t MAX_ENTRIES = 32;

    struct Buffer
    {
        size_t count = 0;
        std::array<AttachmentLivenessEntry, MAX_ENTRIES> entries{};
        uint64_t totalSegments = 0;
        uint64_t totalBoundColorCount = 0;
        uint64_t totalUsedColorCount = 0;
        uint64_t totalBoundDepthCount = 0;
        uint64_t totalUsedDepthCount = 0;
        uint64_t redundantSegments = 0;

        void reset()
        {
            count = 0;
            entries.fill({});
            totalSegments = 0;
            totalBoundColorCount = 0;
            totalUsedColorCount = 0;
            totalBoundDepthCount = 0;
            totalUsedDepthCount = 0;
            redundantSegments = 0;
        }
    };

    void addPending(uint32_t feedbackSource, uint8_t slot, uint32_t physAddress,
                    uint32_t format, uint32_t width, uint32_t height, uint64_t sizeBytes,
                    uint32_t currentDraw, uint32_t currentPass)
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        size_t slotIdx = m_pendingHead % MAX_PENDING;
        m_pendingHead++;

        auto& p = m_pendingItems[slotIdx];
        if (p.active)
        {
            resolveItem(p, NextAccessClass::G_Unknown, currentDraw);
        }

        p.feedbackSource = feedbackSource;
        p.slot = slot;
        p.physAddress = physAddress;
        p.format = format;
        p.width = width;
        p.height = height;
        p.sizeBytes = sizeBytes;
        p.startDraw = currentDraw;
        p.startPass = currentPass;
        p.active = true;
#else
        (void)feedbackSource; (void)slot; (void)physAddress; (void)format;
        (void)width; (void)height; (void)sizeBytes; (void)currentDraw; (void)currentPass;
#endif
    }

    void resolveByAddress(uint32_t physAddress, NextAccessClass access, uint32_t currentDraw)
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        for (size_t i = 0; i < MAX_PENDING; ++i)
        {
            if (m_pendingItems[i].active && m_pendingItems[i].physAddress == physAddress)
            {
                resolveItem(m_pendingItems[i], access, currentDraw);
            }
        }
#else
        (void)physAddress; (void)access; (void)currentDraw;
#endif
    }

    void checkTimeouts(uint32_t currentDraw, uint32_t maxDraws = 1500)
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        for (size_t i = 0; i < MAX_PENDING; ++i)
        {
            if (m_pendingItems[i].active && (currentDraw >= m_pendingItems[i].startDraw) &&
                (currentDraw - m_pendingItems[i].startDraw) > maxDraws)
            {
                resolveItem(m_pendingItems[i], NextAccessClass::G_Unknown, currentDraw);
            }
        }
#else
        (void)currentDraw; (void)maxDraws;
#endif
    }

    void recordSegment(uint8_t boundColorMask, uint8_t usedColorMask, bool boundDepth, bool usedDepth)
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        Buffer& b = m_buffers[m_activeBuffer.load(std::memory_order_relaxed)];
        b.totalSegments++;
        uint32_t boundColorCount = 0;
        uint32_t usedColorCount = 0;
        for (int i = 0; i < 8; ++i)
        {
            if (boundColorMask & (1 << i)) boundColorCount++;
            if (usedColorMask & (1 << i)) usedColorCount++;
        }
        b.totalBoundColorCount += boundColorCount;
        b.totalUsedColorCount += usedColorCount;
        if (boundDepth) b.totalBoundDepthCount++;
        if (usedDepth) b.totalUsedDepthCount++;
        if (boundColorCount > usedColorCount || (boundDepth && !usedDepth))
            b.redundantSegments++;
#else
        (void)boundColorMask; (void)usedColorMask; (void)boundDepth; (void)usedDepth;
#endif
    }

    struct Snapshot
    {
        std::vector<AttachmentLivenessEntry> entries;
        uint64_t totalSegments = 0;
        uint64_t totalBoundColorCount = 0;
        uint64_t totalUsedColorCount = 0;
        uint64_t totalBoundDepthCount = 0;
        uint64_t totalUsedDepthCount = 0;
        uint64_t redundantSegments = 0;
    };

    Snapshot snapshotAndReset()
    {
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
        size_t currentActive = m_activeBuffer.load(std::memory_order_relaxed);
        size_t nextActive = 1 - currentActive;
        m_buffers[nextActive].reset();
        m_activeBuffer.store(nextActive, std::memory_order_release);

        Buffer& b = m_buffers[currentActive];
        Snapshot s;
        s.entries.assign(b.entries.begin(), b.entries.begin() + b.count);
        s.totalSegments = b.totalSegments;
        s.totalBoundColorCount = b.totalBoundColorCount;
        s.totalUsedColorCount = b.totalUsedColorCount;
        s.totalBoundDepthCount = b.totalBoundDepthCount;
        s.totalUsedDepthCount = b.totalUsedDepthCount;
        s.redundantSegments = b.redundantSegments;
        b.reset();
        return s;
#else
        return {};
#endif
    }

private:
    void resolveItem(PendingLivenessItem& p, NextAccessClass access, uint32_t currentDraw)
    {
        uint32_t drawsLater = currentDraw >= p.startDraw ? (currentDraw - p.startDraw) : 0;

        Buffer& b = m_buffers[m_activeBuffer.load(std::memory_order_relaxed)];
        for (size_t i = 0; i < b.count; ++i)
        {
            if (b.entries[i].feedbackSource == p.feedbackSource &&
                b.entries[i].targetSlot == p.slot &&
                b.entries[i].physAddress == p.physAddress)
            {
                auto& e = b.entries[i];
                e.totalBreaks++;
                switch (access)
                {
                case NextAccessClass::A_Sampled: e.countA_Sampled++; break;
                case NextAccessClass::B_Blend: e.countB_Blend++; break;
                case NextAccessClass::C_DrawWrite: e.countC_DrawWrite++; break;
                case NextAccessClass::D_Clear: e.countD_Clear++; break;
                case NextAccessClass::E_FullOverwrite: e.countE_FullOverwrite++; break;
                default: e.countG_Unknown++; break;
                }
                e.addDrawSample(drawsLater);
                p.active = false;
                return;
            }
        }

        if (b.count < MAX_ENTRIES)
        {
            auto& e = b.entries[b.count++];
            e.feedbackSource = p.feedbackSource;
            e.targetSlot = p.slot;
            e.physAddress = p.physAddress;
            e.format = p.format;
            e.width = p.width;
            e.height = p.height;
            e.sizeBytes = p.sizeBytes;
            e.totalBreaks = 1;
            switch (access)
            {
            case NextAccessClass::A_Sampled: e.countA_Sampled = 1; break;
            case NextAccessClass::B_Blend: e.countB_Blend = 1; break;
            case NextAccessClass::C_DrawWrite: e.countC_DrawWrite = 1; break;
            case NextAccessClass::D_Clear: e.countD_Clear = 1; break;
            case NextAccessClass::E_FullOverwrite: e.countE_FullOverwrite = 1; break;
            default: e.countG_Unknown = 1; break;
            }
            e.addDrawSample(drawsLater);
        }
        p.active = false;
    }

    std::atomic<size_t> m_activeBuffer{0};
    std::array<Buffer, 2> m_buffers{};

    size_t m_pendingHead = 0;
    std::array<PendingLivenessItem, MAX_PENDING> m_pendingItems{};
};

inline AttachmentLivenessTracker attachmentLivenessTracker;

class ScopedCbWait
{
public:
    ScopedCbWait() : m_start(std::chrono::steady_clock::now()) {}
    ~ScopedCbWait()
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_start).count();
        commandBufferWaitUs.fetch_add(elapsed, std::memory_order_relaxed);
    }
private:
    std::chrono::steady_clock::time_point m_start;
};

// Compilation is infrequent; these counters remain useful in Release exports.
// Unlike detailed draw counters, they do not touch the steady-state draw path.
inline std::atomic<uint64_t> shaderCompileNs{0}, shaderCompileCount{0};
inline std::atomic<uint64_t> textureLoadNs{0}, textureUploadNs{0}, pipelineSyncNs{0};

// Optional loading probes never read the clock or update a counter when
// diagnostic logging is disabled. They are used on resource loads, not draws.
template<typename Clock = std::chrono::steady_clock>
class OptionalOperationTimer
{
public:
    OptionalOperationTimer(bool enabled, std::atomic<uint64_t>& counter)
        : m_counter(enabled ? &counter : nullptr)
    {
        if (m_counter)
            m_start = Clock::now();
    }
    ~OptionalOperationTimer()
    {
        if (m_counter)
            m_counter->fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - m_start).count(), std::memory_order_relaxed);
    }
    OptionalOperationTimer(const OptionalOperationTimer&) = delete;
    OptionalOperationTimer& operator=(const OptionalOperationTimer&) = delete;
private:
    std::atomic<uint64_t>* m_counter;
    typename Clock::time_point m_start{};
};
// These probes run only on a new required pipeline, never a cached draw.
template<typename Clock = std::chrono::steady_clock>
class OptionalDurationTimer
{
public:
    OptionalDurationTimer(bool enabled, uint64_t& duration) : m_duration(enabled ? &duration : nullptr)
    { if (m_duration) m_start = Clock::now(); }
    ~OptionalDurationTimer()
    { if (m_duration) *m_duration += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start).count(); }
    OptionalDurationTimer(const OptionalDurationTimer&) = delete;
    OptionalDurationTimer& operator=(const OptionalDurationTimer&) = delete;
private:
    uint64_t* m_duration;
    typename Clock::time_point m_start{};
};
struct PipelineCompileTimings { uint64_t initNs = 0, shaderWaitNs = 0, createNs = 0; };
enum PipelineSyncReason : uint32_t
{
    AsyncConfigOff = 1u, Query = 2u, Readback = 4u, RasterKill = 8u,
    Streamout = 16u, Ink1600 = 32u, ColorOnly = 64u, DepthOnly = 128u, OtherRequired = 256u
};
struct PipelineSyncSample
{
    uint64_t key = 0, vsBase = 0, vsAux = 0, gsBase = 0, gsAux = 0, psBase = 0, psAux = 0;
    uint32_t width = 0, height = 0, reasons = 0, depthFormat = 0, activeDepthFormat = 0, activeColorMask = 0;
    std::array<uint32_t, 8> colorFormats{};
    PipelineCompileTimings timings{};
    bool mesh = false, success = false;
};
// Produced and drained by LatteThread. Constant storage retains the strongest
// sample until the next throttled HITCH report, which can span several seconds.
class PipelineSyncRecorder
{
public:
    struct Window
    {
        uint64_t count = 0, slowCount = 0;
        std::array<uint64_t, 9> reasonCounts{};
        PipelineCompileTimings totals{};
        PipelineSyncSample strongest{};
    };
    void record(bool enabled, const PipelineSyncSample& sample)
    {
        if (!enabled) return;
        ++m_window.count;
        for (size_t i = 0; i < m_window.reasonCounts.size(); ++i)
            m_window.reasonCounts[i] += (sample.reasons >> i) & 1u;
        m_window.totals.initNs += sample.timings.initNs;
        m_window.totals.shaderWaitNs += sample.timings.shaderWaitNs;
        m_window.totals.createNs += sample.timings.createNs;
        const auto total = [](const PipelineSyncSample& s) { return s.timings.initNs + s.timings.shaderWaitNs + s.timings.createNs; };
        if (total(sample) < 10000000) return;
        ++m_window.slowCount;
        if (total(sample) > total(m_window.strongest)) m_window.strongest = sample;
    }
    void reset() { m_window = {}; m_lastReportNs = 0; }
    Window takeReport(uint64_t nowNs)
    {
        if (m_lastReportNs && nowNs - m_lastReportNs < 1000000000) return {};
        m_lastReportNs = nowNs;
        const auto result = m_window;
        m_window = {};
        return result;
    }
private:
    Window m_window{};
    uint64_t m_lastReportNs = 0;
};
inline PipelineSyncRecorder pipelineSyncRecorder;
inline std::atomic<uint64_t> pipelineCompileNs{0}, pipelineCompileCount{0};
inline std::atomic<uint64_t> pipelineWaitNs{0}, pipelineWaitCount{0};
inline std::atomic<uint64_t> pendingPipelines{0}, drawsSkippedPendingPipeline{0};
// Read only by the existing low-frequency JIT watchdog when presentation stops.
// These change on first-use pipeline work, never on a normal cached draw.
inline std::atomic<uint32_t> activePipelineWaiters{0}, activePipelineCompiles{0};
inline std::atomic<uint32_t> activeShaderWaiters{0}, activeShaderLibraryCompiles{0};
inline std::atomic<uint32_t> activeShaderFunctionCreates{0}, activePipelineCreates{0};
// Updated only when upload buffers are created/destroyed, never per draw.
inline std::atomic<uint64_t> stagingBufferBytes{0};
// A small fixed table identifies the oldest first-use MSL compiler when the
// watchdog observes a stopped frame. Workers claim one free slot; no
// allocation, lock, or per-draw work is added to the rendering path.
struct ShaderCompileProbe
{
    std::atomic<uint64_t> baseHash{0}, auxHash{0}, sourceBytes{0}, startedNs{0};
    std::atomic<uint32_t> shaderType{0}, preserveInvariance{0};
};
inline std::array<ShaderCompileProbe, 16> shaderCompileProbes{};
inline uint64_t shaderProbeNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline DetailCounter emptyRingWaitNs, emptyRingEpisodes;
class EmptyRingWait
{
public:
    void begin() { if (!m_start) m_start = nowNs(); }
    ~EmptyRingWait()
    {
        if (!m_start) return;
        emptyRingWaitNs.fetch_add(nowNs() - m_start, std::memory_order_relaxed);
        emptyRingEpisodes.fetch_add(1, std::memory_order_relaxed);
    }
private:
    uint64_t m_start = 0;
};
inline void recordPeak(DetailCounter& peak, uint64_t value)
{
#if !defined(NDEBUG) || CEMU_ENABLE_DETAILED_METAL_PERF
    auto previous = peak.load(std::memory_order_relaxed);
    while (previous < value && !peak.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
#else
    (void)peak; (void)value;
#endif
}
}
