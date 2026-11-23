#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/CachedFBOMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.h"

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"

#include <chrono>
#include "Cafe/HW/Latte/Renderer/Metal/MetalGeometryFallback.h"

extern std::atomic_int g_compiling_pipelines;
extern std::atomic_int g_compiling_pipelines_async;
extern std::atomic_uint64_t g_compiling_pipelines_syncTimeSum;

static void rectsEmulationGS_outputSingleVertex(std::string& gsSrc, const LatteDecompilerShader* vertexShader, LatteShaderPSInputTable& psInputTable, sint32 vIdx, const LatteContextRegister& latteRegister)
{
	auto parameterMask = vertexShader->outputParameterMask;
	for (uint32 i = 0; i < 32; i++)
	{
		if ((parameterMask & (1 << i)) == 0)
			continue;
		sint32 vsSemanticId = psInputTable.getVertexShaderOutParamSemanticId(latteRegister.GetRawView(), i);
		if (vsSemanticId < 0)
			continue;
		// make sure PS has matching input
		if (!psInputTable.hasPSImportForSemanticId(vsSemanticId))
			continue;
		gsSrc.append(fmt::format("out.passParameterSem{} = objectPayload.vertexOut[{}].passParameterSem{};\r\n", vsSemanticId, vIdx, vsSemanticId));
	}
	gsSrc.append(fmt::format("out.position = objectPayload.vertexOut[{}].position;\r\n", vIdx));
	gsSrc.append(fmt::format("mesh.set_vertex({}, out);\r\n", vIdx));
}

static void rectsEmulationGS_outputGeneratedVertex(std::string& gsSrc, const LatteDecompilerShader* vertexShader, LatteShaderPSInputTable& psInputTable, const char* variant, const LatteContextRegister& latteRegister)
{
	auto parameterMask = vertexShader->outputParameterMask;
	for (uint32 i = 0; i < 32; i++)
	{
		if ((parameterMask & (1 << i)) == 0)
			continue;
		sint32 vsSemanticId = psInputTable.getVertexShaderOutParamSemanticId(latteRegister.GetRawView(), i);
		if (vsSemanticId < 0)
			continue;
		// make sure PS has matching input
		if (!psInputTable.hasPSImportForSemanticId(vsSemanticId))
			continue;
		gsSrc.append(fmt::format("out.passParameterSem{} = gen4thVertex{}(objectPayload.vertexOut[0].passParameterSem{}, objectPayload.vertexOut[1].passParameterSem{}, objectPayload.vertexOut[2].passParameterSem{});\r\n", vsSemanticId, variant, vsSemanticId, vsSemanticId, vsSemanticId));
	}
	gsSrc.append(fmt::format("out.position = gen4thVertex{}(objectPayload.vertexOut[0].position, objectPayload.vertexOut[1].position, objectPayload.vertexOut[2].position);\r\n", variant));
	gsSrc.append(fmt::format("mesh.set_vertex(3, out);\r\n"));
}

static void rectsEmulationGS_outputVerticesCode(std::string& gsSrc, const LatteDecompilerShader* vertexShader, LatteShaderPSInputTable& psInputTable, sint32 p0, sint32 p1, sint32 p2, sint32 p3, const char* variant, const LatteContextRegister& latteRegister)
{
	sint32 pList[4] = { p0, p1, p2, p3 };
	for (sint32 i = 0; i < 4; i++)
	{
		if (pList[i] == 3)
			rectsEmulationGS_outputGeneratedVertex(gsSrc, vertexShader, psInputTable, variant, latteRegister);
		else
			rectsEmulationGS_outputSingleVertex(gsSrc, vertexShader, psInputTable, pList[i], latteRegister);
	}
	gsSrc.append(fmt::format("mesh.set_index(0, {});\r\n", pList[0]));
	gsSrc.append(fmt::format("mesh.set_index(1, {});\r\n", pList[1]));
	gsSrc.append(fmt::format("mesh.set_index(2, {});\r\n", pList[2]));
	// A triangle strip reverses its second triangle's vertex order. Match that
	// winding when emitting a Metal triangle list so culling keeps both halves.
	gsSrc.append(fmt::format("mesh.set_index(3, {});\r\n", pList[2]));
	gsSrc.append(fmt::format("mesh.set_index(4, {});\r\n", pList[1]));
	gsSrc.append(fmt::format("mesh.set_index(5, {});\r\n", pList[3]));
}

