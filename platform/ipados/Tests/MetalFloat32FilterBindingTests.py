#!/usr/bin/env python3
"""Exercise production selector layout, uniform binding and runtime eligibility."""
from pathlib import Path
import tempfile,subprocess,re
r=Path(__file__).resolve().parents[3]
header=(r/'src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitMSLHeader.hpp').read_text()
analyzer=(r/'src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerAnalyzer.cpp').read_text()
renderer=(r/'src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp').read_text()
def block(s, start):
 a=s.index(start);i=s.index('{',a);b=i+1;level=1
 while level:level+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
layout=block(header,'if (!static_cast<MetalRenderer*>(g_renderer.get())->SupportsFloat32Filtering())')
a=analyzer.index('void _initUniformBindingPoints(')
binding=block(analyzer[a:],'if (g_renderer->GetType() == RendererAPI::Metal &&')
predicate=re.search(r'const bool manualFilter = (.*?);',renderer,re.S).group(1)
fixture=r'''
#include <cassert>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <algorithm>
#include <cstdint>
using sint32=int;using uint32=unsigned;
#define LATTE_NUM_MAX_TEX_UNITS 18
#define _CRLF "\n"
enum class RendererAPI{Metal};
namespace Latte {
 enum class E_DIM{DIM_2D,DIM_2D_ARRAY,DIM_3D,DIM_CUBEMAP};
 struct LATTE_SQ_TEX_SAMPLER_WORD0_0{enum class E_XY_FILTER{POINT,BILINEAR,ANISO_BILINEAR};};
}
struct Shader {bool isCustomShader=false;Latte::E_DIM textureUnitDim[18]{};bool textureCanFilter[18]{},textureManualFilterUnsupported[18]{},textureIsIntegerFormat[18]{};};
struct Context{Shader*shader;bool hasUniformVarBlock=false;};
struct MetalRenderer{bool nativeFloat=true;bool SupportsFloat32Filtering(){return nativeFloat;}RendererAPI GetType(){return RendererAPI::Metal;}};
static auto g_renderer=std::make_unique<MetalRenderer>();
struct StringBuf{std::string s;template<class...T>void addFmt(const char* f,T&&...v){s+=std::vformat(f,std::make_format_args(v...));}};
struct Offsets {int offset_manualFloatFilter[18];Offsets(){std::fill_n(offset_manualFloatFilter,18,-1);}};
void layout(Context*decompilerContext,Offsets&uniformOffsets,unsigned&uniformCurrentOffset,StringBuf*src){auto*shader=decompilerContext->shader;LAYOUT}
void binding(Context*decompilerContext){BINDING}
struct View{unsigned mips;unsigned mipmapLevelCount()const{return mips;}};
struct Words {struct {unsigned aniso=0;unsigned get_MAX_ANISO_RATIO()const{return aniso;}}WORD0;};
bool eligible(Shader*shader,int manualFilterOffset,const View*sampledView,Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER minFilter,Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER magFilter,Words*samplerWords){return PREDICATE;}
int main(){
 for(bool native:{true,false})for(bool unsupported:{true,false})for(bool integer:{true,false})for(auto dim:{Latte::E_DIM::DIM_2D,Latte::E_DIM::DIM_2D_ARRAY,Latte::E_DIM::DIM_3D,Latte::E_DIM::DIM_CUBEMAP}){
  Shader shader;shader.textureCanFilter[3]=true;shader.textureManualFilterUnsupported[3]=unsupported;shader.textureIsIntegerFormat[3]=integer;shader.textureUnitDim[3]=dim;
  Context c{&shader};g_renderer->nativeFloat=native;Offsets offsets;unsigned position=4097;StringBuf out;
  binding(&c);layout(&c,offsets,position,&out);
  bool expected=!native&&!unsupported&&!integer&&(dim==Latte::E_DIM::DIM_2D||dim==Latte::E_DIM::DIM_2D_ARRAY);
  assert(c.hasUniformVarBlock==expected);assert((offsets.offset_manualFloatFilter[3]>=0)==expected);assert(position==(expected?4104u:4097u));
  for(unsigned i=0;i<18;++i)if(i!=3)assert(offsets.offset_manualFloatFilter[i]==-1);
 }
 using F=Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER;
 for(bool custom:{false,true})for(int offset:{-1,0,4096})for(unsigned mips:{0,1,2,8})for(unsigned aniso:{0,1,4})for(auto min:{F::POINT,F::BILINEAR,F::ANISO_BILINEAR})for(auto mag:{F::POINT,F::BILINEAR,F::ANISO_BILINEAR}){
  Shader s;s.isCustomShader=custom;Words words;words.WORD0.aniso=aniso;View view{mips};
  assert(eligible(&s,offset,&view,min,mag,&words)==(!custom&&offset>=0&&mips==1&&aniso==0&&min==F::BILINEAR&&mag==F::BILINEAR));
 }
 puts("PASS: production support-buffer declaration/binding agree; Native layout unchanged; unsupported dimensions/opcodes, mipmaps, anisotropy, mixed filters and custom shaders rejected");
}
'''.replace('LAYOUT',layout).replace('BINDING',binding).replace('PREDICATE',predicate)
with tempfile.TemporaryDirectory(prefix='77u-filter-bind-') as t:
 p=Path(t);(p/'test.cpp').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
