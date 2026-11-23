#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "util/VirtualHeap/VirtualHeap.h"

struct LatteTextureDefinition;
class LatteTexture;
class LatteTextureView;

// High-frequency counters used only by the developer diagnostics panel. A
// release build must not pay for atomic read-modify-write operations in every
// draw, texture bind and render-target transition.
template<typename T>
class LatteDiagnosticCounter
{
public:
	constexpr LatteDiagnosticCounter(T initial = {}) noexcept
#ifndef NDEBUG
		: m_value(initial)
#endif
	{
		(void)initial;
	}

	T load(std::memory_order order = std::memory_order_seq_cst) const noexcept
	{
#ifndef NDEBUG
		return m_value.load(order);
#else
		(void)order;
		return {};
#endif
	}

	void store(T value, std::memory_order order = std::memory_order_seq_cst) noexcept
	{
#ifndef NDEBUG
		m_value.store(value, order);
#else
		(void)value;
		(void)order;
#endif
	}

	T fetch_add(T value, std::memory_order order = std::memory_order_seq_cst) noexcept
	{
#ifndef NDEBUG
		return m_value.fetch_add(value, order);
#else
		(void)value;
		(void)order;
		return {};
#endif
	}

	T operator++() noexcept { return fetch_add(1, std::memory_order_relaxed) + 1; }
	T operator++(int) noexcept { return fetch_add(1, std::memory_order_relaxed); }
	LatteDiagnosticCounter& operator=(T value) noexcept
	{
		store(value, std::memory_order_relaxed);
		return *this;
	}

private:
#ifndef NDEBUG
	std::atomic<T> m_value;
#endif
};

enum LatteUnsafeFBFetchReason : uint32_t
{
	LATTE_UNSAFE_FB_FETCH_WIDTH_MISMATCH = 1u << 0,
	LATTE_UNSAFE_FB_FETCH_HEIGHT_MISMATCH = 1u << 1,
	LATTE_UNSAFE_FB_FETCH_PITCH_MISMATCH = 1u << 2,
	LATTE_UNSAFE_FB_FETCH_MIP_MISMATCH = 1u << 3,
	LATTE_UNSAFE_FB_FETCH_SLICE_MISMATCH = 1u << 4,
	LATTE_UNSAFE_FB_FETCH_SWIZZLE_MISMATCH = 1u << 5,
	LATTE_UNSAFE_FB_FETCH_NON_IDENTITY_COORDINATES = 1u << 6,
	LATTE_UNSAFE_FB_FETCH_LOD_USED = 1u << 7,
	LATTE_UNSAFE_FB_FETCH_OFFSET_USED = 1u << 8,
	LATTE_UNSAFE_FB_FETCH_FILTERED_SAMPLE = 1u << 9,
	LATTE_UNSAFE_FB_FETCH_WRAP_DEPENDENT = 1u << 10,
	LATTE_UNSAFE_FB_FETCH_OTHER_NOT_PROVEN_SAME_PIXEL = 1u << 11,
};

struct gx2GPUSharedArea_t
{
	volatile uint32 flipRequestCountBE; // counts how many buffer swaps were requested
	volatile uint32 flipExecuteCountBE; // counts how many buffer swaps were executed
	volatile uint32 swapInterval; // vsync swap interval (0 means vsync is deactivated)
};

