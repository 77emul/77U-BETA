#pragma once

#include "BufferUploadRanges.h"

#include "Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryPolicy.h"

#include "GameProfile/GameProfile.h"

class MetalMemoryManager
{
public:
    MetalMemoryManager(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer},
        m_stagingAllocator(m_mtlr, m_mtlr->GetOptimalBufferStorageMode(),
            static_cast<uint32>(GetMetalMemoryPolicy().stagingChunkBytes), GetMetalMemoryPolicy().constrained),
        m_indexAllocator(m_mtlr, m_mtlr->GetOptimalBufferStorageMode(), GetMetalMemoryPolicy().indexChunkBytes)
    {
#if TARGET_OS_IPHONE || TARGET_OS_IOS
        // Native BC uploads already match the Metal upload representation.
        // Expanded BC fallback devices keep the separate decoder scratch.
        m_useDirectTextureUploads = (m_mtlr->GetDevice()->supportsBCTextureCompression());
#endif
    }
    ~MetalMemoryManager();

    MetalSynchronizedRingAllocator& GetStagingAllocator()
    {
        return m_stagingAllocator;
    }

    MetalSynchronizedHeapAllocator& GetIndexAllocator()
    {
        return m_indexAllocator;
    }

    MTL::Buffer* GetBufferCache()
    {
        return m_bufferCache;
    }

    void CleanupBuffers(MTL::CommandBuffer* latestFinishedCommandBuffer)
    {
        m_stagingAllocator.CleanupBuffer(latestFinishedCommandBuffer);
        m_indexAllocator.CleanupBuffer(latestFinishedCommandBuffer);
    }

    // Texture upload buffer
    void* AcquireTextureUploadBuffer(size_t size, bool decodedMatchesUpload = false);
    void ReleaseTextureUploadBuffer(uint8* mem);
    bool ResolveTextureUploadBuffer(const void* mem, size_t size,
        MetalSynchronizedRingAllocator::AllocatorReservation_t& allocation) const;

    // Buffer cache
    void InitBufferCache(size_t size);
    bool EnsureBufferCacheCapacity(size_t requiredEnd);
    void UploadToBufferCache(const void* data, size_t offset, size_t size);
    void CopyBufferCache(size_t srcOffset, size_t dstOffset, size_t size);
    // A draw can read a not-yet-copied upload directly from shared staging.
    bool ResolveDeferredUpload(uint32 cacheOffset, uint32 size, MTL::Buffer*& buffer, uint32& bufferOffset) const;
    bool DeferredUploadOverlaps(uint32 cacheOffset, uint32 size) const;
    bool HasDeferredUploads() const { return !m_deferredUploads.empty(); }
    std::optional<BufferUploadRanges<MTL::Buffer*>::Map> TakeAlignedDeferredUploads(uint32_t maxRangeSize)
    {
        return m_deferredUploads.TakeIfAligned(16, maxRangeSize);
    }
    void FlushDeferredUploads();

    // Getters
    bool UseHostMemoryForCache() const
    {
        return (m_metalBufferCacheMode == MetalBufferCacheMode::Host);
    }

    bool UsesPrivateBufferCache() const
    {
        return (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate);
    }

    bool NeedsReducedLatency() const
    {
        return (m_metalBufferCacheMode == MetalBufferCacheMode::Host);
    }

    MPTR GetImportedMemBaseAddress() const
    {
        return m_importedMemBaseAddress;
    }

    size_t GetHostAllocationSize() const
    {
        return m_hostAllocationSize;
    }

private:
    class MetalRenderer* m_mtlr;

    std::vector<uint8> m_textureUploadBuffer;
    bool m_useDirectTextureUploads = false;
    MetalSynchronizedRingAllocator::AllocatorReservation_t m_textureUploadReservation{};

    MetalSynchronizedRingAllocator m_stagingAllocator;
    MetalSynchronizedHeapAllocator m_indexAllocator;

    MTL::Buffer* m_bufferCache = nullptr;
    size_t m_bufferCacheMaximum = 0;
    bool m_demandBufferCache = false;
    MetalBufferCacheMode m_metalBufferCacheMode = MetalBufferCacheMode::Auto;
    bool m_gpuOrderedBufferUpdates = false;
    BufferUploadRanges<MTL::Buffer*> m_deferredUploads;
    MPTR m_importedMemBaseAddress;
    size_t m_hostAllocationSize = 0;
};
