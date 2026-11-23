#!/usr/bin/env python3
"""Run the production shader-analyzer classification: sampling vs reads/gathers.
This validates diagnostic requirements, not a missing hardware filter fallback.
"""
from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[3]
s=(r/'src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerAnalyzer.cpp').read_text()
a=s.index('\t\t\tbool useTexelCoords = false;');b=s.index('\n\t\t}\n\t\telse if',a)
code=s[a:b]
test=r'''
#include <array>
#include <bitset>
#include <cassert>
#include <cstdio>
enum {GPU7_TEX_INST_SAMPLE,GPU7_TEX_INST_LD,GPU7_TEX_INST_FETCH4,GPU7_TEX_INST_SAMPLE_L,GPU7_TEX_INST_SAMPLE_G,GPU7_TEX_INST_SAMPLE_C,GPU7_TEX_INST_SAMPLE_LZ,GPU7_TEX_INST_SAMPLE_C_LZ};
struct Shader {bool textureCanFilter[16]{};bool textureManualFilterUnsupported[16]{};};
struct Context {struct {std::bitset<16> texUnitUsesTexelCoordinates;}analyzer;};
struct Instruction {int opcode;struct{int textureIndex;bool unnormalized[4];int offsetX=0,offsetY=0,offsetZ=0;}textureFetch;};
void analyze(Shader*shader,Context*shaderContext,Instruction texInstruction){BODY}
int main(){
 for(int op:{GPU7_TEX_INST_SAMPLE,GPU7_TEX_INST_LD,GPU7_TEX_INST_FETCH4,GPU7_TEX_INST_SAMPLE_L,GPU7_TEX_INST_SAMPLE_G,GPU7_TEX_INST_SAMPLE_C}){
  Shader shader;Context context;analyze(&shader,&context,{op,{0,{false,false,false,false}}});
  assert(shader.textureCanFilter[0]==(op!=GPU7_TEX_INST_LD&&op!=GPU7_TEX_INST_FETCH4));
 }
 Shader shader;Context context;
 analyze(&shader,&context,{GPU7_TEX_INST_SAMPLE,{0,{true,true,true,true}}});
 assert(!shader.textureCanFilter[0]&&context.analyzer.texUnitUsesTexelCoordinates[0]);
 analyze(&shader,&context,{GPU7_TEX_INST_SAMPLE_L,{1,{false,false,false,false}}});
 analyze(&shader,&context,{GPU7_TEX_INST_LD,{1,{false,false,false,false}}});
 analyze(&shader,&context,{GPU7_TEX_INST_FETCH4,{1,{false,false,false,false}}});
 assert(shader.textureCanFilter[1]); // Mixed operations cannot hide a real sample.
 assert(!shader.textureCanFilter[0]&&!shader.textureCanFilter[2]);
 for(int op:{GPU7_TEX_INST_SAMPLE,GPU7_TEX_INST_SAMPLE_C,GPU7_TEX_INST_SAMPLE_LZ,GPU7_TEX_INST_SAMPLE_C_LZ}){
  Shader s;Context c;analyze(&s,&c,{op,{0,{false,false,false,false}}});
  assert(s.textureCanFilter[0]&&!s.textureManualFilterUnsupported[0]);
  analyze(&s,&c,{GPU7_TEX_INST_SAMPLE_G,{0,{false,false,false,false}}});
  assert(s.textureManualFilterUnsupported[0]); // Mixed-unit gradients remain unsupported.
 }
 {Shader s;Context c;analyze(&s,&c,{GPU7_TEX_INST_SAMPLE,{0,{false,false,false,false},2,0,0}});assert(s.textureManualFilterUnsupported[0]);}
 {Shader s;Context c;analyze(&s,&c,{GPU7_TEX_INST_SAMPLE,{0,{true,false,false,false}}});assert(s.textureManualFilterUnsupported[0]);}
 puts("PASS: production read/gather/filtered-sample classification, texel coordinates, untouched units and mixed-operation accumulation; bounded manual-filter opcode eligibility");
}
'''.replace('BODY',code)
with tempfile.TemporaryDirectory(prefix='77u-filter-requirement-') as t:
 p=Path(t);(p/'test.cpp').write_text(test)
 subprocess.run(['xcrun','clang++','-std=c++20','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
