#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalVoidVertexPipeline.h"

#include "CafeSystem.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Common/precompiled.h"
#include "HW/MMU/MMU.h"
#include "config/CemuConfig.h"
#include <limits>
#include "Common/HostUploadMemory.h"
#if TARGET_OS_IPHONE || TARGET_OS_IOS
#include <os/proc.h>
#endif

MetalMemoryManager::~MetalMemoryManager()
{
    HostUploadMemory::textureScratchCapacity.store(0, std::memory_order_relaxed);
    HostUploadMemory::gpuCacheBackingBytes.store(0, std::memory_order_relaxed);
    HostUploadMemory::stagingFreeBytes.store(0, std::memory_order_relaxed);
    HostUploadMemory::stagingReclaimableBytes.store(0, std::memory_order_relaxed);
    HostUploadMemory::stagingBufferCount.store(0, std::memory_order_relaxed);
    HostUploadMemory::indexCapacityBytes.store(0, std::memory_order_relaxed);
    HostUploadMemory::indexUsedBytes.store(0, std::memory_order_relaxed);
    HostUploadMemory::indexBufferCount.store(0, std::memory_order_relaxed);
    HostUploadMemory::outstandingCommandBuffers.store(0, std::memory_order_relaxed);
    if (m_bufferCache)
    {
        m_bufferCache->release();
    }
}

void* MetalMemoryManager::AcquireTextureUploadBuffer(size_t size, bool decodedMatchesUpload)
{
    if ((m_useDirectTextureUploads || decodedMatchesUpload) && size <= std::numeric_limits<uint32>::max())
    {
        cemu_assert_debug(!m_textureUploadReservation.memPtr);
        // Decode directly into storage protected by the same command-buffer
        // sync points as the existing upload path. EndEncoding does not submit
        // that buffer; its copy is encoded before the normal submission boundary.
        m_textureUploadReservation = m_stagingAllocator.AllocateBufferMemory((uint32)size, 1);
        return m_textureUploadReservation.memPtr;
    }
    if (m_textureUploadBuffer.size() < size)
    {
        m_textureUploadBuffer.resize(size);
        HostUploadMemory::textureScratchCapacity.store(m_textureUploadBuffer.capacity(), std::memory_order_relaxed);
    }

    return m_textureUploadBuffer.data();
}

void MetalMemoryManager::ReleaseTextureUploadBuffer(uint8* mem)
{
    if (mem == m_textureUploadReservation.memPtr && m_textureUploadReservation.mtlBuffer)
    {
        // Release only the decoder's handle. The staging allocator still owns
        // the GPU lifetime; these bytes cannot be overwritten before completion.
        m_textureUploadReservation = {};
        return;
    }
    cemu_assert_debug(m_textureUploadBuffer.data() == mem);
    const size_t retainLimit = GetMetalMemoryPolicy().textureScratchRetainBytes;
    if (retainLimit && m_textureUploadBuffer.capacity() > retainLimit)
    {
        // The BC expansion/copy has finished on the CPU, and Metal reads its
        // distinct staging reservation. No GPU reservation is reclaimed here.
        std::vector<uint8>().swap(m_textureUploadBuffer);
        HostUploadMemory::textureScratchCapacity.store(0, std::memory_order_relaxed);
        return;
    }
    // Keep the initialized scratch storage between uploads. Decoders overwrite
    // the image bytes; clear()+resize() would zero them again on every slice.
    // Capacity was retained before as well, so this does not grow retention.
}

bool MetalMemoryManager::ResolveTextureUploadBuffer(const void* mem, size_t size,
    MetalSynchronizedRingAllocator::AllocatorReservation_t& allocation) const
{
    if (m_textureUploadReservation.mtlBuffer && mem == m_textureUploadReservation.memPtr &&
        size <= m_textureUploadReservation.size)
    {
        allocation = m_textureUploadReservation;
        return true;
    }
    return false;
}