static RendererShaderMtl* rectsEmulationGS_generate(MetalRenderer* metalRenderer, const LatteDecompilerShader* vertexShader, const LatteContextRegister& latteRegister)
{
	std::string gsSrc;
	gsSrc.append("#include <metal_stdlib>\r\n");
	gsSrc.append("using namespace metal;\r\n");

	LatteShaderPSInputTable psInputTable;
	LatteShader_CreatePSInputTable(&psInputTable, latteRegister.GetRawView());

	// inputs & outputs
	std::string vertexOutDefinition = "struct VertexOut {\r\n";
	vertexOutDefinition += "float4 position;\r\n";
	std::string geometryOutDefinition = "struct GeometryOut {\r\n";
	geometryOutDefinition += "float4 position [[position]];\r\n";
	auto parameterMask = vertexShader->outputParameterMask;
	for (uint32 i = 0; i < 32; i++)
	{
		if ((parameterMask & (1 << i)) == 0)
			continue;
		sint32 vsSemanticId = psInputTable.getVertexShaderOutParamSemanticId(latteRegister.GetRawView(), i);
		if (vsSemanticId < 0)
			continue;
		auto psImport = psInputTable.getPSImportBySemanticId(vsSemanticId);
		if (psImport == nullptr)
			continue;

		// VertexOut
		vertexOutDefinition += fmt::format("float4 passParameterSem{};\r\n", vsSemanticId);

		// GeometryOut
		geometryOutDefinition += fmt::format("float4 passParameterSem{}", vsSemanticId);

        geometryOutDefinition += fmt::format(" [[user(locn{})]]", psInputTable.getPSImportLocationBySemanticId(vsSemanticId));
        if (psImport->isFlat)
            geometryOutDefinition += " [[flat]]";
        if (psImport->isNoPerspective)
			geometryOutDefinition += " [[center_no_perspective]]";
        geometryOutDefinition += ";\r\n";
	}
	vertexOutDefinition += "};\r\n";
	geometryOutDefinition += "};\r\n";

	gsSrc.append(vertexOutDefinition);
	gsSrc.append(geometryOutDefinition);

	gsSrc.append("struct ObjectPayload {\r\n");
	gsSrc.append("VertexOut vertexOut[3];\r\n");
	gsSrc.append("};\r\n");

	// gen function
	gsSrc.append("float4 gen4thVertexA(float4 a, float4 b, float4 c)\r\n");
	gsSrc.append("{\r\n");
	gsSrc.append("return b - (c - a);\r\n");
	gsSrc.append("}\r\n");

	gsSrc.append("float4 gen4thVertexB(float4 a, float4 b, float4 c)\r\n");
	gsSrc.append("{\r\n");
	gsSrc.append("return c - (b - a);\r\n");
	gsSrc.append("}\r\n");

	gsSrc.append("float4 gen4thVertexC(float4 a, float4 b, float4 c)\r\n");
	gsSrc.append("{\r\n");
	gsSrc.append("return c + (b - a);\r\n");
	gsSrc.append("}\r\n");

	// main
	gsSrc.append("using MeshType = mesh<GeometryOut, void, 4, 2, topology::triangle>;\r\n");
	gsSrc.append("[[mesh, max_total_threads_per_threadgroup(1)]]\r\n");
	gsSrc.append("void main0(MeshType mesh, const object_data ObjectPayload& objectPayload [[payload]])\r\n");
	gsSrc.append("{\r\n");
	gsSrc.append("GeometryOut out;\r\n");

	// there are two possible winding orders that need different triangle generation:
	// 0 1
	// 2 3
	// and
	// 0 1
	// 3 2
	// all others are just symmetries of these cases

	// we can determine the case by comparing the distance 0<->1 and 0<->2

	gsSrc.append("float dist0_1 = length(objectPayload.vertexOut[1].position.xy - objectPayload.vertexOut[0].position.xy);\r\n");
	gsSrc.append("float dist0_2 = length(objectPayload.vertexOut[2].position.xy - objectPayload.vertexOut[0].position.xy);\r\n");
	gsSrc.append("float dist1_2 = length(objectPayload.vertexOut[2].position.xy - objectPayload.vertexOut[1].position.xy);\r\n");

	// emit vertices
	gsSrc.append("if(dist0_1 > dist0_2 && dist0_1 > dist1_2)\r\n");
	gsSrc.append("{\r\n");
	// p0 to p1 is diagonal
	rectsEmulationGS_outputVerticesCode(gsSrc, vertexShader, psInputTable, 2, 1, 0, 3, "A", latteRegister);
	gsSrc.append("} else if ( dist0_2 > dist0_1 && dist0_2 > dist1_2 ) {\r\n");
	// p0 to p2 is diagonal
	rectsEmulationGS_outputVerticesCode(gsSrc, vertexShader, psInputTable, 1, 2, 0, 3, "B", latteRegister);
	gsSrc.append("} else {\r\n");
	// p1 to p2 is diagonal
	rectsEmulationGS_outputVerticesCode(gsSrc, vertexShader, psInputTable, 0, 1, 2, 3, "C", latteRegister);
	gsSrc.append("}\r\n");

	gsSrc.append("mesh.set_primitive_count(2);\r\n");

	gsSrc.append("}\r\n");

	if (!metalRenderer->SupportsMeshShaders())
        gsSrc = MetalGeometryFallback::GeometrySource(std::move(gsSrc), 2, true);
    auto mtlShader = new RendererShaderMtl(metalRenderer, RendererShader::ShaderType::kGeometry, 0, 0, false, false, gsSrc);
	// The shader constructor already queues compilation. Let Compile() prepare
	// this required stage on the selected worker, after the draw policy is known.
	return mtlShader;
}

