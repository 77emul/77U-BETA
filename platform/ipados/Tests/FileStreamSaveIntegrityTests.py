#!/usr/bin/env python3
"""Exercise the real Unix save/cache stream against temporary files."""
from pathlib import Path
import subprocess, tempfile
repo = Path(__file__).resolve().parents[3]
precompiled = r"""
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace fs = std::filesystem;
using uint8=uint8_t; using uint16=uint16_t; using uint32=uint32_t; using uint64=uint64_t; using sint32=int32_t;
#define cemu_assert(x) assert(x)
#define assert_dbg() ((void)0)
"""
fixture = r"""
#include "Common/unix/FileStream_unix.h"
#include <iostream>
#include <stdexcept>
void require(bool ok, const char* why) { if(!ok) throw std::runtime_error(why); }
int main(int argc, char** argv) {
 fs::path root=argv[1]; std::string mode=argv[2]; auto path=root/"save.dat";
 { std::ofstream out(path, std::ios::binary); out << "abcdefgh"; }
 std::unique_ptr<FileStream> stream(FileStream::openFile2(path,true));
 require(bool(stream), "test save cannot open");
 if(mode=="seek-after-eof") {
  char data[16]{}; require(stream->readData(data,16)==8,"short read incorrect");
  stream->SetPosition(2);
  require(stream->writeData("XY",2)==2,"seek-after-EOF save write failed");
  stream->Flush(); stream.reset();
  require(*FileStream::LoadIntoMemory(path)==std::vector<uint8>({'a','b','X','Y','e','f','g','h'}),"save update silently lost after EOF and seek");
 } else if(mode=="size-after-eof") {
  char data[16]{}; stream->readData(data,16);
  require(stream->GetSize()==8,"EOF converted save size into UINT64_MAX");
  stream->SetPosition(0); require(stream->readData(data,2)==2 && data[0]=='a',"size query prevented subsequent save read");
 } else if(mode=="truncate") {
  stream->SetPosition(3); require(stream->SetEndOfFile(),"save truncation failed");
  require(fs::file_size(path)==3 && stream->GetSize()==3,"truncation returned success without shrinking save");
  stream->SetPosition(7); require(stream->SetEndOfFile(),"save extension failed");
  stream.reset();
  require(*FileStream::LoadIntoMemory(path)==std::vector<uint8>({'a','b','c',0,0,0,0}),"extension did not preserve prefix and zero-fill gap");
 } else if(mode=="rename-open") {
  auto renamed=root/"renamed.dat"; fs::rename(path,renamed);
  { std::ofstream replacement(path); replacement << "replacement"; }
  stream->SetPosition(3); require(stream->SetEndOfFile(),"renamed open save cannot truncate");
  require(fs::file_size(renamed)==3 && fs::file_size(path)==11,"truncation changed a replacement file instead of the open save");
 } else if(mode=="read-only") {
  stream.reset(FileStream::openFile2(path,false));
  require(stream->writeData("BAD",3)==0,"read-only save reported a successful write");
  require(!stream->SetEndOfFile(),"read-only save reported successful truncation");
  require(fs::file_size(path)==8,"read-only save changed");
 } else if(mode=="buffered-save") {
  require(stream->writeData("NEW",3)==3,"save write failed");
  stream->Flush();
  require(*FileStream::LoadIntoMemory(path)==std::vector<uint8>({'N','E','W','d','e','f','g','h'}),"flush did not publish save before handle close");
 } else if(mode=="alternating") {
  stream->SetPosition(2); require(stream->writeData("XY",2)==2,"initial save write failed");
  require(stream->GetSize()==8,"write changed size");
  char data[8]{}; require(stream->readData(data,2)==2 && data[0]=='e' && data[1]=='f',"size query moved save cursor");
  require(stream->writeData("Z",1)==1,"read/write switch failed");
  stream->Flush(); stream.reset();
  require(*FileStream::LoadIntoMemory(path)==std::vector<uint8>({'a','b','X','Y','e','f','Z','h'}),"mixed save read/write corrupted content");
 }
 std::cout << "PASS: " << mode << '\n';
}
"""
with tempfile.TemporaryDirectory(prefix='cemu-save-integrity-') as tmp:
 p=Path(tmp); (p/'Common').mkdir(); (p/'Common/precompiled.h').write_text(precompiled)
 (p/'test.cpp').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG','-I'+str(p),'-I'+str(repo/'src'),str(repo/'src/Common/unix/FileStream_unix.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 failures=[]
 for mode in ['seek-after-eof','size-after-eof','truncate','rename-open','read-only','buffered-save','alternating']:
  result=subprocess.run([str(p/'test'),str(p),mode],capture_output=True,text=True)
  print(result.stdout+result.stderr,end='')
  if result.returncode: failures.append(mode)
 if failures: raise SystemExit('FAIL: '+', '.join(failures))
