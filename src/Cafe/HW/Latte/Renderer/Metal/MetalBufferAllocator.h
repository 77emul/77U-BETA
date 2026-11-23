#pragma once

#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Metal/MTLResource.hpp"
#include "util/ChunkedHeap/ChunkedHeap.h"
#include "util/helpers/MemoryPool.h"

#include <utility>

inline MTL::ResourceOptions GetResourceOptions(MTL::ResourceOptions options)
{
    if (options & MTL::ResourceStorageModeShared || options & MTL::ResourceStorageModeManaged)
        options |= MTL::ResourceCPUCacheModeWriteCombined;

    return options;
}

class MetalBufferChunkedHeap : private ChunkedHeap<>
{
  public:
	MetalBufferChunkedHeap(const class MetalRenderer* mtlRenderer, MTL::ResourceOptions options, size_t minimumBufferAllocationSize) : m_mtlr(mtlRenderer), m_options(GetResourceOptions(options)), m_minimumBufferAllocationSize(minimumBufferAllocationSize) { };
	~MetalBufferChunkedHeap();

	using ChunkedHeap::alloc;
	using ChunkedHeap::allocWithBoundedSearch;
	using ChunkedHeap::free;

	uint8* GetChunkPtr(uint32 index) const
	{
		if (index >= m_chunkBuffers.size())
			return nullptr;

		return m_chunkBuffers[index].basePtr;
	}

	MTL::Buffer* GetBufferByIndex(uint32 index) const
    {
        if (index >= m_chunkBuffers.size())
            return nullptr;

        return m_chunkBuffers[index].buffer;
    }

	size_t GetChunkCount() const
	{
		return m_chunkBuffers.size();
	}

    bool RequiresFlush() const
    {
        return m_options & MTL::ResourceStorageModeManaged;
    }

	void GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const
	{
		numBuffers = (uint32)m_chunkBuffers.size();
		totalBufferSize = m_numHeapBytes;
		freeBufferSize = m_numHeapBytes - m_numAllocatedBytes;
	}

  private:
	uint32 allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize) override;

	const class MetalRenderer* m_mtlr;

	MTL::ResourceOptions m_options;
	size_t m_minimumBufferAllocationSize;

	struct ChunkBuffer
	{
		MTL::Buffer* buffer;
		uint8* basePtr;
	};
	std::vector<ChunkBuffer> m_chunkBuffers;
};

// a circular ring-buffer which tracks and releases memory per command-buffer
class MetalSynchronizedRingAllocator
{
public:
	MetalSynchronizedRingAllocator(class MetalRenderer* mtlRenderer, MTL::ResourceOptions options, uint32 minimumBufferAllocSize, bool constrainedMemory = false) : m_mtlr(mtlRenderer), m_options(GetResourceOptions(options)), m_minimumBufferAllocSize(minimumBufferAllocSize), m_constrainedMemory(constrainedMemory) {};
	~MetalSynchronizedRingAllocator();
	MetalSynchronizedRingAllocator(const MetalSynchronizedRingAllocator&) = delete; // disallow copy

	struct BufferSyncPoint_t
	{
		// todo - modularize sync point
		MTL::CommandBuffer* commandBuffer;
		uint32 offset;

		BufferSyncPoint_t(MTL::CommandBuffer* _commandBuffer, uint32 _offset)
			: commandBuffer(_commandBuffer ? _commandBuffer->retain() : nullptr), offset(_offset) {}
		~BufferSyncPoint_t()
		{
			if (commandBuffer)
				commandBuffer->release();
		}
		BufferSyncPoint_t(const BufferSyncPoint_t& other)
			: commandBuffer(other.commandBuffer ? other.commandBuffer->retain() : nullptr), offset(other.offset) {}
		BufferSyncPoint_t& operator=(const BufferSyncPoint_t& other)
		{
			if (this != &other)
			{
				if (commandBuffer)
					commandBuffer->release();
				commandBuffer = other.commandBuffer ? other.commandBuffer->retain() : nullptr;
				offset = other.offset;
			}
			return *this;
		}
		BufferSyncPoint_t(BufferSyncPoint_t&& other) noexcept
			: commandBuffer(other.commandBuffer), offset(other.offset)
		{
			other.commandBuffer = nullptr;
		}
		BufferSyncPoint_t& operator=(BufferSyncPoint_t&& other) noexcept
		{
			if (this != &other)
			{
				if (commandBuffer)
					commandBuffer->release();
				commandBuffer = other.commandBuffer;
				offset = other.offset;
				other.commandBuffer = nullptr;
			}
			return *this;
		}
	};

