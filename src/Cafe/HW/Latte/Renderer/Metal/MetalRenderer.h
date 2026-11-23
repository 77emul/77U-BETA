#pragma once

#include <unordered_set>

#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include "Cafe/HW/Latte/Renderer/Metal/MetalLayerHandle.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalPerformanceMonitor.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalOutputShaderCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalAttachmentsInfo.h"
#include "Cafe/HW/Latte/Renderer/Metal/FrameGenerationSchedule.h"

#include <memory>

class MetalFrameGeneration;

// Thread-safe one-shot request, consumed by SwapBuffers on the render thread.
void MetalRenderer_RequestFrameCapture();

enum MetalGeneralShaderType
{
    METAL_GENERAL_SHADER_TYPE_VERTEX,
    METAL_GENERAL_SHADER_TYPE_GEOMETRY,
    METAL_GENERAL_SHADER_TYPE_FRAGMENT,

    METAL_GENERAL_SHADER_TYPE_TOTAL
};

inline MetalGeneralShaderType GetMtlGeneralShaderType(LatteConst::ShaderType shaderType)
{
    switch (shaderType)
    {
    case LatteConst::ShaderType::Vertex:
        return METAL_GENERAL_SHADER_TYPE_VERTEX;
    case LatteConst::ShaderType::Geometry:
        return METAL_GENERAL_SHADER_TYPE_GEOMETRY;
    case LatteConst::ShaderType::Pixel:
        return METAL_GENERAL_SHADER_TYPE_FRAGMENT;
    default:
        return METAL_GENERAL_SHADER_TYPE_TOTAL;
    }
}

enum MetalShaderType
{
    METAL_SHADER_TYPE_VERTEX,
    METAL_SHADER_TYPE_OBJECT,
    METAL_SHADER_TYPE_MESH,
    METAL_SHADER_TYPE_FRAGMENT,

    METAL_SHADER_TYPE_TOTAL
};

// Borrowed staging reservations are reusable only within the same command buffer.
struct MetalSupportBufferBinding
{
    uint64 commandBufferSerial = 0;
    MTL::Buffer* buffer = nullptr;
    size_t offset = 0;
    size_t size = 0;
    uint8 data[256]{};
};

inline MetalShaderType GetMtlShaderType(LatteConst::ShaderType shaderType, bool usesGeometryShader)
{
    switch (shaderType)
    {
    case LatteConst::ShaderType::Vertex:
        if (usesGeometryShader)
            return METAL_SHADER_TYPE_OBJECT;
        else
            return METAL_SHADER_TYPE_VERTEX;
    case LatteConst::ShaderType::Geometry:
        return METAL_SHADER_TYPE_MESH;
    case LatteConst::ShaderType::Pixel:
        return METAL_SHADER_TYPE_FRAGMENT;
    default:
        return METAL_SHADER_TYPE_TOTAL;
    }
}

struct MetalEncoderState
{
    MTL::RenderPipelineState* m_renderPipelineState = nullptr;
    MTL::DepthStencilState* m_depthStencilState = nullptr;
    MTL::CullMode m_cullMode = MTL::CullModeNone;
    MTL::Winding m_frontFaceWinding = MTL::WindingClockwise;
    MTL::Viewport m_viewport;
    MTL::ScissorRect m_scissor;
    uint32 m_stencilRefFront = 0;
    uint32 m_stencilRefBack = 0;
    uint32 m_blendColor[4] = {0};
    uint32 m_depthBias = 0;
   	uint32 m_depthSlope = 0;
   	uint32 m_depthClamp = 0;
    bool m_depthClipEnable = true;
    struct {
        MTL::Buffer* m_buffer;
        size_t m_offset;
    } m_buffers[METAL_SHADER_TYPE_TOTAL][MAX_MTL_BUFFERS];
    MTL::Texture* m_textures[METAL_SHADER_TYPE_TOTAL][MAX_MTL_TEXTURES];
    MTL::SamplerState* m_samplers[METAL_SHADER_TYPE_TOTAL][MAX_MTL_SAMPLERS];
};

struct MetalStreamoutState
{
	struct
	{
		bool enabled;
		uint32 ringBufferOffset;
	} buffers[LATTE_NUM_STREAMOUT_BUFFER];
	sint32 verticesPerInstance;
};

struct MetalActiveFBOState
{
    class CachedFBOMtl* m_fbo = nullptr;
    MetalAttachmentsInfo m_attachmentsInfo;
    bool m_hasReadbackAttachment = false;
};

