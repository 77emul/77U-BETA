#!/usr/bin/env python3
"""Exercise production compiler initialization/destruction with owned/borrowed shaders."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = Path(os.environ.get('RECTANGLE_COMPILER_SOURCE',
                            repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.cpp')).read_text()


def definition(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


fixture = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
using uint32=unsigned;
static unsigned liveGenerated = 0, destroyedGenerated = 0, generatedWaits = 0;
struct RendererShaderMtl {
    bool generated;
    std::atomic<bool> ready{false};
    std::thread job;
    explicit RendererShaderMtl(bool own=false) : generated(own) {
        if (generated) {
            ++liveGenerated;
            job = std::thread([this] {
                std::this_thread::yield();
                ready.store(true, std::memory_order_release);
            });
        }
    }
    bool WaitForCompiled() {
        if (generated) {
            ++generatedWaits;
            if (job.joinable()) job.join();
        }
        return ready.load(std::memory_order_acquire);
    }
    ~RendererShaderMtl() {
        if (generated) {
            assert(ready.load(std::memory_order_acquire));
            assert(!job.joinable() && "deleted a shader still owned by its async job");
            --liveGenerated; ++destroyedGenerated;
        }
    }
};
struct MetalRenderer { bool supported=true; bool SupportsMeshShaders() const { return supported; } };
struct LatteFetchShader {};
struct LatteDecompilerShader { RendererShaderMtl* shader; unsigned ringParameterCount=1,ringParameterCountFromPrevStage=1,outputParameterMask=0; };
struct MetalAttachmentsInfo {};
struct LatteContextRegister {
    bool rectangles=false, rasterize=true;
    struct Primitive { unsigned get_PRIMITIVE_MODE()const{return 3;} } VGT_PRIMITIVE_TYPE;
    const unsigned* GetRawView()const {static unsigned r[4]{};return r;}
    bool IsRasterizationEnabled() const { return rasterize; }
};
bool UseRectEmulation(const LatteContextRegister& lcr) { return lcr.rectangles; }
bool UseGeometryShader(const LatteContextRegister& lcr, bool hasGS) { return lcr.rectangles || hasGS; }
RendererShaderMtl* rectsEmulationGS_generate(MetalRenderer*,const LatteDecompilerShader*,const LatteContextRegister&) {
    return new RendererShaderMtl(true);
}
constexpr unsigned mmVGT_GS_OUT_PRIM_TYPE=0;
unsigned GetVerticesPerPrimitive(unsigned){return 3;}
struct LatteShaderPSInputTable {
 int getVertexShaderOutParamSemanticId(const unsigned*,unsigned){return -1;}
 bool hasPSImportForSemanticId(int){return false;}
};
void LatteShader_CreatePSInputTable(LatteShaderPSInputTable*,const unsigned*){}
#include "Cafe/HW/Latte/Renderer/Metal/MetalGeometryFallback.h"
struct PipelineObject { unsigned m_payloadStride=0,m_expandedVertices=0,m_fallbackOutputTopology=0; std::atomic<bool> m_compilationFinished{false}; };
struct Descriptor { void release() {} };
struct MetalPipelineCompiler {
    MetalRenderer* m_mtlr;
    PipelineObject& m_pipelineObj;
    bool m_diagnosticsEnabled=false;
    RuntimePerformance::PipelineCompileTimings m_compileTimings{};
    RendererShaderMtl* m_vertexShaderMtl=nullptr;
    RendererShaderMtl* m_geometryShaderMtl=nullptr;
    RendererShaderMtl* m_pixelShaderMtl=nullptr;
    RendererShaderMtl* m_ownedGeometryShader=nullptr;
    const LatteDecompilerShader* m_pixelShader=nullptr;
    bool m_usesGeometryShader=false, m_rasterizationEnabled=false;
    Descriptor* m_pipelineDescriptor=nullptr;
    ~MetalPipelineCompiler();
    void InitFromState(const LatteFetchShader*,const LatteDecompilerShader*,const LatteDecompilerShader*,
                       const LatteDecompilerShader*,const MetalAttachmentsInfo&,const MetalAttachmentsInfo&,const LatteContextRegister&);
    void InitFromStateMesh(const LatteFetchShader*,const MetalAttachmentsInfo&,const MetalAttachmentsInfo&,const LatteContextRegister&) {}
    void InitFromStateRender(const LatteFetchShader*,const LatteDecompilerShader*,const MetalAttachmentsInfo&,const MetalAttachmentsInfo&,const LatteContextRegister&) {}
};
'''
fixture += definition('MetalPipelineCompiler::~MetalPipelineCompiler()')
fixture += '\n' + definition('void MetalPipelineCompiler::InitFromState(')
fixture += r'''
int main() {
    MetalRenderer renderer;
    RendererShaderMtl vertex, pixel, guestGeometry;
    LatteDecompilerShader vs{&vertex}, ps{&pixel}, gs{&guestGeometry};
    LatteFetchShader fetch;
    MetalAttachmentsInfo attachments;
    for (unsigned i=0;i<400;++i) {
        PipelineObject pipeline;
        {
            MetalPipelineCompiler compiler{&renderer,pipeline};
            LatteContextRegister state{true,true};
            compiler.InitFromState(&fetch,&vs,nullptr,&ps,attachments,attachments,state);
            assert(compiler.m_geometryShaderMtl && compiler.m_geometryShaderMtl->generated);
            // Alternate completed and still-in-flight compiler destruction.
            if (i % 2) compiler.m_geometryShaderMtl->WaitForCompiled();
        }
        if (liveGenerated != 0) {
            std::fprintf(stderr,"FAIL: generated rectangle shader leaked after compiler destruction\n");
            return 1;
        }
        assert(pipeline.m_compilationFinished.load(std::memory_order_acquire));
    }
    assert(destroyedGenerated==400 && generatedWaits>=400);
    for (bool rectangles : {false,true}) {
        PipelineObject pipeline;
        MetalPipelineCompiler compiler{&renderer,pipeline};
        LatteContextRegister state{rectangles,true};
        compiler.InitFromState(&fetch,&vs,&gs,&ps,attachments,attachments,state);
        assert(compiler.m_geometryShaderMtl==&guestGeometry);
        assert(!compiler.m_ownedGeometryShader);
    }
    renderer.supported=false;
    {
        PipelineObject pipeline;
        MetalPipelineCompiler compiler{&renderer,pipeline};
        compiler.InitFromState(&fetch,&vs,nullptr,&ps,attachments,attachments,{true,true});
        assert(compiler.m_ownedGeometryShader);
        assert(pipeline.m_payloadStride==48 && pipeline.m_expandedVertices==6);
    }
    assert(liveGenerated==0 && destroyedGenerated==401);
    puts("PASS: 400 owned rectangle shaders reclaimed, async jobs completed before destruction, borrowed guest shaders preserved, legacy rectangle stage owned and metadata initialized");
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-rectangle-lifetime-') as temp:
    temp = Path(temp)
    (temp / 'test.cpp').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O1','-fsanitize=address,undefined',
                    '-I'+str(repo/'src'),str(temp/'test.cpp'),'-o',str(temp/'test')],check=True)
    subprocess.run([str(temp/'test')],check=True)
