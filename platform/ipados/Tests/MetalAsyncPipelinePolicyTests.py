#!/usr/bin/env python3
"""Exercise production draw eligibility, including changes while reusing a PSO."""
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[3]
s=(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp').read_text()
name='bool MetalPipelineCache::CanCompileDrawAsync('
if name not in s:
    name='bool IsAsyncPipelineAllowed('
a=s.index(name); opening=s.index('{',a); end=opening+1; depth=1
while depth:
    depth += (s[end]=='{') - (s[end]=='}'); end+=1
# Preserve the production body; adapt only the signature for the old baseline.
body=s[opening:end]
fixture=r"""
#include <cstdio>
#include <cstdint>
using uint32=uint32_t;
namespace Latte { enum class E_GX2SURFFMT { INVALID_FORMAT, D32, RGBA8 }; }
struct MetalAttachmentsInfo {
    Latte::E_GX2SURFFMT depthFormat=Latte::E_GX2SURFFMT::INVALID_FORMAT;
    Latte::E_GX2SURFFMT colorFormats[8]{};
};
struct Vector2i { int x,y; };
constexpr int mmVGT_STRMOUT_EN=0;
struct LatteContextRegister {
    bool rasterization=true;
    uint32 registers[1]{};
    bool IsRasterizationEnabled() const { return rasterization; }
    const uint32* GetRawView() const { return registers; }
};
struct Config { bool async_compile=true; } config;
const Config& GetConfig() { return config; }
bool policy(const MetalAttachmentsInfo& attachmentsInfo, Vector2i extend,
            const LatteContextRegister& lcr, bool occlusionQueryActive, bool readbackAttachment=false) {
    constexpr uint32 indexCount=100;
""" + body[1:-1] + r"""
}
int main() {
    MetalAttachmentsInfo scene; scene.depthFormat=Latte::E_GX2SURFFMT::D32; scene.colorFormats[0]=Latte::E_GX2SURFFMT::RGBA8;
    LatteContextRegister registers;
    if (!policy(scene,{1600,900},registers,false)) return 1;
    if (policy(scene,{1600,1600},registers,false)) return 2;
    if (policy(scene,{1600,900},registers,true)) {
        puts("FAIL: occlusion query draws must produce a real result"); return 3;
    }
    registers.registers[0]=1;
    if (policy(scene,{1600,900},registers,false)) return 4;
    registers.registers[0]=0;
    registers.rasterization=false;
    if (policy(scene,{1600,900},registers,false)) return 5;
    registers.rasterization=true;
    config.async_compile=false;
    if (policy(scene,{1600,900},registers,false)) return 6;
    config.async_compile=true;
    MetalAttachmentsInfo persistent;
    if (policy(persistent,{256,256},registers,false)) return 7;
    if (!policy(scene,{2560,1440},registers,false)) return 8;
    if (policy(scene,{1600,900},registers,false,true)) return 9;
    MetalAttachmentsInfo shadow; shadow.depthFormat=Latte::E_GX2SURFFMT::D32;
    if (policy(shadow,{1024,1024},registers,false)) return 10;
    MetalAttachmentsInfo postProcess; postProcess.colorFormats[0]=Latte::E_GX2SURFFMT::RGBA8;
    if (!policy(postProcess,{1600,900},registers,false)) return 11;
    if (policy(postProcess,{256,256},registers,false)) return 12;
    MetalAttachmentsInfo shadowRect; shadowRect.depthFormat=Latte::E_GX2SURFFMT::D32;
    if (!policy(shadowRect,{768,720},registers,false)) return 13;
    puts("PASS: scene geometry defers; protected queries, streamout, ink, color-only and depth-only targets wait");
}
"""
with tempfile.TemporaryDirectory(prefix='cemu-async-policy-') as tmp:
    p=Path(tmp); (p/'test.cpp').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O3',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
