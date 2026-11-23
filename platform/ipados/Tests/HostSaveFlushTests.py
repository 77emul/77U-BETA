#!/usr/bin/env python3
"""Verify host flush honors read-only handles and preserves writable errors."""
from pathlib import Path
import subprocess, tempfile
repo=Path(__file__).resolve().parents[3]
host=(repo/'src/Cafe/Filesystem/fscDeviceHostFS.cpp').read_text()
a=host.index('bool FSCVirtualFile_Host::fscFlush()')
o=host.index('{',a); b=o+1; depth=1
while depth:
 depth+=(host[b]=='{')-(host[b]=='}'); b+=1
production=host[a:b]
fixture=r'''
#include <iostream>
#include <stdexcept>
constexpr int FSC_TYPE_FILE=1, FSC_TYPE_DIRECTORY=2;
struct Stream { bool success=false; int calls=0; bool Flush(){++calls;return success;} };
struct FSCVirtualFile_Host { int m_type=FSC_TYPE_FILE; Stream* m_fs=nullptr; bool m_isWritable=false; bool fscFlush(); };
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
'''
tests=r'''
int main(){
 Stream stream; FSCVirtualFile_Host file{FSC_TYPE_FILE,&stream,false};
 require(file.fscFlush(),"read-only host flush must succeed without a write-capable backend handle");
 require(stream.calls==0,"read-only flush reached FlushFileBuffers or a buffered input stream");
 file.m_isWritable=true;
 require(!file.fscFlush() && stream.calls==1,"writable flush failure was not propagated");
 stream.success=true;
 require(file.fscFlush() && stream.calls==2,"writable file was not flushed");
 file.m_type=FSC_TYPE_DIRECTORY;
 require(!file.fscFlush() && stream.calls==2,"directory flush used a file handle");
 file.m_type=FSC_TYPE_FILE; file.m_fs=nullptr;
 require(!file.fscFlush(),"missing host stream reported flush success");
 std::cout<<"PASS: read-only host flush avoids write-only API; writable errors preserved\n";
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-host-flush-') as tmp:
 p=Path(tmp); (p/'test.cpp').write_text(fixture+production+tests)
 subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
