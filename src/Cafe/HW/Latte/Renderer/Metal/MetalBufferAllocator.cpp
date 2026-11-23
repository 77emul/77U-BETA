#include "Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.h"
#include "Common/MemoryBudget.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include <limits>
#include <new>
#if TARGET_OS_IPHONE || TARGET_OS_IOS
#include <os/proc.h>
#endif

MetalSynchronizedRingAllocator::~MetalSynchronizedRingAllocator()
{
	// MetalRenderer::Shutdown waits for every submitted command buffer before
	// MetalMemoryManager reaches this destructor, so all retained buffers are
	// safe to release here. Idle excess chunks may be reclaimed under memory pressure;
	// otherwise keep the working ring reusable throughout the game.
	for (auto& buffer : m_buffers)
	{
		if (buffer.mtlBuffer)
		{
			buffer.mtlBuffer->release();
			RuntimePerformance::stagingBufferBytes.fetch_sub(buffer.size, std::memory_order_relaxed);
		}
	}
}

MetalBufferChunkedHeap::~MetalBufferChunkedHeap()
{
	for (auto& chunk : m_chunkBuffers)
		chunk.buffer->release();
}

uint32 MetalBufferChunkedHeap::allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize)
{
	size_t allocationSize = std::max<size_t>(m_minimumBufferAllocationSize, minimumAllocationSize);
	if (allocationSize > std::numeric_limits<uint32>::max())
		return 0;
	MTL::Buffer* buffer = m_mtlr->GetDevice()->newBuffer(allocationSize, m_options);
	if (!buffer)
	{
		if (allocationSize > minimumAllocationSize && minimumAllocationSize > 0)
		{
			allocationSize = minimumAllocationSize;
			buffer = m_mtlr->GetDevice()->newBuffer(allocationSize, m_options);
		}
		if (!buffer)
			return 0;
	}
	// The contents address is stable for this buffer's lifetime. Resolving it
	// for every draw also crosses the expensive Metal capture wrapper when tracing.
	auto* basePtr = static_cast<uint8*>(buffer->contents());
	if (!basePtr)
	{
		buffer->release();
		return 0;
	}
	cemu_assert_debug(m_chunkBuffers.size() == chunkIndex);
	try
	{
		m_chunkBuffers.emplace_back(ChunkBuffer{buffer, basePtr});
	}
	catch (...)
	{
		buffer->release();
		return 0;
	}

	return static_cast<uint32>(allocationSize);
}

#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"

void MetalSynchronizedRingAllocator::addUploadBufferSyncPoint(AllocatorBuffer_t& buffer, uint32 offset)
{
	if (m_constrainedMemory)
		buffer.idleSinceNs = 0;
	auto commandBuffer = m_mtlr->GetCommandBuffer();
	if (commandBuffer == buffer.lastSyncpointCommandBuffer)
		return;
	buffer.lastSyncpointCommandBuffer = commandBuffer;
	buffer.queue_syncPoints.emplace(commandBuffer, offset);
}

