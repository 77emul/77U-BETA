#!/usr/bin/env python3
"""Verify host save metadata/cursor and guest errors when truncation fails."""
from pathlib import Path
import subprocess,tempfile
repo=Path(__file__).resolve().parents[3]
def definition(text, signature):
 a=text.index(signature); o=text.index('{',a); b=o+1; depth=1
 while depth:
  depth+=(text[b]=='{')-(text[b]=='}'); b+=1
 return text[a:b]
host=(repo/'src/Cafe/Filesystem/fscDeviceHostFS.cpp').read_text()
fsc=(repo/'src/Cafe/Filesystem/fsc.cpp').read_text()
fsa=(repo/'src/Cafe/IOSU/fsa/iosu_fsa.cpp').read_text()
production=definition(host,'FSCVirtualFile_Host::fscSetFileLength(')
# Include the return type independently of whether the pre-fix API is void.
start=host.index('FSCVirtualFile_Host::fscSetFileLength('); line=host.rfind('\n',0,start)+1
production=host[line:start]+production
fixture=r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>
using uint64=uint64_t; using uint32=uint32_t;
#define cemuLog_log(...) ((void)0)
constexpr uint32 FSC_TYPE_FILE=1;
struct Stream { bool success=true; uint64 position=0; void SetPosition(uint64 v){position=v;} bool SetEndOfFile(){return success;} };
struct FSCVirtualFile_Host { uint32 m_type=FSC_TYPE_FILE; Stream* m_fs; uint64 m_seek=4,m_fileSize=18; RETURN_TYPE fscSetFileLength(uint64 endOffset); };
'''.replace('RETURN_TYPE',host[line:start].strip())
tests=r'''
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main() {
 Stream stream; FSCVirtualFile_Host file{FSC_TYPE_FILE,&stream};
 file.fscSetFileLength(12);
 require(file.m_fileSize==12 && file.m_seek==4 && stream.position==4,"truncation advertised cursor as size instead of requested length");
 stream.success=false; file.fscSetFileLength(2);
 require(file.m_fileSize==12 && file.m_seek==4 && stream.position==4,"failed save truncation changed guest size/cursor");
 std::cout<<"PASS: host truncation preserves accurate size/cursor on success and failure\n";
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-host-truncate-') as tmp:
 p=Path(tmp); (p/'test.cpp').write_text(fixture+production+tests)
 subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