struct LatteGPUState_t
{
	union
	{
		uint32 contextRegister[LATTE_MAX_REGISTER];
		LatteContextRegister contextNew;
	};
	MPTR contextRegisterShadowAddr[LATTE_MAX_REGISTER];
	// context control
	uint32 contextControl0;
	uint32 contextControl1;
	// optional features
	bool allowFramebufferSizeOptimization{false}; // allow using scissor box as size hint to determine non-padded rendertarget size
	// stats
	uint32 frameCounter;
	uint32 flipCounter; // increased by one everytime a vsync + flip happens
	uint32 currentDrawCallTick; // set to current time at the beginning of a drawcall
	uint32 drawCallCounter; // increased after every drawcall
	uint32 textureBindCounter; // increased at the beginning of _updateTextures()
	std::atomic<uint64> flipRequestCount;
	// timer & vsync
	uint64 timer_frequency; // contains frequency of HPC
	uint64 timer_bootUp; // contains the timestamp of when the GPU thread timer was initialized
	uint64 timer_nextVSync;
	// shared
	gx2GPUSharedArea_t* sharedArea; // quick reference to shared area
	MPTR sharedAreaAddr;
	// other
	uint32 gx2InitCalled; // incremented every time GX2Init() is called
	// OpenGL control
	uint32 glVendor; // GLVENDOR_*
	bool isDRCPrimary = false;
	// temporary (replace with proper solution later)
	bool tvBufferUsesSRGB;
	bool drcBufferUsesSRGB;
	float tvGamma = 0.0f;
	float drcGamma = 0.0f;
	// draw state
	bool activeShaderHasError; // if try, at least one currently bound shader stage has an error and cannot be used for drawing
	bool repeatTextureInitialization; // if set during rendertarget or texture initialization, repeat the process (textures likely have been invalidated)
	bool requiresTextureBarrier; // set if glTextureBarrier should be called
	// OSScreen
	struct  
	{
		struct  
		{
			bool isEnabled;
			MPTR physPtr;
			std::atomic<uint32> flipRequestCount;
			std::atomic<uint32> flipExecuteCount;
		}screen[2];
	}osScreen;
	// iPadOS draw pipeline diagnostics telemetry
	struct
	{
		LatteDiagnosticCounter<uint32> gx2DrawIndexedExCount{0};
		LatteDiagnosticCounter<uint32> gx2DrawIndexedExIndexCount{0};
		LatteDiagnosticCounter<uint32> gx2DrawIndexedExGuestPC{0};
		LatteDiagnosticCounter<uint32> pm4DrawIndex2PacketCount{0};
		LatteDiagnosticCounter<uint32> latteCPDrawIndex2Count{0};
		LatteDiagnosticCounter<uint32> latteCPDrawIndex2IndexCount{0};
		LatteDiagnosticCounter<uint32> latteCPDrawIndex2PhysIndices{0};
		std::atomic<bool> latteCPDrawIndex2PointerNull{false};
		LatteDiagnosticCounter<uint32> metalDrawBeginSeqCount{0};
		LatteDiagnosticCounter<uint32> latteMRTUpdateFBOCount{0};
		std::atomic<bool> latteMRTDepthBufferMask{false};
		std::atomic<bool> latteMRTHasValidFBO{false};
		std::atomic<bool> metalSkipDrawSequence{false};
		std::atomic<uint32> depthFormat{0};
		std::atomic<bool> depthTextureViewExists{false};
		LatteDiagnosticCounter<uint32> metalDrawExecuteCount{0};
		LatteDiagnosticCounter<uint32> pm4ClearColorDepthStencilCount{0};
		std::atomic<bool> latteHLEClearDepthFlag{false};
		std::atomic<uint32> gx2WaitForVsyncCount{0};
		std::atomic<uint32> gx2WaitForVsyncReturnedCount{0};
		std::atomic<uint32> gx2WaitForFlipCount{0};
		std::atomic<uint32> gx2WaitForFlipReturnedCount{0};
		std::atomic<uint32> timedVsyncEventCount{0};
		std::atomic<uint32> scanbufferSwapCount{0};
		std::atomic<uint32> vpadReadCount{0};
		// GPU Cache diagnostics
		LatteDiagnosticCounter<uint64_t> texturesCreated{0};
		LatteDiagnosticCounter<uint64_t> cacheHits{0};
		LatteDiagnosticCounter<uint64_t> cacheMisses{0};
		LatteDiagnosticCounter<uint64_t> invalidations{0};
		LatteDiagnosticCounter<uint64_t> cpuWriteRefresh{0};
		LatteDiagnosticCounter<uint64_t> cpuDirtyDetected{0};
		LatteDiagnosticCounter<uint64_t> cpuDirtyMissedCandidates{0};
		LatteDiagnosticCounter<uint64_t> gpuWriteInvalidations{0};
		LatteDiagnosticCounter<uint64_t> guestRangeReuse{0};
		LatteDiagnosticCounter<uint64_t> textureReuploads{0};
		// 1280-Width Bypass Diagnostics
		LatteDiagnosticCounter<uint64_t> skip1280Checks{0};
		LatteDiagnosticCounter<uint64_t> skip1280ActuallyChanged{0};
		LatteDiagnosticCounter<uint64_t> skip1280Unchanged{0};
		std::atomic<uint32_t> unique1280SkippedCount{0};
		struct {
			uint32_t guestAddress;
			uint32_t width;
			uint32_t height;
			uint32_t format;
			uint32_t sizeBytes;
			bool isGPUWritten;
			bool isRenderTarget;
			bool isSampled;
			bool hashChangedWhenForced;
		} unique1280SkippedLog[32];

