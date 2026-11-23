#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Common/cpu_features.h"
#include "Common/HostUploadMemory.h"
#include "Cafe/HW/Latte/Core/TextureCacheRetention.h"
#if BOOST_OS_IOS && defined(ENABLE_METAL)
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryPolicy.h"
#endif

extern "C" void CemuBridge_LogJITMilestone(const char* msg);

std::unordered_set<LatteTexture*> g_allTextures;

void LatteTC_Init()
{
	cemu_assert_debug(g_allTextures.empty());
}

void LatteTC_RegisterTexture(LatteTexture* tex)
{
	g_allTextures.emplace(tex);
}

void LatteTC_UnregisterTexture(LatteTexture* tex)
{
	g_allTextures.erase(tex);
}

// sample few uint64s uniformly over memory range
uint32 _quickStochasticHash(void* texData, uint32 memRange)
{
	uint64* texDataU64 = (uint64*)texData;

	uint64 hashVal = 0;
	memRange /= sizeof(uint64);

	uint32 memStep = memRange / 37; // use prime here to avoid memStep aligning nicely with pitch of texture, leading to sampling only along the border of a texture
	for (sint32 i = 0; i < 37; i++)
	{
		hashVal += *texDataU64;
		hashVal = (hashVal << 3) | (hashVal >> 61);
		texDataU64 += memStep;
	}
	return (uint32)hashVal ^ (uint32)(hashVal >> 32);
}

uint32 LatteTexture_CalculateTextureDataHash(LatteTexture* hostTexture)
{
	if( hostTexture->texDataPtrHigh == hostTexture->texDataPtrLow )
	{
		return 0;
	}
	if (hostTexture->format == Latte::E_GX2SURFFMT::R11_G11_B10_FLOAT)
	{
		// this is an exotic format that usually isn't generated or updated CPU-side
		// therefore as an optimization we can risk to only check a minimal amount of bytes at the beginning of the texture data
		// updates which change the entire texture should still be detected this way
		// this also helps with a bug in BotW which seems to fill the empty areas of the textures with other data which causes unnecessary invalidations and texture reloads

		// Wonderful 101 generates this format in a 8x8x8 3D texture using tiling aperture
		if (hostTexture->tileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THICK && hostTexture->depth == 8 && hostTexture->width == 8 && hostTexture->height == 8)
		{
			// special case for Wonderful 101
			uint32* texDataU32 = (uint32*)memory_getPointerFromPhysicalOffset(hostTexture->texDataPtrLow);
			return texDataU32[0] ^ texDataU32[0x100/4] ^ texDataU32[0x200/4] ^ texDataU32[0x300/4]; // check the first thick slice (each slice has 0x400 bytes, with 0x100 bytes between layers)
		}
		uint32* texDataU32 = (uint32*)memory_getPointerFromPhysicalOffset(hostTexture->texDataPtrLow);
		return texDataU32[0] ^ texDataU32[1] ^ texDataU32[2] ^ texDataU32[3];
	}

	uint32 memRange = hostTexture->texDataPtrHigh - hostTexture->texDataPtrLow;
	uint32* texDataU32 = (uint32*)memory_getPointerFromPhysicalOffset(hostTexture->texDataPtrLow);
	uint32 hashVal = 0;
	uint32 pixelCount = hostTexture->width*hostTexture->height;

	bool isCompressedFormat = hostTexture->IsCompressedFormat();
	if (isCompressedFormat || hostTexture->useLightHash)
	{
		// check only 32 samples of the texture
		if (memRange < 256)
		{
			memRange /= sizeof(uint32);
			while (memRange--)
			{
				hashVal += *texDataU32;
				hashVal = (hashVal << 3) | (hashVal >> 29);
				texDataU32++;
			}
		}
		else
		{
			hashVal = _quickStochasticHash(texDataU32, memRange);
		}
		return hashVal;
	}


	if( pixelCount <= (700*700) )
	{
		// small texture size
		bool isCompressedFormat = hostTexture->IsCompressedFormat();
		if( isCompressedFormat == false || memRange < 0x200 )
		{
			memRange /= (4*sizeof(uint32));
			while( memRange-- )
			{
				hashVal += *texDataU32;
				hashVal = (hashVal<<3)|(hashVal>>29);
				texDataU32 += 4;
			}
		}
		else
		{
			memRange /= (32*sizeof(uint32));
			while( memRange-- )
			{
				hashVal += *texDataU32;
				hashVal = (hashVal<<3)|(hashVal>>29);
				texDataU32 += 32;
			}
		}
	}
	else if( pixelCount <= (1200*1200) )
	{
		// medium texture size
		bool isCompressedFormat = hostTexture->IsCompressedFormat();
		if( isCompressedFormat == false )
		{
			memRange /= (12*sizeof(uint32));
			while( memRange-- )
			{
				hashVal += *texDataU32;
				hashVal = (hashVal<<3)|(hashVal>>29);
				texDataU32 += 12;
			}
		}
		else
		{
			memRange /= (96*sizeof(uint32));
			while( memRange-- )
			{
				hashVal += *texDataU32;
				hashVal = (hashVal<<3)|(hashVal>>29);
				texDataU32 += 96;
			}
		}
	}
	else
	{
		// huge texture size
		bool isCompressedFormat = hostTexture->IsCompressedFormat();
		if( isCompressedFormat == false )
		{
#if BOOST_OS_WINDOWS
			if (g_CPUFeatures.x86.avx2)
			{
				__m256i h256 = { 0 };
				__m256i* readPtr = (__m256i*)texDataU32;
				memRange /= (288);
				while (memRange--)
				{
					__m256i temp = _mm256_load_si256(readPtr);
					readPtr += (288 / 32);
					h256 = _mm256_xor_si256(h256, temp);
				}
#ifdef __clang__
				hashVal = h256[0] + h256[1] + h256[2] + h256[3] + h256[4] + h256[5] + h256[6] + h256[7];
#else
				hashVal = h256.m256i_u32[0] + h256.m256i_u32[1] + h256.m256i_u32[2] + h256.m256i_u32[3] + h256.m256i_u32[4] + h256.m256i_u32[5] + h256.m256i_u32[6] + h256.m256i_u32[7];
#endif
			}
			else
#endif
			{
				memRange /= (32 * sizeof(uint64));
				uint64 h64 = 0;
				uint64* texDataU64 = (uint64*)texDataU32;
				while (memRange--)
				{
					h64 += *texDataU64;
					h64 = (h64 << 3) | (h64 >> 61);
					texDataU64 += 32;
				}
				hashVal = (h64 & 0xFFFFFFFF) + (h64 >> 32);
			}
		}
		else
		{
			memRange /= (512*sizeof(uint32));
			while( memRange-- )
			{
				hashVal += *texDataU32;
				hashVal = (hashVal<<3)|(hashVal>>29);
				texDataU32 += 512;
			}
		}
	}

	return hashVal;
}

