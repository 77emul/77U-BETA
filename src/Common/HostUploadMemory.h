#pragma once
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

// Retained CPU allocation capacities, NOT resident/physical memory estimates.
// Updated by the owning render thread only when storage grows or is released;
// diagnostic readers must not access the vectors concurrently.
namespace HostUploadMemory
{
inline std::atomic<uint64_t> textureScratchCapacity{0};
inline std::atomic<uint64_t> bufferSnapshotCapacity{0};
// Allocated capacities overlap Metal/process totals. Heap usage includes deferred
// offsets until completion, and texture roots exclude views and queued-only refs.
inline std::atomic<uint64_t> gpuCacheBackingBytes{0}, gpuCacheVirtualBytes{0}, gpuCacheUsedBytes{0}, gpuCacheAllocationCount{0}, gpuCacheGrowthCount{0};
inline std::atomic<uint64_t> gpuCacheRequiredEndBytes{0}, idleRAMTextureEvictions{0};
inline std::atomic<uint64_t> trackedTextureRootBytes{0}, trackedTextureRootCount{0};
// Disjoint creation categories for unique roots, not additional allocations.
// RenderTargetEligible describes usage capability, not a proven GPU write.
enum class TextureCategory : size_t { Other, DecodedBC, Depth, RenderTargetEligible, Count };
inline std::array<std::atomic<uint64_t>, size_t(TextureCategory::Count)> textureCategoryBytes{};
inline std::atomic<uint64_t> textureRootAllocatedBytes{0}, textureRootReleasedBytes{0};
inline std::atomic<uint64_t> stagingFreeBytes{0}, stagingReclaimableBytes{0}, stagingBufferCount{0};
inline std::atomic<uint64_t> indexCapacityBytes{0}, indexUsedBytes{0}, indexBufferCount{0};
inline std::atomic<uint64_t> outstandingCommandBuffers{0};
inline std::atomic<uint64_t> textureUploadSlices{0}, textureUploadBytes{0};
inline std::atomic<uint64_t> bcDecodeInputBytes{0}, bcDecodeOutputBytes{0}, bcDecodeFailures{0};
}
