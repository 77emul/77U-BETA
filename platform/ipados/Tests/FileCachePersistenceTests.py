#!/usr/bin/env python3
"""Exercise the real compressed shader cache and its asynchronous close lifecycle."""
from pathlib import Path
import subprocess, tempfile, runpy
repo=Path(__file__).resolve().parents[3]
# Shared test precompiled header only supplies platform types, not stream/cache logic.
stream_test=(repo/'platform/ipados/Tests/FileStreamSaveIntegrityTests.py').read_text()
a=stream_test.index('precompiled = r"""')+len('precompiled = r"""'); b=stream_test.index('"""',a)
precompiled=stream_test[a:b]+r'''
#include <mutex>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <condition_variable>
#include <cstdlib>
using sint64=int64_t;
#define cemu_assert_debug(x) assert(x)
#define cemuLog_log(...) ((void)0)
#define debug_printf(...) ((void)0)
inline std::string _pathToUtf8(const fs::path& path) { return path.string(); }
inline void SetThreadName(const char*) {}
inline uint32 GetTickCount() { return 0; }
'''
fixture=r'''
#include "Common/precompiled.h"
#include "Cemu/FileCache/FileCache.h"
#include <iostream>
#include <stdexcept>
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
int main(int argc,char** argv) {
 auto path=fs::path(argv[1])/"shader.bin";
 std::vector<uint8> value(16384,71);
 for(uint64 i=1;i<=256;++i) {
  std::unique_ptr<FileCache> cache(FileCache::Open(path,true,123));
  require(bool(cache),"cache open failed");
  cache->AddFileAsync({i,0},value.data(),value.size());
  // Closing must drain the producer's acknowledged cache write before freeing
  // its stream/table; a timing sleep would conceal the lifetime bug.
  cache.reset();
 }
 std::unique_ptr<FileCache> cache(FileCache::Open(path,false,123));
 require(bool(cache),"cache could not reopen");
 for(uint64 i=1;i<=256;++i) {
  std::vector<uint8> got;
  require(cache->GetFile({i,0},got) && got==value,"acknowledged shader record lost on cache close");
 }
 require(cache->GetFileCount()==256,"shader record count wrong");
 std::cout<<"PASS: 256 async compressed records survive close/reopen without lifetime errors\n";
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-cache-persistence-') as tmp:
 p=Path(tmp); (p/'Common').mkdir(); (p/'util/helpers').mkdir(parents=True)
 (p/'Common/precompiled.h').write_text(precompiled)
 (p/'util/helpers/helpers.h').write_text('#include "Common/precompiled.h"\n')
 (p/'test.cpp').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-include',str(p/'Common/precompiled.h'),'-I'+str(p),'-I'+str(repo/'src'),str(repo/'src/Common/unix/FileStream_unix.cpp'),str(repo/'src/Cemu/FileCache/FileCache.cpp'),str(p/'test.cpp'),'-lz','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p)],check=True)