uint64 _botwLargeTexHax = 0;

uint32 LatteTexture_CalculateFullTextureDataHash(LatteTexture* hostTexture)
{
	if (hostTexture->texDataPtrHigh <= hostTexture->texDataPtrLow)
		return 0;
	uint32 memRange = hostTexture->texDataPtrHigh - hostTexture->texDataPtrLow;
	const uint8* data = (const uint8*)memory_getPointerFromPhysicalOffset(hostTexture->texDataPtrLow);
	if (!data) return 0;

	// FNV-1a 32-bit hash across entire texture range
	uint32 hash = 0x811c9dc5;
	const uint32* dwords = (const uint32*)data;
	size_t dwordCount = memRange / sizeof(uint32);
	for (size_t i = 0; i < dwordCount; ++i)
	{
		hash ^= dwords[i];
		hash *= 0x01000193;
	}
	size_t rem = memRange % sizeof(uint32);
	if (rem > 0)
	{
		const uint8* remBytes = data + (dwordCount * sizeof(uint32));
		for (size_t i = 0; i < rem; ++i)
		{
			hash ^= remBytes[i];
			hash *= 0x01000193;
		}
	}
	return hash;
}

#ifndef NDEBUG
void Latte_RecordUnique1280SkippedTexture(LatteTexture* hostTexture, bool hashChanged)
{
	uint32_t currentCount = LatteGPUState.telemetry.unique1280SkippedCount.load(std::memory_order_relaxed);
	if (currentCount >= 32) return;

	for (uint32_t i = 0; i < currentCount; ++i)
	{
		if (LatteGPUState.telemetry.unique1280SkippedLog[i].guestAddress == (uint32_t)hostTexture->physAddress)
			return; // already recorded
	}

	uint32_t idx = LatteGPUState.telemetry.unique1280SkippedCount.fetch_add(1, std::memory_order_relaxed);
	if (idx < 32)
	{
		auto& entry = LatteGPUState.telemetry.unique1280SkippedLog[idx];
		entry.guestAddress = (uint32_t)hostTexture->physAddress;
		entry.width = hostTexture->width;
		entry.height = hostTexture->height;
		entry.format = (uint32_t)hostTexture->format;
		entry.sizeBytes = (hostTexture->texDataPtrHigh > hostTexture->texDataPtrLow) ? (hostTexture->texDataPtrHigh - hostTexture->texDataPtrLow) : 0;
		entry.isGPUWritten = hostTexture->isUpdatedOnGPU;
		entry.isRenderTarget = (hostTexture->lastRenderTargetSwizzle != 0 || hostTexture->isUpdatedOnGPU);
		entry.isSampled = true;
		entry.hashChangedWhenForced = hashChanged;
	}
}

void Latte_RecordSuspiciousAlias(LatteTexture* src, LatteTexture* dst, bool sampledWhileStale)
{
	uint32_t currentCount = LatteGPUState.telemetry.uniqueAliasCount.load(std::memory_order_relaxed);
	if (currentCount >= 32) return;

	for (uint32_t i = 0; i < currentCount; ++i)
	{
		if (LatteGPUState.telemetry.uniqueAliasLog[i].srcGuestAddr == (uint32_t)src->physAddress &&
		    LatteGPUState.telemetry.uniqueAliasLog[i].dstGuestAddr == (uint32_t)dst->physAddress &&
		    LatteGPUState.telemetry.uniqueAliasLog[i].srcWidth == src->width &&
		    LatteGPUState.telemetry.uniqueAliasLog[i].dstWidth == dst->width)
		{
			return; // already recorded
		}
	}

	uint32_t idx = LatteGPUState.telemetry.uniqueAliasCount.fetch_add(1, std::memory_order_relaxed);
	if (idx < 32)
	{
		auto& entry = LatteGPUState.telemetry.uniqueAliasLog[idx];
		entry.srcGuestAddr = (uint32_t)src->physAddress;
		entry.dstGuestAddr = (uint32_t)dst->physAddress;

		entry.srcWidth = src->width;
		entry.srcHeight = src->height;
		entry.srcPitch = src->pitch;
		entry.srcFormat = (uint32_t)src->format;
		entry.srcTileMode = (uint32_t)src->tileMode;
		entry.srcMip = 0;
		entry.srcSlice = 0;
		entry.srcMTLTexturePtr = (uint64_t)(uintptr_t)src->baseView;
		entry.srcGPUWriteGen = src->lastGPUWriteGeneration;

		entry.dstWidth = dst->width;
		entry.dstHeight = dst->height;
		entry.dstPitch = dst->pitch;
		entry.dstFormat = (uint32_t)dst->format;
		entry.dstTileMode = (uint32_t)dst->tileMode;
		entry.dstMip = 0;
		entry.dstSlice = 0;
		entry.dstMTLTexturePtr = (uint64_t)(uintptr_t)dst->baseView;
		entry.dstDynamicSyncGen = dst->lastDynamicSyncGeneration;

		entry.sameBase = (src->physAddress == dst->physAddress);
		entry.rangesOverlap = (src->texDataPtrLow < dst->texDataPtrHigh && dst->texDataPtrLow < src->texDataPtrHigh);
		entry.sameMetalTexture = (src->baseView == dst->baseView);

		bool hasRel = false;
		for (auto& r : src->list_compatibleRelations)
		{
			if ((r->baseTexture == src && r->subTexture == dst) || (r->baseTexture == dst && r->subTexture == src))
			{
				hasRel = true;
				break;
			}
		}
		entry.relationExists = hasRel;
		entry.syncAttempted = false;
		entry.syncSucceeded = false;
		entry.sampledWhileStale = sampledWhileStale;
	}
}

