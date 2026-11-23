#pragma once

#define __STRINGIFY(x) #x
#define _STRINGIFY(x) __STRINGIFY(x)

constexpr const char* utilityShaderSource = R"(#include <metal_stdlib>
using namespace metal;

#define GET_BUFFER_BINDING(index) (28 + index)
#define GET_TEXTURE_BINDING(index) (29 + index)
#define GET_SAMPLER_BINDING(index) (14 + index)

constant float2 positions[] = {float2(-1.0, -3.0), float2(-1.0, 1.0), float2(3.0, 1.0)};

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex VertexOut vertexFullscreen(ushort vid [[vertex_id]]) {
    VertexOut out;
    out.position = float4(positions[vid], 0.0, 1.0);
    out.texCoord = positions[vid] * 0.5 + 0.5;
    out.texCoord.y = 1.0 - out.texCoord.y;

    return out;
}

fragment float4 fragmentPresent(VertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler samplr [[sampler(0)]]) {
    return tex.sample(samplr, in.texCoord);
}

vertex VertexOut vertexFullscreenUD(ushort vid [[vertex_id]]) {
    VertexOut out;
    float2 pos = positions[vid];
    out.texCoord = pos * 0.5 + 0.5;
    out.texCoord.y = 1.0 - out.texCoord.y;
    pos.y = -pos.y;
    out.position = float4(pos, 0.0, 1.0);
    return out;
}

// Streamout ranges are 16-byte aligned. Copy a vector per invocation so an
// in-pass copy does not need one vertex invocation for every byte.
vertex void vertexCopyBufferToBuffer(uint vid [[vertex_id]], device uint4* src [[buffer(GET_BUFFER_BINDING(0))]], device uint4* dst [[buffer(GET_BUFFER_BINDING(1))]]) {
    dst[vid] = src[vid];
}

fragment float4 fragmentCopyDepthToColor(VertexOut in [[stage_in]], texture2d<float, access::read> src [[texture(GET_TEXTURE_BINDING(0))]]) {
    return float4(src.read(uint2(in.position.xy)).r, 0.0, 0.0, 0.0);
}

struct CopyDepthOut {
    float depth [[depth(any)]];
};

fragment CopyDepthOut fragmentCopyColorToDepth(VertexOut in [[stage_in]], texture2d<float, access::read> src [[texture(GET_TEXTURE_BINDING(0))]]) {
    return {src.read(uint2(in.position.xy)).r};
}

struct MotionInterpolateParams {
    float2 texelSize;
    float blendFactor;
    float pad;
};

inline float mtl_rgb2luma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