void MetalSynchronizedRingAllocator::allocateAdditionalUploadBuffer(uint32 sizeRequiredForAlloc)
{
    // Round without wrapping a 32-bit request into a smaller GPU allocation.
    const uint64_t quantum = m_minimumBufferAllocSize;
    if (!quantum)
        throw std::bad_alloc();
    const uint64_t rounded = std::max<uint64_t>(quantum,
        ((uint64_t(sizeRequiredForAlloc) + quantum - 1) / quantum) * quantum);
    if (rounded > std::numeric_limits<uint32>::max() || rounded > m_mtlr->GetDevice()->maxBufferLength())
        throw std::bad_alloc();
    const uint32 bufferAllocSize = static_cast<uint32>(rounded);

	AllocatorBuffer_t newBuffer{};
	newBuffer.writeIndex = 0;
	newBuffer.basePtr = nullptr;
	newBuffer.mtlBuffer = m_mtlr->GetDevice()->newBuffer(bufferAllocSize, m_options);
	if (!newBuffer.mtlBuffer)
        throw std::bad_alloc();
    newBuffer.basePtr = (uint8*)newBuffer.mtlBuffer->contents();
    if (!newBuffer.basePtr)
    {
        newBuffer.mtlBuffer->release();
        throw std::bad_alloc();
    }
	newBuffer.size = bufferAllocSize;
	// Reuse a trimmed slot so indices of pending reservations never change.
	auto vacant = std::find_if(m_buffers.begin(), m_buffers.end(),
		[](const auto& buffer) { return !buffer.mtlBuffer; });
	newBuffer.index = (uint32)(vacant - m_buffers.begin());
	try
	{
		if (vacant != m_buffers.end())
			*vacant = std::move(newBuffer);
		else
			m_buffers.push_back(newBuffer);
		RuntimePerformance::stagingBufferBytes.fetch_add(newBuffer.size, std::memory_order_relaxed);
	}
	catch (...)
	{
		newBuffer.mtlBuffer->release();
		throw;
	}
}