struct MetalRetainedAttachmentDiagnostic
{
	class LatteTexture* baseTexture = nullptr;
	uint32 guestBaseAddress = 0;
	uint32 width = 0;
	uint32 height = 0;
	uint32 pitch = 0;
	uint32 format = 0;
	uint32 tileMode = 0;
	uint32 mip = 0;
	uint32 slice = 0;
	uint8 attachmentType = 0;
	uint8 attachmentIndex = 0;
};

struct MetalAttachmentWriteDiagnostic
{
	class LatteTexture* baseTexture = nullptr;
	uint64 encoderGeneration = 0;
	uint32 lastWriteDrawIndex = 0;
	uint64 lastWriteGeneration = 0;
};

struct MetalState
{
    MetalEncoderState m_encoderState{};

    bool m_usesSRGB = false;

    bool m_skipDrawSequence = false;
    bool m_sequenceRasterized = false;
    bool m_isFirstDrawInRenderPass = true;

    MetalActiveFBOState m_activeFBO;
    // If the FBO changes, but it's the same FBO as the last one with some omitted attachments, this FBO doesn't change
    MetalActiveFBOState m_lastUsedFBO;
    bool m_fboChanged = false;

    size_t m_vertexBufferOffsets[MAX_MTL_VERTEX_BUFFERS];
    uint32 m_vertexBufferSizes[MAX_MTL_VERTEX_BUFFERS] = {};
    class LatteTextureViewMtl* m_textures[LATTE_NUM_MAX_TEX_UNITS * 3] = {nullptr};
    size_t m_uniformBufferOffsets[METAL_GENERAL_SHADER_TYPE_TOTAL][MAX_MTL_BUFFERS];
    uint32 m_uniformBufferSizes[METAL_GENERAL_SHADER_TYPE_TOTAL][MAX_MTL_BUFFERS] = {};

    MTL::Viewport m_viewport;
    MTL::ScissorRect m_scissor;

    MetalStreamoutState m_streamoutState;
};

struct MetalCommandBuffer
{
    MTL::CommandBuffer* m_commandBuffer = nullptr;
    bool m_commited = false;
    bool m_frameEndMarked = false;
    size_t m_textureUploadBytes = 0;
    bool m_readbackPending = false;
};

// Retained, completed source from the existing DRC backbuffer-copy path.
// The caller owns one retain on both Metal objects when valid is true.
struct MetalDRCPreviewFrame
{
	bool valid = false;
	MTL::Texture* texture = nullptr;
	MTL::CommandQueue* commandQueue = nullptr;
	uint64 generation = 0;
	uint32 width = 0;
	uint32 height = 0;
};

enum class MetalResourceCrashEventType : uint32
{
	TextureCreate = 1,
	TextureDestroy,
	TextureCacheEvict,
	TextureReplace,
	PipelineCreateBegin,
	PipelineCreateSuccess,
	PipelineCreateError,
	CommandBufferCommit,
	CommandBufferComplete,
	CommandBufferError,
};

// Fixed scalar snapshot used by the bounded resource-transition crash history.
// Pointer values are diagnostic identities only and are never dereferenced by readers.
struct MetalResourceCrashEvent
{
	uint64 sequence = 0;
	uint64 hostTimeNs = 0;
	MetalResourceCrashEventType type = MetalResourceCrashEventType::TextureCreate;
	uint64 texturePtr = 0;
	uint64 ownerPtr = 0;
	uint64 ownerObjectId = 0;
	uint64 commandBufferPtr = 0;
	uint64 relatedPtr = 0;
	uint32 guestAddress = 0;
	uint32 width = 0;
	uint32 height = 0;
	uint32 pitch = 0;
	uint32 pixelFormat = 0;
	uint32 tileMode = 0;
	uint32 commandBufferStatus = 0;
	uint32 flags = 0;
};

void MetalResourceCrashDiagnostics_Record(MetalResourceCrashEvent event);
bool MetalResourceCrashDiagnostics_ShouldRecordTextureEvent(MetalResourceCrashEventType type);
uint32 MetalResourceCrashDiagnostics_GetEventCount();
bool MetalResourceCrashDiagnostics_GetEvent(uint32 index, MetalResourceCrashEvent& event);
void MetalResourceCrashDiagnostics_DumpToCrashLog();

