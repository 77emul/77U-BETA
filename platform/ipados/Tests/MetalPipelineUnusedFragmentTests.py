#!/usr/bin/env python3
"""The real compiler must not gate streamout on an unused fragment stage."""
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[3]
source=(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.cpp').read_text()
start=source.index('    if (forceCompile)',source.index('bool MetalPipelineCompiler::Compile('))
end=source.index('\n\t// Compile',start)
preparation=source[start:end]
fixture=r'''
#include <cassert>
#include <chrono>
#include <cstdio>
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include <thread>
struct Shader {
 bool ready=false;
 int requested=0;
 bool IsCompiled() const { return ready; }
 void PreponeCompilation(bool) { ++requested; ready=true; }
};
bool prepare(bool forceCompile,bool rasterizationEnabled,Shader* vertex,Shader* geometry,Shader* pixel) {
 bool isRenderThread=true;
 bool m_diagnosticsEnabled=false;
 RuntimePerformance::PipelineCompileTimings m_compileTimings{};
 bool m_rasterizationEnabled=rasterizationEnabled;
 auto* m_vertexShaderMtl=vertex;
 auto* m_geometryShaderMtl=geometry;
 auto* m_pixelShaderMtl=pixel;
''' + preparation + r'''
 return true;
}
int main() {
 Shader vertex,geometry,pixel;
 if (!prepare(true,false,&vertex,&geometry,&pixel)) return 1;
 if (!vertex.ready || !geometry.ready) return 2;
 if (pixel.ready || pixel.requested) {
  puts("FAIL: streamout blocks on a fragment stage absent from its Metal descriptor"); return 3;
 }
 // A readiness check must also ignore an uncompiled unused fragment stage.
 if (!prepare(false,false,&vertex,&geometry,&pixel)) return 4;
 // Visible passes must still wait: skipping first-use UI/shadow/ink draws is unsafe.
 if (prepare(false,true,&vertex,&geometry,&pixel)) return 5;
 if (!prepare(true,true,&vertex,&geometry,&pixel) || !pixel.ready) return 6;
 Shader freshVertex;
 if (prepare(false,false,&freshVertex,nullptr,&pixel)) return 7;
 if (!prepare(true,false,&freshVertex,nullptr,nullptr)) return 8;
 puts("PASS: streamout needs only vertex/geometry; rasterized draws retain fragment readiness");
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-unused-fragment-') as temp:
 p=Path(temp); (p/'test.cpp').write_text(fixture)
 subprocess.run(['xcrun','clang++','-std=c++20','-O3','-I'+str(repo/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