MetalSynchronizedRingAllocator::AllocatorReservation_t MetalSynchronizedRingAllocator::AllocateBufferMemory(uint32 size, uint32 alignment)
{
	if (alignment < 128)
		alignment = 128;
	if (size > std::numeric_limits<uint32>::max() - 127u)
		throw std::bad_alloc();
	size = (size + 127) & ~127;

	auto tryAllocate = [this, size, alignment](AllocatorReservation_t& res) -> bool {
		// Reuse the most recently successful ring first. Streaming can grow the
		// buffer list, and scanning every older (often busy) chunk for each small
		// vertex or uniform allocation makes draw cost grow with its peak size.
		for (size_t scanned = 0; scanned < m_buffers.size(); ++scanned)
		{
			const size_t bufferIndex = (m_preferredBufferIndex + scanned) % m_buffers.size();
			auto& itr = m_buffers[bufferIndex];
			if (!itr.mtlBuffer)
				continue;

			// Fast-path: check available contiguous capacity without polling GPU completion
			uint32 alignmentPadding = (alignment - (itr.writeIndex % alignment)) % alignment;
			uint32 spaceNeeded = alignmentPadding + size;
			uint32 distanceToSyncPoint = 0xFFFFFFFF;
			if (!itr.queue_syncPoints.empty())
			{
				if (itr.queue_syncPoints.front().offset >= itr.writeIndex)
					distanceToSyncPoint = itr.queue_syncPoints.front().offset - itr.writeIndex;
			}

			if (spaceNeeded <= distanceToSyncPoint && (itr.writeIndex + spaceNeeded) <= itr.size)
			{
				addUploadBufferSyncPoint(itr, itr.writeIndex);
				itr.writeIndex += alignmentPadding;
				uint32 offset = itr.writeIndex;
				itr.writeIndex += size;
				itr.cleanupCounter = 0;
				res.mtlBuffer = itr.mtlBuffer;
				res.memPtr = itr.basePtr + offset;
				res.bufferOffset = offset;
				res.size = size;
				res.bufferIndex = itr.index;
				m_preferredBufferIndex = bufferIndex;

				return true;
			}

			// Eagerly reclaim completed sync points before testing available capacity
			while (!itr.queue_syncPoints.empty())
			{
				auto cb = itr.queue_syncPoints.front().commandBuffer;
				if (!cb || CommandBufferCompleted(cb))
				{
					// The sync point may own the last reference. Clear the borrowed
					// pointer before pop() releases it; never query it afterwards.
					if (itr.lastSyncpointCommandBuffer == cb)
						itr.lastSyncpointCommandBuffer = nullptr;
					itr.queue_syncPoints.pop();
				}
				else
				{
					break;
				}
			}
			// align pointer
			alignmentPadding = (alignment - (itr.writeIndex % alignment)) % alignment;
			if (!itr.queue_syncPoints.empty())
			{
				if (itr.queue_syncPoints.front().offset < itr.writeIndex)
					distanceToSyncPoint = 0xFFFFFFFF;
				else
					distanceToSyncPoint = itr.queue_syncPoints.front().offset - itr.writeIndex;
			}
			else
				distanceToSyncPoint = 0xFFFFFFFF;
			spaceNeeded = alignmentPadding + size;
			if (spaceNeeded > distanceToSyncPoint)
				continue; // not enough space in current buffer
			if ((itr.writeIndex + spaceNeeded) > itr.size)
			{
				// wrap-around
				spaceNeeded = size;
				alignmentPadding = 0;
				// check if there is enough space in current buffer after wrap-around
				if (!itr.queue_syncPoints.empty())
				{
					distanceToSyncPoint = itr.queue_syncPoints.front().offset - 0;
					if (spaceNeeded > distanceToSyncPoint)
						continue;
				}
				else if (spaceNeeded > itr.size)
					continue;
				itr.writeIndex = 0;
			}
			addUploadBufferSyncPoint(itr, itr.writeIndex);
			itr.writeIndex += alignmentPadding;
			uint32 offset = itr.writeIndex;
			itr.writeIndex += size;
			itr.cleanupCounter = 0;
			res.mtlBuffer = itr.mtlBuffer;
			res.memPtr = itr.basePtr + offset;
			res.bufferOffset = offset;
			res.size = size;
			res.bufferIndex = itr.index;
			m_preferredBufferIndex = bufferIndex;

			return true;
		}
		return false;
	};

	AllocatorReservation_t res{};

	// 1. Fast path: try existing buffers immediately
	if (tryAllocate(res))
		return res;

	// 2. Poll GPU completion for submitted command buffers to reclaim sync points
	m_mtlr->ProcessFinishedCommandBuffers();
	if (tryAllocate(res))
		return res;

	// Only submitted work can release staging without changing the currently
	// encoded draw. Never flush here: callers may still hold an active encoder
	// or a decoder reservation which has not been copied into a texture yet.
	auto recycleSubmittedWork = [&]() -> bool {
		for (;;)
		{
			MTL::CommandBuffer* submitted = nullptr;
			for (const auto& buffer : m_buffers)
			{
				if (buffer.queue_syncPoints.empty())
					continue;
				auto* candidate = buffer.queue_syncPoints.front().commandBuffer;
				if (!candidate)
					continue;
				const auto status = candidate->status();
				if (status == MTL::CommandBufferStatusCommitted || status == MTL::CommandBufferStatusScheduled)
				{
					submitted = candidate->retain();
					break;
				}
			}
			if (!submitted)
				return false;
			submitted->waitUntilCompleted();
			submitted->release();
			m_mtlr->ProcessFinishedCommandBuffers();
			CleanupBuffer(nullptr);
			if (tryAllocate(res))
				return true;
		}
	};

	// Small-memory devices should recycle completed upload batches before a
	// burst reserves another chunk. Unsubmitted bytes still require growth;
	// blocking on them would deadlock the renderer. Larger devices retain the
	// existing nonblocking growth path and do not query headroom here.
	if (m_constrainedMemory)
	{
		const size_t activeBuffers = std::count_if(m_buffers.begin(), m_buffers.end(),
			[](const auto& buffer) { return buffer.mtlBuffer != nullptr; });
		bool needsBackpressure = activeBuffers >= 16;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
		needsBackpressure |= MemoryBudget::EffectiveAvailableMemory(os_proc_available_memory()) < 512u * 1024 * 1024;
#endif
		if (needsBackpressure && recycleSubmittedWork())
			return res;
	}

	// 3. Keep enough in-flight upload space on iPadOS to absorb short
	// asset-streaming bursts without a blocking full-GPU drain.
#if BOOST_OS_IOS
	// Match MAX_INFLIGHT_COMMAND_BUFFERS (16) so each in-flight command buffer
	// has headroom during intense streaming scenes (such as BotW Great Plateau descent).
	constexpr size_t kMaxStagingBuffers = 16;
#else
	constexpr size_t kMaxStagingBuffers = 4;
#endif
	bool stagingAllocationFailed = false;
	if (m_buffers.size() < kMaxStagingBuffers)
	{
		try
		{
			allocateAdditionalUploadBuffer(size);
			if (tryAllocate(res))
				return res;
		}
		catch (const std::bad_alloc&)
		{
			stagingAllocationFailed = true;
		}
	}

	// 4. Preserve nonblocking growth on unrestricted devices. Constrained
	// devices also reach here if only unsubmitted work holds the full ring;
	// those reservations must stay intact until their caller encodes the copy.
	if (!stagingAllocationFailed)
	{
		try
		{
			allocateAdditionalUploadBuffer(size);
			if (tryAllocate(res))
				return res;
		}
		catch (const std::bad_alloc&)
		{
			stagingAllocationFailed = true;
		}
	}

	// 5. Recover from allocation failure using submitted work first, including
	// when an active render encoder makes a global flush unsafe.
	if (stagingAllocationFailed && recycleSubmittedWork())
		return res;
	if (!m_mtlr->GetCommandEncoder() || m_mtlr->GetEncoderType() != MetalEncoderType::Render)
	{
		m_mtlr->Flush(true);
		if (tryAllocate(res))
			return res;
	}

	throw std::bad_alloc();
}

