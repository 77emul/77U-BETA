#include "Cafe/HW/Latte/Renderer/Metal/MetalSamplerCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"

MTL::SamplerBorderColor GetBorderColor(LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords, bool logWorkaround = false)
{
    auto borderType = samplerWords->WORD0.get_BORDER_COLOR_TYPE();

    MTL::SamplerBorderColor borderColor;
    if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::TRANSPARENT_BLACK)
        borderColor = MTL::SamplerBorderColorTransparentBlack;
    else if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::OPAQUE_BLACK)
        borderColor = MTL::SamplerBorderColorOpaqueBlack;
    else if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::OPAQUE_WHITE)
        borderColor = MTL::SamplerBorderColorOpaqueWhite;
    else [[unlikely]]
    {
        stageSamplerIndex = std::min(stageSamplerIndex, static_cast<uint32>(Latte::GPU_LIMITS::NUM_SAMPLERS_PER_STAGE - 1));
        _LatteRegisterSetSamplerBorderColor* borderColorReg;
		if (shaderType == LatteConst::ShaderType::Vertex)
			borderColorReg = LatteGPUState.contextNew.TD_VS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
		else if (shaderType == LatteConst::ShaderType::Pixel)
			borderColorReg = LatteGPUState.contextNew.TD_PS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
		else // geometry
			borderColorReg = LatteGPUState.contextNew.TD_GS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
		float r = borderColorReg->red.get_channelValue();
		float g = borderColorReg->green.get_channelValue();
		float b = borderColorReg->blue.get_channelValue();
		float a = borderColorReg->alpha.get_channelValue();

		// Metal doesn't support custom border color
		// Let's find the best match
		bool opaque = (a == 1.0f);
		bool white = (r == 1.0f);
		if (opaque)
		{
		    if (white)
                borderColor = MTL::SamplerBorderColorOpaqueWhite;
            else
                borderColor = MTL::SamplerBorderColorOpaqueBlack;
		}
		else
		{
		    borderColor = MTL::SamplerBorderColorTransparentBlack;
		}

		if (logWorkaround)
		{
		    float newR, newG, newB, newA;
			switch (borderColor)
			{
			case MTL::SamplerBorderColorTransparentBlack:
                newR = 0.0f;
                newG = 0.0f;
                newB = 0.0f;
                newA = 0.0f;
                break;
            case MTL::SamplerBorderColorOpaqueBlack:
                newR = 0.0f;
                newG = 0.0f;
                newB = 0.0f;
                newA = 1.0f;
                break;
            case MTL::SamplerBorderColorOpaqueWhite:
                newR = 1.0f;
                newG = 1.0f;
                newB = 1.0f;
                newA = 1.0f;
                break;
            }

            if (r != newR || g != newG || b != newB || a != newA)
                cemuLog_log(LogType::APIErrors, "Custom border color ({}, {}, {}, {}) is not supported on Metal, using ({}, {}, {}, {}) instead", r, g, b, a, newR, newG, newB, newA);
		}
    }

    return borderColor;
}

MetalSamplerCache::~MetalSamplerCache()
{
    for (auto& pair : m_samplerCache)
    {
        pair.second->release();
    }
    m_samplerCache.clear();
    m_lastSamplerKey = {0, false};
    m_lastSamplerState = nullptr;
    m_fastCache = {};
    m_stageSlots = {};
}