		// Alias & Dynamic Relation Diagnostics
		LatteDiagnosticCounter<uint64_t> aliasRelations{0};
		LatteDiagnosticCounter<uint64_t> aliasGPUWrites{0};
		LatteDiagnosticCounter<uint64_t> aliasSyncs{0};
		LatteDiagnosticCounter<uint64_t> aliasStaleSampleCandidates{0};
		LatteDiagnosticCounter<uint64_t> aliasDescriptorMismatch{0};
		LatteDiagnosticCounter<uint64_t> aliasSameBaseDiffSize{0};
		LatteDiagnosticCounter<uint64_t> aliasSameBaseDiffPitch{0};
		LatteDiagnosticCounter<uint64_t> aliasSameBaseDiffTile{0};
		std::atomic<uint32_t> uniqueAliasCount{0};
		struct {
			uint32_t srcGuestAddr;
			uint32_t dstGuestAddr;
			uint32_t srcWidth, srcHeight, srcPitch, srcFormat, srcTileMode, srcMip, srcSlice;
			uint64_t srcMTLTexturePtr;
			uint64_t srcGPUWriteGen;
			uint32_t dstWidth, dstHeight, dstPitch, dstFormat, dstTileMode, dstMip, dstSlice;
			uint64_t dstMTLTexturePtr;
			uint64_t dstDynamicSyncGen;
			bool rangesOverlap;
			bool sameBase;
			bool sameMetalTexture;
			bool relationExists;
			bool syncAttempted;
			bool syncSucceeded;
			bool sampledWhileStale;
		} uniqueAliasLog[32];

		// Compatible Relation Diagnostics (Strict RELATION=YES)
		LatteDiagnosticCounter<uint64_t> compatibleRelationGPUWrites{0};
		LatteDiagnosticCounter<uint64_t> compatibleRelationMarkedReload{0};
		LatteDiagnosticCounter<uint64_t> compatibleRelationSamples{0};
		LatteDiagnosticCounter<uint64_t> compatibleRelationSyncs{0};
		LatteDiagnosticCounter<uint64_t> compatibleRelationSampleBeforeSync{0};
		LatteDiagnosticCounter<uint64_t> compatibleRelationDeletedBeforeSync{0};
		LatteDiagnosticCounter<uint64_t> compatibleRelationRecreatedBeforeSync{0};
		std::atomic<uint32_t> compatibleRelationCaptureCount{0};
		struct {
			uint32_t srcGuestAddr;
			uint32_t dstGuestAddr;
			uint32_t srcWidth, srcHeight, srcPitch, srcFormat;
			uint32_t dstWidth, dstHeight, dstPitch, dstFormat;
			uint64_t srcGPUWriteGen;
			uint64_t dstDynamicSyncGen;
			uint64_t srcObjectId;
			uint64_t dstObjectId;
			uint64_t srcMTLTexturePtr;
			uint64_t dstMTLTexturePtr;
			bool reloadFromDynamicTextures;
			bool syncAttempted;
			bool syncSucceeded;
			bool relationStillPresent;
		} compatibleRelationLog[32];

		// Texture Object Lifetime / Address Reuse Diagnostics
		std::atomic<uint32_t> textureRecreationCaptureCount{0};
		struct {
			uint32_t guestAddress;
			uint64_t oldObjectId;
			uint64_t newObjectId;
			uint64_t oldMetalTexturePtr;
			uint64_t newMetalTexturePtr;
			uint64_t oldGPUWriteGen;
			uint64_t newInitialGen;
			bool oldWasGPUUpdated;
			bool hadCompatibleRelations;
			bool pendingReloadLost;
		} textureRecreationLog[32];

		// Dynamic Sync Diagnostic Events (First 16 Reason-Coded Events)
		std::atomic<uint32_t> dynamicSyncEventCount{0};
		struct {
			uint64_t dstObjectId;
			uint32_t dstGuestAddr;
			uint32_t dstWidth, dstHeight, dstPitch, dstFormat;
			uint32_t relationCount;
			bool enteredUpdateCache;
			uint64_t srcObjectId;
			uint32_t srcGuestAddr;
			bool srcIsUpdatedOnGPU;
			uint64_t srcLastDynamicUpdate;
			uint64_t dstLastDynamicUpdate;
			bool copyAttempted;
			bool copySucceeded;
			bool reloadFlagBefore;
			bool reloadFlagAfter;
			uint32_t reasonCode;
		} dynamicSyncEvents[16];