void MetalSynchronizedRingAllocator::FlushReservation(AllocatorReservation_t& uploadReservation)
{
    if (RequiresFlush())
    {
        uploadReservation.mtlBuffer->didModifyRange(NS::Range(uploadReservation.bufferOffset, uploadReservation.size));
    }
}

void MetalSynchronizedRingAllocator::CleanupBuffer(MTL::CommandBuffer* latestFinishedCommandBuffer)
{
	for (auto& itr : m_buffers)
	{
		while (!itr.queue_syncPoints.empty())
		{
			auto cb = itr.queue_syncPoints.front().commandBuffer;
			if (cb == latestFinishedCommandBuffer || !cb || CommandBufferCompleted(cb))
			{
				if (itr.lastSyncpointCommandBuffer == cb)
					itr.lastSyncpointCommandBuffer = nullptr;
				itr.queue_syncPoints.pop();
			}
			else
			{
				break;
			}
		}
		if (itr.queue_syncPoints.empty())
			itr.cleanupCounter++;
	}

	// Do not destroy staging buffers in the middle of an active render session.
	// They are ring buffers and are immediately reusable once their sync queue is
	// empty. Releasing the last 32 MB block here caused an iPadOS std::bad_alloc
	// termination while Splatoon transitioned from matchmaking into a stage, and
	// it also made later uploads allocate the same block again. The destructor
	// releases the retained peak after MetalRenderer has drained the GPU.
	// TrimIdleBuffers separately reclaims completed excess chunks under pressure.
}

void MetalSynchronizedRingAllocator::ReleaseUploadBuffer(AllocatorBuffer_t& buffer)
{
	buffer.mtlBuffer->release();
	RuntimePerformance::stagingBufferBytes.fetch_sub(buffer.size, std::memory_order_relaxed);
	buffer.mtlBuffer = nullptr;
	buffer.basePtr = nullptr;
	buffer.size = 0;
	buffer.writeIndex = 0;
	buffer.lastSyncpointCommandBuffer = nullptr;
	buffer.idleSinceNs = 0;
}

size_t MetalSynchronizedRingAllocator::TrimIdleBuffers()
{
	// Pending reservations, including unsubmitted copies, remain protected by
	// their sync queue. This runs on the renderer thread, never a memory-warning
	// callback or compiler worker, and does not wait for GPU completion.
	CleanupBuffer(nullptr);
	size_t releasedBytes = 0;
	for (size_t i = 0; i < m_buffers.size(); ++i)
	{
		auto& buffer = m_buffers[i];
		// Keep the preferred working chunk to avoid normal-frame reallocation.
		if (!buffer.mtlBuffer || i == m_preferredBufferIndex || !buffer.queue_syncPoints.empty())
			continue;
		releasedBytes += buffer.size;
		ReleaseUploadBuffer(buffer);
	}
	// Keep stable slots: pending reservation indices must not move. New uploads
	// lazily recreate vacant chunks only when the working ring cannot fit them.
	return releasedBytes;
}