	struct AllocatorBuffer_t
	{
		MTL::Buffer* mtlBuffer;
		uint8* basePtr;
		uint32 size;
		uint32 writeIndex;
		std::queue<BufferSyncPoint_t> queue_syncPoints;
		MTL::CommandBuffer* lastSyncpointCommandBuffer{ nullptr };
		uint32 index;
		uint32 cleanupCounter{ 0 }; // counts idle cleanups; excess staging may be reclaimed under memory pressure
		uint64_t idleSinceNs{ 0 }; // owner-thread observation, cleared whenever this chunk is reserved
	};

	struct AllocatorReservation_t
	{
		MTL::Buffer* mtlBuffer;
		uint8* memPtr;
		uint32 bufferOffset;
		uint32 size;
		uint32 bufferIndex;
	};

	AllocatorReservation_t AllocateBufferMemory(uint32 size, uint32 alignment);
	void FlushReservation(AllocatorReservation_t& uploadReservation);
	void CleanupBuffer(MTL::CommandBuffer* latestFinishedCommandBuffer);
	size_t TrimIdleBuffers();
	size_t TrimLongIdleBuffers(uint64_t nowNs);
	size_t GetReclaimableIdleBytes() const;
	MTL::Buffer* GetBufferByIndex(uint32 index) const;

    bool RequiresFlush() const
    {
        return m_options & MTL::ResourceStorageModeManaged;
    }

	void GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const;

private:
	void allocateAdditionalUploadBuffer(uint32 sizeRequiredForAlloc);
	void addUploadBufferSyncPoint(AllocatorBuffer_t& buffer, uint32 offset);
	void ReleaseUploadBuffer(AllocatorBuffer_t& buffer);

	class MetalRenderer* m_mtlr;

	MTL::ResourceOptions m_options;
	const uint32 m_minimumBufferAllocSize;
	const bool m_constrainedMemory;

	std::vector<AllocatorBuffer_t> m_buffers;
	size_t m_preferredBufferIndex{0};
};

// heap style allocator with released memory being freed after the current command buffer finishes
class MetalSynchronizedHeapAllocator
{
  public:
	MetalSynchronizedHeapAllocator(class MetalRenderer* mtlRenderer, MTL::ResourceOptions options, size_t minimumBufferAllocSize) : m_mtlr(mtlRenderer), m_chunkedHeap(mtlRenderer, options, minimumBufferAllocSize) {}
	~MetalSynchronizedHeapAllocator();
	MetalSynchronizedHeapAllocator(const MetalSynchronizedHeapAllocator&) = delete; // disallow copy

	struct AllocatorReservation
	{
		MTL::Buffer* mtlBuffer;
		uint8* memPtr;
		uint32 bufferOffset;
		uint32 size;
		uint32 bufferIndex;
		CHAddr chAddr;
	};

	AllocatorReservation* AllocateBufferMemory(uint32 size, uint32 alignment, bool boundedIndexSearch = false);
	void FreeReservation(AllocatorReservation* uploadReservation);
	void FlushReservation(AllocatorReservation* uploadReservation);

	void CleanupBuffer(MTL::CommandBuffer* latestFinishedCommandBuffer);

	void GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const;
  private:
	class MetalRenderer* m_mtlr;
	MetalBufferChunkedHeap m_chunkedHeap;
	MemoryPool<AllocatorReservation> m_poolAllocatorReservation{32};
	// release queue
	std::unordered_map<MTL::CommandBuffer*, std::vector<CHAddr>> m_releaseQueue;
};
