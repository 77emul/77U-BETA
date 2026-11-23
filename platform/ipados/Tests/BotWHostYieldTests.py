#!/usr/bin/env python3
"""Run the production guest-yield and scheduler boundary with host-yield instrumentation."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

yield_body = function((repo/'src/Cafe/OS/libs/coreinit/coreinit_Thread.cpp').read_text(), 'void OSYieldThread()')
switch_body = function((repo/'src/Cafe/HW/Espresso/PPCScheduler.cpp').read_text(), 'void PPCCore_switchToScheduler()')
fixture = r'''
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>
using uint64=uint64_t;
void require(bool b) {if(!b) std::abort();}
#define cemu_assert_debug(x) require(x)
bool locked=false;unsigned switches=0,hostYields=0,phoneQueries=0,currentGuest=0;
std::vector<int> events;
bool __OSHasSchedulerLock(){return locked;}
void __OSLockScheduler(){require(!locked);locked=true;events.push_back(1);}
void __OSUnlockScheduler(){require(locked);locked=false;events.push_back(3);}
struct CPU {unsigned coreInterruptMask=1;}cpu;
CPU* PPCInterpreter_getCurrentInstance(){return &cpu;}
namespace WindowSystem {bool phone;bool IsPhone(){++phoneQueries;return phone;}}
namespace CafeSystem {uint64 title;uint64 GetForegroundTitleId(){return title;}}
namespace std::this_thread {void yield(){require(!locked);++hostYields;events.push_back(4);}}
namespace coreinit {
void __OSThreadSwitchToNext(){require(locked);++switches;currentGuest^=1;events.push_back(2);}
}
'''
fixture += switch_body + '\nnamespace coreinit {\n' + yield_body + '\n}\n'
fixture += r'''
int main(int argc,char** argv){
 WindowSystem::phone=argc>1&&argv[1][0]=='1';
 for(uint64 title:{0x00050000101c9300ULL,0x00050000101c9400ULL,0x00050000101c9500ULL,
                   0x0005000010176a00ULL,0x0005000010145c00ULL}){
  CafeSystem::title=title;
  bool botw=title==0x00050000101c9300ULL||title==0x00050000101c9400ULL||title==0x00050000101c9500ULL;
  bool hostYield=TARGET_OS_IPHONE && !(WindowSystem::phone&&botw);
  for(int i=0;i<1000;++i){
   events.clear();unsigned beforeSwitch=switches,beforeHost=hostYields,guest=currentGuest;
   coreinit::OSYieldThread();
   require(!locked&&switches==beforeSwitch+1&&currentGuest!=guest);
   require(hostYields==beforeHost+(hostYield?1:0));
   require(events==(hostYield?std::vector<int>{1,2,3,4}:std::vector<int>{1,2,3}));
  }
 }
 require(phoneQueries==(TARGET_OS_IPHONE?1:0));
 printf("PASS: iOS=%d phone=%d, 5000 guest scheduler switches; correct lock order, one hardware query, title-specific host yielding\n",TARGET_OS_IPHONE,WindowSystem::phone);
}
'''
with tempfile.TemporaryDirectory(prefix='botw-host-yield-') as tmp:
    p=Path(tmp);(p/'test.cpp').write_text(fixture)
    for ios in [0,1]:
        binary=p/f'test-{ios}'
        subprocess.run(['xcrun','clang++','-std=c++20',f'-DTARGET_OS_IPHONE={ios}',str(p/'test.cpp'),'-o',str(binary)],check=True)
        for phone in [0,1]:subprocess.run([str(binary),str(phone)],check=True)
