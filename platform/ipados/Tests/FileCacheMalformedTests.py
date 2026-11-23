#!/usr/bin/env python3
"""Reject damaged shader caches before large allocations, using the real cache/stream."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
existing = (repo / 'platform/ipados/Tests/FileCachePersistenceTests.py').read_text()
stream_test = (repo / 'platform/ipados/Tests/FileStreamSaveIntegrityTests.py').read_text()
precompiled = stream_test.split('precompiled = r"""', 1)[1].split('"""', 1)[0]
precompiled += existing.split("precompiled=stream_test[a:b]+r'''", 1)[1].split("'''", 1)[0]

fixture = r'''
#include "Common/precompiled.h"
#include "Cemu/FileCache/FileCache.h"
#include <iostream>
#include <new>
#include <stdexcept>
static size_t largestAllocation = 0;
void* operator new(size_t size) {
    largestAllocation = std::max(largestAllocation, size);
    // A regression must fail this test rather than allocate a forged 4 GiB.
    if (size > 32 * 1024 * 1024) throw std::bad_alloc();
    if (void* value = malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { free(value); }
void operator delete[](void* value) noexcept { free(value); }
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class T> T readAt(const fs::path& path, uint64 position) {
    T value{}; std::ifstream input(path, std::ios::binary);
    input.seekg(position); input.read(reinterpret_cast<char*>(&value), sizeof(value));
    require(bool(input), "fixture read failed"); return value;
}
template<class T> void writeAt(const fs::path& path, uint64 position, T value) {
    std::fstream output(path, std::ios::in | std::ios::out | std::ios::binary);
    output.seekp(position); output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    require(bool(output), "fixture write failed");
}
int main(int argc, char** argv) {
    const fs::path root = argv[1];
    const auto plain = root / "plain.bin", compressed = root / "compressed.bin";
    const std::vector<uint8> data(1024 * 1024, 71);
    for (bool useCompression : {false, true}) {
        const auto path = useCompression ? compressed : plain;
        std::unique_ptr<FileCache> cache(FileCache::Create(path, 123));
        require(bool(cache), "create failed"); cache->UseCompression(useCompression);
        cache->AddFile({1, 2}, data.data(), data.size()); cache.reset();
        cache.reset(FileCache::Open(path, false, 123));
        std::vector<uint8> result;
        require(cache && cache->GetFile({1, 2}, result) && result == data, "valid large record rejected");
    }
    const auto copy = [&](const fs::path& source) {
        const auto victim = root / "victim.bin";
        fs::copy_file(source, victim, fs::copy_options::overwrite_existing); return victim;
    };
    const auto rejectedOpen = [&](const fs::path& victim) {
        largestAllocation = 0;
        std::unique_ptr<FileCache> cache(FileCache::Open(victim, false, 123));
        require(!cache, "malformed cache opened");
        require(largestAllocation < 64 * 1024, "malformed table triggered a large allocation");
    };
    auto victim = copy(plain);
    writeAt<uint32>(victim, 24, 0xffffffe0u); rejectedOpen(victim);
    victim = copy(plain); writeAt<uint32>(victim, 24, 0); rejectedOpen(victim);
    victim = copy(plain); writeAt<uint64>(victim, 8, UINT64_MAX - 15); rejectedOpen(victim);
    victim = copy(plain); writeAt<uint64>(victim, 16, UINT64_MAX - 15); rejectedOpen(victim);
    const uint64 dataOffset = readAt<uint64>(plain, 8);
    const uint64 tableOffset = readAt<uint64>(plain, 16);
    const uint64 entry = dataOffset + tableOffset + 32; // record after table's own slot
    victim = copy(plain); writeAt<uint32>(victim, entry + 24, 0xfffffff0u); rejectedOpen(victim);
    victim = copy(plain); writeAt<uint64>(victim, entry + 16, UINT64_MAX - 15); rejectedOpen(victim);
    victim = copy(plain); fs::resize_file(victim, 27); rejectedOpen(victim);

    // A file provider or interrupted write may truncate a cache after opening it.
    victim = copy(plain);
    {
        std::unique_ptr<FileCache> cache(FileCache::Open(victim, false, 123));
        require(bool(cache), "fixture open failed"); fs::resize_file(victim, fs::file_size(victim) - 1);
        std::vector<uint8> result{99};
        require(!cache->GetFile({1, 2}, result) && result.empty(), "truncated record returned as valid");
    }

    const uint64 compressedEntry = readAt<uint64>(compressed, 8) + readAt<uint64>(compressed, 16) + 32;
    const uint64 payload = readAt<uint64>(compressed, 8) + readAt<uint64>(compressed, compressedEntry + 16);
    victim = copy(compressed);
    writeAt<uint32>(victim, payload, 0xf0ffffffu); // big-endian uncompressed length 0xfffffff0
    {
        std::unique_ptr<FileCache> cache(FileCache::Open(victim, false, 123));
        require(bool(cache), "length-prefix fixture should open");
        std::vector<uint8> result; largestAllocation = 0;
        require(!cache->GetFile({1, 2}, result) && result.empty(), "forged decompressed length accepted");
        require(largestAllocation < 64 * 1024, "forged length triggered huge output allocation");
    }
    // Validate the entire compressed stream, including trailing garbage.
    victim = copy(compressed);
    { std::ofstream output(victim, std::ios::binary | std::ios::app); output.put('x'); }
    writeAt<uint32>(victim, compressedEntry + 24, readAt<uint32>(compressed, compressedEntry + 24) + 1);
    {
        std::unique_ptr<FileCache> cache(FileCache::Open(victim, false, 123));
        require(bool(cache), "trailing-data fixture should open");
        std::vector<uint8> result;
        require(!cache->GetFile({1, 2}, result) && result.empty(), "trailing compressed data accepted");
    }

    // V2's padding/flags were unspecified; legitimate old caches still load.
    victim = copy(plain); writeAt<uint32>(victim, 0, 0x8371b695u);
    writeAt<uint32>(victim, entry + 28, 0xffffffffu);
    {
        std::unique_ptr<FileCache> cache(FileCache::Open(victim, false, 123));
        std::vector<uint8> result;
        require(cache && cache->GetFile({1, 2}, result) && result == data, "legacy V2 cache rejected");
    }
    // Rejected allocation must preserve the usable cache, not dereference NULL
    // or discard the old table before its replacement exists.
    extern bool failCacheMalloc, failCacheRealloc;
    failCacheMalloc = true;
    { std::unique_ptr<FileCache> cache(FileCache::Create(root / "failed-create.bin", 123));
      require(!cache && !failCacheMalloc, "failed table allocation did not fail cleanly"); }
    failCacheMalloc = true;
    { std::unique_ptr<FileCache> cache(FileCache::Open(plain, false, 123));
      require(!cache && !failCacheMalloc, "failed table loading did not fail cleanly"); }
    const auto growth = root / "growth.bin";
    {
        std::unique_ptr<FileCache> cache(FileCache::Create(growth, 123)); cache->UseCompression(false);
        for (uint64 id = 1; id < 32; ++id) cache->AddFile({id, 0}, data.data(), 4);
        failCacheRealloc = true;
        cache->AddFile({32, 0}, data.data(), 4);
        require(!failCacheRealloc && cache->GetFileCount() == 31, "failed growth corrupted record count");
        for (uint64 id = 1; id < 32; ++id) {
            std::vector<uint8> result;
            require(cache->GetFile({id, 0}, result) && result == std::vector<uint8>(4, 71), "failed growth damaged old data");
        }
        cache->AddFile({32, 0}, data.data(), 4);
    }
    {
        std::unique_ptr<FileCache> cache(FileCache::Open(growth, false, 123));
        require(cache && cache->GetFileCount() == 32, "growth retry did not survive reopen");
        failCacheMalloc = true;
        cache->AddFile({33, 0}, data.data(), 4); // Compression allocation falls back to raw data.
        std::vector<uint8> result;
        require(!failCacheMalloc && cache->GetFile({33, 0}, result) && result == std::vector<uint8>(4, 71), "compression allocation failure lost record");
    }
    std::cout << "PASS: malformed table/entry offsets, oversized claims, truncation and compressed trailers rejected; valid V2/V3 and high-ratio compressed records preserved\n";
    std::cout << "PASS: injected cache creation/loading/growth/compression allocation failures preserve existing records and allow retry\n";
}
'''

with tempfile.TemporaryDirectory(prefix='cemu-cache-malformed-') as tmp:
    path = Path(tmp)
    (path / 'Common').mkdir()
    (path / 'util/helpers').mkdir(parents=True)
    (path / 'Common/precompiled.h').write_text(precompiled)
    (path / 'util/helpers/helpers.h').write_text('#include "Common/precompiled.h"\n')
    (path / 'test.cpp').write_text(fixture)
    # Include the unchanged production source with only its C allocator routed
    # through fault injectors, so table allocation/reallocation failures are real.
    wrapper = r'''
#include "Common/precompiled.h"
#include <array>
#include <limits>
#include <new>
#include <zlib.h>
bool failCacheMalloc = false, failCacheRealloc = false;
void* cacheTestMalloc(size_t size) {
    if (failCacheMalloc) { failCacheMalloc = false; return nullptr; }
    return std::malloc(size);
}
void* cacheTestRealloc(void* value, size_t size) {
    if (failCacheRealloc) { failCacheRealloc = false; return nullptr; }
    return std::realloc(value, size);
}
#define malloc cacheTestMalloc
#define realloc cacheTestRealloc
#include "Cemu/FileCache/FileCache.cpp"
'''
    (path / 'cache-with-allocation-failures.cpp').write_text(wrapper)
    subprocess.run([
        'xcrun', 'clang++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-pthread', '-include', str(path / 'Common/precompiled.h'),
        '-I' + str(path), '-I' + str(repo / 'src'),
        str(repo / 'src/Common/unix/FileStream_unix.cpp'), str(path / 'cache-with-allocation-failures.cpp'),
        str(path / 'test.cpp'), '-lz', '-o', str(path / 'test')
    ], check=True)
    subprocess.run([str(path / 'test'), str(path)], check=True)