void Latte_RecordCompatibleRelationEvent(LatteTexture* src, LatteTexture* dst, bool reloadFlag, bool syncAttempted, bool syncSucceeded)
{
	uint32_t currentCount = LatteGPUState.telemetry.compatibleRelationCaptureCount.load(std::memory_order_relaxed);
	if (currentCount >= 32) return;

	for (uint32_t i = 0; i < currentCount; ++i)
	{
		if (LatteGPUState.telemetry.compatibleRelationLog[i].srcGuestAddr == (uint32_t)src->physAddress &&
		    LatteGPUState.telemetry.compatibleRelationLog[i].dstGuestAddr == (uint32_t)dst->physAddress &&
		    LatteGPUState.telemetry.compatibleRelationLog[i].srcObjectId == src->hostTextureObjectId &&
		    LatteGPUState.telemetry.compatibleRelationLog[i].dstObjectId == dst->hostTextureObjectId)
		{
			return; // already recorded
		}
	}

	uint32_t idx = LatteGPUState.telemetry.compatibleRelationCaptureCount.fetch_add(1, std::memory_order_relaxed);
	if (idx < 32)
	{
		auto& entry = LatteGPUState.telemetry.compatibleRelationLog[idx];
		entry.srcGuestAddr = (uint32_t)src->physAddress;
		entry.dstGuestAddr = (uint32_t)dst->physAddress;
		entry.srcWidth = src->width;
		entry.srcHeight = src->height;
		entry.srcPitch = src->pitch;
		entry.srcFormat = (uint32_t)src->format;
		entry.dstWidth = dst->width;
		entry.dstHeight = dst->height;
		entry.dstPitch = dst->pitch;
		entry.dstFormat = (uint32_t)dst->format;
		entry.srcGPUWriteGen = src->lastGPUWriteGeneration;
		entry.dstDynamicSyncGen = dst->lastDynamicSyncGeneration;
		entry.srcObjectId = src->hostTextureObjectId;
		entry.dstObjectId = dst->hostTextureObjectId;
		entry.srcMTLTexturePtr = (uint64_t)(uintptr_t)src->baseView;
		entry.dstMTLTexturePtr = (uint64_t)(uintptr_t)dst->baseView;
		entry.reloadFromDynamicTextures = reloadFlag;
		entry.syncAttempted = syncAttempted;
		entry.syncSucceeded = syncSucceeded;
		entry.relationStillPresent = true;
	}
}

void Latte_RecordTextureRecreation(LatteTexture* oldTex, LatteTexture* newTex)
{
	uint32_t currentCount = LatteGPUState.telemetry.textureRecreationCaptureCount.load(std::memory_order_relaxed);
	if (currentCount >= 32) return;

	uint32_t idx = LatteGPUState.telemetry.textureRecreationCaptureCount.fetch_add(1, std::memory_order_relaxed);
	if (idx < 32)
	{
		auto& entry = LatteGPUState.telemetry.textureRecreationLog[idx];
		entry.guestAddress = (uint32_t)newTex->physAddress;
		entry.oldObjectId = oldTex ? oldTex->hostTextureObjectId : 0;
		entry.newObjectId = newTex->hostTextureObjectId;
		entry.oldMetalTexturePtr = oldTex ? (uint64_t)(uintptr_t)oldTex->baseView : 0;
		entry.newMetalTexturePtr = (uint64_t)(uintptr_t)newTex->baseView;
		entry.oldGPUWriteGen = oldTex ? oldTex->lastGPUWriteGeneration : 0;
		entry.newInitialGen = newTex->lastGPUWriteGeneration;
		entry.oldWasGPUUpdated = oldTex ? oldTex->isUpdatedOnGPU : false;
		entry.hadCompatibleRelations = oldTex ? !oldTex->list_compatibleRelations.empty() : false;
		entry.pendingReloadLost = oldTex ? oldTex->reloadFromDynamicTextures : false;
	}
}