void MetalMemoryManager::InitBufferCache(size_t size)
{
    cemu_assert_debug(!m_bufferCache);

    m_metalBufferCacheMode = g_current_game_profile->GetBufferCacheMode();

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // BotW reuses camera/terrain buffers while earlier passes still consume
    // them. Shared storage does not snapshot CPU writes for queued draws.
    // Reuse the immutable upload bindings and GPU-ordered cache copies rather
    // than overwrite those bytes or wait for the GPU on every update.
    switch (CafeSystem::GetForegroundTitleId())
    {
    case 0x00050000101C9500: // EUR
    case 0x00050000101C9400: // USA
    case 0x00050000101C9300: // JPN
        m_gpuOrderedBufferUpdates = true;
        break;
    default:
        break;
    }
#endif

    if (m_metalBufferCacheMode == MetalBufferCacheMode::Auto)
    {
        if (m_mtlr->IsAppleGPU())
        {
            switch (CafeSystem::GetForegroundTitleId())
            {
            // The Legend of Zelda: Wind Waker HD
            case 0x0005000010143600: // EUR
            case 0x0005000010143500: // USA
            case 0x0005000010143400: // JPN
            // The Legend of Zelda: Breath of the Wild
            case 0x00050000101C9500: // USA
            case 0x00050000101C9400: // EUR
            case 0x00050000101C9300: // JPN
                m_metalBufferCacheMode = MetalBufferCacheMode::DeviceShared;
                break;
            default:
#if TARGET_OS_IPHONE || TARGET_OS_IOS
                // All Apple Silicon iOS devices feature unified memory architectures.
                // DeviceShared eliminates hundreds of staging allocations, deferred upload maps,
                // in-pass point-draw shader copies, and render-pass breaking blit fallbacks per frame.
                m_metalBufferCacheMode = MetalBufferCacheMode::DeviceShared;
#else
                m_metalBufferCacheMode = MetalBufferCacheMode::DevicePrivate;
#endif
                break;
            }
        }
        else
        {
            m_metalBufferCacheMode = MetalBufferCacheMode::DevicePrivate;
        }
    }

    cemuLog_log(LogType::Force, "Metal buffer cache mode initialized to: {} (0=Auto, 1=DevicePrivate, 2=DeviceShared, 3=Host)", (int)m_metalBufferCacheMode);

    // First, try to import the host memory as a buffer
    if (m_metalBufferCacheMode == MetalBufferCacheMode::Host)
    {
        if (m_mtlr->HasUnifiedMemory())
        {
            m_importedMemBaseAddress = mmuRange_MEM2.getBase();
           	m_hostAllocationSize = mmuRange_MEM2.getSize();
            m_bufferCache = m_mtlr->GetDevice()->newBuffer(memory_getPointerFromVirtualOffset(m_importedMemBaseAddress), m_hostAllocationSize, MTL::ResourceStorageModeShared, nullptr);
            if (!m_bufferCache)
            {
                cemuLog_log(LogType::Force, "Failed to import host memory as a buffer, using device shared mode instead");
                m_metalBufferCacheMode = MetalBufferCacheMode::DeviceShared;
            }
        }
        else
        {
            cemuLog_log(LogType::Force, "Host buffer cache mode is only available on unified memory systems, using device shared mode instead");
            m_metalBufferCacheMode = MetalBufferCacheMode::DeviceShared;
        }
    }

    m_bufferCacheMaximum = size;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // Only constrained shared buffers use demand backing. Host imports and
    // explicitly selected private cache mode retain their existing behavior.
    m_demandBufferCache = GetMetalMemoryPolicy().constrained &&
        m_metalBufferCacheMode == MetalBufferCacheMode::DeviceShared;
#endif
    const size_t initialSize = m_demandBufferCache ? std::min(size, size_t(32 * 1024 * 1024)) : size;
    if (!m_bufferCache)
        m_bufferCache = m_mtlr->GetDevice()->newBuffer(initialSize, (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate ? MTL::ResourceStorageModePrivate : MTL::ResourceStorageModeShared));
    if (!m_bufferCache) throw std::bad_alloc();
    HostUploadMemory::gpuCacheBackingBytes.store(m_bufferCache->length(), std::memory_order_relaxed);

#ifdef CEMU_DEBUG_ASSERT
    m_bufferCache->setLabel(GetLabel("Buffer cache", m_bufferCache));
#endif
}

