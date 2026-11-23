#!/usr/bin/env python3
"""First-use presentation draws defer; required draws still wait for real results."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo / "src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp").read_text()
start = source.index("static MTL::RenderPipelineState* ResolvePipelineForDraw(")
opening = source.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
helper = source[start:end]

fixture = r"""
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#undef TARGET_OS_IOS
#define TARGET_OS_IOS 1
namespace MTL { struct RenderPipelineState {}; }
namespace RuntimePerformance {
    std::atomic<uint64_t> pipelineWaitNs{0}, pipelineWaitCount{0}, drawsSkippedPendingPipeline{0};
}
struct PipelineObject {
    MTL::RenderPipelineState* pipeline = nullptr;
    int waits = 0;
    MTL::RenderPipelineState* GetPipeline() const { return pipeline; }
    void WaitForCompilation() { ++waits; pipeline = &readyPipeline; }
    static MTL::RenderPipelineState readyPipeline;
};
MTL::RenderPipelineState PipelineObject::readyPipeline;
struct MetalPipelineCache {
    int priorities = 0;
    static MetalPipelineCache& GetInstance() { static MetalPipelineCache cache; return cache; }
    void PrioritizePendingPipeline(PipelineObject*) { ++priorities; }
};
""" + helper + r"""
template<typename Resolver>
MTL::RenderPipelineState* resolveWithPolicy(Resolver resolve, PipelineObject* object, bool defer) {
    // Run the same behavioral test against the pre-policy implementation too.
    if constexpr (std::is_invocable_v<Resolver, PipelineObject*, bool>)
        return resolve(object, defer);
    else
        return resolve(object);
}
int main() {
    PipelineObject firstUse;
    if (ResolvePipelineForDraw(&firstUse) != &PipelineObject::readyPipeline) return 1;
    if (firstUse.waits != 1) return 2;
    if (MetalPipelineCache::GetInstance().priorities != 1) return 6;
    if (RuntimePerformance::pipelineWaitCount != 1) return 7;
    if (ResolvePipelineForDraw(&firstUse) != &PipelineObject::readyPipeline) return 3;
    if (firstUse.waits != 1) return 4;
    if (MetalPipelineCache::GetInstance().priorities != 1) return 8;
    if (ResolvePipelineForDraw(nullptr) != nullptr) return 5;
    PipelineObject scene[400];
    const auto waitsBefore = RuntimePerformance::pipelineWaitCount.load();
    const auto prioritiesBefore = MetalPipelineCache::GetInstance().priorities;
    for (auto& draw : scene) {
        if (resolveWithPolicy(&ResolvePipelineForDraw, &draw, true) != nullptr || draw.waits != 0) {
            puts("FAIL: a deferrable scene draw blocks on first-use compilation"); return 10;
        }
    }
    if (RuntimePerformance::pipelineWaitCount != waitsBefore ||
        MetalPipelineCache::GetInstance().priorities != prioritiesBefore) return 11;
    // A pipeline can be used for both a scene draw and a later ink/query draw.
    // Re-evaluate the policy on each draw rather than storing it in the PSO.
    if (resolveWithPolicy(&ResolvePipelineForDraw, &scene[0], false) != &PipelineObject::readyPipeline ||
        scene[0].waits != 1) return 12;
    scene[1].pipeline = &PipelineObject::readyPipeline;
    if (resolveWithPolicy(&ResolvePipelineForDraw, &scene[1], true) != &PipelineObject::readyPipeline ||
        scene[1].waits != 0) return 13;
    if (resolveWithPolicy(&ResolvePipelineForDraw, nullptr, true) != nullptr) return 14;
    puts("PASS: 400 cold scene draws cause zero waits; required draws wait and completed draws render");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="cemu-first-use-pipeline-") as temporary:
    path = Path(temporary)
    for ios in (1, 0):
        (path / "draw.cpp").write_text(fixture.replace("#define TARGET_OS_IOS 1", f"#define TARGET_OS_IOS {ios}"))
        subprocess.run(["xcrun", "clang++", "-std=c++20", str(path / "draw.cpp"),
                        "-o", str(path / "draw")], check=True)
        subprocess.run([str(path / "draw")], check=True)
print("PASS: iOS and macOS preserve required draws without serializing eligible scene compilation")