void Latte_RecordDynamicSyncEvent(LatteTexture* dstTex, bool enteredUpdateCache, LatteTexture* srcTex, uint64 srcDynamicUpdate, uint64 dstDynamicUpdate, bool copyAttempted, bool copySucceeded, bool reloadBefore, bool reloadAfter, uint32 reasonCode)
{
	uint32_t currentCount = LatteGPUState.telemetry.dynamicSyncEventCount.load(std::memory_order_relaxed);
	if (currentCount >= 16) return;

	uint32_t idx = LatteGPUState.telemetry.dynamicSyncEventCount.fetch_add(1, std::memory_order_relaxed);
	if (idx < 16)
	{
		auto& entry = LatteGPUState.telemetry.dynamicSyncEvents[idx];
		entry.dstObjectId = dstTex ? dstTex->hostTextureObjectId : 0;
		entry.dstGuestAddr = dstTex ? (uint32_t)dstTex->physAddress : 0;
		entry.dstWidth = dstTex ? dstTex->width : 0;
		entry.dstHeight = dstTex ? dstTex->height : 0;
		entry.dstPitch = dstTex ? dstTex->pitch : 0;
		entry.dstFormat = dstTex ? (uint32_t)dstTex->format : 0;
		entry.relationCount = dstTex ? (uint32_t)dstTex->list_compatibleRelations.size() : 0;
		entry.enteredUpdateCache = enteredUpdateCache;
		entry.srcObjectId = srcTex ? srcTex->hostTextureObjectId : 0;
		entry.srcGuestAddr = srcTex ? (uint32_t)srcTex->physAddress : 0;
		entry.srcIsUpdatedOnGPU = srcTex ? srcTex->isUpdatedOnGPU : false;
		entry.srcLastDynamicUpdate = srcDynamicUpdate;
		entry.dstLastDynamicUpdate = dstDynamicUpdate;
		entry.copyAttempted = copyAttempted;
		entry.copySucceeded = copySucceeded;
		entry.reloadFlagBefore = reloadBefore;
		entry.reloadFlagAfter = reloadAfter;
		entry.reasonCode = reasonCode;
	}
}

void Latte_RecordSuspiciousRecreation(LatteTexture* oldTex, LatteTexture* newTex)
{
	if (!oldTex || !newTex) return;
	bool suspicious = (!oldTex->list_compatibleRelations.empty() || oldTex->reloadFromDynamicTextures || oldTex->isUpdatedOnGPU);
	if (!suspicious) return;

	uint32_t currentCount = LatteGPUState.telemetry.recreationLifecycleCount.load(std::memory_order_relaxed);
	if (currentCount >= 16) return;

	uint32_t idx = LatteGPUState.telemetry.recreationLifecycleCount.fetch_add(1, std::memory_order_relaxed);
	if (idx < 16)
	{
		uint32_t eventId = idx + 1;
		newTex->trackedRecreationEventId = eventId;

		auto& entry = LatteGPUState.telemetry.recreationLifecycleLog[idx];
		entry.eventId = eventId;
		entry.guestAddress = (uint32_t)newTex->physAddress;
		entry.oldObjectId = oldTex->hostTextureObjectId;
		entry.newObjectId = newTex->hostTextureObjectId;
		entry.oldWidth = oldTex->width;
		entry.oldHeight = oldTex->height;
		entry.oldPitch = oldTex->pitch;
		entry.oldFormat = (uint32_t)oldTex->format;
		entry.oldTileMode = (uint32_t)oldTex->tileMode;

		entry.newWidth = newTex->width;
		entry.newHeight = newTex->height;
		entry.newPitch = newTex->pitch;
		entry.newFormat = (uint32_t)newTex->format;
		entry.newTileMode = (uint32_t)newTex->tileMode;

		entry.oldWasGPUUpdated = oldTex->isUpdatedOnGPU;
		entry.oldReloadFromDynamicTextures = oldTex->reloadFromDynamicTextures;
		entry.oldRelationCount = (uint32_t)oldTex->list_compatibleRelations.size();
		entry.oldLastDynamicUpdate = oldTex->sliceMipInfo ? oldTex->sliceMipInfo[0].lastDynamicUpdate : 0;
		entry.pendingReloadLost = oldTex->reloadFromDynamicTextures;

		entry.firstUseRecorded = false;
		entry.relationRebuiltRecorded = false;

#ifdef CEMU_DEBUG_ASSERT
		char msg[256];
		snprintf(msg, sizeof(msg),
		         "[RECREATE LOST #%u] addr=0x%08X oldObj=%llu newObj=%llu oldRel=%u oldReload=%s oldGPU=%s pendingLost=%s",
		         eventId,
		         entry.guestAddress,
		         (unsigned long long)entry.oldObjectId,
		         (unsigned long long)entry.newObjectId,
		         entry.oldRelationCount,
		         entry.oldReloadFromDynamicTextures ? "Y" : "N",
		         entry.oldWasGPUUpdated ? "Y" : "N",
		         entry.pendingReloadLost ? "YES" : "NO");
		CemuBridge_LogJITMilestone(msg);
#endif
	}
}

void Latte_RecordFirstUse(LatteTexture* tex, uint32 useType)
{
	if (!tex || tex->trackedRecreationEventId == 0 || tex->trackedRecreationEventId > 16)
		return;

	uint32_t idx = tex->trackedRecreationEventId - 1;
	auto& entry = LatteGPUState.telemetry.recreationLifecycleLog[idx];
	if (entry.firstUseRecorded)
		return;

	entry.firstUseRecorded = true;
	entry.firstUseType = useType;
	entry.firstUseRelationCount = (uint32_t)tex->list_compatibleRelations.size();
	entry.firstUseReloadFromDynamicTextures = tex->reloadFromDynamicTextures;
	entry.firstUseIsUpdatedOnGPU = tex->isUpdatedOnGPU;
	entry.firstUseLastDynamicUpdate = tex->sliceMipInfo ? tex->sliceMipInfo[0].lastDynamicUpdate : 0;
	entry.firstUseCpuUploadOccurred = tex->cpuUploadOccurred;
	entry.firstUseDynamicSyncOccurred = tex->dynamicSyncOccurred;
	entry.firstUseRelationRecreated = entry.relationRebuiltRecorded;

	const char* typeStr = "UNKNOWN";
	switch (useType)
	{
	case 1: typeStr = "SAMPLE"; break;
	case 2: typeStr = "RENDER_TARGET"; break;
	case 3: typeStr = "COPY_SRC"; break;
	case 4: typeStr = "COPY_DST"; break;
	default: break;
	}

#ifdef CEMU_DEBUG_ASSERT
	char msg[256];
	snprintf(msg, sizeof(msg),
	         "[RECREATE FIRST_USE #%u] obj=%llu type=%s rels=%u reload=%s cpuUpload=%s dynSync=%s relRecreated=%s",
	         entry.eventId,
	         (unsigned long long)entry.newObjectId,
	         typeStr,
	         entry.firstUseRelationCount,
	         entry.firstUseReloadFromDynamicTextures ? "Y" : "N",
	         entry.firstUseCpuUploadOccurred ? "Y" : "N",
	         entry.firstUseDynamicSyncOccurred ? "Y" : "N",
	         entry.firstUseRelationRecreated ? "Y" : "N");
	CemuBridge_LogJITMilestone(msg);
#endif
}