using MetalDRCPreviewPublishCallback = void (*)();

enum class MetalEncoderType
{
    None,
    Render,
    Compute,
    Blit,
};

class MetalRenderer : public Renderer
{
public:
    static constexpr uint32 OCCLUSION_QUERY_POOL_SIZE = 1024;
    static constexpr uint32 TEXTURE_READBACK_SIZE = 32 * 1024 * 1024; // 32 MB

    struct DeviceInfo
    {
        std::string name;
        uint64 uuid;
    };

    static std::vector<DeviceInfo> GetDevices();

    MetalRenderer();
	~MetalRenderer() override;

	static MetalRenderer* GetInstance() {
	    return static_cast<MetalRenderer*>(g_renderer.get());
	}

	// Helper functions
	MTL::Device* GetDevice() const {
        return m_device;
    }

	CA::MetalLayer* GetLayerForDiagnostics(bool mainWindow) const {
		return mainWindow ? m_mainLayer.GetLayer() : m_padLayer.GetLayer();
	}

	void InitializeLayer(const Vector2i& size, bool mainWindow);
	void ShutdownLayer(bool mainWindow);
	void ResizeLayer(const Vector2i& size, bool mainWindow);

	void Initialize() override;
	void Shutdown() override;
	bool IsPadWindowActive() override;

	bool GetVRAMInfo(int& usageInMB, int& totalInMB) const override;

	void ClearColorbuffer(bool padView) override;
	void ClearColorbuffer(bool padView, float r, float g, float b, float a);
	void DrawEmptyFrame(bool mainWindow) override;
	void SwapBuffers(bool swapTV, bool swapDRC) override;

	void HandleScreenshotRequest(LatteTextureView* texView, bool padView) override;

