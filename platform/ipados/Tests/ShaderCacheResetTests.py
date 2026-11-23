#!/usr/bin/env python3
"""Run the actual cache-reset bridge against temporary files and launch state."""
from pathlib import Path
import subprocess
import tempfile
repo = Path(__file__).resolve().parents[3]
source = (repo / 'platform/ipados/Bridge/CemuBridge.mm').read_text()
def definition(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
production = definition('class SaveOperationScope') + ';\n' + '\n'.join(definition(signature) for signature in [
    'extern "C" bool CemuBridge_CanManageSaves(void)',
    'bool CemuBridge_ClearShaderCacheForTitle(uint64_t titleId)',
    'bool CemuBridge_ClearAllShaderCaches(void)',
])
fixture = r'''
#include <filesystem>
#include <fstream>
#include <atomic>
#include <vector>
#include <cstdio>
#include <string>
#include <cstdlib>
namespace fs = std::filesystem;
#define os_log(...) ((void)0)
#define os_log_error(...) ((void)0)
std::atomic<bool> s_titleEverLaunched{false};
std::atomic<int> s_titleSaveLifecycleState{0};
struct ActiveSettings {
    static inline fs::path root;
    static fs::path GetCachePath(const std::string& name) { return root / name; }
    static fs::path GetCachePath(std::string name, uint64_t title) {
        char id[17]; std::snprintf(id, sizeof(id), "%016llx", (unsigned long long)title);
        name.replace(name.find("{:016x}"), 7, id);
        return root / name;
    }
};
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
void seed(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "test contents";
}
''' + production + r'''
int main(int argc, char** argv) {
    require(argc == 2, "temporary test directory required");
    fs::path temporary = argv[1];
    ActiveSettings::root = temporary / "cache";
    auto shader = ActiveSettings::GetCachePath("shaderCache/transferable/0005000010176a00_mtlshaders.bin");
    auto pipeline = ActiveSettings::GetCachePath("shaderCache/transferable/0005000010176a00_mtlpipeline.bin");
    auto update = ActiveSettings::GetCachePath("shaderCache/transferable/0005000e10176a00_mtlpipeline.bin");
    auto other = ActiveSettings::GetCachePath("shaderCache/transferable/0005000010145d00_mtlshaders.bin");
    auto save = temporary / "mlc01/usr/save/progress.dat";
    auto config = temporary / "config/settings.xml";
    for (auto& path : {shader, pipeline, update, other, save, config}) seed(path);
    s_titleEverLaunched = true;
    require(!CemuBridge_ClearAllShaderCaches() && fs::exists(shader), "Active title cache was deleted and reported success");
    require(!CemuBridge_ClearShaderCacheForTitle(0x0005000010176a00ULL) && fs::exists(shader), "Per-game reset touched an active title");
    s_titleEverLaunched = false;
    s_titleSaveLifecycleState = 2; // launch/loading owns the same lifecycle gate
    require(!CemuBridge_ClearAllShaderCaches() && fs::exists(shader), "Reset raced title loading");
    s_titleSaveLifecycleState = 1; // another library operation
    require(!CemuBridge_ClearShaderCacheForTitle(0x0005000010176a00ULL), "Reset raced a library operation");
    s_titleSaveLifecycleState = 0;
    require(!CemuBridge_ClearShaderCacheForTitle(0), "Zero title accepted");
    require(CemuBridge_ClearShaderCacheForTitle(0x0005000e10176a00ULL), "Update/base title reset failed");
    require(!fs::exists(shader) && !fs::exists(pipeline) && !fs::exists(update), "Real Metal records were not removed");
    require(fs::exists(other) && fs::exists(save) && fs::exists(config), "Reset affected another game or user data");
    seed(shader); seed(pipeline);
    require(CemuBridge_ClearAllShaderCaches(), "Global reset failed");
    require(!fs::exists(shader) && !fs::exists(pipeline) && !fs::exists(other), "Global reset retained game records");
    require(fs::exists(save) && fs::exists(config), "Global reset removed saves/configuration");
    require(s_titleSaveLifecycleState == 0, "Reset leaked the lifecycle gate");
    require(CemuBridge_ClearAllShaderCaches(), "Empty-cache reset was not idempotent");
    // Real ENOTDIR failure: the cache root cannot contain child paths.
    ActiveSettings::root = temporary / "blocked";
    seed(ActiveSettings::root);
    require(!CemuBridge_ClearAllShaderCaches(), "Filesystem failure reported global success");
    require(!CemuBridge_ClearShaderCacheForTitle(0x0005000010176a00ULL), "Filesystem failure reported per-game success");
    require(s_titleSaveLifecycleState == 0, "Error path leaked the lifecycle gate");
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-cache-reset-') as tmp:
    path = Path(tmp)
    (path / 'test.cpp').write_text(fixture)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O3', '-DNDEBUG', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test'), str(path / 'files')], check=True)
print('PASS: real Metal record deletion, base/update IDs, save protection, launch/operation guard and filesystem errors')