		// Recreation Lifecycle Trace (First 16 RECREATE -> FIRST_USE -> RELATION_REBUILT Events)
		std::atomic<uint32_t> recreationLifecycleCount{0};
		struct {
			uint32_t eventId;
			uint32_t guestAddress;
			uint64_t oldObjectId;
			uint64_t newObjectId;
			uint32_t oldWidth, oldHeight, oldPitch, oldFormat, oldTileMode;
			uint32_t newWidth, newHeight, newPitch, newFormat, newTileMode;
			bool oldWasGPUUpdated;
			bool oldReloadFromDynamicTextures;
			uint32_t oldRelationCount;
			uint64_t oldLastDynamicUpdate;
			bool pendingReloadLost;

			// FIRST_USE info
			bool firstUseRecorded;
			uint32_t firstUseType;
			uint32_t firstUseRelationCount;
			bool firstUseReloadFromDynamicTextures;
			bool firstUseIsUpdatedOnGPU;
			uint64_t firstUseLastDynamicUpdate;
			bool firstUseCpuUploadOccurred;
			bool firstUseDynamicSyncOccurred;
			bool firstUseRelationRecreated;

			// RELATION_REBUILT info
			bool relationRebuiltRecorded;
			uint64_t relationPeerObjectId;
			bool relationRebuiltBeforeFirstUse;
			uint32_t peerWidth, peerHeight, peerPitch, peerFormat;
			bool reloadFlagAfterRelationRebuilt;
		} recreationLifecycleLog[16];

		// Final Texture Bind State Trace (First 32 Unique Bound Objects)
		std::atomic<uint32_t> bindFinalCount{0};
		struct {
			uint32_t entryIndex;
			uint32_t guestAddress;
			uint64_t hostTextureObjectId;
			uint64_t baseTextureObjectId;
			uint64_t textureViewPtr;
			uint64_t mtlTexturePtr;
			uint64_t baseMTLTexturePtr;
			uint32_t width, height, pitch, format, tileMode;
			uint32_t firstMip, numMip, firstSlice, numSlice;
			bool isUpdatedOnGPU;
			bool reloadFromDynamicTextures;
			uint64_t lastDynamicUpdate;
			uint64_t lastGPUWriteGeneration;
			uint64_t lastDynamicSyncGeneration;
			uint32_t swizzle;
			bool isIdentityView;
			bool isRenderTarget;
			bool differsFromBaseFormat;
		} bindFinalLog[32];

		// Metal Pipeline & Geometry Shader Diagnostics
		LatteDiagnosticCounter<uint64_t> metalPipelineRequests{0};
		LatteDiagnosticCounter<uint64_t> metalPipelineRequestsWithGeometryShader{0};
		LatteDiagnosticCounter<uint64_t> metalPipelineCacheHitsWithGeometryShader{0};
		LatteDiagnosticCounter<uint64_t> metalPipelineCacheMissesWithGeometryShader{0};

		// Metal guest color logic-op diagnostics
		LatteDiagnosticCounter<uint64_t> metalLogicOpTotalRequests{0};
		LatteDiagnosticCounter<uint64_t> metalLogicOpEnabledRequests{0};
		LatteDiagnosticCounter<uint64_t> metalLogicOpDisabledRequests{0};
		LatteDiagnosticCounter<uint64_t> metalLogicOpClearRequests{0};
		LatteDiagnosticCounter<uint64_t> metalLogicOpCopyRequests{0};
		LatteDiagnosticCounter<uint64_t> metalLogicOpOrRequests{0};
		LatteDiagnosticCounter<uint64_t> metalLogicOpSetRequests{0};
		LatteDiagnosticCounter<uint64_t> metalLogicOpOtherRequests{0};

		std::atomic<uint32_t> gsPipelineCaptureCount{0};
		struct {
			uint64_t vsBaseHash;
			uint64_t psBaseHash;
			uint64_t gsBaseHash;
			uint32_t colorFormat0;
			uint32_t cbColorControl;
			uint32_t cbTargetMask;
			uint32_t cbBlend0Control;
			bool isHit;
		} gsPipelineLog[8];

		// Metal unsafe framebuffer-fetch candidates (First 16 Unique Shader/Texture/Instruction Events)
		LatteDiagnosticCounter<uint64_t> acceptedFramebufferFetchCount{0};
		LatteDiagnosticCounter<uint64_t> unsafeFramebufferFetchCount{0};
		std::atomic<uint32_t> unsafeFBFetchCaptureCount{0};
		std::atomic_flag unsafeFBFetchCaptureLock = ATOMIC_FLAG_INIT;
		uint64_t unsafeFBFetchDedupKeys[16]{};
		struct {
			uint64_t psBaseHash;
			uint32_t reasonMask;