	void DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter,
									sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight,
									bool padView, bool clearBackground) override;
	MetalDRCPreviewFrame AcquireCompletedDRCPreviewFrame();
	bool BeginFrame(bool mainWindow) override;

	// flush control
	void Flush(bool waitIdle = false) override;		// called when explicit flush is required (e.g. by imgui)
	void NotifyLatteCommandProcessorIdle() override; // called when command processor has no more commands available or when stalled

	// imgui
	bool ImguiBegin(bool mainWindow) override;
	void ImguiEnd() override;
	ImTextureID GenerateTexture(const std::vector<uint8>& data, const Vector2i& size) override;
	void DeleteTexture(ImTextureID id) override;
	void DeleteFontTextures() override;

	bool UseTFViaSSBO() const override { return true; }
	void AppendOverlayDebugInfo() override;

	// rendertarget
	void renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ = false) override;
	void renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight) override;

	LatteCachedFBO* rendertarget_createCachedFBO(uint64 key) override;
	void rendertarget_deleteCachedFBO(LatteCachedFBO* fbo) override;
	void rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo) override;

	// texture functions
	void* texture_acquireTextureUploadBuffer(uint32 size) override;
	void* texture_acquireTextureUploadBufferForTexture(uint32 size, LatteTexture* texture) override;
	void texture_releaseTextureUploadBuffer(uint8* mem) override;

	TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height) override;

	void texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex) override;
	void texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize) override;
	void texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a) override;
	void texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue) override;
	void DropPendingTextureClears(LatteTexture* texture);
	void FlushPendingTextureClears(LatteTexture* texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount);

	LatteTexture* texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth) override;

	void texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit) override;
	void texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth) override;

	LatteTextureReadbackInfo* texture_createReadback(LatteTextureView* textureView) override;

	// surface copy
	void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height) override;

	// buffer cache
	void bufferCache_init(const sint32 bufferSize) override;
    bool bufferCache_ensureCapacity(uint32 requiredEnd) override;
	void bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset) override;
	void bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size) override;

	void buffer_bindVertexBuffer(uint32 bufferIndex, uint32 offset, uint32 size) override;
	void buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size) override;

	// shader
	RendererShader* shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool compileAsync, bool isGfxPackSource) override;

	// streamout
	void streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize) override;
	void streamout_begin() override;
	void streamout_rendererFinishDrawcall() override;

    struct PipelineDiagnosticContext
    {
        Vector2i size{};
        bool queryActive = false, readbackAttachment = false;
    };
    PipelineDiagnosticContext GetPipelineDiagnosticContext() const;

	// core drawing logic
	void draw_beginSequence() override;
	void draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext) override;
	void draw_endSequence() override;

	void draw_updateVertexBuffersDirectAccess();
	void draw_updateUniformBuffersDirectAccess(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset);

	void draw_handleSpecialState5();

	// index
	IndexAllocation indexData_reserveIndexMemory(uint32 size) override;
	void indexData_releaseIndexMemory(IndexAllocation& allocation) override;
	void indexData_uploadIndexMemory(IndexAllocation& allocation) override;

	// occlusion queries
	LatteQueryObject* occlusionQuery_create() override;
	void occlusionQuery_destroy(LatteQueryObject* queryObj) override;
	void occlusionQuery_flush() override;
	void occlusionQuery_updateState() override;

	// Helpers
	MetalPerformanceMonitor& GetPerformanceMonitor() { return m_performanceMonitor; }

	void SetShouldMaximizeConcurrentCompilation(bool shouldMaximizeConcurrentCompilation)
	{
#if TARGET_OS_IPHONE || TARGET_OS_IOS
		// shouldMaximizeConcurrentCompilation is macOS-only (API_UNAVAILABLE(ios))
		(void)shouldMaximizeConcurrentCompilation;
#else
	    if (m_supportsMetal3)
	        m_device->setShouldMaximizeConcurrentCompilation(shouldMaximizeConcurrentCompilation);
#endif
	}

    void MarkReadbackPendingSubmission()
    {
        m_currentCommandBuffer.m_readbackPending = true;
    }

	bool IsCommandBufferActive() const
	{
        return (m_currentCommandBuffer.m_commandBuffer && !m_currentCommandBuffer.m_commited);
    }

	MTL::CommandBuffer* GetCurrentCommandBuffer() const
    {
        cemu_assert_debug(m_currentCommandBuffer.m_commandBuffer);

        return m_currentCommandBuffer.m_commandBuffer;
    }

    MTL::CommandBuffer* GetAndRetainCurrentCommandBufferIfNotCompleted() const
    {
        // The command buffer has been commited and has finished execution
        if (m_currentCommandBuffer.m_commited && m_executingCommandBuffers.size() == 0)
            return nullptr;

        return GetCurrentCommandBuffer()->retain();
    }

    void RequestSoonCommit()
    {
        m_commitTreshold = m_recordedDrawcalls + 8;
    }

    MTL::CommandEncoder* GetCommandEncoder()
    {
        return m_commandEncoder;
    }

    MetalEncoderType GetEncoderType()
    {
        return m_encoderType;
    }

    void ResetEncoderState()
    {
        m_state.m_encoderState = {};
    }

    MetalEncoderState& GetEncoderState()
    {
        return m_state.m_encoderState;
    }

    void SetBuffer(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Buffer* buffer, size_t offset, uint32 index);
    void SetTexture(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Texture* texture, uint32 index);
    void SetSamplerState(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::SamplerState* samplerState, uint32 index);

	MTL::CommandBuffer* GetCommandBuffer();
	MTL::RenderCommandEncoder* GetTemporaryRenderCommandEncoder(MTL::RenderPassDescriptor* renderPassDescriptor);
	MTL::RenderCommandEncoder* GetRenderCommandEncoder(bool forceRecreate = false);
    bool CanReuseRenderPassAttachments();
    MTL::ComputeCommandEncoder* GetComputeCommandEncoder();
    MTL::BlitCommandEncoder* GetBlitCommandEncoder();
    void EndEncoding();
    void FinishTextureUpload(size_t uploadedBytes);
    void CommitCommandBuffer(bool endFrame = false);
    void ProcessFinishedCommandBuffers();
    void FinishGpuFrameTiming();

    bool AcquireDrawable(bool mainWindow);

    bool CheckIfRenderPassNeedsFlush(LatteDecompilerShader* shader, bool forceAccurateBarrier = false);
    void BindStageResources(MTL::RenderCommandEncoder*& renderCommandEncoder, LatteDecompilerShader* shader, bool usesGeometryShader);
    void BindSupportBuffer(MTL::RenderCommandEncoder* encoder, MetalShaderType stage, uint32 binding, const void* data, size_t size);

    void ClearColorTextureInternal(MTL::Texture* mtlTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a);

    void CopyBufferToBuffer(MTL::Buffer* src, uint32 srcOffset, MTL::Buffer* dst, uint32 dstOffset, uint32 size, MTL::RenderStages after, MTL::RenderStages before);

    // Getters
    bool GetPositionInvariance() const
    {
        return m_positionInvariance;
    }

    bool IsAppleGPU() const
    {
        return m_isAppleGPU;
    }

    bool UsesPhoneLosslessTextures() const
    {
        return m_submitBeforeDrawableWait && m_supportsLosslessTextureCompression;
    }

    bool SupportsFramebufferFetch() const override
    {
        return m_supportsFramebufferFetch;
    }

    bool HasActiveRenderEncoder() const override
    {
        return m_commandEncoder != nullptr && m_encoderType == MetalEncoderType::Render;
    }

    bool ShouldSubmitReadbacksEarly() const override { return m_submitReadbacksEarly; }
    bool RequiresAccurateBarriers() const override;
    bool RequiresAccurateQueryResults() const override;

    bool HasUnifiedMemory() const
    {
        return m_hasUnifiedMemory;
    }

    bool SupportsMetal3() const
    {
        return m_supportsMetal3;
    }

    bool SupportsQueryTextureLOD() const {
        return m_device->supportsQueryTextureLOD();
    }
    bool SupportsFloat32Filtering() const {
        return m_supportsFloat32Filtering;
    }
    bool SupportsSamplerBorderColor() const { return m_appleFamily != 6; }
    bool UsesMeshDrawBatching() const {
        return m_supportsMeshShaders && (m_appleFamily == 7 || m_appleFamily == 8);
    }
    MTL::RenderStages SupportedStages(MTL::RenderStages stages) const {
        return m_supportsMeshShaders ? stages : stages & (MTL::RenderStageVertex | MTL::RenderStageFragment);
    }
    bool SupportsMeshShaders() const
    {
        return m_supportsMeshShaders;
    }

    //MTL::StorageMode GetOptimalTextureStorageMode() const
    //{
    //    return (m_isAppleGPU ? MTL::StorageModeShared : MTL::StorageModePrivate);
    //}

    MTL::ResourceOptions GetOptimalBufferStorageMode() const
    {
        return (m_hasUnifiedMemory ? MTL::ResourceStorageModeShared : MTL::ResourceStorageModeManaged);
    }

    MTL::Texture* GetNullTexture2D() const
    {
        return m_nullTexture2D;
    }

    MTL::SamplerState* GetNearestSampler() const
    {
        return m_nearestSampler;
    }

    MTL::SamplerState* GetLinearSampler() const
    {
        return m_linearSampler;
    }

    MTL::Buffer* GetTextureReadbackBuffer()
    {
        if (!m_readbackBuffer)
        {
            m_readbackBuffer = m_device->newBuffer(TEXTURE_READBACK_SIZE, MTL::ResourceStorageModeShared);
#ifdef CEMU_DEBUG_ASSERT
            m_readbackBuffer->setLabel(GetLabel("Texture readback buffer", m_readbackBuffer));
#endif
        }

        return m_readbackBuffer;
    }

    MTL::Buffer* GetXfbRingBuffer()
    {
        if (!m_xfbRingBuffer)
        {
            // HACK: using just LatteStreamout_GetRingBufferSize will cause page faults
            m_xfbRingBuffer = m_device->newBuffer(LatteStreamout_GetRingBufferSize() * 4, MTL::ResourceStorageModePrivate);
#ifdef CEMU_DEBUG_ASSERT
            m_xfbRingBuffer->setLabel(GetLabel("Transform feedback buffer", m_xfbRingBuffer));
#endif
        }

        return m_xfbRingBuffer;
    }

    MTL::Buffer* GetOcclusionQueryResultBuffer() const
    {
        return m_occlusionQuery.m_resultBuffer;
    }

    uint64* GetOcclusionQueryResultsPtr()
    {
        return m_occlusionQuery.m_resultsPtr;
    }

    uint32 GetOcclusionQueryIndex()
    {
        return m_occlusionQuery.m_currentIndex;
    }

    void BeginOcclusionQuery()
    {
        m_occlusionQuery.m_active = true;
    }

    void EndOcclusionQuery()
    {
        m_occlusionQuery.m_active = false;

        // Visibility mode is encoder state. Leaving it enabled counts later
        // draws into a query slot even after the guest query has ended.
        if (m_encoderType == MetalEncoderType::Render && m_commandEncoder)
            static_cast<MTL::RenderCommandEncoder*>(m_commandEncoder)->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0);

        // Release the old command buffer
        if (m_occlusionQuery.m_lastCommandBuffer)
            m_occlusionQuery.m_lastCommandBuffer->release();

        // Get and retain the current command buffer
        m_occlusionQuery.m_lastCommandBuffer = GetAndRetainCurrentCommandBufferIfNotCompleted();
    }

    // GPU capture
    void CaptureFrame()
    {
        m_captureFrame = true;
    }

