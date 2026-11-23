#pragma once
#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

// The decompiler prefixes MSL with "// shader <16 hex digits>". This is
// diagnostic metadata: different guest hashes can emit byte-identical programs.
// Strip only that exact header from the reuse key, never normalize program text.
inline std::string_view MetalShaderLibrarySourceKey(std::string_view source)
{
    constexpr std::string_view prefix = "// shader ";
    constexpr size_t hashEnd = prefix.size() + 16;
    if (!source.starts_with(prefix) || source.size() <= hashEnd)
        return source;
    for (size_t i = prefix.size(); i < hashEnd; ++i)
        if (!((source[i] >= '0' && source[i] <= '9') || (source[i] >= 'a' && source[i] <= 'f')))
            return source;
    if (source.substr(hashEnd, 2) == "\r\n")
        return source.substr(hashEnd + 2);
    if (source[hashEnd] == '\n')
        return source.substr(hashEnd + 1);
    return source;
}

// Called only when a new shader is compiled, never on the draw/cache-hit path.
// Exact source equality is required: the guest's aux hash may include states
// that do not affect emitted MSL, but guessing equivalent programs is unsafe.
struct MetalShaderCacheStatistics
{
    uint64_t requests = 0, hits = 0, misses = 0, waits = 0, evictions = 0, uncached = 0;
    size_t entries = 0, sourceBytes = 0;
};

template<class Library>
class MetalShaderLibraryCache
{
    struct KeyView { const void* device; uint32_t options; std::string_view source; };
    struct Key {
        const void* device;
        uint32_t options;
        std::string source;
        operator KeyView() const { return {device, options, source}; }
    };
    struct Hash {
        using is_transparent = void;
        size_t operator()(KeyView key) const {
            return std::hash<std::string_view>{}(key.source) ^
                std::hash<const void*>{}(key.device) ^ (size_t(key.options) << 1);
        }
    };
    struct Equal {
        using is_transparent = void;
        bool operator()(KeyView a, KeyView b) const {
            return a.device == b.device && a.options == b.options && a.source == b.source;
        }
    };
    struct Entry {
        std::condition_variable ready;
        std::shared_ptr<Library> library;
        uint64_t lastUse = 0;
        bool compiling = true;
    };
public:
    explicit MetalShaderLibraryCache(size_t maxEntries = 256, size_t maxSourceBytes = 4 * 1024 * 1024)
        : m_maxEntries(maxEntries), m_maxSourceBytes(maxSourceBytes) {}

    template<class Compile>
    std::shared_ptr<Library> GetOrCompile(const void* device, uint32_t options, std::string_view source, Compile compile)
    {
        const KeyView key{device, options, source};
        std::unique_lock lock(m_mutex);
        ++m_statistics.requests;
        if (auto found = m_entries.find(key); found != m_entries.end()) {
            ++m_statistics.hits;
            m_reuses.fetch_add(1, std::memory_order_relaxed);
            auto entry = found->second;
            entry->lastUse = ++m_clock;
            if (entry->compiling) ++m_statistics.waits;
            entry->ready.wait(lock, [&] { return !entry->compiling; });
            return entry->library;
        }
        ++m_statistics.misses;
        if (source.size() > m_maxSourceBytes || m_maxEntries == 0) {
            ++m_statistics.uncached;
            lock.unlock();
            return compile();
        }
        while (m_entries.size() >= m_maxEntries || m_sourceBytes + source.size() > m_maxSourceBytes) {
            auto oldest = m_entries.end();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
                if (!it->second->compiling && (oldest == m_entries.end() || it->second->lastUse < oldest->second->lastUse))
                    oldest = it;
            // Do not evict an in-flight job: other workers must share its result.
            // If all slots are busy, compile uncached rather than grow memory.
            if (oldest == m_entries.end()) {
                ++m_statistics.uncached;
                lock.unlock();
                return compile();
            }
            m_sourceBytes -= oldest->first.source.size();
            m_entries.erase(oldest);
            ++m_statistics.evictions;
        }
        auto entry = std::make_shared<Entry>();
        entry->lastUse = ++m_clock;
        m_entries.emplace(Key{device, options, std::string(source)}, entry);
        m_sourceBytes += source.size();
        lock.unlock();
        auto publish = [&](std::shared_ptr<Library> library) {
            std::lock_guard publishLock(m_mutex);
            entry->library = std::move(library);
            entry->compiling = false;
            // A failed compiler invocation must remain retryable. Clear() may
            // have removed this job while another game started; don't erase it.
            if (!entry->library) {
                auto found = m_entries.find(key);
                if (found != m_entries.end() && found->second == entry) {
                    m_sourceBytes -= found->first.source.size();
                    m_entries.erase(found);
                }
            }
            entry->ready.notify_all();
        };
        try { publish(compile()); }
        catch (...) { publish({}); throw; }
        return entry->library;
    }

    uint64_t GetReuseCount() const { return m_reuses.load(std::memory_order_relaxed); }

    MetalShaderCacheStatistics GetStatistics() {
        std::lock_guard lock(m_mutex);
        auto result = m_statistics;
        result.entries = m_entries.size();
        result.sourceBytes = m_sourceBytes;
        return result;
    }

    size_t TrimCompleted() {
        std::lock_guard lock(m_mutex);
        size_t released = 0;
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->second->compiling) { ++it; continue; }
            released += it->first.source.size();
            m_sourceBytes -= it->first.source.size();
            it = m_entries.erase(it);
        }
        return released;
    }
    size_t RetainedSourceBytes() {
        std::lock_guard lock(m_mutex); return m_sourceBytes;
    }
    void Clear()
    {
        std::lock_guard lock(m_mutex);
        m_entries.clear();
        m_sourceBytes = 0;
        m_statistics = {};
        m_reuses.store(0, std::memory_order_relaxed);
    }
private:
    std::mutex m_mutex;
    std::unordered_map<Key, std::shared_ptr<Entry>, Hash, Equal> m_entries;
    size_t m_maxEntries, m_maxSourceBytes, m_sourceBytes = 0;
    uint64_t m_clock = 0;
    std::atomic<uint64_t> m_reuses{0};
    MetalShaderCacheStatistics m_statistics{}; // Guarded by the existing cache mutex.
};
