"""Check the menu and the running game's rate against an isolated defaults suite."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
bridge = (root / "platform/ipados/Bridge/CemuBridge.mm").read_text()
start = bridge.index('extern "C" int CemuBridge_GetRunningGameFrameGenerationTarget(')
end = bridge.index('extern "C" bool CemuBridge_GetRunningGameFrameGenerationFast(', start)
functions = bridge[start:end].replace("NSUserDefaults.standardUserDefaults", "defaultsSuite")
fixture = r'''
#import <Foundation/Foundation.h>
#include <cassert>
#include <initializer_list>
static NSUserDefaults *defaultsSuite;
enum class CPUMode { Interpreter, SinglecoreRecompiler, DualcoreRecompiler, MulticoreRecompiler };
namespace ActiveSettings {
    CPUMode mode = CPUMode::MulticoreRecompiler;
    CPUMode GetCPUMode() { return mode; }
}
namespace CafeSystem { uint64_t GetForegroundTitleId() { return 1; } }
struct Target { int value = 120; int GetValue() const { return value; } };
struct Config { Target frame_generation_target; };
static Config& GetConfig() { static Config config; return config; }
extern "C" int CemuBridge_GetFrameGenerationTargetForTitle(uint64_t);
'''
cases = r'''
int main() { @autoreleasepool {
    NSString *suite = NSUUID.UUID.UUIDString;
    defaultsSuite = [[NSUserDefaults alloc] initWithSuiteName:suite];
    NSString *key = @"CemuIOS.FrameGenerationTarget.1";
    assert(CemuBridge_GetRunningGameFrameGenerationTarget() == 120);
    for (int target : {30, 60, 120, 0}) {
        [defaultsSuite setInteger:target forKey:key];
        assert(CemuBridge_GetRunningGameFrameGenerationTarget() == target);
        assert(CemuBridge_GetFrameGenerationTargetForTitle(1) == target);
        assert(CemuBridge_GetFrameGenerationTargetForTitle(2) == 120);
    }
    [defaultsSuite setInteger:42 forKey:key];
    assert(CemuBridge_GetRunningGameFrameGenerationTarget() == 120);
    [defaultsSuite removeObjectForKey:key];
    ActiveSettings::mode = CPUMode::Interpreter;
    assert(CemuBridge_GetRunningGameFrameGenerationTarget() == 0);
    assert(CemuBridge_GetFrameGenerationTargetForTitle(1) == 120);
    [defaultsSuite removePersistentDomainForName:suite];
}}
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    source = path / "preferences.mm"
    source.write_text(fixture + functions + cases)
    subprocess.run(["xcrun", "clang++", "-std=c++20", "-fobjc-arc", str(source),
                    "-framework", "Foundation", "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: per-game Off, overrides, inheritance, invalid values and No JIT")