void Latte_RecordRelationRebuilt(LatteTexture* tex1, LatteTexture* tex2)
{
	if (!tex1 || !tex2) return;

	auto checkAndRecord = [](LatteTexture* t1, LatteTexture* t2) {
		if (t1->trackedRecreationEventId > 0 && t1->trackedRecreationEventId <= 16)
		{
			uint32_t idx = t1->trackedRecreationEventId - 1;
			auto& entry = LatteGPUState.telemetry.recreationLifecycleLog[idx];
			if (!entry.relationRebuiltRecorded)
			{
				entry.relationRebuiltRecorded = true;
				entry.relationPeerObjectId = t2->hostTextureObjectId;
				entry.relationRebuiltBeforeFirstUse = !entry.firstUseRecorded;
				entry.peerWidth = t2->width;
				entry.peerHeight = t2->height;
				entry.peerPitch = t2->pitch;
				entry.peerFormat = (uint32_t)t2->format;
				entry.reloadFlagAfterRelationRebuilt = t1->reloadFromDynamicTextures;

#ifdef CEMU_DEBUG_ASSERT
				char msg[256];
				snprintf(msg, sizeof(msg),
				         "[RECREATE RELATION #%u] obj=%llu peer=%llu timing=%s peerSize=%ux%u reload=%s",
				         entry.eventId,
				         (unsigned long long)entry.newObjectId,
				         (unsigned long long)entry.relationPeerObjectId,
				         entry.relationRebuiltBeforeFirstUse ? "BEFORE_FIRST_USE" : "AFTER_FIRST_USE",
				         entry.peerWidth, entry.peerHeight,
				         entry.reloadFlagAfterRelationRebuilt ? "Y" : "N");
				CemuBridge_LogJITMilestone(msg);
#endif
			}
		}
	};

	checkAndRecord(tex1, tex2);
	checkAndRecord(tex2, tex1);
}

void Latte_RecordBindFinal(LatteTextureView* view, uint32 word4)
{
	if (!view || !view->baseTexture) return;
	LatteTexture* baseTexture = view->baseTexture;

	uint32_t currentCount = LatteGPUState.telemetry.bindFinalCount.load(std::memory_order_relaxed);
	if (currentCount >= 32) return;

	uint64 targetObjId = baseTexture->hostTextureObjectId;
	for (uint32_t i = 0; i < currentCount && i < 32; ++i)
	{
		if (LatteGPUState.telemetry.bindFinalLog[i].hostTextureObjectId == targetObjId)
			return;
	}

	uint32_t idx = LatteGPUState.telemetry.bindFinalCount.fetch_add(1, std::memory_order_relaxed);
	if (idx < 32)
	{
		auto& entry = LatteGPUState.telemetry.bindFinalLog[idx];
		entry.entryIndex = idx + 1;
		entry.guestAddress = (uint32_t)baseTexture->physAddress;
		entry.hostTextureObjectId = targetObjId;
		entry.baseTextureObjectId = targetObjId;
		entry.textureViewPtr = (uint64_t)(uintptr_t)view;
		entry.mtlTexturePtr = 0;
		entry.baseMTLTexturePtr = 0;
		entry.width = baseTexture->width;
		entry.height = baseTexture->height;
		entry.pitch = baseTexture->pitch;
		entry.format = (uint32_t)baseTexture->format;
		entry.tileMode = (uint32_t)baseTexture->tileMode;
		entry.firstMip = view->firstMip;
		entry.numMip = view->numMip;
		entry.firstSlice = view->firstSlice;
		entry.numSlice = view->numSlice;
		entry.isUpdatedOnGPU = baseTexture->isUpdatedOnGPU;
		entry.reloadFromDynamicTextures = baseTexture->reloadFromDynamicTextures;
		entry.lastDynamicUpdate = baseTexture->sliceMipInfo ? baseTexture->sliceMipInfo[0].lastDynamicUpdate : 0;
		entry.lastGPUWriteGeneration = baseTexture->lastGPUWriteGeneration;
		entry.lastDynamicSyncGeneration = baseTexture->lastDynamicSyncGeneration;
		entry.swizzle = word4 & 0x0FFF0000;
		entry.isIdentityView = (view == baseTexture->baseView);
		entry.isRenderTarget = baseTexture->isUpdatedOnGPU;
		entry.differsFromBaseFormat = (view->format != baseTexture->format);
	}
}