			uint32_t textureUnit;
			uint32_t samplerIndex;
			uint32_t opcode;
			uint32_t srcGpr;
			uint32_t srcSel;
			uint32_t coordUnnormalizedMask;
			uint32_t textureGuestAddr;
			uint32_t textureWidth, textureHeight, texturePitch;
			uint32_t textureFormat, textureTileMode;
			uint32_t textureMip, textureSlice;
			uint32_t textureSwizzle;

			uint32_t attachmentTarget;
			uint32_t attachmentGuestAddr;
			uint32_t attachmentWidth, attachmentHeight, attachmentPitch;
			uint32_t attachmentFormat, attachmentTileMode;
			uint32_t attachmentMip, attachmentSlice;

			uint32_t clampX, clampY;
			uint32_t minFilter, magFilter, mipFilter;
			int32_t offsetX, offsetY, offsetZ;
			bool coordinatesUsed;
			bool explicitLod;
			bool implicitLod;
			bool offsetUsed;
			bool filteredSample;
			bool wrapDependent;
		} unsafeFBFetchLog[16];

		// Metal stale attachment sampling diagnostics
		LatteDiagnosticCounter<uint64_t> metalFBOChanges{0};
		LatteDiagnosticCounter<uint64_t> metalEncoderReusesAfterFBOChange{0};
		LatteDiagnosticCounter<uint64_t> metalRemovedAttachmentsRetainedDuringReuse{0};
		LatteDiagnosticCounter<uint64_t> metalStaleAttachmentSampleHazards{0};
		std::atomic<uint32_t> staleAttachmentSampleCaptureCount{0};
		std::atomic_flag staleAttachmentSampleCaptureLock = ATOMIC_FLAG_INIT;
		uint64_t staleAttachmentSampleDedupKeys[16]{};
		struct {
			uint32_t drawIndex;
			uint32_t shaderStage;
			uint32_t textureUnit;
			uint32_t formerAttachmentType;
			uint32_t formerAttachmentIndex;
			uint32_t guestBaseAddress;
			uint32_t width;
			uint32_t height;
			uint32_t pitch;
			uint32_t format;
			uint32_t tileMode;
			uint32_t mip;
			uint32_t slice;
			uint64_t activeFBOIdentity;
			uint64_t lastUsedFBOIdentity;
			bool encoderReused;
			bool isDepthStencilAttachment;
		} staleAttachmentSampleLog[16];

		// Metal normal texture samples that alias an active attachment written by the
		// currently open render encoder (First 16 Unique Events)
		LatteDiagnosticCounter<uint64_t> metalNormalTextureBindsWhileRenderEncoderOpen{0};
		LatteDiagnosticCounter<uint64_t> metalActiveAttachmentBaseMatches{0};
		LatteDiagnosticCounter<uint64_t> metalActiveAttachmentSamplesAfterWrite{0};
		std::atomic<uint32_t> activeAttachmentSampleCaptureCount{0};
		std::atomic_flag activeAttachmentSampleCaptureLock = ATOMIC_FLAG_INIT;
		uint64_t activeAttachmentSampleDedupKeys[16]{};
		struct {
			uint32_t drawIndex;
			uint32_t shaderStage;
			uint32_t textureUnit;
			uint64_t shaderHash;
			uint32_t attachmentType;
			uint32_t attachmentIndex;
			uint64_t baseTextureObjectId;
			uint32_t guestBaseAddress;

			uint32_t sampledMip;
			uint32_t sampledMipCount;
			uint32_t sampledSlice;
			uint32_t sampledSliceCount;
			uint32_t sampledWidth;
			uint32_t sampledHeight;
			uint32_t sampledFormat;

			uint32_t attachmentMip;
			uint32_t attachmentSlice;
			uint32_t attachmentWidth;
			uint32_t attachmentHeight;
			uint32_t attachmentFormat;

			uint64_t encoderGeneration;
			uint32_t lastRTWriteDrawIndex;
			uint64_t lastRTWriteGeneration;
			uint64_t sampledMetalBasePointer;
			uint64_t sampledMetalViewPointer;
			uint64_t attachmentMetalBasePointer;
			uint64_t attachmentMetalViewPointer;
			bool attachmentWrittenThisEncoder;
			bool framebufferFetchClassificationAbsent;
		} activeAttachmentSampleLog[16];