bool MetalMemoryManager::EnsureBufferCacheCapacity(size_t requiredEnd)
{
    if (UseHostMemoryForCache()) return true; // Guest address import uses a different offset space.
    if (m_demandBufferCache && requiredEnd > HostUploadMemory::gpuCacheRequiredEndBytes.load(std::memory_order_relaxed))
        HostUploadMemory::gpuCacheRequiredEndBytes.store(requiredEnd, std::memory_order_relaxed);
    if (m_bufferCache && requiredEnd <= m_bufferCache->length()) return true;
    if (!m_demandBufferCache || !m_bufferCache || requiredEnd > m_bufferCacheMaximum) return false;
    // The sustained Splatoon run required a ~90 MiB virtual end, not just
    // the ~60 MiB initial working set. Keep that case at 128 MiB instead of
    // immediately retaining 384 MiB. At most three promotions; maximum overlap
    // is 128+384 MiB, never the 256+384 MiB peak of unrestricted doubling.
    // Guest offsets, byte contents and the original cache maximum are unchanged.
    constexpr size_t MiB = 1024u * 1024u;
    const size_t requestedCapacity = requiredEnd <= 64 * MiB ? 64 * MiB :
        requiredEnd <= 128 * MiB ? 128 * MiB : m_bufferCacheMaximum;
    const size_t capacity = std::min(requestedCapacity, m_bufferCacheMaximum);
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    if (os_proc_available_memory() < capacity) return false;
#endif

    // Commit deferred copies and streamout, finish the encoder, then wait for
    // all old-cache users before changing the backing. No in-flight resource is
    // evicted and no existing virtual offset or guest byte changes.
    m_mtlr->Flush(true);
    m_mtlr->ProcessFinishedCommandBuffers();
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // The system can lower the process limit while the GPU is finishing.
    if (os_proc_available_memory() < capacity) return false;
#endif
    auto* replacement = m_mtlr->GetDevice()->newBuffer(capacity, MTL::ResourceStorageModeShared);
    if (!replacement) return false; // Old backing remains valid on failure.
    if (!replacement->contents() || !m_bufferCache->contents()) {
        replacement->release(); return false;
    }
    memcpy(replacement->contents(), m_bufferCache->contents(), m_bufferCache->length());
    auto* previous = m_bufferCache;
    m_bufferCache = replacement;
    previous->release();
    HostUploadMemory::gpuCacheBackingBytes.store(capacity, std::memory_order_relaxed);
    HostUploadMemory::gpuCacheGrowthCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void MetalMemoryManager::UploadToBufferCache(const void* data, size_t offset, size_t size)
{
    cemu_assert_debug(m_metalBufferCacheMode != MetalBufferCacheMode::Host);
    cemu_assert_debug(m_bufferCache);
    cemu_assert_debug((offset + size) <= m_bufferCache->length());

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    RuntimePerformance::bufferUploads.fetch_add(1, std::memory_order_relaxed);
    RuntimePerformance::bufferUploadBytes.fetch_add(size, std::memory_order_relaxed);
    if (size <= 256)
        RuntimePerformance::bufferUploadsLE256.fetch_add(1, std::memory_order_relaxed);
    else if (size <= 1024)
        RuntimePerformance::bufferUploadsLE1K.fetch_add(1, std::memory_order_relaxed);
    else if (size <= 4096)
        RuntimePerformance::bufferUploadsLE4K.fetch_add(1, std::memory_order_relaxed);
    else
        RuntimePerformance::bufferUploadsGT4K.fetch_add(1, std::memory_order_relaxed);

    switch (RuntimePerformance::currentBufferSyncSource)
    {
    case RuntimePerformance::BufferSyncSource::UniformVS:
        RuntimePerformance::bufferUploadsUniformVS.fetch_add(1, std::memory_order_relaxed);
        RuntimePerformance::bufferUploadBytesUniformVS.fetch_add(size, std::memory_order_relaxed);
        break;
    case RuntimePerformance::BufferSyncSource::UniformPS:
        RuntimePerformance::bufferUploadsUniformPS.fetch_add(1, std::memory_order_relaxed);
        RuntimePerformance::bufferUploadBytesUniformPS.fetch_add(size, std::memory_order_relaxed);
        break;
    case RuntimePerformance::BufferSyncSource::UniformGS:
        RuntimePerformance::bufferUploadsUniformGS.fetch_add(1, std::memory_order_relaxed);
        RuntimePerformance::bufferUploadBytesUniformGS.fetch_add(size, std::memory_order_relaxed);
        break;
    case RuntimePerformance::BufferSyncSource::VertexAttribute:
        RuntimePerformance::bufferUploadsVertex.fetch_add(1, std::memory_order_relaxed);
        RuntimePerformance::bufferUploadBytesVertex.fetch_add(size, std::memory_order_relaxed);
        RuntimePerformance::vertexUploadCalls.fetch_add(1, std::memory_order_relaxed);
        RuntimePerformance::vertexUploadBytes.fetch_add(size, std::memory_order_relaxed);
        break;
    case RuntimePerformance::BufferSyncSource::Unknown:
    default:
        break;
    }
#endif

    if (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate || m_gpuOrderedBufferUpdates)
    {
        if (!data || size == 0 || offset > std::numeric_limits<uint32>::max() - size)
            return;
        MetalSynchronizedRingAllocator::AllocatorReservation_t allocation{};
        try
        {
            allocation = m_stagingAllocator.AllocateBufferMemory((uint32)size, 16);
        }
        catch (const std::bad_alloc&)
        {
            cemuLog_log(LogType::Force, "buffer cache upload skipped: staging allocation failed ({} bytes)", size);
            return;
        }
        if (!allocation.memPtr || !allocation.mtlBuffer)
            return;
        memcpy(allocation.memPtr, data, size);
        m_stagingAllocator.FlushReservation(allocation);
        // Keep the bytes in shared staging for this command buffer and copy them
        // into the private cache once, at submit. A blit here would end the
        // current render pass.
        m_deferredUploads.Insert((uint32)offset, (uint32)size, allocation.mtlBuffer, allocation.bufferOffset);
    }
    else
    {
        memcpy((uint8*)m_bufferCache->contents() + offset, data, size);
    }
}

bool MetalMemoryManager::ResolveDeferredUpload(uint32 cacheOffset, uint32 size, MTL::Buffer*& buffer, uint32& bufferOffset) const
{
    return m_deferredUploads.Resolve(cacheOffset, size, buffer, bufferOffset);
}

bool MetalMemoryManager::DeferredUploadOverlaps(uint32 cacheOffset, uint32 size) const
{
    return m_deferredUploads.Overlaps(cacheOffset, size);
}

void MetalMemoryManager::FlushDeferredUploads()
{
    if (m_deferredUploads.empty() || !m_bufferCache)
        return;
    // Remove the pending ranges before opening an encoder: EndEncoding may submit.
    auto uploads = m_deferredUploads.Take();
    auto blitCommandEncoder = m_mtlr->GetBlitCommandEncoder();
    for (const auto& [cacheOffset, upload] : uploads)
        blitCommandEncoder->copyFromBuffer(upload.buffer, upload.bufferOffset, m_bufferCache, cacheOffset, upload.size);
}

void MetalMemoryManager::CopyBufferCache(size_t srcOffset, size_t dstOffset, size_t size)
{
    cemu_assert_debug(m_metalBufferCacheMode != MetalBufferCacheMode::Host);
    cemu_assert_debug(m_bufferCache);

#if TARGET_OS_IPHONE || TARGET_OS_IOS
    RuntimePerformance::bufferCopies.fetch_add(1, std::memory_order_relaxed);
#endif

    if (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate || m_gpuOrderedBufferUpdates)
    {
        FlushDeferredUploads();
        m_mtlr->CopyBufferToBuffer(m_bufferCache, (uint32)srcOffset, m_bufferCache, (uint32)dstOffset, (uint32)size, ALL_MTL_RENDER_STAGES, ALL_MTL_RENDER_STAGES);
    }
    else
        memmove((uint8*)m_bufferCache->contents() + dstOffset, (uint8*)m_bufferCache->contents() + srcOffset, size);
}