void Latte_RecordStaleViewCacheHit(LatteTextureView* view, uint64 cachedBaseObjId, uint64 currentBaseObjId)
{
	if (!view) return;
	uint32_t currentCount = LatteGPUState.telemetry.staleViewCount.load(std::memory_order_relaxed);
	if (currentCount >= 16) return;

	uint32_t idx = LatteGPUState.telemetry.staleViewCount.fetch_add(1, std::memory_order_relaxed);
	if (idx < 16)
	{
		auto& entry = LatteGPUState.telemetry.staleViewLog[idx];
		entry.eventId = idx + 1;
		entry.guestAddress = view->baseTexture ? (uint32_t)view->baseTexture->physAddress : 0;
		entry.cachedViewPtr = (uint64_t)(uintptr_t)view;
		entry.cachedMTLTexturePtr = 0;
		entry.cachedBaseObjectId = cachedBaseObjId;
		entry.currentBaseObjectId = currentBaseObjId;
		entry.currentBaseMTLTexturePtr = 0;
		entry.width = view->baseTexture ? view->baseTexture->width : 0;
		entry.height = view->baseTexture ? view->baseTexture->height : 0;
		entry.pitch = view->baseTexture ? view->baseTexture->pitch : 0;
		entry.format = view->baseTexture ? (uint32_t)view->baseTexture->format : 0;
		entry.swizzle = view->baseTexture ? view->baseTexture->swizzle : 0;
		entry.isRenderTarget = view->baseTexture ? view->baseTexture->isUpdatedOnGPU : false;
	}
}

void Latte_RecordRTFirstPass(LatteTexture* tex, uint8 loadAction, bool hadClear, bool blendEnabled, uint8 colorWriteMask, uint32 vpWidth, uint32 vpHeight)
{
	if (!tex) return;
	uint32_t currentCount = LatteGPUState.telemetry.rtFirstPassCount.load(std::memory_order_relaxed);
	if (currentCount >= 16) return;

	uint32_t idx = LatteGPUState.telemetry.rtFirstPassCount.fetch_add(1, std::memory_order_relaxed);
	if (idx < 16)
	{
		auto& entry = LatteGPUState.telemetry.rtFirstPassLog[idx];
		entry.eventId = idx + 1;
		entry.objectId = tex->hostTextureObjectId;
		entry.guestAddr = (uint32_t)tex->physAddress;
		entry.width = tex->width;
		entry.height = tex->height;
		entry.format = (uint32_t)tex->format;
		entry.loadAction = loadAction;
		entry.firstPassHadClear = hadClear;
		entry.firstDrawBlendEnabled = blendEnabled;
		entry.firstDrawColorWriteMask = colorWriteMask;
		entry.firstDrawViewportWidth = vpWidth;
		entry.firstDrawViewportHeight = vpHeight;
		entry.firstUseGeneration = tex->lastGPUWriteGeneration;

		if (tex->lastGPUWriteGeneration == 0 && !tex->isUpdatedOnGPU && !tex->dynamicSyncOccurred && tex->list_compatibleRelations.empty() && loadAction == 1)
		{
			char msg[256];
			snprintf(msg, sizeof(msg),
			         "[RT INIT SUSPECT #%u] obj=%llu addr=0x%08X %ux%u pitch=%u fmt=0x%X tm=%u cpuUpload=%s rels=0 dynSync=N gpuWrite=0 loadAction=LOAD vp=%ux%u mask=0x%X blend=%s fullOverwrite=%s",
			         entry.eventId,
			         (unsigned long long)entry.objectId,
			         entry.guestAddr,
			         entry.width,
			         entry.height,
			         tex->pitch,
			         entry.format,
			         (uint32)tex->tileMode,
			         tex->cpuUploadOccurred ? "Y" : "N",
			         entry.firstDrawViewportWidth,
			         entry.firstDrawViewportHeight,
			         entry.firstDrawColorWriteMask,
			         entry.firstDrawBlendEnabled ? "Y" : "N",
			         entry.firstPassHadClear ? "CLEAR" : "UNPROVEN");
			CemuBridge_LogJITMilestone(msg);
		}
	}
}
#endif

bool LatteTC_HasTextureChanged(LatteTexture* hostTexture, bool force)
{
	if (hostTexture->forceInvalidate)
	{
		force = true;
		debug_printf("Force invalidate 0x%08x\n", hostTexture->physAddress);
		hostTexture->forceInvalidate = false;
	}
	int diagMode = g_textureCacheDiagMode.load(std::memory_order_relaxed);

	// if texture is written by GPU operations we switch to a faster hash implementation
	if (hostTexture->isUpdatedOnGPU && hostTexture->useLightHash == false)
	{
		hostTexture->useLightHash = true;
		// update hash
		hostTexture->texDataHash2 = (diagMode == 2) ? LatteTexture_CalculateFullTextureDataHash(hostTexture) : LatteTexture_CalculateTextureDataHash(hostTexture);
	}
	// only check each texture for updates once a frame
	// todo: Instead of relying on frames, it would be better to recheck only after any GPU wait operation occurred.
	if (hostTexture->lastDataUpdateFrameCounter == LatteGPUState.frameCounter && force == false)
		return false;
	hostTexture->lastDataUpdateFrameCounter = LatteGPUState.frameCounter;

	// Diagnostic modes collect extra detail for 1280-width textures. Normal
	// gameplay must still reach the ordinary change check below: skipping it
	// forever leaves CPU-updated render targets stale.
	if (diagMode != 0 && hostTexture->width == 1280 && hostTexture->format != Latte::E_GX2SURFFMT::R8_UNORM && force == false)
	{
		LatteGPUState.telemetry.skip1280Checks.fetch_add(1, std::memory_order_relaxed);

		// Diagnostic modes intentionally evaluate the guest-memory hash.
		uint32 forcedHash = (diagMode == 2) ? LatteTexture_CalculateFullTextureDataHash(hostTexture) : LatteTexture_CalculateTextureDataHash(hostTexture);
		bool hashChanged = (forcedHash != hostTexture->texDataHash2);
		if (hashChanged)
			LatteGPUState.telemetry.skip1280ActuallyChanged.fetch_add(1, std::memory_order_relaxed);
		else
			LatteGPUState.telemetry.skip1280Unchanged.fetch_add(1, std::memory_order_relaxed);

		// Capture first 32 unique skipped 1280 textures
		Latte_RecordUnique1280SkippedTexture(hostTexture, hashChanged);

		// Mode 1 (Check 1280 Width) or Mode 2 (Full Hash Diagnostic): process the change!
		if (hashChanged)
		{
			LatteGPUState.telemetry.cpuDirtyDetected.fetch_add(1, std::memory_order_relaxed);
			hostTexture->texDataHash2 = forcedHash;
			return true;
		}
		return false;
	}

	uint32 texDataHash = (diagMode == 2) ? LatteTexture_CalculateFullTextureDataHash(hostTexture) : LatteTexture_CalculateTextureDataHash(hostTexture);
	if (texDataHash != hostTexture->texDataHash2)
	{
		LatteGPUState.telemetry.cpuDirtyDetected.fetch_add(1, std::memory_order_relaxed);
		hostTexture->texDataHash2 = texDataHash;
		if (hostTexture->depth == 83 && hostTexture->width == 1024 && hostTexture->height == 1024)
		{
			_botwLargeTexHax = LatteGPUState.frameCounter;
		}
		return true;
	}
	if (_botwLargeTexHax != 0 && hostTexture->depth == 83 && hostTexture->width == 1024 && hostTexture->height == 1024 && _botwLargeTexHax != LatteGPUState.frameCounter)
	{
		_botwLargeTexHax = 0;
		return true;
	}
	return false;
}