		// View Cache Generation Validation Diagnostics
		LatteDiagnosticCounter<uint64_t> viewCacheHits{0};
		LatteDiagnosticCounter<uint64_t> viewCacheMisses{0};
		LatteDiagnosticCounter<uint64_t> staleBaseGenerationHits{0};
		LatteDiagnosticCounter<uint64_t> identityBaseHits{0};
		LatteDiagnosticCounter<uint64_t> metalViewHits{0};
		LatteDiagnosticCounter<uint64_t> recreatedBaseInvalidations{0};

		std::atomic<uint32_t> staleViewCount{0};
		struct {
			uint32_t eventId;
			uint32_t guestAddress;
			uint64_t cachedViewPtr;
			uint64_t cachedMTLTexturePtr;
			uint64_t cachedBaseObjectId;
			uint64_t currentBaseObjectId;
			uint64_t currentBaseMTLTexturePtr;
			uint32_t width, height, pitch, format;
			uint32_t swizzle;
			bool isRenderTarget;
		} staleViewLog[16];

		// 1280x720 RT First Pass Diagnostics (First 16 Passes)
		std::atomic<uint32_t> rtFirstPassCount{0};
		struct {
			uint32_t eventId;
			uint64_t objectId;
			uint32_t guestAddr;
			uint32_t width, height, format;
			uint8_t loadAction;
			bool firstPassHadClear;
			bool firstDrawBlendEnabled;
			uint8_t firstDrawColorWriteMask;
			uint32_t firstDrawViewportWidth, firstDrawViewportHeight;
			uint64_t firstUseGeneration;
		} rtFirstPassLog[16];

		// Render Target diagnostics
		LatteDiagnosticCounter<uint64_t> targetsCreated{0};
		LatteDiagnosticCounter<uint64_t> targetsReused{0};
		LatteDiagnosticCounter<uint64_t> clears{0};
		LatteDiagnosticCounter<uint64_t> loadActions{0};
		LatteDiagnosticCounter<uint64_t> storeActions{0};
		// RT Transitions diagnostics
		LatteDiagnosticCounter<uint64_t> rtWrites{0};
		LatteDiagnosticCounter<uint64_t> rtToTextureTransitions{0};
		LatteDiagnosticCounter<uint64_t> rtSampleAfterWrite{0};
		LatteDiagnosticCounter<uint64_t> rtSampleWithoutStore{0};
		LatteDiagnosticCounter<uint64_t> rtAliasReuse{0};
		LatteDiagnosticCounter<uint64_t> rtTextureRecreate{0};

		std::atomic<uint32_t> rtTransitionCaptureCount{0};
		struct {
			uint32_t guestAddress;
			uint32_t width;
			uint32_t height;
			uint32_t format;
			uint32_t slice;
			uint32_t mip;
			uint64_t metalTexturePtr;
			bool isRenderTarget;
			bool laterSampled;
			uint8_t loadAction;
			uint8_t storeAction;
			bool sameUnderlyingTexture;
		} rtTransitionLog[32];
	} telemetry;
};

extern LatteGPUState_t LatteGPUState;
extern std::atomic<int> g_textureCacheDiagMode;
extern std::atomic<int> g_rtStoreDiagMode;
extern std::atomic<int> g_strictAliasSyncDiagMode;
extern std::atomic<uint64_t> g_emulatedGuestCycles;
extern std::atomic<uint64_t> g_guestSwapCount;
extern std::atomic<uint64_t> g_gpuWriteGeneration;
extern std::atomic<uint64_t> g_textureObjectIdGenerator;

