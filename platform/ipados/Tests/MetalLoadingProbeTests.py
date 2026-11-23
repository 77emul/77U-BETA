#!/usr/bin/env python3
"""Check that optional loading probes do no clock/counter work when disabled."""
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[3]
fixture=r'''
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include <cassert>
struct Clock {
 using duration=std::chrono::nanoseconds;
 using time_point=std::chrono::time_point<Clock,duration>;
 static inline uint64_t calls=0;
 static time_point now() { return time_point(duration(++calls*1000)); }
};
int main() {
 std::atomic<uint64_t> total{7};
 for (int i=0;i<100000;++i) {
  RuntimePerformance::OptionalOperationTimer<Clock> timer(false,total);
 }
 assert(Clock::calls==0 && total==7);
 { RuntimePerformance::OptionalOperationTimer<Clock> timer(true,total); }
 assert(Clock::calls==2 && total==1007);
 // Existing data is accumulated, never reset by a resource operation.
 { RuntimePerformance::OptionalOperationTimer<Clock> timer(true,total); }
 assert(Clock::calls==4 && total==2007);
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-loading-probe-') as temp:
 p=Path(temp);(p/'test.cpp').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-I'+str(repo/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: 100,000 disabled probes make zero clock reads and counter updates; enabled probes accumulate')