void LatteTC_ResetTextureChangeTracker(LatteTexture* hostTexture, bool force)
{
	if( hostTexture->lastDataUpdateFrameCounter == LatteGPUState.frameCounter && force == false)
		return;
	hostTexture->lastDataUpdateFrameCounter = LatteGPUState.frameCounter;
	LatteTC_HasTextureChanged(hostTexture, true);
}

/*
 * This function should be called whenever the texture is still used in some form (any kind of access counts)
 * The purpose of this function is to prevent garbage collection of textures that are still actively used
 */
void LatteTC_MarkTextureStillInUse(LatteTexture* texture)
{
	texture->lastAccessTick = LatteGPUState.currentDrawCallTick;
	texture->lastAccessFrameCount = LatteGPUState.frameCounter;
}

// check if a texture has been overwritten by another texture using GPU-writes
bool LatteTC_IsTextureDataOverwritten(LatteTexture* texture)
{
	// check overlaps
	sint32 mipLevels = texture->mipLevels;
	sint32 sliceCount = texture->depth;
	mipLevels = std::min(mipLevels, 3); // only check first 3 mip levels
	for (sint32 mipIndex = 0; mipIndex < mipLevels; mipIndex++)
	{
		sint32 mipSliceCount;
		if (texture->Is3DTexture())
			mipSliceCount = std::max(1, sliceCount >> mipIndex);
		else
			mipSliceCount = sliceCount;
		for (sint32 sliceIndex = 0; sliceIndex < mipSliceCount; sliceIndex++)
		{
			LatteTextureSliceMipInfo* sliceMipInfo = texture->sliceMipInfo + texture->GetSliceMipArrayIndex(sliceIndex, mipIndex);
			bool isSliceMipOutdated = false;
			for (auto& overlapData : sliceMipInfo->list_dataOverlap)
			{
				if (sliceMipInfo->lastDynamicUpdate < overlapData.destMipSliceInfo->lastDynamicUpdate)
				{
					isSliceMipOutdated = true;
					break;
				}
			}
			if (isSliceMipOutdated == false)
				return false;
		}
	}
	return true;
}

void LatteTexture_Delete(LatteTexture* texture)
{
	LatteTexture_PreserveDeferredDynamicReloads(texture);
	LatteTexture_RemovePendingDynamicReload(texture);
	LatteTexture_NotifyLifecycleDiagnostic(LatteTextureLifecycleDiagnosticEvent::CacheEvict, texture);
	LatteGPUState.telemetry.invalidations.fetch_add(1, std::memory_order_relaxed);
	LatteGPUState.telemetry.recreatedBaseInvalidations.fetch_add(1, std::memory_order_relaxed);
	LatteTC_UnregisterTexture(texture);
	LatteMRT::NotifyTextureDeletion(texture);
	LatteTextureReadback_NotifyTextureDeletion(texture);
	LatteTexture_DeleteTextureRelations(texture);
	// delete views
	while (!texture->views.empty())
		delete texture->views[0];
	cemu_assert_debug(texture->views.empty());
	cemu_assert_debug(texture->baseView == nullptr);
	// free data overlap tracking
	LatteTexture_DeleteDataOverlapTracking(texture);
	// remove from lists
	LatteTexture_UnregisterTextureMemoryOccupancy(texture);
	// free memory
	if (texture->sliceMipInfo)
	{
		delete[] texture->sliceMipInfo;
		texture->sliceMipInfo = nullptr;
	}
	delete texture;
}

/*
 * Checks if the texture can be dropped from the cache and if yes, delete it
 * Returns true if the texture was deleted
 */
