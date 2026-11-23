#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Fixed-capacity FIFO. The caller provides synchronization. No allocations or
// movement of queued audio during read/write; allocate once before stream start.
class AudioByteQueue
{
public:
    void reserve(size_t capacity) { assert(empty()); m_storage.resize(capacity); m_read = 0; }
    size_t capacity() const { return m_storage.size(); }
    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    void write(const uint8_t* source, size_t count)
    {
        assert(count <= capacity() - m_size);
        if (!count) return;
        const size_t offset = (m_read + m_size) % capacity();
        const size_t first = std::min(count, capacity() - offset);
        std::memcpy(m_storage.data() + offset, source, first);
        if (count > first) std::memcpy(m_storage.data(), source + first, count - first);
        m_size += count;
    }
    size_t read(void* destination, size_t requested)
    {
        const size_t count = std::min(requested, m_size);
        if (!count) return 0;
        const size_t first = std::min(count, capacity() - m_read);
        std::memcpy(destination, m_storage.data() + m_read, first);
        if (count > first) std::memcpy(static_cast<uint8_t*>(destination) + first, m_storage.data(), count - first);
        m_read = (m_read + count) % capacity();
        m_size -= count;
        return count;
    }
private:
    std::vector<uint8_t> m_storage;
    size_t m_read = 0;
    size_t m_size = 0;
};
