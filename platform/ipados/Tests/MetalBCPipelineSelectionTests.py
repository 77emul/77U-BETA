#!/usr/bin/env python3
"""Execute production specialization caching and failure fallback with Metal doubles."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp').read_text();start=s.index('MTL::ComputePipelineState* MetalRenderer::GetBCTranscodePipeline(');s=s[start:s.index('void MetalRenderer::surfaceCopy_copySurfaceWithFormatConversion(',start)]
fixture=r'''
#include <array>
#include <cstdint>
#include <cassert>
using uint32=uint32_t;
#define NS_STACK_SCOPED
#define cemuLog_log(...) ((void)0)
namespace NS{using UInteger=unsigned long;struct Error{};}
namespace MTL{
 enum {DataTypeUInt};
 unsigned selected=99,lookups=0,compiles=0;bool failFunction=false,failPipeline=false;
 struct Function{};
 struct ComputePipelineState{};
 struct FunctionConstantValues{static FunctionConstantValues* alloc(){static FunctionConstantValues v;return &v;}auto*init(){return this;}void setConstantValue(const void*v,int,NS::UInteger){selected=*(const uint32*)v;}};
 struct Library{Function*newFunction(const char*,FunctionConstantValues*,NS::Error**){++lookups;static Function f;return failFunction?nullptr:&f;}};
 struct Device{ComputePipelineState*newComputePipelineState(Function*,NS::Error**){++compiles;static ComputePipelineState p[7];return failPipeline?nullptr:&p[selected];}};
}
const char*ToNSString(const char*s){return s;}
struct MetalRenderer{
 MTL::Device*m_device=nullptr;MTL::Library*m_bcTranscodeLibrary=nullptr;MTL::ComputePipelineState*m_bcTranscodePipeline=nullptr;
 std::array<MTL::ComputePipelineState*,7>m_bcFormatPipelines{};std::array<bool,7>m_bcFormatPipelineAttempted{};
 MTL::ComputePipelineState*GetBCTranscodePipeline(uint32);
};
PRODUCTION
int main(){
 MTL::Device d;MTL::Library l;MTL::ComputePipelineState generic;
 MetalRenderer r;r.m_device=&d;r.m_bcTranscodePipeline=&generic;
 assert(r.GetBCTranscodePipeline(0)==&generic&&MTL::lookups==0);
 r.m_bcTranscodeLibrary=&l;assert(r.GetBCTranscodePipeline(99)==&generic&&MTL::lookups==0);
 auto*p=r.GetBCTranscodePipeline(3);assert(p!=&generic&&MTL::selected==3&&MTL::compiles==1);
 assert(r.GetBCTranscodePipeline(3)==p&&MTL::compiles==1);
 MTL::failFunction=true;assert(r.GetBCTranscodePipeline(4)==&generic);auto n=MTL::lookups;
 MTL::failFunction=false;assert(r.GetBCTranscodePipeline(4)==&generic&&MTL::lookups==n);
 MTL::failPipeline=true;assert(r.GetBCTranscodePipeline(5)==&generic);n=MTL::compiles;
 assert(r.GetBCTranscodePipeline(5)==&generic&&MTL::compiles==n);
}
'''.replace('PRODUCTION',s)
with tempfile.TemporaryDirectory(prefix='bc-selection-') as d:
 d=Path(d);(d/'test.cpp').write_text(fixture);subprocess.run(['xcrun','clang++','-std=c++20',str(d/'test.cpp'),'-o',str(d/'test')],check=True);subprocess.run([str(d/'test')],check=True)
print('PASS: lazy per-format cache; one-time failure fallback; native/unavailable and invalid format unchanged')
