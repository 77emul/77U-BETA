#!/usr/bin/env python3
"""Exercise the real PPC export wrapper and microphone registrations in Release."""
from pathlib import Path
import re, subprocess, tempfile
repo = Path(__file__).resolve().parents[3]
source = (repo / 'src/Cafe/OS/common/OSUtil.h').read_text()
start = source.index('template<auto fn, typename TNames, LogType TLogType>\nvoid cafeExportCallWrapper(')
end = source.index('\nvoid osLib_addFunctionInternal', start)
wrapper = source[start:end]
registrations = re.findall(r'cafeExportRegisterFunc\((MIC\w+), "mic", "[^"]+", LogType::(\w+)\);', (repo / 'src/Cafe/OS/libs/mic/mic.cpp').read_text())
assert len(registrations) == 7
fixture = r'''
#include <tuple>
#include <cstdint>
#include <cstdio>
using MPTR = uint32_t;
enum class LogType { Force, SoundAPI };
struct PPCInterpreter_t { struct { uint32_t LR = 42; } spr; uint32_t instructionPointer=0; int result=0; };
int calls=0, logs=0, formatBuilds=0;
bool soundTrace=false;
bool cemuLog_isLoggingEnabled(LogType type) { return type==LogType::Force || soundTrace; }
bool cemuLog_advancedPPCLoggingEnabled() { return false; }
namespace coreinit { void* OSGetCurrentThread() { return nullptr; } }
MPTR memory_getVirtualOffsetFromPointer(void*) { return 0; }
PPCInterpreter_t* PPCInterpreter_getCurrentInstance() { return nullptr; }
template<class F> auto cafeExportBuildArgTuple(PPCInterpreter_t*, F) { return std::tuple<>(); }
template<class F> auto cafeExportBuildFormatTuple(PPCInterpreter_t*, F) { ++formatBuilds; return std::tuple<>(); }
template<class... Args> bool cemuLog_log(LogType, Args...) { ++logs; return true; }
namespace cafeExportParamWrapper {
 template<class T> void setReturnResult(PPCInterpreter_t* cpu, T result) { cpu->result=result; }
 template<class T> T getFormatResult(T result) { return result; }
}
int poll() { ++calls; return 123; }
struct Names { static const char* GetLib() { return "mic"; } static const char* GetFunc() { return "poll"; } };
''' + wrapper
fixture += 'int main() { PPCInterpreter_t cpu;\n'
for name, level in registrations:
    fixture += f' calls=logs=formatBuilds=0; soundTrace=false; cafeExportCallWrapper<poll,Names,LogType::{level}>(&cpu);\n'
    fixture += f' if(calls!=1 || cpu.result!=123 || cpu.instructionPointer!=42) return 1;\n'
    fixture += f' if(logs!=0 || formatBuilds!=0) {{ std::fprintf(stderr,"{name}: default API trace is on\\n"); return 2; }}\n'
    fixture += f' calls=logs=formatBuilds=0; soundTrace=true; cafeExportCallWrapper<poll,Names,LogType::{level}>(&cpu);\n'
    fixture += ' if(calls!=1 || logs!=2 || formatBuilds!=1 || cpu.result!=123) return 3;\n'
fixture += 'return 0; }\n'
with tempfile.TemporaryDirectory(prefix='cemu-mic-logging-') as tmp:
    path=Path(tmp); (path/'test.cpp').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O3','-DNDEBUG',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PASS: microphone exports keep results; no default formatting/logging; optional audio tracing works.')