size_t MetalSynchronizedRingAllocator::TrimLongIdleBuffers(uint64_t nowNs)
{
	// Native high-memory devices retain their existing peak-reuse behavior.
	if (!m_constrainedMemory || !nowNs)
		return 0;
	CleanupBuffer(nullptr);
	constexpr uint64_t graceNs = uint64_t{30} * 1000 * 1000 * 1000;
	size_t live = std::count_if(m_buffers.begin(), m_buffers.end(),
		[](const auto& buffer) { return buffer.mtlBuffer != nullptr; });
	size_t released = 0;
	for (size_t i = 0; i < m_buffers.size(); ++i)
	{
		auto& buffer = m_buffers[i];
		if (!buffer.mtlBuffer)
			continue;
		if (i == m_preferredBufferIndex || !buffer.queue_syncPoints.empty())
		{
			buffer.idleSinceNs = 0;
			continue;
		}
		if (!buffer.idleSinceNs || nowNs < buffer.idleSinceNs)
			buffer.idleSinceNs = nowNs;
		// Keep two working chunks for the existing two-batch upload pipeline.
		// Growth is still allowed; this is idle reclamation, not an allocation cap.
		if (live > 2 && nowNs - buffer.idleSinceNs >= graceNs)
		{
			released += buffer.size;
			ReleaseUploadBuffer(buffer);
			--live;
		}
	}
	return released;
}

size_t MetalSynchronizedRingAllocator::GetReclaimableIdleBytes() const
{
    size_t bytes = 0;
    for (size_t i = 0; i < m_buffers.size(); ++i)
        if (i != m_preferredBufferIndex && m_buffers[i].mtlBuffer && m_buffers[i].queue_syncPoints.empty())
            bytes += m_buffers[i].size;
    return bytes;
}

MTL::Buffer* MetalSynchronizedRingAllocator::GetBufferByIndex(uint32 index) const
{
	return m_buffers[index].mtlBuffer;
}

void MetalSynchronizedRingAllocator::GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const
{
	numBuffers = 0;
	totalBufferSize = 0;
	freeBufferSize = 0;
	for (auto& itr : m_buffers)
	{
		if (!itr.mtlBuffer)
			continue;
		++numBuffers;
		totalBufferSize += itr.size;
		// calculate free space in buffer
		uint32 distanceToSyncPoint;
		if (!itr.queue_syncPoints.empty())
		{
			if (itr.queue_syncPoints.front().offset < itr.writeIndex)
				distanceToSyncPoint = (itr.size - itr.writeIndex) + itr.queue_syncPoints.front().offset; // size with wrap-around
			else
				distanceToSyncPoint = itr.queue_syncPoints.front().offset - itr.writeIndex;
		}
		else
			distanceToSyncPoint = itr.size;
		freeBufferSize += distanceToSyncPoint;
	}
}

/* MetalSynchronizedHeapAllocator */

MetalSynchronizedHeapAllocator::~MetalSynchronizedHeapAllocator()
{
	for (auto& [cb, allocations] : m_releaseQueue)
	{
		for (auto& addr : allocations)
			m_chunkedHeap.free(addr);
		if (cb)
			cb->release();
	}
	m_releaseQueue.clear();
}