uint32 LatteTexture_CalculateFullTextureDataHash(LatteTexture* hostTexture);
#ifndef NDEBUG
void Latte_RecordUnique1280SkippedTexture(LatteTexture* hostTexture, bool hashChanged);
void Latte_RecordSuspiciousAlias(LatteTexture* src, LatteTexture* dst, bool sampledWhileStale);
void Latte_RecordCompatibleRelationEvent(LatteTexture* src, LatteTexture* dst, bool reloadFlag, bool syncAttempted, bool syncSucceeded);
void Latte_RecordTextureRecreation(LatteTexture* oldTex, LatteTexture* newTex);
void Latte_RecordDynamicSyncEvent(LatteTexture* dstTex, bool enteredUpdateCache, LatteTexture* srcTex, uint64 srcDynamicUpdate, uint64 dstDynamicUpdate, bool copyAttempted, bool copySucceeded, bool reloadBefore, bool reloadAfter, uint32 reasonCode);
void Latte_RecordSuspiciousRecreation(LatteTexture* oldTex, LatteTexture* newTex);
void Latte_RecordFirstUse(LatteTexture* tex, uint32 useType);
void Latte_RecordRelationRebuilt(LatteTexture* tex1, LatteTexture* tex2);
void Latte_RecordBindFinal(LatteTextureView* view, uint32 word4);
void Latte_RecordStaleViewCacheHit(LatteTextureView* view, uint64 cachedBaseObjId, uint64 currentBaseObjId);
void Latte_RecordRTFirstPass(LatteTexture* tex, uint8 loadAction, bool hadClear, bool blendEnabled, uint8 colorWriteMask, uint32 vpWidth, uint32 vpHeight);
#else
inline void Latte_RecordUnique1280SkippedTexture(LatteTexture*, bool) {}
inline void Latte_RecordSuspiciousAlias(LatteTexture*, LatteTexture*, bool) {}
inline void Latte_RecordCompatibleRelationEvent(LatteTexture*, LatteTexture*, bool, bool, bool) {}
inline void Latte_RecordTextureRecreation(LatteTexture*, LatteTexture*) {}
inline void Latte_RecordDynamicSyncEvent(LatteTexture*, bool, LatteTexture*, uint64, uint64, bool, bool, bool, bool, uint32) {}
inline void Latte_RecordSuspiciousRecreation(LatteTexture*, LatteTexture*) {}
inline void Latte_RecordFirstUse(LatteTexture*, uint32) {}
inline void Latte_RecordRelationRebuilt(LatteTexture*, LatteTexture*) {}
inline void Latte_RecordBindFinal(LatteTextureView*, uint32) {}
inline void Latte_RecordStaleViewCacheHit(LatteTextureView*, uint64, uint64) {}
inline void Latte_RecordRTFirstPass(LatteTexture*, uint8, bool, bool, uint8, uint32, uint32) {}
#endif

// drawcall context

struct LatteDrawcallContext
{
	bool isFirst{}; // first _execute() in current sequence
	// these are only valid if isFirst is false:
	mutable uint32 vertexBufferDirtyMask{}; // mask of vertex buffer indices which have been modified since last draw execute
	uint32 vsUniformBufferDirtyMask{}; // mask of uniform buffer indices which have been modified (changed address or size) since last draw execute
	uint32 psUniformBufferDirtyMask{};
	uint32 gsUniformBufferDirtyMask{};
	bool aluConstVSDirty{};
	bool aluConstPSDirty{};
};

// texture

#include "Cafe/HW/Latte/Core/LatteTexture.h"

// texture loader

void LatteTextureLoader_estimateAccessedDataRange(LatteTexture* texture, sint32 sliceIndex, sint32 mipIndex, uint32& addrStart, uint32& addrEnd);

// render target

#define RENDER_TARGET_TV (1 << 0)
#define RENDER_TARGET_DRC (1 << 2)

void LatteRenderTarget_updateScissorBox();

void LatteRenderTarget_trackUpdates();

void LatteRenderTarget_getScreenImageArea(sint32* x, sint32* y, sint32* width, sint32* height, sint32* fullWidth, sint32* fullHeight, bool padView = false);
void LatteRenderTarget_copyToBackbuffer(LatteTextureView* textureView, bool isPadView);

void LatteRenderTarget_GetCurrentVirtualViewportSize(sint32* viewportWidth, sint32* viewportHeight);

void LatteRenderTarget_itHLESwapScanBuffer();
void LatteRenderTarget_itHLEClearColorDepthStencil(uint32 clearMask, MPTR colorBufferMPTR, Latte::E_GX2SURFFMT colorBufferFormat, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferPitch, uint32 colorBufferViewFirstSlice, uint32 colorBufferViewNumSlice, MPTR depthBufferMPTR, Latte::E_GX2SURFFMT depthBufferFormat, Latte::E_HWTILEMODE depthBufferTileMode, sint32 depthBufferWidth, sint32 depthBufferHeight, sint32 depthBufferPitch, sint32 depthBufferViewFirstSlice, sint32 depthBufferViewNumSlice, float r, float g, float b, float a, float clearDepth, uint32 clearStencil);
void LatteRenderTarget_itHLECopyColorBufferToScanBuffer(MPTR colorBufferPtr, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferSliceIndex, uint32 colorBufferFormat, uint32 colorBufferPitch, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferSwizzle, uint32 renderTarget);