fragment float4 fragmentMotionInterpolate(
    VertexOut in [[stage_in]],
    texture2d<float> currTex [[texture(0)]],
    texture2d<float> prevTex [[texture(1)]],
    sampler samplr [[sampler(0)]],
    constant MotionInterpolateParams& params [[buffer(0)]])
{
    float2 uv = in.texCoord;
    float4 currCol = currTex.sample(samplr, uv);
    float4 prevCol = prevTex.sample(samplr, uv);

    float lumaCurr = mtl_rgb2luma(currCol.rgb);
    float lumaPrev = mtl_rgb2luma(prevCol.rgb);
    float diff = length(currCol.rgb - prevCol.rgb);
    float lumaDiff = abs(lumaCurr - lumaPrev);

    // Static UI / HUD protection: preserve crisp text, health bars, minimap
    if (diff < 0.015 || lumaDiff < 0.008)
        return currCol;

    // Scene transition / teleport / menu cut protection: no interpolation across cuts
    if (diff > 0.65)
        return currCol;

    float baseErr = lumaDiff * 2.0 + diff;
    float minError = baseErr;

    // Stage 1: Coarse Grid Search covering wide 2D motion gamut
    // Prioritizes horizontal camera yaw, plus vertical pitch and diagonals
    const float2 coarseGrid[43] = {
        float2( 0.0,  0.0),
        float2(-36.0,  0.0), float2(-24.0,  0.0), float2(-16.0,  0.0),
        float2(-10.0,  0.0), float2( -6.0,  0.0), float2( -3.0,  0.0),
        float2(  3.0,  0.0), float2(  6.0,  0.0), float2( 10.0,  0.0),
        float2( 16.0,  0.0), float2( 24.0,  0.0), float2( 36.0,  0.0),
        float2(-24.0, -4.0), float2(-24.0,  4.0),
        float2(-14.0, -3.0), float2(-14.0,  3.0),
        float2( -8.0, -2.0), float2( -8.0,  2.0),
        float2(  8.0, -2.0), float2(  8.0,  2.0),
        float2( 14.0, -3.0), float2( 14.0,  3.0),
        float2( 24.0, -4.0), float2( 24.0,  4.0),
        float2(  0.0, -18.0), float2(  0.0, -10.0), float2(  0.0,  -4.0),
        float2(  0.0,   4.0), float2(  0.0,  10.0), float2(  0.0,  18.0),
        float2(-20.0, -10.0), float2(-20.0,  10.0),
        float2( 20.0, -10.0), float2( 20.0,  10.0),
        float2(-10.0,  -6.0), float2(-10.0,   6.0),
        float2( 10.0,  -6.0), float2( 10.0,   6.0),
        float2( -5.0,  -4.0), float2( -5.0,   4.0),
        float2(  5.0,  -4.0), float2(  5.0,   4.0)
    };

    float2 bestCandidate = float2(0.0);
    for (int i = 0; i < 43; ++i)
    {
        float2 v = coarseGrid[i] * params.texelSize;
        float4 colP = prevTex.sample(samplr, uv - 0.5 * v);
        float4 colC = currTex.sample(samplr, uv + 0.5 * v);

        float sLumaP = mtl_rgb2luma(colP.rgb);
        float sLumaC = mtl_rgb2luma(colC.rgb);

        float err = abs(sLumaP - sLumaC) * 2.0 + length(colP.rgb - colC.rgb);
        if (err < minError)
        {
            minError = err;
            bestCandidate = coarseGrid[i];
        }
    }

    // Stage 2: Medium Refinement around best candidate with 1.5-texel steps
    float2 refinedCandidate = bestCandidate;
    for (float dy = -2.0; dy <= 2.0; dy += 2.0)
    {
        for (float dx = -3.0; dx <= 3.0; dx += 1.5)
        {
            if (dx == 0.0 && dy == 0.0) continue;
            float2 cand = bestCandidate + float2(dx, dy);
            float2 v = cand * params.texelSize;
            float4 colP = prevTex.sample(samplr, uv - 0.5 * v);
            float4 colC = currTex.sample(samplr, uv + 0.5 * v);

            float sLumaP = mtl_rgb2luma(colP.rgb);
            float sLumaC = mtl_rgb2luma(colC.rgb);

            float err = abs(sLumaP - sLumaC) * 2.0 + length(colP.rgb - colC.rgb);
            if (err < minError)
            {
                minError = err;
                refinedCandidate = cand;
            }
        }
    }

    // Stage 3: Sub-pixel Refinement (0.5 texel steps) for razor-sharp edge alignment
    float2 finalCandidate = refinedCandidate;
    const float2 subSteps[4] = {
        float2( 0.5,  0.0), float2(-0.5,  0.0),
        float2( 0.0,  0.5), float2( 0.0, -0.5)
    };
    for (int j = 0; j < 4; ++j)
    {
        float2 cand = refinedCandidate + subSteps[j];
        float2 v = cand * params.texelSize;
        float4 colP = prevTex.sample(samplr, uv - 0.5 * v);
        float4 colC = currTex.sample(samplr, uv + 0.5 * v);

        float sLumaP = mtl_rgb2luma(colP.rgb);
        float sLumaC = mtl_rgb2luma(colC.rgb);

        float err = abs(sLumaP - sLumaC) * 2.0 + length(colP.rgb - colC.rgb);
        if (err < minError)
        {
            minError = err;
            finalCandidate = cand;
        }
    }

    float2 bestV = finalCandidate * params.texelSize;

    // High quality motion found along this trajectory
    if (minError < baseErr && length(finalCandidate) >= 0.5)
    {
        float4 finalP = prevTex.sample(samplr, uv - 0.5 * bestV);
        float4 finalC = currTex.sample(samplr, uv + 0.5 * bestV);

        float warpLumaP = mtl_rgb2luma(finalP.rgb);
        float warpLumaC = mtl_rgb2luma(finalC.rgb);
        float warpDiff = abs(warpLumaP - warpLumaC) * 2.0 + length(finalP.rgb - finalC.rgb);

        // Symmetric midpoint blend at t = 0.5
        float4 motionCol = mix(finalP, finalC, 0.5);

        // Confidence: if warped pixels match well (warpDiff low), trust motionCol 100%.
        // If there's an occlusion boundary (warpDiff high), blend towards currCol.
        float confidence = clamp(1.0 - warpDiff / max(baseErr, 0.04), 0.0, 1.0);
        float4 result = mix(currCol, motionCol, confidence);
        result.a = currCol.a;
        return result;
    }
    else
    {
        // Static background or undetected motion: maintain pixel-perfect sharpness
        return currCol;
    }
}
)";
