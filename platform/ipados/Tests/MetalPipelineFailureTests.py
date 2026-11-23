#!/usr/bin/env python3
"""Check the production failed-pipeline cleanup with AddressSanitizer."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo / 'src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp').read_text()
start = source.index('if (!compiled)')
opening = source.index('{', start)
closing = source.index('}', opening)
cleanup = source[opening + 1:closing]
fixture = r'''
#include <cassert>
#include <map>
#include <mutex>
using FSpinlock = std::mutex;
FSpinlock m_pipelineCacheLock;
int destroyed = 0;
struct PipelineObject { ~PipelineObject() { ++destroyed; } };
std::map<int, PipelineObject*> m_pipelineCache;
PipelineObject* fail(int hash) {
    PipelineObject*& pipelineObj = m_pipelineCache[hash];
    pipelineObj = new PipelineObject;
'''
tests = r'''
}
int main() {
    for (int i = 0; i < 1000; ++i) {
        assert(fail(i) == nullptr);
        assert(m_pipelineCache.empty());
    }
    assert(destroyed == 1000);
}
'''
with tempfile.TemporaryDirectory(prefix='cemu-pipeline-failure-') as temporary:
    path = Path(temporary)
    (path / 'test.cpp').write_text(fixture + cleanup + tests)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-fsanitize=address', '-g',
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: failed pipeline removed without accessing an erased map entry (ASan)')