private:
	void BindBufferCache(MTL::RenderCommandEncoder*& encoder, MetalShaderType shaderType, uint32 cacheOffset, uint32 size, uint32 binding);
	bool CacheBindNeedsBlit(uint32 cacheOffset, uint32 size) const;
	void FlushCacheBindsBeforeRenderPass(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, bool usesGeometryShader);
	MTL::RenderCommandEncoder* LiveRenderEncoder(MTL::RenderCommandEncoder* encoder);

	MetalLayerHandle m_mainLayer;
	MetalLayerHandle m_padLayer;

	MetalPerformanceMonitor m_performanceMonitor;

	// Options
	bool m_positionInvariance;

	// Metal objects
	MTL::Device* m_device = nullptr;
	MTL::CommandQueue* m_commandQueue;

	// Feature support
	bool m_isAppleGPU;
	bool m_supportsFramebufferFetch;
	bool m_hasUnifiedMemory;
	bool m_supportsMetal3;
	bool m_supportsMeshShaders;
	uint32 m_recommendedMaxVRAMUsage;
	MetalPixelFormatSupport m_pixelFormatSupport;

	// Managers and caches
	class MetalMemoryManager* m_memoryManager;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
	uint32 m_memoryPressurePollCounter{0};
#endif
	class MetalOutputShaderCache* m_outputShaderCache;
	class MetalPipelineCache* m_pipelineCache;
	class MetalDepthStencilCache* m_depthStencilCache;
	uint32 m_cachedDepthControlRaw{0xFFFFFFFF};
	uint32 m_cachedStencilRefMaskRaw{0xFFFFFFFF};
	uint32 m_cachedStencilRefMaskBfRaw{0xFFFFFFFF};
	bool m_cachedHasDepth{false};
	bool m_cachedHasStencil{false};
	MTL::DepthStencilState* m_cachedDepthStencilState{nullptr};
	class MetalSamplerCache* m_samplerCache;

	// Pipelines
	MTL::RenderPipelineDescriptor* m_copyDepthToColorDesc;
	std::map<MTL::PixelFormat, MTL::RenderPipelineState*> m_copyDepthToColorPipelines;
	MTL::RenderPipelineDescriptor* m_copyColorToDepthDesc;
	std::map<MTL::PixelFormat, MTL::RenderPipelineState*> m_copyColorToDepthPipelines;
	MTL::DepthStencilState* m_copyColorToDepthState;

	// Frame Generation
	std::unique_ptr<MetalFrameGeneration> m_frameGeneration;
	MTL::Texture* m_frameGenTarget = nullptr;
	bool m_frameGenRendered = false;
	FrameGenerationMode m_frameGenMode;
	int ActiveFrameGenerationTarget();
	void EnsureFrameGenTextures(uint32 width, uint32 height, MTL::PixelFormat pixelFormat, bool initialize = true);

	// Void vertex pipelines
	class MetalVoidVertexPipeline* m_copyBufferToBufferPipeline;
	// BC->ETC2/EAC on GPUs without BC on 4/6 GiB devices (see MetalBCTranscodeShaderSource.h).
	MTL::ComputePipelineState* m_bcTranscodePipeline = nullptr;
    MTL::Library* m_bcTranscodeLibrary = nullptr;
    std::array<MTL::ComputePipelineState*, 7> m_bcFormatPipelines{};
    std::array<bool, 7> m_bcFormatPipelineAttempted{};
    MTL::ComputePipelineState* GetBCTranscodePipeline(uint32 format);
	// Diagnostic per-pass GPU timing (one frame every 5 s with diagnostic logging).
	uint32_t m_appleFamily = 0; // highest supported MTLGPUFamilyApple generation, 0 if not Apple
    bool m_optimizeZeldaIndexHeap = false;
    bool m_submitBeforeDrawableWait = false;
    bool m_submitReadbacksEarly = false;
    bool m_supportsLosslessTextureCompression = false;
    bool m_supportsFloat32Filtering = false;

	// Synchronization resources
	MTL::Event* m_event;
	uint64_t m_eventValue = 0;

	// Resources
	MTL::SamplerState* m_nearestSampler;
	MTL::SamplerState* m_linearSampler;

	// Null resources
	MTL::Texture* m_nullTexture1D;
	MTL::Texture* m_nullTexture2D;

	// Texture readback
	MTL::Buffer* m_readbackBuffer = nullptr;
	uint32 m_readbackBufferWriteOffset = 0;

	// Transform feedback
	MTL::Buffer* m_xfbRingBuffer = nullptr;

	// Occlusion queries
	struct
	{
    	MTL::Buffer* m_resultBuffer;
    	uint64* m_resultsPtr;
    	uint32 m_currentIndex = 0;
        bool m_active = false;
        MTL::CommandBuffer* m_lastCommandBuffer = nullptr;
	} m_occlusionQuery;

	// Active objects
	MetalCommandBuffer m_currentCommandBuffer{};
	std::vector<MTL::CommandBuffer*> m_executingCommandBuffers;
	MetalEncoderType m_encoderType = MetalEncoderType::None;
	MTL::CommandEncoder* m_commandEncoder = nullptr;

	struct DRCPreviewPublication
	{
		MTL::Texture* texture = nullptr;
		MTL::CommandBuffer* commandBuffer = nullptr;
		uint64 generation = 0;
		uint32 width = 0;
		uint32 height = 0;
	};
	static constexpr uint32 DRC_PREVIEW_PUBLICATION_COUNT = 4;
	std::mutex m_drcPreviewPublicationMutex;
	DRCPreviewPublication m_drcPreviewPublications[DRC_PREVIEW_PUBLICATION_COUNT]{};
	uint64 m_drcPreviewPublicationGeneration = 0;
	MTL::Texture* m_lastDRCPreviewSourceTexture = nullptr;

    uint32 m_recordedDrawcalls = 0;
    uint64 m_commandBufferSerial = 0;
    MetalSupportBufferBinding m_supportBufferBindings[METAL_SHADER_TYPE_TOTAL][MAX_MTL_BUFFERS]{};
    MetalSupportBufferBinding m_previousSupportBufferBindings[METAL_SHADER_TYPE_TOTAL][MAX_MTL_BUFFERS]{};
    uint64 m_supportBufferReuseHits = 0;
    uint64 m_supportBufferUploads = 0;
    uint32 m_idleReadbackSubmissions = 0;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    uint32 m_defaultCommitTreshlod = 4096;
