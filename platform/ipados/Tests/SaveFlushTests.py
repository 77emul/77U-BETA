#!/usr/bin/env python3
"""Run actual FSA save-flush handlers with real buffered host files."""
from pathlib import Path
import subprocess, tempfile
repo=Path(__file__).resolve().parents[3]
fsa=(repo/'src/Cafe/IOSU/fsa/iosu_fsa.cpp').read_text()
def definition(text, signature):
 a=text.index(signature); o=text.index('{',a); b=o+1; depth=1
 while depth:
  depth+=(text[b]=='{')-(text[b]=='}'); b+=1
 return text[a:b]
production=definition(fsa,'class _FSAHandleTable')+';\n_FSAHandleTable sFileHandleTable;\n'
production+='\n'.join(definition(fsa,s) for s in ['FSA_RESULT FSAProcessCmd_flushFile(', 'FSA_RESULT FSAProcessCmd_flushQuota(', 'FSA_RESULT FSAProcessCmd_truncateFile('])
fixture=r'''
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <stdexcept>
#include <iostream>
using uint16=uint16_t; using uint32=uint32_t; using sint32=int32_t; using FSResHandle=uint32_t; using FSFileHandle2=uint32_t;
#define cemuLog_log(...) ((void)0)
#define cemuLog_logDebug(...) ((void)0)
namespace fs=std::filesystem;
enum class FSA_RESULT { OK, INVALID_FILE_HANDLE, FATAL_ERROR };
struct FSAClient {};
struct FSAShimBuffer { struct { struct { uint32 fileHandle; } cmdFlushFile, cmdTruncateFile; } request; };
constexpr uint32 FSC_QUERY_WRITEABLE=1;
struct FSCVirtualFile {
 std::FILE* file; bool writable; bool fail=false; int flushes=0;
 uint64_t fscQueryValueU64(uint32) const { return writable; }
 bool fscFlush() { ++flushes; return !fail && std::fflush(file)==0; }
};
bool fsc_isWritable(FSCVirtualFile* file) { return file->writable; }
uint32 fsc_getFileSeek(FSCVirtualFile* file) { return std::ftell(file->file); }
bool fsc_setFileLength(FSCVirtualFile* file,uint32) { return !file->fail; }
'''
tests=r'''
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
std::string read(const fs::path& path) { std::ifstream in(path); return {std::istreambuf_iterator<char>(in),{}}; }
int main(int argc,char** argv) {
 fs::path root=argv[1]; std::string mode=argv[2];
 auto aPath=root/"a.dat", bPath=root/"b.dat";
 FSCVirtualFile a{std::fopen(aPath.c_str(),"w+b"),true},b{std::fopen(bPath.c_str(),"w+b"),true};
 char aBuffer[4096],bBuffer[4096]; std::setvbuf(a.file,aBuffer,_IOFBF,sizeof(aBuffer)); std::setvbuf(b.file,bBuffer,_IOFBF,sizeof(bBuffer));
 std::fwrite("progress",1,8,a.file); std::fwrite("second",1,6,b.file);
 FSResHandle aHandle,bHandle; require(sFileHandleTable.AllocateHandle(aHandle,&a)==FSA_RESULT::OK,"handle allocation failed");
 require(sFileHandleTable.AllocateHandle(bHandle,&b)==FSA_RESULT::OK,"second handle failed");
 FSAShimBuffer request{}; request.request.cmdFlushFile.fileHandle=aHandle;
 if(mode=="truncate-error") {
  request.request.cmdTruncateFile.fileHandle=aHandle; a.fail=true;
  require(FSAProcessCmd_truncateFile(nullptr,&request)==FSA_RESULT::FATAL_ERROR,"failed save truncation acknowledged by guest FSA");
 } else if(mode=="file") {
  require(FSAProcessCmd_flushFile(nullptr,&request)==FSA_RESULT::OK,"save flush failed");
  require(read(aPath)=="progress","guest flush acknowledged but buffered save was not published");
  require(std::ftell(a.file)==8,"flush moved save cursor");
 } else if(mode=="invalid") {
  request.request.cmdFlushFile.fileHandle=0xffffffff;
  require(FSAProcessCmd_flushFile(nullptr,&request)==FSA_RESULT::INVALID_FILE_HANDLE,"invalid save handle reported success");
 } else if(mode=="error") {
  a.fail=true; require(FSAProcessCmd_flushFile(nullptr,&request)==FSA_RESULT::FATAL_ERROR,"save flush failure reported success");
 } else if(mode=="quota") {
  require(FSAProcessCmd_flushQuota(nullptr,&request)==FSA_RESULT::OK,"quota flush failed");
  require(read(aPath)=="progress" && read(bPath)=="second","quota flush left open save buffers unpublished");
 } else if(mode=="quota-error") {
  a.fail=true; require(FSAProcessCmd_flushQuota(nullptr,&request)==FSA_RESULT::FATAL_ERROR,"quota flush failure reported success");
  require(read(bPath)=="second","quota error prevented remaining save buffers flushing");
 } else if(mode=="read-only") {
  b.writable=false; b.fail=true;
  require(FSAProcessCmd_flushQuota(nullptr,&request)==FSA_RESULT::OK,"read-only content caused quota failure");
  require(b.flushes==0 && read(aPath)=="progress","quota touched read-only game content or skipped writable save");
 }
 std::fclose(a.file); std::fclose(b.file); std::cout<<"PASS: "<<mode<<'\n';
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-save-flush-') as tmp:
 p=Path(tmp); (p/'test.cpp').write_text(fixture+production+tests)
 subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 failures=[]
 for mode in ['truncate-error','file','invalid','error','quota','quota-error','read-only']:
  result=subprocess.run([str(p/'test'),str(p),mode],capture_output=True,text=True)
  print(result.stdout+result.stderr,end='')
  if result.returncode: failures.append(mode)
 if failures: raise SystemExit('FAIL: '+', '.join(failures))
