#!/usr/bin/env python3
"""Exercise production MSL emission and execute register-boundary reads on Metal.

This is a Mac GPU regression test, not a BOTW or A16 gameplay benchmark.
"""
from pathlib import Path
import subprocess
import tempfile
import os

repo = Path(__file__).resolve().parents[3]
base = Path(os.environ.get('CEMU_TEST_DECOMPILER_DIR', repo / 'src/Cafe/HW/Latte/LegacyShaderDecompiler'))
source = (base / 'LatteDecompilerEmitMSL.cpp').read_text()
header = (base / 'LatteDecompilerEmitMSLHeader.hpp').read_text()

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    level, end = 1, opening + 1
    while level:
        level += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

index_code = function(source, 'static void _emitUniformAccessIndexCode(')
start = source.index('else if( shaderContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE )')
opening = source.index('{', start)
end = source.index('else if( shaderContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK )', opening)
cfile_code = source[opening:end].rstrip()
start = header.index('uint32 cfileSize =', header.index('LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE'))
end = header.index('// special uniforms', start)
declaration = header[start:end].rsplit('}', 1)[0]

fixture = r'''
#include <cassert>
#include <format>
#include <fstream>
#include <string>
#include <cstdint>
#include <algorithm>
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalConstantRegisterAccess.h"
using sint32=int32_t; using uint32=uint32_t;
#define cemu_assert_unimplemented() assert(false)
#define LATTE_DECOMPILER_DTYPE_SIGNED_INT 1
#define _CRLF "\r\n"
struct StringBuf {
 std::string text;
 void add(std::string_view s){text+=s;}
 template<class... T> void addFmt(std::format_string<T...> f,T&&... v){text+=std::format(f,std::forward<T>(v)...);}
};
struct CF {int cBank0AddrBase=0,cBank1AddrBase=0;};
struct Operand {int sel=256,rel=0,chan=0;};
struct LatteDecompilerALUInstruction {Operand sourceOperand[3];int indexMode=0;CF* cfInstruction=nullptr;};
struct Tracker {int count=256;int DetermineSize(uint64_t,int) const {return count;}};
struct LatteDecompilerShaderContext {
 StringBuf* shaderSource;
 uint64_t shaderBaseHash=0x37b9100c1310d3bb;
 struct {Tracker uniformRegisterAccessTracker;} analyzer;
};
void _emitTypeConversionPrefixMSL(LatteDecompilerShaderContext*,int,int){}
void _emitTypeConversionSuffixMSL(LatteDecompilerShaderContext*,int,int){}
void _appendChannelAccess(StringBuf* src,int channel){src->addFmt(".{}","xyzw"[channel]);}
INDEX_CODE
void cfile(LatteDecompilerShaderContext* shaderContext, LatteDecompilerALUInstruction* aluInstruction,int operandIndex,int requiredType){
 StringBuf* src=shaderContext->shaderSource;
 CFILE_CODE
}
std::string declaration(LatteDecompilerShaderContext* decompilerContext){
 StringBuf output;auto src=&output;
 struct {int offset_uniformRegister=0,count_uniformRegister=0;} uniformOffsets;
 unsigned uniformCurrentOffset=0;
 DECLARATION
 const auto count=decompilerContext->analyzer.uniformRegisterAccessTracker.count;
 assert(uniformOffsets.count_uniformRegister==count && uniformCurrentOffset==unsigned(count*16));
 return output.text;
}
int main(int argc,char** argv){
 StringBuf output;LatteDecompilerShaderContext context{&output};
 LatteDecompilerALUInstruction instruction;
 assert(declaration(&context)=="int4 uniformRegister[256];\r\n");
 for(int axis=0;axis<4;++axis)for(int channel=0;channel<4;++channel)for(int base:{0,17,255}){
  output.text.clear();instruction.indexMode=axis;instruction.sourceOperand[0]={256+base,1,channel};
  cfile(&context,&instruction,0,1);
  auto expected=std::format("latteReadConstantRegister(supportBuffer.uniformRegister, uint(ARi.{})+{}u).{}","xyzw"[axis],base,"xyzw"[channel]);
  assert(output.text==expected);
 }
 instruction.sourceOperand[0]={511,0,2};output.text.clear();cfile(&context,&instruction,0,1);
 assert(output.text=="supportBuffer.uniformRegister[255].z"); // Static path unchanged.
 // Keep title-specific compact uploads and their valid access expressions.
 context.analyzer.uniformRegisterAccessTracker.count=24;
 assert(declaration(&context)=="int4 uniformRegister[24];\r\n");
 instruction.sourceOperand[0]={256,1,0};output.text.clear();cfile(&context,&instruction,0,1);
 assert(output.text=="supportBuffer.uniformRegister[ARi.w+0].x");
 context.analyzer.uniformRegisterAccessTracker.count=256;
 // CBANK callers retain their existing expression and resource layout.
 CF cf;cf.cBank0AddrBase=32;instruction.cfInstruction=&cf;
 instruction.sourceOperand[0]={129,1,0};output.text.clear();
 _emitUniformAccessIndexCode(&context,&instruction,0);assert(output.text=="ARi.w+33");
 std::ofstream file(argv[1]);file<<"#include <metal_stdlib>\nusing namespace metal;\n";
 file<<MetalConstantRegisterAccess::Source;
 file<<"struct SupportBuffer{"<<declaration(&context)<<"};\n";
 for(int axis=0;axis<4;++axis)for(int base:{0,17,255}){
  instruction.indexMode=axis;instruction.sourceOperand[0]={256+base,1,0};output.text.clear();cfile(&context,&instruction,0,1);
  // Emit all four result components through the actual production expression.
  std::string expression=output.text.substr(0,output.text.size()-2);
  file<<std::format("kernel void read{}_{}(constant SupportBuffer& supportBuffer [[buffer(0)]],device const int* indices [[buffer(1)]],device int4* result [[buffer(2)]],uint tid [[thread_position_in_grid]]){{int4 ARi=int4(indices[tid]);result[tid]={};}}\n",axis,base,expression);
 }
}
'''.replace('INDEX_CODE', index_code).replace('CFILE_CODE', cfile_code).replace('DECLARATION', declaration)