#else
    uint32 m_defaultCommitTreshlod = 512;
#endif
    std::unordered_set<MTL::CommandBuffer*> m_frameEndCommandBuffers;
    double m_pendingGpuFrameMs = 0.0;
    double m_pendingGpuRenderMs = 0.0;
    double m_pendingGpuOutputMs = 0.0;
    bool m_pendingGpuFrameValid = true;
    double m_lastCompletedGpuMs = 0.0;
    double m_lastCompletedGpuRenderMs = 0.0;
    double m_lastCompletedGpuOutputMs = 0.0;
    double m_completedGpuWindowMs = 0.0;
    uint32 m_completedGpuWindowFrames = 0;
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    uint32 m_commitTreshold = 4096;
#else
    uint32 m_commitTreshold = 512;
#endif
	// State
	MetalState m_state;
	// A newly acquired CAMetalDrawable has undefined contents. Host UI passes
	// may preserve it only after this renderer has written or cleared it.
	bool m_mainDrawableContentsDefined = false;
	bool m_padDrawableContentsDefined = false;

	// Diagnostic-only snapshot of attachments omitted from the logical FBO while
	// they remain physically attached to a reused render encoder.
	MetalRetainedAttachmentDiagnostic m_retainedAttachmentDiagnostics[9]{};
	uint8 m_retainedAttachmentDiagnosticCount = 0;
	uint64 m_retainedAttachmentActiveFBOIdentity = 0;
	uint64 m_retainedAttachmentLastUsedFBOIdentity = 0;

	// Exact, fixed-size write history for the nine possible attachments in the
	// currently open render encoder. Slots 0-7 are color and slot 8 is depth.
	uint64 m_renderEncoderDiagnosticGeneration = 0;
	MetalAttachmentWriteDiagnostic m_activeAttachmentWriteDiagnostics[9]{};
	bool m_depthWrittenInCurrentEncoder = false;
	uint8 m_colorWrittenMaskInCurrentEncoder = 0;
	// Set when this encoder's load action cleared an attachment. EndEncoding must
	// store that attachment even if no draw wrote it, or the clear never reaches memory.
	uint8 m_loadClearedColorMask = 0;
	bool m_loadClearedDepth = false;
	bool m_loadClearedStencil = false;

	struct PendingTextureClear
	{
		LatteTexture* hostTexture = nullptr;
		uint32 slice = 0;
		uint32 mip = 0;
		bool isDepth = false;
		bool clearDepth = false;
		bool clearStencil = false;
		float r = 0.f;
		float g = 0.f;
		float b = 0.f;
		float a = 0.f;
		float depth = 0.f;
		uint32 stencil = 0;
	};
	std::vector<PendingTextureClear> m_pendingClears;
	bool m_flushingPendingClears = false;
	bool m_committingCommandBuffer = false;
	uint64 m_renderEncoderSerial = 0;

	static bool PendingClearOverlaps(const PendingTextureClear& entry, LatteTexture* texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount);
	static bool ClearTargetsAttachment(const PendingTextureClear& entry, const LatteTextureView* view, bool asDepth);
	static bool ClearMustRunBeforePass(const PendingTextureClear& entry, const class CachedFBOMtl* fbo);
	static bool FoldPendingClear(PendingTextureClear& entry, class CachedFBOMtl* fbo, MTL::RenderPassDescriptor* descriptor, uint8& colorMask, bool& clearedDepth, bool& clearedStencil);

	void QueueOrReplaceColorClear(LatteTexture* texture, uint32 slice, uint32 mip, float r, float g, float b, float a);
	void QueueOrReplaceDepthClear(LatteTexture* texture, uint32 slice, uint32 mip, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue);
	void DropPendingClearsOverlapping(LatteTexture* texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount);
	void FlushAllPendingClears();
	void ExecutePendingClears(const std::vector<PendingTextureClear>& clears);
	void FlushPendingClearsSampledByDraw(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader);
	bool PendingClearAffectsFBO(const class CachedFBOMtl* fbo) const;
	void ResolvePendingClearsForRenderPass();
	void RestoreFoldedLoadActions(MTL::RenderPassDescriptor* descriptor);

	// GPU capture
	bool m_captureFrame = false;
	bool m_capturing = false;

	// Helpers
	MetalLayerHandle& GetLayer(bool mainWindow)
	{
	    return (mainWindow ? m_mainLayer : m_padLayer);
	}

	void SwapBuffer(bool mainWindow);
	bool ShouldDrawPadDisplay() const;

	void EnsureImGuiBackend();
	void ResetRetainedAttachmentDiagnostics();
	void TrackRetainedAttachmentsForReusedEncoder();
	void RecordStaleAttachmentSample(LatteDecompilerShader* shader, uint32 textureUnit, const MetalRetainedAttachmentDiagnostic& attachment);
	void BeginRenderEncoderDiagnosticPass();
	void TrackActiveAttachmentWritesForCurrentEncoder();
	void RecordActiveAttachmentSample(LatteDecompilerShader* shader, uint32 textureUnit, LatteTextureViewMtl* sampledView, MTL::Texture* sampledMetalView);
	void PublishDRCPreviewFrame(MTL::Texture* padDrawableTexture, MTL::Texture* renderTargetTexture);
	void ReleaseDRCPreviewFrames();

	// GPU capture
	void StartCapture();
	void EndCapture();
};

struct MetalDRCRoutingDiagnosticSnapshot
{
	bool valid = false;
	bool padView = false;
	bool selectedPadLayer = false;
	uint64 sourceTexturePtr = 0;
	uint32 sourceWidth = 0;
	uint32 sourceHeight = 0;
	uint64 drawableTexturePtr = 0;
	uint64 copyCounter = 0;
	uint64 tvCopyCount = 0;
	uint64 drcCopyCount = 0;
	uint64 previewPublishCount = 0;
	uint64 previewSourceChangeCount = 0;
	uint64 previewSourceTexturePtr = 0;
	uint64 previewPublishedTexturePtr = 0;
};

MetalDRCRoutingDiagnosticSnapshot MetalRenderer_GetDRCRoutingDiagnosticSnapshot();
void MetalRenderer_SetDRCPreviewPublishCallback(MetalDRCPreviewPublishCallback callback);
