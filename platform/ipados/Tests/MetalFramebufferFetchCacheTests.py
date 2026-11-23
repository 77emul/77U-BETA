#!/usr/bin/env python3
"""The actual PS key must distinguish fetch programs with different channels."""
from pathlib import Path
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[3]
source=(repo/'src/Cafe/HW/Latte/Core/LatteShader.cpp').read_text()
header=(repo/'src/Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h').read_text()
def function(text, signature):
    start=text.index(signature);opening=text.index('{',start);end=opening+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
helper=function(header,'inline bool MetalFramebufferFetchReplicatesRed(') if 'inline bool MetalFramebufferFetchReplicatesRed(' in header else ''
fixture=r'''
#define ENABLE_METAL 1
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <memory>
using uint8=uint8_t;using uint32=uint32_t;using uint64=uint64_t;
constexpr uint32 mmCB_SHADER_MASK=1,LATTE_NUM_COLOR_TARGET=8;
namespace Latte {
 enum class E_GX2SURFFMT { R8_UNORM,R8_SNORM,R8_G8_UNORM,RGBA };
 namespace REGADDR { constexpr uint32 SX_ALPHA_TEST_CONTROL=2,SQ_TEX_RESOURCE_WORD0_N_PS=8; }
}
struct Registers { std::array<Latte::E_GX2SURFFMT,8> formats{}; };
struct { Registers contextNew; } LatteGPUState;
namespace LatteMRT {
 Latte::E_GX2SURFFMT GetColorBufferFormat(uint32 i,const Registers& r) {return r.formats[i];}
 bool GetActiveDepthBufferMask(const Registers&) {return false;}
}
struct FormatInfo {uint8 dataType=3;};
FormatInfo GetMtlPixelFormatInfo(Latte::E_GX2SURFFMT,bool) {return {};}
enum class RendererAPI { Metal };
struct Renderer {RendererAPI GetType() {return RendererAPI::Metal;}};
auto g_renderer=std::make_unique<Renderer>();
struct LatteDecompilerShader {
 uint8 textureUnitListCount=1;std::array<uint8,32> textureUnitList{},textureRenderTargetIndex{};
};
HELPER
PRODUCTION
int main() {
 LatteDecompilerShader ps;ps.textureRenderTargetIndex.fill(255);ps.textureRenderTargetIndex[0]=0;
 std::array<uint32,256> raw{};raw[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS]=1;
 auto key=[&](Latte::E_GX2SURFFMT format) {LatteGPUState.contextNew.formats[0]=format;return LatteSHRC_CalcPSAuxHash(&ps,raw.data());};
 auto r8=key(Latte::E_GX2SURFFMT::R8_UNORM),rgba=key(Latte::E_GX2SURFFMT::RGBA);
 // Golden unchanged-program key from the actual 32b19230 implementation.
 if(rgba!=0x00060c183060c18bull) {puts("FAIL: unaffected float fetch loses its warm cache key");return 5;}
 if(r8==rgba) {puts("FAIL: replicated R8 fetch reuses the unreplicated RGBA shader key");return 1;}
 if(r8!=key(Latte::E_GX2SURFFMT::R8_SNORM)) {puts("FAIL: identical fetch channel programs don't share a key");return 2;}
 if(rgba!=key(Latte::E_GX2SURFFMT::R8_G8_UNORM)) return 3;
 ps.textureRenderTargetIndex[0]=255;
 if(key(Latte::E_GX2SURFFMT::R8_UNORM)!=key(Latte::E_GX2SURFFMT::RGBA)) {
  puts("FAIL: non-fetch programs acquire redundant format variants");return 4;
 }
 puts("PASS: actual PS cache separates differing fetch channels and reuses identical programs");
}
'''.replace('HELPER',helper).replace('PRODUCTION',function(source,'uint64 LatteSHRC_CalcPSAuxHash('))
with tempfile.TemporaryDirectory(prefix='cemu-fetch-key-') as temp:
    p=Path(temp);(p/'test.cpp').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