metal_test = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cassert>
#include <vector>
#include <limits>
#include <cstdio>
#include <cstdint>
int main(int argc,char**argv){@autoreleasepool{
 auto device=MTLCreateSystemDefaultDevice();assert(device);
 auto text=[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[1]] encoding:NSUTF8StringEncoding error:nil];
 auto queue=[device newCommandQueue];
 std::vector<int32_t> indices;for(int i=0;i<256;++i)indices.push_back(i);
 for(int i:{-300,-256,-255,-18,-17,-1,256,257,4096,INT32_MIN,INT32_MAX})indices.push_back(i);
 int32_t registers[256][4];for(int i=0;i<256;++i)for(int c=0;c<4;++c)registers[i][c]=(i*0x010001)+c+1;
 auto input=[device newBufferWithBytes:registers length:sizeof(registers) options:MTLResourceStorageModeShared];
 auto address=[device newBufferWithBytes:indices.data() length:indices.size()*sizeof(int32_t) options:MTLResourceStorageModeShared];
 auto output=[device newBufferWithLength:indices.size()*4*sizeof(int32_t) options:MTLResourceStorageModeShared];
 unsigned checked=0;
 for(bool fast:{false,true}){
  auto options=[MTLCompileOptions new];options.fastMathEnabled=fast;
  NSError* error=nil;auto library=[device newLibraryWithSource:text options:options error:&error];
  if(!library){NSLog(@"%@",error);abort();}
  for(int axis=0;axis<4;++axis)for(int base:{0,17,255}){
   auto name=[NSString stringWithFormat:@"read%d_%d",axis,base];auto function=[library newFunctionWithName:name];assert(function);
   auto pipeline=[device newComputePipelineStateWithFunction:function error:&error];assert(pipeline);
   auto cb=[queue commandBuffer];auto encoder=[cb computeCommandEncoder];[encoder setComputePipelineState:pipeline];
   [encoder setBuffer:input offset:0 atIndex:0];[encoder setBuffer:address offset:0 atIndex:1];[encoder setBuffer:output offset:0 atIndex:2];
   [encoder dispatchThreads:MTLSizeMake(indices.size(),1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[encoder endEncoding];
   [cb commit];[cb waitUntilCompleted];if(cb.status!=MTLCommandBufferStatusCompleted){NSLog(@"%@",cb.error);abort();}
   auto actual=(int32_t*)output.contents;
   for(size_t t=0;t<indices.size();++t){int64_t index=int64_t(indices[t])+base;
    for(int c=0;c<4;++c){int32_t expected=index>=0&&index<256?registers[index][c]:INT32_MAX;
     assert(actual[t*4+c]==expected);++checked;}}
  }
 }
 printf("PASS: production constant-register emission and %u real Metal component reads; all 256 valid registers, negative/extreme AR, 4 axes, 3 bases, fast math on/off; no GPU errors\n",checked);
 // Isolated valid-index GPU microbenchmark. The baseline reads only valid
 // indices; never submit the original unsafe expression with invalid input.
 NSString* baseline=@"\nkernel void directRead(constant SupportBuffer& supportBuffer [[buffer(0)]],device const int* indices [[buffer(1)]],device int4* result [[buffer(2)]],uint tid [[thread_position_in_grid]]){result[tid]=supportBuffer.uniformRegister[uint(indices[tid])];}\n";
 NSError* error=nil;auto library=[device newLibraryWithSource:[text stringByAppendingString:baseline] options:nil error:&error];assert(library);
 id<MTLComputePipelineState> pipelines[2];
 pipelines[0]=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"directRead"] error:&error];
 pipelines[1]=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"read0_0"] error:&error];
 assert(pipelines[0]&&pipelines[1]);
 constexpr size_t count=65536;std::vector<int32_t> valid(count);for(size_t i=0;i<count;++i)valid[i]=i%256;
 auto validIndices=[device newBufferWithBytes:valid.data() length:count*4 options:MTLResourceStorageModeShared];
 auto results=[device newBufferWithLength:count*16 options:MTLResourceStorageModeShared];
 std::vector<double> times[2];
 for(unsigned round=0;round<50;++round)for(unsigned order=0;order<2;++order){
  unsigned kind=order^(round&1);auto cb=[queue commandBuffer];auto e=[cb computeCommandEncoder];[e setComputePipelineState:pipelines[kind]];
  [e setBuffer:input offset:0 atIndex:0];[e setBuffer:validIndices offset:0 atIndex:1];[e setBuffer:results offset:0 atIndex:2];
  for(unsigned n=0;n<16;++n)[e dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
  [e endEncoding];[cb commit];[cb waitUntilCompleted];assert(cb.status==MTLCommandBufferStatusCompleted);
  if(round>=10&&cb.GPUEndTime>cb.GPUStartTime)times[kind].push_back((cb.GPUEndTime-cb.GPUStartTime)*1000.0);
 }
 for(unsigned k=0;k<2;++k)std::sort(times[k].begin(),times[k].end());
 if(!times[0].empty()&&!times[1].empty()){
  double old=times[0][times[0].size()/2],safe=times[1][times[1].size()/2];
  printf("MICROBENCH: Mac GPU, 16 x 65536 valid CFILE reads/batch, median direct=%.6fms guarded=%.6fms ratio=%.4f; not game FPS or A16 timing\n",old,safe,safe/old);
 }
}}
'''

with tempfile.TemporaryDirectory(prefix='cemu-constant-register-') as tmp:
    work = Path(tmp)
    cpp = work / 'emit.cpp'
    cpp.write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O1','-fsanitize=address,undefined','-I'+str(repo/'src'),str(cpp),'-o',str(work/'emit')],check=True)
    msl = work / 'generated.metal'
    subprocess.run([str(work/'emit'),str(msl)],check=True)
    mm = work / 'gpu.mm'
    mm.write_text(metal_test)
    subprocess.run(['xcrun','clang++','-std=c++20','-O2','-fobjc-arc',str(mm),'-framework','Foundation','-framework','Metal','-o',str(work/'gpu')],check=True)
    subprocess.run([str(work/'gpu'),str(msl)],check=True,timeout=60)
