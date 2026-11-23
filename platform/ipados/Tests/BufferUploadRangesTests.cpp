#include "Cafe/HW/Latte/Renderer/Metal/BufferUploadRanges.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

int main()
{
    BufferUploadRanges<unsigned> ranges;
    unsigned buffer = 0;
    uint32_t source = 0;
    ranges.Insert(100, 100, 1, 500);
    ranges.Insert(140, 20, 2, 800);
    assert(!ranges.Resolve(100, 100, buffer, source)); // Must not return stale upload 1.
    assert(ranges.Resolve(100, 40, buffer, source) && buffer == 1 && source == 500);
    assert(ranges.Resolve(140, 20, buffer, source) && buffer == 2 && source == 800);
    assert(ranges.Resolve(160, 40, buffer, source) && buffer == 1 && source == 560);
    assert(!ranges.Overlaps(0, 100));
    assert(!ranges.Overlaps(200, 10));
    assert(ranges.Overlaps(99, 2));
    assert(!ranges.Overlaps(140, 0));
    ranges.Insert(0xfffffff0u, 16, 3, 0);
    assert(ranges.Resolve(0xffffffffu, 1, buffer, source) && buffer == 3 && source == 15);
    assert(!ranges.Resolve(0xffffffffu, 2, buffer, source));
    ranges.Take();
    assert(ranges.empty());

    // A render-pass upload may only use 16-byte vector copies. Rejecting a
    // split or unaligned range must leave every byte for the blit fallback.
    ranges.Insert(32, 32, 4, 64);
    ranges.Insert(48, 16, 5, 96);
    auto aligned = ranges.TakeIfAligned(16);
    assert(aligned && aligned->size() == 2);
    assert(aligned->at(32).buffer == 4 && aligned->at(32).size == 16);
    assert(aligned->at(48).buffer == 5 && aligned->at(48).bufferOffset == 96);
    assert(ranges.empty());

    ranges.Insert(32, 32, 4, 64);
    ranges.Insert(40, 16, 5, 96);
    assert(!ranges.TakeIfAligned(16));
    assert(ranges.Overlaps(32, 32));
    assert(ranges.Take().size() == 3);

    ranges.Insert(64, 65552, 6, 128);
    assert(!ranges.TakeIfAligned(16, 65536));
    assert(ranges.Overlaps(64, 65552));
    ranges.Take();

    // Compare real range operations with a bytewise reference under overlapping writes.
    std::array<unsigned, 1024> expected{};
    std::array<uint32_t, 1024> expectedSource{};
    std::mt19937 random(77);
    for (unsigned id = 1; id <= 10000; ++id)
    {
        uint32_t offset = random() % 1024;
        uint32_t size = 1 + random() % (1024 - offset);
        ranges.Insert(offset, size, id, 32);
        for (uint32_t i = 0; i < size; ++i)
        {
            expected[offset + i] = id;
            expectedSource[offset + i] = 32 + i;
        }
        for (unsigned query = 0; query < 16; ++query)
        {
            offset = random() % 1024;
            size = 1 + random() % (1024 - offset);
            bool overlaps = false;
            bool contiguous = expected[offset] != 0;
            for (uint32_t i = 0; i < size; ++i)
            {
                overlaps |= expected[offset + i] != 0;
                contiguous &= expected[offset + i] == expected[offset] &&
                    expectedSource[offset + i] == expectedSource[offset] + i;
            }
            assert(ranges.Overlaps(offset, size) == overlaps);
            assert(ranges.Resolve(offset, size, buffer, source) == contiguous);
            if (contiguous)
                assert(buffer == expected[offset] && source == expectedSource[offset]);
        }
        if (id % 100 == 0)
        {
            std::array<unsigned, 1024> flushed{};
            for (const auto& [begin, range] : ranges.Take())
                for (uint32_t i = 0; i < range.size; ++i)
                {
                    assert(flushed[begin + i] == 0);
                    flushed[begin + i] = range.buffer;
                    assert(range.bufferOffset + i == expectedSource[begin + i]);
                }
            assert(flushed == expected);
            expected.fill(0);
            expectedSource.fill(0);
            assert(ranges.empty());
        }
    }
}