#define INVALID_TITLE_ID 0xFFFFFFFFFFFFFFFF

uint64 s_cacheTitleId = INVALID_TITLE_ID;

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

template<typename T>
void SetFragmentState(T* desc, const LatteDecompilerShader* pixelShader, const MetalAttachmentsInfo& lastUsedAttachmentsInfo, const MetalAttachmentsInfo& activeAttachmentsInfo, bool rasterizationEnabled, const LatteContextRegister& lcr)
{
	// TODO: check if the pixel shader is valid as well?
	if (!rasterizationEnabled/* || !pixelShaderMtl*/)
	{
	    desc->setRasterizationEnabled(false);
		return;
	}

    // Color attachments
	const Latte::LATTE_CB_COLOR_CONTROL& colorControlReg = lcr.CB_COLOR_CONTROL;
	uint32 blendEnableMask = colorControlReg.get_BLEND_MASK();
	uint32 renderTargetMask = lcr.CB_TARGET_MASK.get_MASK();
	for (uint8 i = 0; i < LATTE_NUM_COLOR_TARGET; i++)
	{
	    Latte::E_GX2SURFFMT format = lastUsedAttachmentsInfo.colorFormats[i];
		bool hasFBFetch = false;
		if (pixelShader)
		{
			for (sint32 t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
			{
				if (pixelShader->textureRenderTargetIndex[t] == i)
				{
					hasFBFetch = true;
					break;
				}
			}
		}

		if (format == Latte::E_GX2SURFFMT::INVALID_FORMAT)
		{
			if (hasFBFetch)
			{
				auto colorAttachment = desc->colorAttachments()->object(i);
				colorAttachment->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
				colorAttachment->setWriteMask(MTL::ColorWriteMaskNone);
			}
		    continue;
		}

		MTL::PixelFormat pixelFormat = GetMtlPixelFormat(format, false);
		auto colorAttachment = desc->colorAttachments()->object(i);
		colorAttachment->setPixelFormat(pixelFormat);

		// Disable writes if not in the active FBO
		if (activeAttachmentsInfo.colorFormats[i] == Latte::E_GX2SURFFMT::INVALID_FORMAT)
        {
            colorAttachment->setWriteMask(MTL::ColorWriteMaskNone);
            continue;
        }

		colorAttachment->setWriteMask(GetMtlColorWriteMask((renderTargetMask >> (i * 4)) & 0xF));

		// Blending
		bool blendEnabled = ((blendEnableMask & (1 << i))) != 0;
		// Only float data type is blendable
		if (blendEnabled && GetMtlPixelFormatInfo(format, false).dataType == MetalDataType::FLOAT)
		{
       		colorAttachment->setBlendingEnabled(true);

       		const auto& blendControlReg = lcr.CB_BLENDN_CONTROL[i];

       		auto rgbBlendOp = GetMtlBlendOp(blendControlReg.get_COLOR_COMB_FCN());
       		auto srcFactor = blendControlReg.get_COLOR_SRCBLEND();
       		auto dstFactor = blendControlReg.get_COLOR_DSTBLEND();
       		// BOTH_* lives in the source field and replaces both factors.
       		// Mapping it to zero makes a translucent overlay draw black.
       		if (srcFactor == Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_BOTH_SRC_ALPHA)
       		{
       			srcFactor = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_SRC_ALPHA;
       			dstFactor = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_ONE_MINUS_SRC_ALPHA;
       		}
       		else if (srcFactor == Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_BOTH_INV_SRC_ALPHA)
       		{
       			srcFactor = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_ONE_MINUS_SRC_ALPHA;
       			dstFactor = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_SRC_ALPHA;
       		}
       		auto srcRgbBlendFactor = GetMtlBlendFactor(srcFactor);
       		auto dstRgbBlendFactor = GetMtlBlendFactor(dstFactor);

       		colorAttachment->setRgbBlendOperation(rgbBlendOp);
       		colorAttachment->setSourceRGBBlendFactor(srcRgbBlendFactor);
       		colorAttachment->setDestinationRGBBlendFactor(dstRgbBlendFactor);
       		if (blendControlReg.get_SEPARATE_ALPHA_BLEND())
       		{
       			colorAttachment->setAlphaBlendOperation(GetMtlBlendOp(blendControlReg.get_ALPHA_COMB_FCN()));
       			auto srcAlphaFactor = blendControlReg.get_ALPHA_SRCBLEND();
       			auto dstAlphaFactor = blendControlReg.get_ALPHA_DSTBLEND();
       			if (srcAlphaFactor == Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_BOTH_SRC_ALPHA)
       			{
       				srcAlphaFactor = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_SRC_ALPHA;
       				dstAlphaFactor = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_ONE_MINUS_SRC_ALPHA;
       			}
       			else if (srcAlphaFactor == Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_BOTH_INV_SRC_ALPHA)
       			{
       				srcAlphaFactor = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_ONE_MINUS_SRC_ALPHA;
       				dstAlphaFactor = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_SRC_ALPHA;
       			}
      		    colorAttachment->setSourceAlphaBlendFactor(GetMtlBlendFactor(srcAlphaFactor));
      		    colorAttachment->setDestinationAlphaBlendFactor(GetMtlBlendFactor(dstAlphaFactor));
       		}
       		else
       		{
           		colorAttachment->setAlphaBlendOperation(rgbBlendOp);
           		colorAttachment->setSourceAlphaBlendFactor(srcRgbBlendFactor);
           		colorAttachment->setDestinationAlphaBlendFactor(dstRgbBlendFactor);
       		}
		}
	}

	// Depth stencil attachment
	if (lastUsedAttachmentsInfo.depthFormat != Latte::E_GX2SURFFMT::INVALID_FORMAT)
	{
	    MTL::PixelFormat pixelFormat = GetMtlPixelFormat(lastUsedAttachmentsInfo.depthFormat, true);
        desc->setDepthAttachmentPixelFormat(pixelFormat);
        if (lastUsedAttachmentsInfo.hasStencil || GetMtlPixelFormatInfo(lastUsedAttachmentsInfo.depthFormat, true).hasStencil)
            desc->setStencilAttachmentPixelFormat(pixelFormat);
	}
}

MetalPipelineCompiler::~MetalPipelineCompiler()
{
    if (m_ownedGeometryShader)
    {
        m_ownedGeometryShader->WaitForCompiled();
        delete m_ownedGeometryShader;
    }
    // Publish success or failure, including unsupported pipelines, so callers
    // never race the compiler or wait forever for a failed compilation.
    m_pipelineObj.m_compilationFinished.store(true, std::memory_order_release);
    m_pipelineObj.m_compilationFinished.notify_all();

    /*
    for (auto& pair : m_pipelineCache)
    {
        pair.second->release();
    }
    m_pipelineCache.clear();

    NS::Error* error = nullptr;
    m_binaryArchive->serializeToURL(m_binaryArchiveURL, &error);
    if (error)
    {
        cemuLog_log(LogType::Force, "error serializing binary archive: {}", error->localizedDescription()->utf8String());
        error->release();
    }
    m_binaryArchive->release();

    m_binaryArchiveURL->release();
    */
    if (m_pipelineDescriptor)
        m_pipelineDescriptor->release();
}

void MetalPipelineCompiler::InitFromState(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, const MetalAttachmentsInfo& lastUsedAttachmentsInfo, const MetalAttachmentsInfo& activeAttachmentsInfo, const LatteContextRegister& lcr)
{
    RuntimePerformance::OptionalDurationTimer initTimer(m_diagnosticsEnabled, m_compileTimings.initNs);
    m_usesGeometryShader = UseGeometryShader(lcr, geometryShader != nullptr);
    const bool fallback = m_usesGeometryShader && !m_mtlr->SupportsMeshShaders();
    if (fallback) {
        const uint32 inputVertices = GetVerticesPerPrimitive(lcr.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE());
        uint32 fields = vertexShader->ringParameterCount;
        if (!geometryShader) {
            fields = 1;
            LatteShaderPSInputTable table; LatteShader_CreatePSInputTable(&table, lcr.GetRawView());
            for (uint32 i = 0; i < 32; ++i) {
                if (!(vertexShader->outputParameterMask & (1u << i))) continue;
                const auto semantic = table.getVertexShaderOutParamSemanticId(lcr.GetRawView(), i);
                if (semantic >= 0 && table.hasPSImportForSemanticId(semantic)) ++fields;
            }
        }
        m_pipelineObj.m_payloadStride = inputVertices * fields * 16;
        if (geometryShader && geometryShader->ringParameterCountFromPrevStage != fields) {
            m_pipelineObj.m_payloadStride = 0;
        }
        m_pipelineObj.m_fallbackOutputTopology = geometryShader ? lcr.GetRawView()[mmVGT_GS_OUT_PRIM_TYPE] : 2;
        m_pipelineObj.m_expandedVertices = geometryShader ? MetalGeometryFallback::ExpandedVertices(m_pipelineObj.m_fallbackOutputTopology) : 6;
    }

    // Rasterization
	m_rasterizationEnabled = lcr.IsRasterizationEnabled();

    // Shaders
    m_vertexShaderMtl = static_cast<RendererShaderMtl*>(vertexShader->shader);
    if (geometryShader)
        m_geometryShaderMtl = static_cast<RendererShaderMtl*>(geometryShader->shader);
    else if (UseRectEmulation(lcr))
    {
        m_ownedGeometryShader = rectsEmulationGS_generate(m_mtlr, vertexShader, lcr);
        m_geometryShaderMtl = m_ownedGeometryShader;
    }
    else
        m_geometryShaderMtl = nullptr;
    m_pixelShaderMtl = static_cast<RendererShaderMtl*>(pixelShader->shader);
    m_pixelShader = pixelShader;

    if (m_usesGeometryShader && !fallback)
        InitFromStateMesh(fetchShader, lastUsedAttachmentsInfo, activeAttachmentsInfo, lcr);
    else
        InitFromStateRender(fetchShader, vertexShader, lastUsedAttachmentsInfo, activeAttachmentsInfo, lcr);
}

bool MetalPipelineCompiler::Compile(bool forceCompile, bool isRenderThread, bool showInOverlay)
{
    // Both gameplay and cache-loading workers are long-lived C++ threads.
    // Drain compiler diagnostics and temporaries after each pipeline job.
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    struct ActiveCompileScope
    {
        ActiveCompileScope() { RuntimePerformance::activePipelineCompiles.fetch_add(1, std::memory_order_relaxed); }
        ~ActiveCompileScope() { RuntimePerformance::activePipelineCompiles.fetch_sub(1, std::memory_order_relaxed); }
    } activeCompileScope;

    const bool fallback = m_usesGeometryShader && !m_mtlr->SupportsMeshShaders();
    if (fallback && (!m_pipelineObj.m_payloadStride || m_pipelineObj.m_fallbackOutputTopology > 2)) {
        return false;
    }

    if (forceCompile)
	{
        RuntimePerformance::OptionalDurationTimer shaderTimer(m_diagnosticsEnabled, m_compileTimings.shaderWaitNs);
		// Compile only stages used by the descriptor. Streamout has no fragment
		// function; waiting for its currently bound pixel shader delays an unrelated
		// draw and can turn a cold animation/effect into a frame-long stall.
		if (m_vertexShaderMtl && !m_vertexShaderMtl->IsCompiled())
			m_vertexShaderMtl->PreponeCompilation(isRenderThread);
		if (m_geometryShaderMtl && !m_geometryShaderMtl->IsCompiled())
			m_geometryShaderMtl->PreponeCompilation(isRenderThread);
		if (m_rasterizationEnabled && m_pixelShaderMtl && !m_pixelShaderMtl->IsCompiled())
			m_pixelShaderMtl->PreponeCompilation(isRenderThread);
	}
	else
	{
	    // fail early if some shader stages are not compiled
		if (m_vertexShaderMtl && !m_vertexShaderMtl->IsCompiled())
			return false;
		if (m_geometryShaderMtl && !m_geometryShaderMtl->IsCompiled())
			return false;
		if (m_rasterizationEnabled && m_pixelShaderMtl && !m_pixelShaderMtl->IsCompiled())
			return false;
	}

	// Compile
    MTL::RenderPipelineState* pipeline = nullptr;
    NS::Error* error = nullptr;
	MetalResourceCrashEvent pipelineEvent{};
	pipelineEvent.type = MetalResourceCrashEventType::PipelineCreateBegin;
	pipelineEvent.ownerPtr = reinterpret_cast<uint64>(&m_pipelineObj);
	pipelineEvent.relatedPtr = reinterpret_cast<uint64>(m_pipelineDescriptor);
	pipelineEvent.flags = (m_usesGeometryShader ? 1u : 0u) | (m_rasterizationEnabled ? 2u : 0u) |
		(isRenderThread ? 4u : 0u);
	MetalResourceCrashDiagnostics_Record(pipelineEvent);

    auto start = std::chrono::high_resolution_clock::now();
    RuntimePerformance::activePipelineCreates.fetch_add(1, std::memory_order_relaxed);
    if (m_usesGeometryShader && !fallback)
    {
        auto desc = static_cast<MTL::MeshRenderPipelineDescriptor*>(m_pipelineDescriptor);

        // Shaders
        desc->setObjectFunction(m_vertexShaderMtl->GetFunction());
        desc->setMeshFunction(m_geometryShaderMtl->GetFunction());
        if (m_rasterizationEnabled)
            desc->setFragmentFunction(m_pixelShaderMtl->GetFunction());

#ifdef CEMU_DEBUG_ASSERT
        desc->setLabel(GetLabel("Mesh render pipeline state", desc));
#endif
        RuntimePerformance::OptionalDurationTimer createTimer(m_diagnosticsEnabled, m_compileTimings.createNs);
       	pipeline = m_mtlr->GetDevice()->newRenderPipelineState(desc, MTL::PipelineOptionNone, nullptr, &error);
    }
    else
    {
        auto desc = static_cast<MTL::RenderPipelineDescriptor*>(m_pipelineDescriptor);

        // Shaders
        desc->setVertexFunction(fallback ? m_geometryShaderMtl->GetFunction() : m_vertexShaderMtl->GetFunction());
        if (m_rasterizationEnabled)
            desc->setFragmentFunction(m_pixelShaderMtl->GetFunction());

#ifdef CEMU_DEBUG_ASSERT
        desc->setLabel(GetLabel("Render pipeline state", desc));
#endif
        RuntimePerformance::OptionalDurationTimer createTimer(m_diagnosticsEnabled, m_compileTimings.createNs);
       	pipeline = m_mtlr->GetDevice()->newRenderPipelineState(desc, MTL::PipelineOptionNone, nullptr, &error);
        if (fallback && pipeline) {
            NS_STACK_SCOPED auto capture = static_cast<MTL::RenderPipelineDescriptor*>(desc->copy());
            capture->setVertexFunction(m_vertexShaderMtl->GetFunction());
            capture->setFragmentFunction(nullptr);
            capture->setRasterizationEnabled(false);
            m_pipelineObj.m_capturePipeline = m_mtlr->GetDevice()->newRenderPipelineState(capture, MTL::PipelineOptionNone, nullptr, &error);
            if (!m_pipelineObj.m_capturePipeline) { pipeline->release(); pipeline = nullptr; }
        }
    }
    RuntimePerformance::activePipelineCreates.fetch_sub(1, std::memory_order_relaxed);
    auto end = std::chrono::high_resolution_clock::now();

    auto creationDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
#if BOOST_OS_IOS
    RuntimePerformance::pipelineCompileNs.fetch_add(creationDuration, std::memory_order_relaxed);
    RuntimePerformance::pipelineCompileCount.fetch_add(1, std::memory_order_relaxed);
#endif

	if (error)
	{
		pipelineEvent.type = MetalResourceCrashEventType::PipelineCreateError;
		pipelineEvent.texturePtr = reinterpret_cast<uint64>(pipeline);
		pipelineEvent.relatedPtr = reinterpret_cast<uint64>(error);
		MetalResourceCrashDiagnostics_Record(pipelineEvent);
       	cemuLog_log(LogType::Force, "error creating render pipeline state: {}", error->localizedDescription()->utf8String());
	}
	else
	{
		pipelineEvent.type = MetalResourceCrashEventType::PipelineCreateSuccess;
		pipelineEvent.texturePtr = reinterpret_cast<uint64>(pipeline);
		pipelineEvent.relatedPtr = static_cast<uint64>(creationDuration);
		MetalResourceCrashDiagnostics_Record(pipelineEvent);
	}

    if (showInOverlay)
	{
		if (isRenderThread)
			g_compiling_pipelines_syncTimeSum += creationDuration;
		else
			g_compiling_pipelines_async++;
		g_compiling_pipelines++;
	}

    m_pipelineObj.m_pipeline = pipeline;

	// A Metal error may leave `pipeline` null. Treat that as a failed compile so
	// the cache never publishes a permanently unusable pipeline object.
	return pipeline != nullptr;
}

void MetalPipelineCompiler::InitFromStateRender(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const MetalAttachmentsInfo& lastUsedAttachmentsInfo, const MetalAttachmentsInfo& activeAttachmentsInfo, const LatteContextRegister& lcr)
{
	// Render pipeline state
	MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();

    // Vertex descriptor
    if (!m_usesGeometryShader && !fetchShader->mtlFetchVertexManually)
    {
    	NS_STACK_SCOPED MTL::VertexDescriptor* vertexDescriptor = MTL::VertexDescriptor::alloc()->init();
    	for (auto& bufferGroup : fetchShader->bufferGroups)
    	{
    		std::optional<LatteConst::VertexFetchType2> fetchType;

    		uint32 minBufferStride = 0;
    		for (sint32 j = 0; j < bufferGroup.attribCount; ++j)
    		{
    			auto& attr = bufferGroup.attrib[j];

    			uint32 semanticId = vertexShader->resourceMapping.attributeMapping[attr.semanticId];
    			if (semanticId == (uint32)-1)
    				continue; // attribute not used?

    			auto attribute = vertexDescriptor->attributes()->object(semanticId);
    			attribute->setOffset(attr.offset);
    			attribute->setBufferIndex(GET_MTL_VERTEX_BUFFER_INDEX(attr.attributeBufferIndex));
    			attribute->setFormat(GetMtlVertexFormat(attr.format));

    			minBufferStride = std::max(minBufferStride, attr.offset + GetMtlVertexFormatSize(attr.format));

    			if (fetchType.has_value())
    				cemu_assert_debug(fetchType == attr.fetchType);
    			else
    				fetchType = attr.fetchType;

    			if (attr.fetchType == LatteConst::INSTANCE_DATA)
    			{
    				cemu_assert_debug(attr.aluDivisor == 1); // other divisor not yet supported
    			}
    		}

    		uint32 bufferIndex = bufferGroup.attributeBufferIndex;
    		uint32 bufferBaseRegisterIndex = mmSQ_VTX_ATTRIBUTE_BLOCK_START + bufferIndex * 7;
    		uint32 bufferStride = (lcr.GetRawView()[bufferBaseRegisterIndex + 2] >> 11) & 0xFFFF;

    		auto layout = vertexDescriptor->layouts()->object(GET_MTL_VERTEX_BUFFER_INDEX(bufferIndex));
    		if (bufferStride == 0)
    		{
    		    // Buffer stride cannot be zero, let's use the minimum stride
    			bufferStride = minBufferStride;

    			// Additionally, constant vertex function must be used
    			layout->setStepFunction(MTL::VertexStepFunctionConstant);
    			layout->setStepRate(0);
    		}
    		else
    		{
      		if (!fetchType.has_value() || fetchType == LatteConst::VertexFetchType2::VERTEX_DATA)
     			layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
      		else if (fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA)
     			layout->setStepFunction(MTL::VertexStepFunctionPerInstance);
      		else
      		{
      		    cemuLog_log(LogType::Force, "unimplemented vertex fetch type {}", (uint32)fetchType.value());
     			cemu_assert(false);
      		}
    		}
		bufferStride = static_cast<uint32>(Align(bufferStride, 4));
    		layout->setStride(bufferStride);
    	}

    	desc->setVertexDescriptor(vertexDescriptor);
    }

	SetFragmentState(desc, m_pixelShader, lastUsedAttachmentsInfo, activeAttachmentsInfo, m_rasterizationEnabled, lcr);

	m_pipelineDescriptor = desc;
}

void MetalPipelineCompiler::InitFromStateMesh(const LatteFetchShader* fetchShader, const MetalAttachmentsInfo& lastUsedAttachmentsInfo, const MetalAttachmentsInfo& activeAttachmentsInfo, const LatteContextRegister& lcr)
{
	// Render pipeline state
	MTL::MeshRenderPipelineDescriptor* desc = MTL::MeshRenderPipelineDescriptor::alloc()->init();

	SetFragmentState(desc, m_pixelShader, lastUsedAttachmentsInfo, activeAttachmentsInfo, m_rasterizationEnabled, lcr);

	m_pipelineDescriptor = desc;
}
