#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || TARGET_OS_IOS
#include <sys/sysctl.h>
#endif
#endif

// Storage granularity and compilation parallelism, not guest-memory limits.
// A failed hardware query preserves the existing unrestricted policy.
struct MetalMemoryPolicy
{
    bool constrained = false;
    size_t stagingChunkBytes = 64u * 1024 * 1024;
    size_t indexChunkBytes = 16u * 1024 * 1024;
    // Zero preserves unrestricted scratch reuse. Only large, completed CPU
    // decoder scratch is dropped on small devices; GPU staging has its own life.
    size_t textureScratchRetainBytes = 0;

    size_t CompilationWorkers(size_t legacyWorkerCount) const
    {
        return (constrained ? std::min<size_t>(legacyWorkerCount, 2) : legacyWorkerCount);
    }

    // Shader/pipeline cache loading screens may use more threads: little else
    // is resident yet (Splatoon on the iPhone 12 profile: ~800 MiB of a 2 GiB
    // budget while loading 2195 shaders). Four matches an A14's usable cores.
    size_t LoadingWorkers(size_t legacyWorkerCount) const
    {
        return (constrained ? std::min<size_t>(legacyWorkerCount, 4) : legacyWorkerCount);
    }
};

constexpr MetalMemoryPolicy MakeMetalMemoryPolicy(uint64_t physicalMemoryBytes)
{
    constexpr uint64_t GiB = uint64_t{1024} * 1024 * 1024;
    if (physicalMemoryBytes && physicalMemoryBytes <= 6 * GiB)
        return {true, 16u * 1024 * 1024, 4u * 1024 * 1024, 2u * 1024 * 1024};
    return {};
}

inline const MetalMemoryPolicy& GetMetalMemoryPolicy()
{
    static const MetalMemoryPolicy policy = [] {
#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_IOS)
        uint64_t physicalBytes = 0;
        size_t size = sizeof(physicalBytes);
        if (sysctlbyname("hw.memsize", &physicalBytes, &size, nullptr, 0) == 0 &&
            size == sizeof(physicalBytes))
            return MakeMetalMemoryPolicy(physicalBytes);
#endif
        return MetalMemoryPolicy{};
    }();
    return policy;
}
