#!/usr/bin/env python3
"""Run the production cache key against dynamic state and restored GPU state."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp').read_text()
start = source.index('uint64 MetalPipelineCache::CalculatePipelineHash(')
end = source.index('\nstruct\n', start)
production = source[start:end]
fixture = r'''
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <vector>
using uint64=uint64_t; using uint32=uint32_t;
constexpr uint32 mmVGT_STRMOUT_EN=0x28ab, mmVGT_PRIMITIVE_TYPE=0x2256;
namespace Latte {
namespace GPU_LIMITS { constexpr int NUM_COLOR_ATTACHMENTS=8; }
enum class E_GX2SURFFMT { INVALID_FORMAT=0, RGBA=1, DEPTH=2 };
struct LATTE_VGT_PRIMITIVE_TYPE { enum class E_PRIMITIVE_TYPE { TRIANGLES=4, RECTS=17 }; };
namespace REGADDR {
constexpr int CB_BLEND0_CONTROL=0xa1e0, CB_COLOR_CONTROL=0xa202,
CB_TARGET_MASK=0xa08e, PA_CL_CLIP_CNTL=0xa204, PA_SU_SC_MODE_CNTL=0xa205;
}
}
using LattePrimitiveMode=Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
struct RawReg {
 uint32* value;
 uint32 getRawValue() const { return *value; }
 bool get_DX_RASTERIZATION_KILL() const { return (*value & (1u<<22))!=0; }
 LattePrimitiveMode get_PRIMITIVE_MODE() const { return LattePrimitiveMode(*value); }
};
struct LatteContextRegister {
 mutable std::array<uint32,0x10000> raw{};
 RawReg PA_CL_CLIP_CNTL{&raw[0xa204]}, PA_SU_SC_MODE_CNTL{&raw[0xa205]}, VGT_PRIMITIVE_TYPE{&raw[0x2256]};
 uint32* GetRawView() const { return raw.data(); }
 bool IsRasterizationEnabled() const {
  bool enabled = !PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL();
  if (!(raw[0xa206] & (1u<<1))) enabled=true;
  if ((raw[0xa205]&3)==3) enabled=false;
  return enabled;
 }
};
struct { std::array<uint32,0x10000> contextRegister{}; } LatteGPUState;
struct Group { uint32 getCurrentBufferStride(uint32* raw) const { return raw[0xe382]; } };
struct LatteFetchShader { std::vector<Group> bufferGroups{Group{}}; uint64 getVkPipelineHashFragment() const { return 17; } };
struct LatteDecompilerShader { uint64 baseHash=123,auxHash=456; };
struct MetalAttachmentsInfo {
 std::array<Latte::E_GX2SURFFMT,8> colorFormats{};
 Latte::E_GX2SURFFMT depthFormat=Latte::E_GX2SURFFMT::INVALID_FORMAT;
};
uint64 GetMtlPixelFormat(Latte::E_GX2SURFFMT format,bool) { return uint64(format); }
struct MetalPipelineCache {
 static uint64 CalculatePipelineHash(const LatteFetchShader*,const LatteDecompilerShader*,const LatteDecompilerShader*,const LatteDecompilerShader*,const MetalAttachmentsInfo&,const MetalAttachmentsInfo&,const LatteContextRegister&);
};
'''
tests = r'''
int main() {
 LatteFetchShader fetch;
 LatteDecompilerShader vs,ps,gs;
 MetalAttachmentsInfo last,active;
 last.colorFormats[0]=active.colorFormats[0]=Latte::E_GX2SURFFMT::RGBA;
 LatteContextRegister lcr;
 lcr.raw[mmVGT_PRIMITIVE_TYPE]=4;
 lcr.raw[0xa08e]=15;
 lcr.raw[0xa206]=2;
 auto key=[&](const LatteDecompilerShader* geometry=nullptr) {
  return MetalPipelineCache::CalculatePipelineHash(&fetch,&vs,geometry,&ps,last,active,lcr);
 };
 const auto original=key();
 // Encoder-owned culling, winding, offset and depth clipping do not recreate PSOs.
 for (uint32 polygon : {0u,1u,2u,0x281a41u,0x281a42u}) {
  lcr.raw[0xa205]=polygon;
  if (key()!=original) { puts("FAIL: dynamic polygon state creates a duplicate pipeline"); return 1; }
 }
 lcr.raw[0xa204]=(1u<<16)|(1u<<17);
 if (key()!=original) { puts("FAIL: dynamic depth clipping creates a duplicate pipeline"); return 2; }
 // Rasterization changes still need separate pipelines.
 lcr.raw[0xa205]=3;
 if (key()==original) return 3;
 lcr.raw[0xa205]=0;
 lcr.raw[0xa204]=(1u<<22);
 if (key()==original) return 4;
 // Preserve the existing guest rasterization rule when viewport offsets are disabled.
 lcr.raw[0xa206]=0;
 if (key()!=original) return 5;
 lcr.raw[0xa204]=0; lcr.raw[0xa206]=2;
 // Compile-affecting attachment, write mask, blending and vertex layout remain distinct.
 lcr.raw[0xa08e]=7; if (key()==original) return 6; lcr.raw[0xa08e]=15;
 lcr.raw[0xa202]=1<<8; if (key()==original) return 7; lcr.raw[0xa202]=0;
 lcr.raw[0xe382]=24; if (key()==original) return 8; lcr.raw[0xe382]=0;
 active.colorFormats[0]=Latte::E_GX2SURFFMT::INVALID_FORMAT;
 if (key()==original) return 9; active=last;
 // A restored pipeline must never depend on the live/global GPU registers.
 lcr.raw[mmVGT_PRIMITIVE_TYPE]=17;
 LatteGPUState.contextRegister[mmVGT_PRIMITIVE_TYPE]=0;
 const auto restored=key();
 LatteGPUState.contextRegister[mmVGT_PRIMITIVE_TYPE]=17;
 if (key()!=restored) { puts("FAIL: restored rectangle cache key reads global state"); return 10; }
 if (restored==original) return 11;
 const auto withGS=key(&gs);
 LatteGPUState.contextRegister[mmVGT_PRIMITIVE_TYPE]=4;
 if (key(&gs)!=withGS) return 12;

 // Streamout has no fragment function or attachments in SetFragmentState.
 lcr.raw[0xa205]=3;
 lcr.raw[mmVGT_PRIMITIVE_TYPE]=4;
 const auto streamout=key();
 vs.auxHash+=1;
 if (key()==streamout) { puts("FAIL: required vertex auxiliary variant collides with streamout pipeline"); return 17; }
 vs.auxHash-=1;
 const auto streamoutWithGS=key(&gs);
 gs.auxHash+=1;
 if (key(&gs)==streamoutWithGS) { puts("FAIL: required geometry auxiliary variant collides with streamout pipeline"); return 18; }
 gs.auxHash-=1;
 ps.baseHash+=1; ps.auxHash+=1;
 if (key()!=streamout) { puts("FAIL: unused fragment shader creates a duplicate streamout PSO"); return 13; }
 last.colorFormats[0]=Latte::E_GX2SURFFMT::INVALID_FORMAT;
 active.colorFormats[0]=Latte::E_GX2SURFFMT::INVALID_FORMAT;
 last.depthFormat=Latte::E_GX2SURFFMT::DEPTH;
 lcr.raw[0xa08e]=1; lcr.raw[0xa202]=0xffff;
 lcr.raw[Latte::REGADDR::CB_BLEND0_CONTROL]=0x123456;
 if (key()!=streamout) { puts("FAIL: unused framebuffer/blend state creates a duplicate streamout PSO"); return 14; }
 const auto meshStreamout=key(&gs);
 gs.baseHash+=1;
 if (key(&gs)==meshStreamout) return 15;
 vs.baseHash+=1;
 if (key()==streamout) return 16;
 puts("PASS: cache keys share dynamic and unused fragment state; preserve required stages and restored state");
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-pipeline-key-') as temporary:
 path=Path(temporary)
 (path/'key.cpp').write_text(fixture+production+tests)
 subprocess.run(['xcrun','clang++','-std=c++20',str(path/'key.cpp'),'-o',str(path/'key')],check=True)
 subprocess.run([str(path/'key')],check=True)