MetalSynchronizedHeapAllocator::AllocatorReservation* MetalSynchronizedHeapAllocator::AllocateBufferMemory(uint32 size, uint32 alignment, bool boundedIndexSearch)
{
	uint32 allocationSize = size;
	if (boundedIndexSearch)
	{
		// Index offsets require 128-byte alignment. Reserving only a 32-byte
		// multiple leaves tiny free heads which subsequent draws scan repeatedly.
		// Keep the requested size for uploads; reserve its alignment padding too.
		cemu_assert_debug(alignment >= 32 && std::has_single_bit(alignment));
		if (size > 0x7FFFFFFFu - alignment)
			throw std::bad_alloc();
		allocationSize = (std::max(size, 1u) + alignment - 1) & ~(alignment - 1);
	}
	CHAddr addr = boundedIndexSearch ? m_chunkedHeap.allocWithBoundedSearch<16>(allocationSize, alignment) :
		m_chunkedHeap.alloc(allocationSize, alignment);

	if (!addr.isValid() || !m_chunkedHeap.GetChunkPtr(addr.chunkIndex))
	{
		m_mtlr->ProcessFinishedCommandBuffers();
		CleanupBuffer(nullptr);
		addr = boundedIndexSearch ? m_chunkedHeap.allocWithBoundedSearch<16>(allocationSize, alignment) :
			m_chunkedHeap.alloc(allocationSize, alignment);
	}

	if (!addr.isValid() || !m_chunkedHeap.GetChunkPtr(addr.chunkIndex))
	{
		if (!m_releaseQueue.empty())
		{
			for (auto& [cb, allocations] : m_releaseQueue)
			{
				if (cb)
					cb->waitUntilCompleted();
			}
			m_mtlr->ProcessFinishedCommandBuffers();
			CleanupBuffer(nullptr);
			addr = boundedIndexSearch ? m_chunkedHeap.allocWithBoundedSearch<16>(allocationSize, alignment) :
				m_chunkedHeap.alloc(allocationSize, alignment);
		}
	}

	AllocatorReservation* res = m_poolAllocatorReservation.allocObj();
	res->bufferIndex = addr.chunkIndex;
	res->bufferOffset = addr.offset;
	res->size = size;
	res->chAddr = addr;
	res->mtlBuffer = m_chunkedHeap.GetBufferByIndex(addr.chunkIndex);
	uint8* chunkPtr = m_chunkedHeap.GetChunkPtr(addr.chunkIndex);
	res->memPtr = chunkPtr ? (chunkPtr + addr.offset) : nullptr;

	return res;
}

void MetalSynchronizedHeapAllocator::FreeReservation(AllocatorReservation* uploadReservation)
{
	// put the allocation on a delayed release queue for the current command buffer
	MTL::CommandBuffer* currentCommandBuffer = m_mtlr->GetCommandBuffer();
	if (!currentCommandBuffer)
	{
		m_chunkedHeap.free(uploadReservation->chAddr);
		m_poolAllocatorReservation.freeObj(uploadReservation);
		return;
	}
	auto& queue = m_releaseQueue[currentCommandBuffer];
	if (queue.empty())
	{
		currentCommandBuffer->retain();
	}
	queue.emplace_back(uploadReservation->chAddr);
	m_poolAllocatorReservation.freeObj(uploadReservation);
}

void MetalSynchronizedHeapAllocator::FlushReservation(AllocatorReservation* uploadReservation)
{
	if (m_chunkedHeap.RequiresFlush())
	{
	    uploadReservation->mtlBuffer->didModifyRange(NS::Range(uploadReservation->bufferOffset, uploadReservation->size));
	}
}

void MetalSynchronizedHeapAllocator::CleanupBuffer(MTL::CommandBuffer* latestFinishedCommandBuffer)
{
	for (auto it = m_releaseQueue.begin(); it != m_releaseQueue.end(); )
	{
		auto cb = it->first;
		if (cb == latestFinishedCommandBuffer || !cb || CommandBufferCompleted(cb))
		{
			for (auto& addr : it->second)
				m_chunkedHeap.free(addr);
			if (cb)
				cb->release();
			it = m_releaseQueue.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void MetalSynchronizedHeapAllocator::GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const
{
	m_chunkedHeap.GetStats(numBuffers, totalBufferSize, freeBufferSize);
}