void LatteRenderTarget_unloadAll();

// texture cache

void LatteTC_Init();

void LatteTC_RegisterTexture(LatteTexture* tex);
void LatteTC_UnregisterTexture(LatteTexture* tex);

uint32 LatteTexture_CalculateTextureDataHash(LatteTexture* hostTexture);
void LatteTexture_ReloadData(LatteTexture* hostTexture);

bool LatteTC_HasTextureChanged(LatteTexture* hostTexture, bool force = false);
void LatteTC_ResetTextureChangeTracker(LatteTexture* hostTexture, bool force = false);

void LatteTC_MarkTextureStillInUse(LatteTexture* texture); // lets the texture garbage collector know the texture is still in use at the time of this function call
void LatteTC_CleanupUnusedTextures();

// May be called from a platform thread. The requested cleanup is performed by
// LatteTC_CleanupUnusedTextures() on the renderer thread at the end of a frame.
void LatteTC_RequestMemoryPressureCleanup();
// Near the real iOS limit: delete every eligible texture at once, not 64.
void LatteTC_RequestEmergencyMemoryCleanup();
uint32 LatteTC_GetMemoryPressureCleanupRequestCount();
uint32 LatteTC_GetMemoryPressureCleanupCompletedCount();
uint32 LatteTC_GetMemoryPressureCleanupLastDeletedCount();

std::vector<LatteTexture*> LatteTC_GetDeleteableTextures();

void LatteTC_UnloadAllTextures();

// texture readback

void LatteTextureReadback_Initate(LatteTextureView* textureView);
void LatteTextureReadback_StartTransfer(LatteTextureView* textureView);
bool LatteTextureReadback_Update(bool forceStart = false);
bool LatteTextureReadback_HasEligibleTransfers();
void LatteTextureReadback_NotifyTextureDeletion(LatteTexture* texture);
void LatteTextureReadback_UpdateFinishedTransfers(bool forceFinish);
void LatteTextureReadback_Reset();
bool LatteTextureReadback_ReadbackToLinearBlocking(LatteTextureView* sourceView, uint8* dstPtr, uint32 dstWidth, uint32 dstHeight, uint32 dstPitch);

// query

void LatteQuery_Init();
void LatteQuery_Reset();
void LatteQuery_BeginOcclusionQuery(MPTR queryMPTR);
void LatteQuery_EndOcclusionQuery(MPTR queryMPTR);
void LatteQuery_UpdateFinishedQueries();
void LatteQuery_UpdateFinishedQueriesForceFinishAll();
void LatteQuery_CancelActiveGPU7Queries();

// streamout

void LatteStreamout_InitCache();
sint32 LatteStreamout_GetRingBufferSize();
void LatteStreamout_PrepareDrawcall(uint32 count, uint32 instanceCount);
void LatteStreamout_FinishDrawcall(bool useDirectMemoryMode);

// timing

void LatteTiming_Init();
void LatteTiming_HandleTimedVsync();

// command processor

void LatteCP_ProcessRingbuffer();

// buffer cache

void LatteBufferCache_Sync(uint32 maxIndex, uint32 baseInstance, uint32 instanceCount, uint32 attribBufferDirtyMask, uint32 vsUniformBufferDirtyMask, uint32 psUniformBufferDirtyMask, uint32 gsUniformBufferDirtyMask, uint8& stageUniformModifiedMask, bool isIncremental = false);
bool LatteBufferCache_LoadRemappedUniforms(struct LatteDecompilerShader* shader, float* uniformData, bool aluConstDirty, uint32 uniformBufferDirtyMask);

void LatteRenderTarget_updateViewport();

#define LATTE_GLSL_DYNAMIC_UNIFORM_BLOCK_SIZE	(4096) // maximum size for uniform blocks (in vec4s). On Nvidia hardware 4096 is the maximum (64K / 16 = 4096) all other vendors have much higher limits

//static uint32 glTempError;
//#define catchOpenGLError() glFinish(); if( (glTempError = glGetError()) != 0 ) { printf("OpenGL error 0x%x: %s : %d timestamp %08x\n", glTempError, __FILE__, __LINE__, GetTickCount()); __debugbreak(); }

#define catchOpenGLError()

// Latte emulation control
void Latte_Start();
void Latte_Stop();
bool Latte_GetStopSignal(); // returns true if stop was requested or if in stopped state
void LatteThread_Exit();
