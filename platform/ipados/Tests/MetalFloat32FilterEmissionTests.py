#!/usr/bin/env python3
"""Compile production TEX emission for Native and fallback 2D/array samples.
The small fixture models registers; the TEX emitter body is used verbatim.
"""
from pathlib import Path
import subprocess,tempfile,re,os
r=Path(__file__).resolve().parents[3]
s=(r/'src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitMSL.cpp').read_text()
def function(s, sig):
 a=s.index(sig);b=s.index('{',a);end=b+1;level=1
 while level:level+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[a:end]
code=function(s,'static void _emitTEXSampleTextureCode(')
opcodes=sorted(set(re.findall(r'GPU7_TEX_INST_\w+',code)))
dims=sorted(set(re.findall(r'Latte::E_DIM::(\w+)',code)))
fixture=r'''
#include <cassert>
#include <cstdio>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <algorithm>
using sint32=int;using uint32=unsigned;using uint8=unsigned char;
#define LATTE_NUM_MAX_TEX_UNITS 18
#define LATTE_DECOMPILER_DTYPE_FLOAT 0
#define LATTE_DECOMPILER_DTYPE_SIGNED_INT 1
#define _CRLF "\n"
#define cemu_assert_debug(x) assert(x)
#define cemu_assert_unimplemented() assert(false)
#define debugBreakpoint() assert(false)
OPCODES
namespace Latte {enum class E_DIM{DIMS};}
namespace LatteConst {enum class ShaderType{Vertex,Pixel};}
namespace ActiveSettings {bool ForceSamplerRoundToPrecision(){return false;}}
struct StringBuf {std::string text;void add(std::string_view s){text+=s;}
 template<class... T>void addFmt(const char* f,T&&... v){text+=std::vformat(f,std::make_format_args(v...));}};
struct Shader{Latte::E_DIM textureUnitDim[18]{};bool textureUsesDepthCompare[18]{},textureIsIntegerFormat[18]{};unsigned char textureRenderTargetIndex[18];Shader(){std::fill_n(textureRenderTargetIndex,18,255);}};
struct LatteDecompilerTEXInstruction {int dstGpr=1,srcGpr=0,opcode=GPU7_TEX_INST_SAMPLE;int dstSel[4]{0,1,2,3};
 struct {int textureIndex=0;int srcSel[4]{0,1,2,3};int offsetX=0,offsetY=0,offsetZ=0,lodBias=0;bool unnormalized[4]{};}textureFetch;};
struct Output {struct {int offset_manualFloatFilter[18];}uniformOffsetsVK;Output(){std::fill_n(uniformOffsetsVK.offset_manualFloatFilter,18,-1);}};
struct LatteDecompilerShaderContext {StringBuf*shaderSource;Shader*shader;Output*output;LatteConst::ShaderType shaderType=LatteConst::ShaderType::Pixel;struct{int defaultDataType=0;}typeTracker;int*contextRegistersNew=nullptr;};
struct MetalRenderer {bool SupportsFramebufferFetch(){return false;}};static auto g_renderer=std::make_unique<MetalRenderer>();
namespace LatteMRT {int GetColorBufferFormat(int,int){return 0;}}bool MetalFramebufferFetchReplicatesRed(int){return false;}
void _recordUnsafeFramebufferFetch(LatteDecompilerShaderContext*,LatteDecompilerTEXInstruction*,int){}
bool IsValidDepthTextureType(Latte::E_DIM){return true;}
const char* _getRegisterVarName(LatteDecompilerShaderContext*,int g){return g?"GPR1":"GPR0";}
const char* _getTexGPRAccess(LatteDecompilerShaderContext*,int,int,int x,int y,int z,int w,char* tmp){std::string v="GPR0.";for(int a:{x,y,z,w})if(a>=0)v+="xyzw"[a];snprintf(tmp,32,"%s",v.c_str());return tmp;}
void _emitTEXSampleCoordInputComponent(LatteDecompilerShaderContext*c,LatteDecompilerTEXInstruction*,int channel,int){c->shaderSource->addFmt("GPR0.{}","xyzw"[channel]);}
const char* _FormatFloatAsConstant(float){return "0.0";}
EMITTER
int main(int argc,char**argv){
 for(bool array:{false,true})for(bool compare:{false,true})for(bool fallback:{false,true})for(bool zero:{false,true}){
  StringBuf src;Shader sh;Output out;LatteDecompilerShaderContext context{&src,&sh,&out};LatteDecompilerTEXInstruction tex;
  sh.textureUnitDim[0]=array?Latte::E_DIM::DIM_2D_ARRAY:Latte::E_DIM::DIM_2D;sh.textureUsesDepthCompare[0]=compare;
  if(fallback)out.uniformOffsetsVK.offset_manualFloatFilter[0]=0;
  tex.opcode=compare?(zero?GPU7_TEX_INST_SAMPLE_C_LZ:GPU7_TEX_INST_SAMPLE_C):(zero?GPU7_TEX_INST_SAMPLE_LZ:GPU7_TEX_INST_SAMPLE);
  if(compare)std::fill_n(tex.dstSel,4,0);
  _emitTEXSampleTextureCode(&context,&tex);
  auto name=std::format("{}{}{}{}",array,compare,fallback,zero);
  std::ofstream(std::string(argv[1])+"/"+name+".body")<<src.text;
  std::string type=compare?"depth2d":"texture2d";if(array)type+="_array";type+="<float>";
  std::ofstream f(std::string(argv[1])+"/"+name+".metal");
  f<<"#include <metal_stdlib>\nusing namespace metal;\nHELPER\nstruct SupportBuffer {uint tex0ManualFloatFilter;};\n";
  f<<"fragment float4 main0("<<type<<" tex0 [[texture(0)]],sampler samplr0 [[sampler(0)]],constant SupportBuffer& supportBuffer [[buffer(0)]]) {float4 GPR0=float4(0.3,0.4,0,0.47);float4 GPR1;"<<src.text<<"return GPR1;}";
 }
}
'''.replace('OPCODES','enum {'+','.join(opcodes)+'};').replace('DIMS',','.join(dims)).replace('EMITTER',code)
# Embed production helper safely as emitted literal lines in C++.
helper=(r/'src/Cafe/HW/Latte/Renderer/Metal/MetalFloat32Filter.h').read_text().split('R"MSL(')[1].split(')MSL"')[0]
import json
fixture=fixture.replace('  f<<"#include <metal_stdlib>\\nusing namespace metal;\\nHELPER\\nstruct', '  f<<"#include <metal_stdlib>\\nusing namespace metal;\\n"<<'+json.dumps(helper)+'<<"\\nstruct')
baseline=os.environ.get('CEMU_TEST_NATIVE_TEX_BASELINE')
if baseline:
 before=function(Path(baseline).read_text(),'static void _emitTEXSampleTextureCode(').replace('_emitTEXSampleTextureCode(', '_emitTEXSampleTextureCodeBefore(',1)
 fixture=fixture.replace('int main(int argc,char**argv){', before+'\nint main(int argc,char**argv){')
 fixture=fixture.replace('  auto name=std::format', '  if(!fallback){StringBuf old;context.shaderSource=&old;_emitTEXSampleTextureCodeBefore(&context,&tex);context.shaderSource=&src;assert(old.text==src.text); }\n  auto name=std::format')
with tempfile.TemporaryDirectory(prefix='77u-filter-emission-') as t:
 p=Path(t);(p/'test.cpp').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),t],check=True)
 for m in sorted(p.glob('*.metal')):
  subprocess.run(['xcrun','-sdk','iphoneos','metal','-std=metal3.0','-target','air64-apple-ios16.0','-c',str(m),'-o',str(m.with_suffix('.air'))],check=True,stdout=subprocess.DEVNULL)
 for m in sorted(p.glob('*.metal')):
  vertex=m.with_suffix('.vertex.metal')
  vertex.write_text(m.read_text().replace('fragment float4 main0(', 'struct VertexResult {float4 position [[position]];};\nvertex VertexResult main0(').replace('return GPR1;', 'return VertexResult{GPR1};'))
  subprocess.run(['xcrun','-sdk','iphoneos','metal','-std=metal3.0','-target','air64-apple-ios16.0','-c',str(vertex),'-o',str(vertex.with_suffix('.air'))],check=True,stdout=subprocess.DEVNULL)
 print(('Native byte-identical to baseline; ' if baseline else '')+'PASS: 32 production TEX emissions compile for iOS Metal: fragment/vertex, 2D/array, colour/depth, Native/fallback, implicit/zero LOD')
