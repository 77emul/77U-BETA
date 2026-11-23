#!/usr/bin/env python3
"""Exercise the production archive extraction function's bounds and cleanup contracts."""
from pathlib import Path
import subprocess
import tempfile
repo = Path(__file__).resolve().parents[3]
source = (repo / 'platform/ipados/Bridge/CemuBridge.mm').read_text()
function = source[source.index('extern "C" bool CemuBridge_ExtractGameBootMedia('):source.index('extern "C" CemuSaveOperationResult CemuBridge_ExportSave(')]
pch = (repo / 'src/Common/precompiled.h').read_text()
scope = pch[pch.index('\ttemplate <typename Fn>\n\tclass scope_exit'):pch.index('\t// Xcode 16 doesn\'t have std::atomic_ref')]
harness = r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace fs = std::filesystem;
using sint32 = int32_t; using uint32 = uint32_t; using uint8 = uint8_t;
struct State { bool mount = true, open = true, create = true, partial = false, throws = false, fileOpen = false; uint32 size = 18; int reads = 0, closes = 0, unmounts = 0; std::vector<uint8> output; } state;
struct TitleInfo {
 explicit TitleInfo(fs::path) {} bool IsValid() { return true; }
 static std::string GetUniqueTempMountingPath() { return "/fixture/"; }
 bool Mount(const std::string&, const char*, int) { return state.mount; }
 void Unmount(const std::string&) { assert(!state.fileOpen); ++state.unmounts; }
};
constexpr int FSC_PRIORITY_BASE = 0, FSC_STATUS_UNDEFINED = 0;
enum class FSC_ACCESS_FLAG { OPEN_FILE = 1, READ_PERMISSION = 2 };
FSC_ACCESS_FLAG operator|(FSC_ACCESS_FLAG a, FSC_ACCESS_FLAG b) { return FSC_ACCESS_FLAG(int(a)|int(b)); }
struct FSCVirtualFile {} file;
namespace fmt { std::string format(const char*, const std::string& path, const char* name) { return path + "meta/" + name; } }
FSCVirtualFile* fsc_open(const char* path, FSC_ACCESS_FLAG, sint32*) { assert(std::string(path) == "/fixture/meta/bootTvTex.tga" || std::string(path) == "/fixture/meta/bootSound.btsnd"); if (!state.open) return nullptr; state.fileOpen = true; return &file; }
void fsc_close(FSCVirtualFile*) { state.fileOpen = false; ++state.closes; }
uint32 fsc_getFileSize(FSCVirtualFile*) { return state.size; }
uint32 fsc_readFile(FSCVirtualFile*, void* output, uint32 size) { ++state.reads; if (state.throws) throw std::runtime_error("read"); std::memset(output, 7, size); return state.partial ? size - 1 : size; }
struct FileStream {
 static FileStream* createFile2(fs::path) { return state.create ? new FileStream : nullptr; }
 sint32 writeData(const void* data, sint32 size) { const auto* bytes = static_cast<const uint8*>(data); state.output.assign(bytes, bytes + size); return size; }
};
'''
tests = r'''
int main() {
 assert(!CemuBridge_ExtractGameBootMedia(nullptr, "out", false));
 assert(!CemuBridge_ExtractGameBootMedia("game", "", false));
 state = {}; assert(CemuBridge_ExtractGameBootMedia("game", "out", false)); assert(state.output == std::vector<uint8>(18, 7)); assert(state.closes == 1 && state.unmounts == 1);
 state = {}; state.size = 16 * 1024 * 1024 + 1; assert(!CemuBridge_ExtractGameBootMedia("game", "out", false)); assert(state.reads == 0 && state.closes == 1 && state.unmounts == 1);
 state = {}; state.size = 12; assert(CemuBridge_ExtractGameBootMedia("game", "out", true)); assert(state.output.size() == 12);
 state = {}; state.size = 8 * 1024 * 1024 + 1; assert(!CemuBridge_ExtractGameBootMedia("game", "out", true)); assert(state.reads == 0 && state.unmounts == 1);
 state = {}; state.partial = true; assert(!CemuBridge_ExtractGameBootMedia("game", "out", false)); assert(state.output.empty() && state.closes == 1 && state.unmounts == 1);
 state = {}; state.create = false; assert(!CemuBridge_ExtractGameBootMedia("game", "out", false)); assert(state.closes == 1 && state.unmounts == 1);
 state = {}; state.open = false; assert(!CemuBridge_ExtractGameBootMedia("game", "out", false)); assert(state.closes == 0 && state.unmounts == 1);
 state = {}; state.mount = false; assert(!CemuBridge_ExtractGameBootMedia("game", "out", false)); assert(state.unmounts == 0);
 state = {}; state.throws = true; assert(!CemuBridge_ExtractGameBootMedia("game", "out", false)); assert(state.closes == 1 && state.unmounts == 1);
}
'''
with tempfile.TemporaryDirectory(prefix='boot-artwork-') as temp:
    path = Path(temp)
    (path / 'test.cpp').write_text(harness + '\nnamespace stdx {\n' + scope + '\n}\n' + function + tests)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', str(path/'test.cpp'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('PASS: production boot-art extraction, exact bytes, size cap, partial reads, failed writes, exceptions, close-before-unmount')
