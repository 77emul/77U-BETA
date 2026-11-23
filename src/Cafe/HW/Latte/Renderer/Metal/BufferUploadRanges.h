#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <utility>

// Non-overlapping views of the newest pending bytes in the private buffer cache.
// Source allocations are immutable and remain owned by the staging allocator.
template<typename Buffer>
class BufferUploadRanges
{
public:
    struct Range
    {
        uint32_t size;
        Buffer buffer;
        uint32_t bufferOffset;
    };
    using Map = std::map<uint32_t, Range>;

    bool empty() const { return m_ranges.empty(); }

    void Insert(uint32_t offset, uint32_t size, Buffer buffer, uint32_t bufferOffset)
    {
        if (!size)
            return;
        const uint64_t end = uint64_t(offset) + size;
        m_maxEnd = std::max(m_maxEnd, end);
        auto it = m_ranges.lower_bound(offset);
        if (it != m_ranges.begin())
        {
            auto previous = std::prev(it);
            if (uint64_t(previous->first) + previous->second.size > offset)
                it = previous;
        }
        while (it != m_ranges.end() && it->first < end)
        {
            const auto begin = it->first;
            const auto old = it->second;
            const uint64_t oldEnd = uint64_t(begin) + old.size;
            it = m_ranges.erase(it);
            if (begin < offset)
                m_ranges.emplace(begin, Range{offset - begin, old.buffer, old.bufferOffset});
            if (oldEnd > end)
            {
                m_ranges.emplace(uint32_t(end), Range{uint32_t(oldEnd - end), old.buffer,
                    old.bufferOffset + uint32_t(end - begin)});
                break;
            }
        }
        m_ranges.emplace(offset, Range{size, buffer, bufferOffset});
    }

    bool Resolve(uint32_t offset, uint32_t size, Buffer& buffer, uint32_t& bufferOffset) const
    {
        if (!size || m_ranges.empty())
            return false;
        const uint64_t end = uint64_t(offset) + size;
        if (offset < m_ranges.begin()->first || end > m_maxEnd)
            return false;
        auto it = m_ranges.upper_bound(offset);
        if (it == m_ranges.begin())
            return false;
        --it;
        if (end > uint64_t(it->first) + it->second.size)
            return false;
        buffer = it->second.buffer;
        bufferOffset = it->second.bufferOffset + (offset - it->first);
        return true;
    }

    bool Overlaps(uint32_t offset, uint32_t size) const
    {
        if (!size || m_ranges.empty())
            return false;
        const uint64_t end = uint64_t(offset) + size;
        if (end <= m_ranges.begin()->first || offset >= m_maxEnd)
            return false;
        auto it = m_ranges.lower_bound(offset);
        if (it != m_ranges.end() && it->first < end)
            return true;
        if (it == m_ranges.begin())
            return false;
        --it;
        return uint64_t(it->first) + it->second.size > offset;
    }

    Map Take()
    {
        m_maxEnd = 0;
        Map result;
        result.swap(m_ranges);
        return result;
    }

    // A render-pass helper copies complete vectors. Keep every pending byte
    // for the blit fallback if overlapping writes split a range off alignment.
    std::optional<Map> TakeIfAligned(uint32_t alignment, uint32_t maxRangeSize = UINT32_MAX)
    {
        if (!alignment || (alignment & (alignment - 1)))
            return std::nullopt;
        for (const auto& [offset, range] : m_ranges)
            if (range.size > maxRangeSize ||
                ((offset | range.size | range.bufferOffset) & (alignment - 1)))
                return std::nullopt;
        return Take();
    }

private:
    Map m_ranges;
    uint64_t m_maxEnd = 0;
};
