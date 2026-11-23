#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureReadbackMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include "Cemu/Logging/CemuLogging.h"

#include <algorithm>
#include <array>
#include <chrono>

#if TARGET_OS_IPHONE || TARGET_OS_IOS
extern "C" bool CemuBridge_IsDiagnosticLoggingEnabled(void);

namespace
{
struct SlowReadbackBucket
{
    uint32 physAddress = 0, width = 0, height = 0, format = 0, tileMode = 0;
    uint32 count = 0;
    uint64_t totalWaitUs = 0, maxWaitUs = 0;
    double maxCommandGpuMs = 0, maxTransferAgeMs = 0;
};

void RecordSlowReadback(const LatteTextureReadbackInfoMtl& readback, MTL::CommandBuffer* commandBuffer,
                        uint64_t waitUs, std::chrono::steady_clock::time_point now)
{
    // Fixed storage and no per-wait formatting. Only waits exceeding 2 ms are
    // collected; the two heaviest textures are logged once every five seconds.
    static thread_local std::array<SlowReadbackBucket, 8> buckets{};
    static thread_local uint32 totalCount = 0;
    static thread_local uint64_t totalWaitUs = 0;
    static thread_local auto lastLog = now;

    if (waitUs >= 2000)
    {
        ++totalCount;
        totalWaitUs += waitUs;
        auto* bucket = static_cast<SlowReadbackBucket*>(nullptr);
        for (auto& candidate : buckets)
        {
            if (candidate.count && candidate.physAddress == readback.hostTextureCopy.physAddress)
            {
                bucket = &candidate;
                break;
            }
            if (!bucket && !candidate.count)
                bucket = &candidate;
        }
        if (!bucket)
            bucket = &*std::min_element(buckets.begin(), buckets.end(),
                [](const auto& a, const auto& b) { return a.totalWaitUs < b.totalWaitUs; });
        if (!bucket->count || bucket->physAddress != readback.hostTextureCopy.physAddress)
            *bucket = {};
        bucket->physAddress = readback.hostTextureCopy.physAddress;
        bucket->width = readback.hostTextureCopy.width;
        bucket->height = readback.hostTextureCopy.height;
        bucket->format = static_cast<uint32>(readback.hostTextureCopy.format);
        bucket->tileMode = static_cast<uint32>(readback.hostTextureCopy.tileMode);
        ++bucket->count;
        bucket->totalWaitUs += waitUs;
        if (waitUs > bucket->maxWaitUs)
        {
            bucket->maxWaitUs = waitUs;
            const double start = commandBuffer->GPUStartTime();
            const double end = commandBuffer->GPUEndTime();
            bucket->maxCommandGpuMs = end >= start ? (end - start) * 1000.0 : 0.0;
            // The direct blocking-readback path does not set transferStartTime.
            if (readback.forceFinish)
                bucket->maxTransferAgeMs = HighResolutionTimer::getTimeDiff(
                    readback.transferStartTime, HighResolutionTimer().now().getTick()) * 1000.0;
        }
    }

    if (std::chrono::duration<double>(now - lastLog).count() < 5.0)
        return;
    lastLog = now;
    if (totalCount)
    {
        cemuLog_log(LogType::Force, "[READBACK WAIT] window count:{} total:{:.1f}ms",
                    totalCount, double(totalWaitUs) / 1000.0);
        std::sort(buckets.begin(), buckets.end(),
            [](const auto& a, const auto& b) { return a.totalWaitUs > b.totalWaitUs; });
        for (size_t i = 0; i < 2 && buckets[i].count; ++i)
        {
            const auto& b = buckets[i];
            cemuLog_log(LogType::Force,
                "[READBACK TARGET] phys:{:08x} {}x{} format:{} tile:{} count:{} total:{:.1f}ms max:{:.1f}ms maxCBGpu:{:.1f}ms maxTransferAge:{:.1f}ms",
                b.physAddress, b.width, b.height, b.format, b.tileMode, b.count,
                double(b.totalWaitUs) / 1000.0, double(b.maxWaitUs) / 1000.0, b.maxCommandGpuMs, b.maxTransferAgeMs);
        }
    }
    buckets = {};
    totalCount = 0;
    totalWaitUs = 0;
}
}
#endif