bool LatteTC_CleanupCheckTexture(LatteTexture* texture, uint32 currentTick)
{
	uint32 currentFrameCount = LatteGPUState.frameCounter;
	uint32 ticksSinceLastAccess = currentTick - texture->lastAccessTick;
	uint32 framesSinceLastAccess = currentFrameCount - texture->lastAccessFrameCount;
	if( !texture->isUpdatedOnGPU )
	{
        // Reconstructable assets from an earlier area need not occupy an old
        // phone's texture cache for two minutes. GPU-written/dynamic/related
        // contents retain the original policy; deletion uses the existing
        // lifecycle path and Metal keeps queued command-buffer references.
        bool constrained = false;
#if BOOST_OS_IOS && defined(ENABLE_METAL)
        constrained = g_renderer->GetType() == RendererAPI::Metal && GetMetalMemoryPolicy().constrained;
#endif
        const bool disposableRAMCopy = !texture->reloadFromDynamicTextures && texture->list_compatibleRelations.empty();
        if (TextureCacheRetention::ShouldEvictRAMCopy(constrained, disposableRAMCopy, ticksSinceLastAccess, framesSinceLastAccess))
		{
            if (constrained && disposableRAMCopy)
                HostUploadMemory::idleRAMTextureEvictions.fetch_add(1, std::memory_order_relaxed);
			LatteTexture_Delete(texture);
			return true;
		}
	}

	if ((LatteGPUState.currentDrawCallTick - texture->lastAccessTick) >= 100 && 
		LatteTC_IsTextureDataOverwritten(texture))
	{
		LatteTexture_Delete(texture);
		return true;
	}
	// if unused for more than 5 seconds, start deleting views since they are cheap to recreate
	if (ticksSinceLastAccess >= 5 * 1000 && framesSinceLastAccess >= 30)
	{
		for (sint32 i = 0; i < 3; i++)
		{
			if (texture->views.size() <= 1)
				break;
			LatteTextureView* view = texture->views[0];
			if (view == texture->baseView)
				view = texture->views[1];
			delete view;
		}
	}
	return false;
}

void LatteTexture_RefreshInfoCache();

static std::atomic_bool s_memoryPressureCleanupPending{false};
static std::atomic_bool s_emergencyCleanupPending{false};
static std::atomic_uint32_t s_memoryPressureCleanupRequestCount{0};
static std::atomic_uint32_t s_memoryPressureCleanupCompletedCount{0};
static std::atomic_uint32_t s_memoryPressureCleanupLastDeletedCount{0};

void LatteTC_RequestMemoryPressureCleanup()
{
	s_memoryPressureCleanupRequestCount.fetch_add(1, std::memory_order_relaxed);
	s_memoryPressureCleanupPending.store(true, std::memory_order_release);
}

void LatteTC_RequestEmergencyMemoryCleanup()
{
	s_emergencyCleanupPending.store(true, std::memory_order_release);
	LatteTC_RequestMemoryPressureCleanup();
}

uint32 LatteTC_GetMemoryPressureCleanupRequestCount()
{
	return s_memoryPressureCleanupRequestCount.load(std::memory_order_relaxed);
}

uint32 LatteTC_GetMemoryPressureCleanupCompletedCount()
{
	return s_memoryPressureCleanupCompletedCount.load(std::memory_order_relaxed);
}

uint32 LatteTC_GetMemoryPressureCleanupLastDeletedCount()
{
	return s_memoryPressureCleanupLastDeletedCount.load(std::memory_order_relaxed);
}

/*
 * Scans for unused textures and deletes them
 * Called at the end of every frame
 */
void LatteTC_CleanupUnusedTextures()
{
	static size_t currentScanIndex = 0;
	if (s_memoryPressureCleanupPending.exchange(false, std::memory_order_acq_rel))
	{
		// This uses the same conservative eligibility rules as the existing cache
		// inspection path: recent textures and non-reconstructable GPU contents
		// remain protected. Keep the work bounded to avoid a long frame stall.
		auto deleteableTextures = LatteTC_GetDeleteableTextures();
		// Emergency (near the real iOS limit) frees everything eligible: a
		// longer frame is better than the process being killed.
		const uint32 maxDeletions = s_emergencyCleanupPending.exchange(false, std::memory_order_acq_rel) ? UINT32_MAX : 64;
		uint32 deletedCount = 0;
		for (LatteTexture* texture : deleteableTextures)
		{
			LatteTexture_Delete(texture);
			if (++deletedCount >= maxDeletions)
				break;
		}
		s_memoryPressureCleanupLastDeletedCount.store(deletedCount, std::memory_order_relaxed);
		s_memoryPressureCleanupCompletedCount.fetch_add(1, std::memory_order_relaxed);
	}

	uint32 currentTick = GetTickCount();
	sint32 maxDelete = 10;
	std::vector<LatteTexture*>& allTextures = LatteTexture::GetAllTextures();
	if (!allTextures.empty())
	{
		for (sint32 c = 0; c < 25; c++)
		{
			if (currentScanIndex >= allTextures.size())
				currentScanIndex = 0;
			LatteTexture* texItr = allTextures[currentScanIndex];
			currentScanIndex++;
			if (!texItr)
				continue;
			if (LatteTC_CleanupCheckTexture(texItr, currentTick))
			{
				maxDelete--;
				if (maxDelete <= 0)
					break; // deleting can be an expensive operation, dont delete too many at once to avoid micro stutter
				if (allTextures.empty())
					break;
			}
		}
	}
	LatteTexture_RefreshInfoCache(); // find a better place to call this from?
}

std::vector<LatteTexture*> LatteTC_GetDeleteableTextures()
{
	std::vector<LatteTexture*> texList;
	uint32 currentFrameCount = LatteGPUState.frameCounter;

	for (auto& itr : g_allTextures)
	{
		if(itr->lastAccessFrameCount == 0)
			continue; // not initialized
		uint32 framesSinceLastAccess = currentFrameCount - itr->lastAccessFrameCount;
		if(framesSinceLastAccess < 3)
			continue;
		if (itr->isUpdatedOnGPU)
		{
			if (LatteTC_IsTextureDataOverwritten(itr))
				texList.emplace_back(itr);
		}
		else
		{
			texList.emplace_back(itr);
		}
	}

	return texList;
}

void LatteTC_UnloadAllTextures()
{
	std::vector<LatteTexture*> allTexturesCopy = LatteTexture::GetAllTextures();
	for (auto& itr : allTexturesCopy)
	{
		if(itr)
			LatteTexture_Delete(itr);
	}
	LatteRenderTarget_unloadAll();
}
