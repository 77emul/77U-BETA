#pragma once
#include <atomic>
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"

#include "Cafe/HW/Latte/Renderer/Metal/MetalAttachmentsInfo.h"

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"

struct PipelineObject
{
    MTL::RenderPipelineState* m_pipeline = nullptr;
    MTL::RenderPipelineState* m_capturePipeline = nullptr;
    uint32_t m_payloadStride = 0, m_expandedVertices = 0, m_fallbackOutputTopology = 0;
    ~PipelineObject() { if (m_capturePipeline) m_capturePipeline->release(); }
    std::atomic<bool> m_compilationFinished{false};
    MTL::RenderPipelineState* GetPipeline() const {
        return m_compilationFinished.load(std::memory_order_acquire) ? m_pipeline : nullptr;
    }
    void WaitForCompilation() const {
        if (!m_compilationFinished.load(std::memory_order_acquire))
        {
            RuntimePerformance::activePipelineWaiters.fetch_add(1, std::memory_order_relaxed);
            m_compilationFinished.wait(false, std::memory_order_acquire);
            RuntimePerformance::activePipelineWaiters.fetch_sub(1, std::memory_order_relaxed);
        }
    }

};

class MetalPipelineCompiler
{
public:
    MetalPipelineCompiler(class MetalRenderer* metalRenderer, PipelineObject& pipelineObj, bool diagnosticsEnabled = false) : m_mtlr{metalRenderer}, m_pipelineObj{pipelineObj}, m_diagnosticsEnabled{diagnosticsEnabled} {}
    ~MetalPipelineCompiler();

    void InitFromState(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, const class MetalAttachmentsInfo& lastUsedAttachmentsInfo, const class MetalAttachmentsInfo& activeAttachmentsInfo, const LatteContextRegister& lcr);

    bool Compile(bool forceCompile, bool isRenderThread, bool showInOverlay);
    const RuntimePerformance::PipelineCompileTimings& GetCompileTimings() const { return m_compileTimings; }
    bool IsMeshPipeline() const { return m_usesGeometryShader; }

private:
    class MetalRenderer* m_mtlr;
    PipelineObject& m_pipelineObj;
    bool m_diagnosticsEnabled = false;
    RuntimePerformance::PipelineCompileTimings m_compileTimings{};

    class RendererShaderMtl* m_vertexShaderMtl;
    class RendererShaderMtl* m_geometryShaderMtl;
    // Guest stages are borrowed. Rectangle emulation creates a private stage
    // whose queued compiler job must finish before this owner releases it.
    class RendererShaderMtl* m_ownedGeometryShader = nullptr;
    class RendererShaderMtl* m_pixelShaderMtl;
    const class LatteDecompilerShader* m_pixelShader = nullptr;
    bool m_usesGeometryShader;
    bool m_rasterizationEnabled;

    NS::Object* m_pipelineDescriptor = nullptr;

    void InitFromStateRender(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const class MetalAttachmentsInfo& lastUsedAttachmentsInfo, const class MetalAttachmentsInfo& activeAttachmentsInfo, const LatteContextRegister& lcr);

    void InitFromStateMesh(const LatteFetchShader* fetchShader, const class MetalAttachmentsInfo& lastUsedAttachmentsInfo, const class MetalAttachmentsInfo& activeAttachmentsInfo, const LatteContextRegister& lcr);
};