LatteTextureReadbackInfoMtl::~LatteTextureReadbackInfoMtl()
{
    if (m_commandBuffer)
        m_commandBuffer->release();
}

void LatteTextureReadbackInfoMtl::StartTransfer()
{
	cemu_assert(m_textureView);

	auto* baseTexture = (LatteTextureMtl*)m_textureView->baseTexture;

	cemu_assert_debug(m_textureView->firstSlice == 0);
	cemu_assert_debug(m_textureView->firstMip == 0);
	cemu_assert_debug(m_textureView->baseTexture->dim != Latte::E_DIM::DIM_3D);

	size_t bytesPerRow = GetMtlTextureBytesPerRow(baseTexture->format, baseTexture->isDepth, baseTexture->width);
	size_t bytesPerImage = GetMtlTextureBytesPerImage(baseTexture->format, baseTexture->isDepth, baseTexture->height, bytesPerRow);

	m_mtlr->FlushPendingTextureClears(baseTexture, 0, 1, 0, 1);
	auto blitCommandEncoder = m_mtlr->GetBlitCommandEncoder();

	const auto mtlFormat = baseTexture->GetTexture()->pixelFormat();
	MTL::BlitOption blitOptions = GetMtlBlitOption(mtlFormat);

	blitCommandEncoder->copyFromTexture(baseTexture->GetTexture(), 0, 0, MTL::Origin{0, 0, 0}, MTL::Size{(uint32)baseTexture->width, (uint32)baseTexture->height, 1}, m_mtlr->GetTextureReadbackBuffer(), m_bufferOffset, bytesPerRow, bytesPerImage, blitOptions);

	m_commandBuffer = m_mtlr->GetCurrentCommandBuffer()->retain();
	m_mtlr->MarkReadbackPendingSubmission();
#if !TARGET_OS_IPHONE && !TARGET_OS_IOS
	m_mtlr->CommitCommandBuffer();
#endif
}

bool LatteTextureReadbackInfoMtl::IsFinished()
{
    if (!m_commandBuffer)
        return true;
    return CommandBufferCompleted(m_commandBuffer);
}

void LatteTextureReadbackInfoMtl::ForceFinish()
{
    if (!m_commandBuffer)
        return;
    // StartTransfer batches copies on iOS. An explicit CPU dependency must
    // submit that batch before waiting, regardless of query dimensions.
    // A previously submitted readback must not flush an unrelated new buffer.
    if (m_mtlr && m_mtlr->IsCommandBufferActive() &&
        m_mtlr->GetCurrentCommandBuffer() == m_commandBuffer)
        m_mtlr->CommitCommandBuffer();

    // This method is an explicit CPU dependency. Returning before completion
    // can publish stale data or leave the guest polling forever, even for a
    // small texture. Ordinary IsFinished polling remains asynchronous.


    const auto start = std::chrono::steady_clock::now();
    m_commandBuffer->waitUntilCompleted();
    const auto now = std::chrono::steady_clock::now();
    const uint64_t waitUs = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
    RuntimePerformance::commandBufferWaitUs.fetch_add(waitUs, std::memory_order_relaxed);
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    if (waitUs >= 2000 && CemuBridge_IsDiagnosticLoggingEnabled())
        RecordSlowReadback(*this, m_commandBuffer, waitUs, now);
#endif
}

uint8* LatteTextureReadbackInfoMtl::GetData()
{
	if (!m_mtlr)
		return nullptr;
	auto* readbackBuf = m_mtlr->GetTextureReadbackBuffer();
	if (!readbackBuf)
		return nullptr;
	auto* contents = readbackBuf->contents();
	if (!contents)
		return nullptr;
	return (uint8*)contents + m_bufferOffset;
}