MTL::SamplerState* MetalSamplerCache::GetSamplerState(const LatteContextRegister& lcr, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords, bool depthCompare, bool supportArgumentBuffers, sint32 lodBias)
{
    const size_t stageIdx = static_cast<size_t>(shaderType);
    if (stageIdx < m_stageSlots.size() && stageSamplerIndex < Latte::GPU_LIMITS::NUM_SAMPLERS_PER_STAGE)
    {
        auto& slot = m_stageSlots[stageIdx][stageSamplerIndex];
        const uint32 w0 = samplerWords->WORD0.getRawValue();
        const uint32 w1 = samplerWords->WORD1.getRawValue();
        const uint32 w2 = samplerWords->WORD2.getRawValue();
        if (slot.valid &&
            slot.word0 == w0 &&
            slot.word1 == w1 &&
            slot.word2 == w2 &&
            slot.lodBias == lodBias &&
            slot.depthCompare == depthCompare &&
            slot.supportArgumentBuffers == supportArgumentBuffers &&
            samplerWords->WORD0.get_BORDER_COLOR_TYPE() != Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::REGISTER)
        {
            return slot.state;
        }
    }

    auto updateSlot = [&](MTL::SamplerState* state) {
        if (stageIdx < m_stageSlots.size() && stageSamplerIndex < Latte::GPU_LIMITS::NUM_SAMPLERS_PER_STAGE)
        {
            auto& slot = m_stageSlots[stageIdx][stageSamplerIndex];
            slot.word0 = samplerWords->WORD0.getRawValue();
            slot.word1 = samplerWords->WORD1.getRawValue();
            slot.word2 = samplerWords->WORD2.getRawValue();
            slot.lodBias = lodBias;
            slot.depthCompare = depthCompare;
            slot.supportArgumentBuffers = supportArgumentBuffers;
            slot.state = state;
            slot.valid = true;
        }
    };

    uint64 stateHash = CalculateSamplerHash(lcr, shaderType, stageSamplerIndex, samplerWords, depthCompare, lodBias);
    const std::pair<uint64, bool> key{stateHash, supportArgumentBuffers};
    if (key == m_lastSamplerKey && m_lastSamplerState)
    {
        updateSlot(m_lastSamplerState);
        return m_lastSamplerState;
    }

    // Draws cycle through a small set of samplers. Avoid a red-black-tree
    // lookup for each texture binding in that common case.
    auto& fast = m_fastCache[(stateHash ^ (stateHash >> 32) ^
        (supportArgumentBuffers ? 0x9e3779b9u : 0u)) & (m_fastCache.size() - 1)];
    if (fast.state && fast.key == key)
    {
        m_lastSamplerKey = key;
        m_lastSamplerState = fast.state;
        updateSlot(fast.state);
        return fast.state;
    }

    auto& samplerState = m_samplerCache[key];
    if (samplerState)
    {
        m_lastSamplerKey = key;
        m_lastSamplerState = samplerState;
        fast = {key, samplerState};
        updateSlot(samplerState);
        return samplerState;
    }

	// Sampler state

    NS_STACK_SCOPED MTL::SamplerDescriptor* samplerDescriptor = MTL::SamplerDescriptor::alloc()->init();
    samplerDescriptor->setSupportArgumentBuffers(supportArgumentBuffers);

    // lod
    uint32 iMinLOD = samplerWords->WORD1.get_MIN_LOD();
    uint32 iMaxLOD = samplerWords->WORD1.get_MAX_LOD();
#if TARGET_OS_IPHONE || TARGET_OS_IOS
    // Metal exposes sampler LOD bias on iPadOS 26 and later.
    samplerDescriptor->setLodBias(static_cast<float>(lodBias) / 64.0f);
#endif

    float minClamp = (float)iMinLOD / 64.0f;
    float maxClamp = (float)iMaxLOD / 64.0f;
    if (minClamp > maxClamp)
        std::swap(minClamp, maxClamp);

    auto filterMip = samplerWords->WORD0.get_MIP_FILTER();
    if (filterMip == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::NONE)
    {
        samplerDescriptor->setMipFilter(MTL::SamplerMipFilterNearest);
        samplerDescriptor->setLodMinClamp(0.0f);
        samplerDescriptor->setLodMaxClamp(0.25f);
    }
    else if (filterMip == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::POINT)
    {
        samplerDescriptor->setMipFilter(MTL::SamplerMipFilterNearest);
        samplerDescriptor->setLodMinClamp(minClamp);
        samplerDescriptor->setLodMaxClamp(maxClamp);
    }
    else
    {
        samplerDescriptor->setMipFilter(MTL::SamplerMipFilterLinear);
        samplerDescriptor->setLodMinClamp(minClamp);
        samplerDescriptor->setLodMaxClamp(maxClamp);
    }

    auto filterMin = samplerWords->WORD0.get_XY_MIN_FILTER();
    cemu_assert_debug(filterMin != Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::BICUBIC); // todo
    samplerDescriptor->setMinFilter((filterMin == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT || filterMin == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_POINT) ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear);

    auto filterMag = samplerWords->WORD0.get_XY_MAG_FILTER();
    samplerDescriptor->setMagFilter((filterMag == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT || filterMag == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_POINT) ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear);

    auto filterZ = samplerWords->WORD0.get_Z_FILTER();
    // todo: z-filter for texture array samplers is customizable for GPU7 but OpenGL/Vulkan doesn't expose this functionality?

    auto clampX = samplerWords->WORD0.get_CLAMP_X();
    auto clampY = samplerWords->WORD0.get_CLAMP_Y();
    auto clampZ = samplerWords->WORD0.get_CLAMP_Z();

    auto modeX = GetMtlSamplerAddressMode(clampX);
    if (!m_mtlr->SupportsSamplerBorderColor() && modeX == MTL::SamplerAddressModeClampToBorderColor)
        modeX = MTL::SamplerAddressModeClampToZero;
    samplerDescriptor->setSAddressMode(modeX);
    auto modeY = GetMtlSamplerAddressMode(clampY);
    if (!m_mtlr->SupportsSamplerBorderColor() && modeY == MTL::SamplerAddressModeClampToBorderColor)
        modeY = MTL::SamplerAddressModeClampToZero;
    samplerDescriptor->setTAddressMode(modeY);
    auto modeZ = GetMtlSamplerAddressMode(clampZ);
    if (!m_mtlr->SupportsSamplerBorderColor() && modeZ == MTL::SamplerAddressModeClampToBorderColor)
        modeZ = MTL::SamplerAddressModeClampToZero;
    samplerDescriptor->setRAddressMode(modeZ);

    auto maxAniso = samplerWords->WORD0.get_MAX_ANISO_RATIO();
    if (maxAniso > 0)
    {
        uint32 aniso = 1u << maxAniso;
        samplerDescriptor->setMaxAnisotropy(std::clamp<uint32>(aniso, 1, 16));
    }

    // depth compare
    if (depthCompare)
    {
        samplerDescriptor->setCompareFunction(GetMtlCompareFunc((Latte::E_COMPAREFUNC)samplerWords->WORD0.get_DEPTH_COMPARE_FUNCTION()));
    }
    else
    {
        samplerDescriptor->setCompareFunction(MTL::CompareFunctionNever);
    }

    // Border color
    auto borderColor = GetBorderColor(shaderType, stageSamplerIndex, samplerWords, false);
    if (m_mtlr->SupportsSamplerBorderColor()) samplerDescriptor->setBorderColor(borderColor);
    else if ((GetMtlSamplerAddressMode(clampX) == MTL::SamplerAddressModeClampToBorderColor ||
              GetMtlSamplerAddressMode(clampY) == MTL::SamplerAddressModeClampToBorderColor ||
              GetMtlSamplerAddressMode(clampZ) == MTL::SamplerAddressModeClampToBorderColor) &&
             borderColor != MTL::SamplerBorderColorTransparentBlack) {
        cemuLog_logOnce(LogType::Force, "Apple6 opaque sampler border is unsupported; compatibility test failed");
    }

    samplerState = m_mtlr->GetDevice()->newSamplerState(samplerDescriptor);
    if (!samplerState)
    {
        cemuLog_log(LogType::Force, "MetalSamplerCache: newSamplerState failed, falling back to nearest");
        m_samplerCache.erase({stateHash, supportArgumentBuffers});
        return m_mtlr->GetNearestSampler();
    }

    m_lastSamplerKey = key;
    m_lastSamplerState = samplerState;
    fast = {key, samplerState};
    updateSlot(samplerState);

    return samplerState;
}

uint64 MetalSamplerCache::CalculateSamplerHash(const LatteContextRegister& lcr, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords, bool depthCompare, sint32 lodBias)
{
    uint64 hash = 0;
    hash = std::rotl<uint64>(hash, 17);
    hash += (uint64)samplerWords->WORD0.getRawValue();
    hash = std::rotl<uint64>(hash, 17);
    hash += (uint64)samplerWords->WORD1.getRawValue();
    hash = std::rotl<uint64>(hash, 17);
    hash += (uint64)samplerWords->WORD2.getRawValue();
    hash = std::rotl<uint64>(hash, 17);
    hash += static_cast<uint32>(lodBias);

    auto borderColor = GetBorderColor(shaderType, stageSamplerIndex, samplerWords);

    hash = std::rotl<uint64>(hash, 5);
    hash += (uint64)borderColor;

    hash = std::rotl<uint64>(hash, 1);
    hash += (depthCompare ? 1 : 0);

	return hash;
}
