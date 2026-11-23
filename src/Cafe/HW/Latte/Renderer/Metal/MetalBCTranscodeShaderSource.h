#pragma once

// GPUs without BC support (Apple7/8: A14-A16) can still sample ETC2/EAC.
// Expanding BC to RGBA8 costs 4-8x the guest texture memory, which does not
// fit 4 GiB devices. This kernel transcodes each 4x4 BC block into an ETC2/EAC
// block of the same footprint on the GPU (one thread per block), so textures
// stay compressed and the CPU does no per-pixel work.
//
// format: 0 BC1, 1 BC2, 2 BC3 -> EAC_RGBA8 (16 bytes: EAC alpha + ETC2 color)
//         3 BC4 unorm -> EAC_R11Unorm, 4 BC4 snorm -> EAC_R11Snorm (8 bytes)
//         5 BC5 unorm -> EAC_RG11Unorm, 6 BC5 snorm -> EAC_RG11Snorm (16 bytes)
// BC decoding matches MetalBCFallback.cpp bit for bit.

inline const char* bcTranscodeShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct BCTranscodeParams
{
    uint blocksWide;
    uint blocksHigh;
    uint format;
    uint width;   // visible texels; edge blocks replicate them into hidden slots
    uint height;
    uint reserved[3];
};

constant int kEACTables[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12}, {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},  {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},   {-3, -5, -7, -9, 2, 4, 6, 8}};

constant int kETCTables[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};

static uint Read16(device const uchar* p) { return uint(p[0]) | (uint(p[1]) << 8); }
static uint Read32(device const uchar* p) { return Read16(p) | (Read16(p + 2) << 16); }

static int3 Decode565(uint c)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return int3((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

// Pixels are row-major (y * 4 + x), as in the CPU decoder.
static void DecodeColorBlock(device const uchar* block, bool allowTransparency, thread int4 (&px)[16])
{
    const uint c0 = Read16(block), c1 = Read16(block + 2);
    int4 palette[4];
    palette[0] = int4(Decode565(c0), 255);
    palette[1] = int4(Decode565(c1), 255);
    const bool threeColor = allowTransparency && c0 <= c1;
    if (threeColor)
    {
        palette[2] = int4((palette[0].rgb + palette[1].rgb) / 2, 255);
        palette[3] = int4(0, 0, 0, 0);
    }
    else
    {
        palette[2] = int4((2 * palette[0].rgb + palette[1].rgb + 1) / 3, 255);
        palette[3] = int4((palette[0].rgb + 2 * palette[1].rgb + 1) / 3, 255);
    }
    const uint indices = Read32(block + 4);
    for (uint i = 0; i < 16; ++i)
        px[i] = palette[(indices >> (2 * i)) & 3];
}

static void DecodeChannelBlock(device const uchar* block, bool isSigned, thread int (&values)[16])
{
    int e0 = isSigned ? int(as_type<char>(block[0])) : int(block[0]);
    int e1 = isSigned ? int(as_type<char>(block[1])) : int(block[1]);
    if (isSigned) { e0 = max(e0, -127); e1 = max(e1, -127); }
    int palette[8];
    palette[0] = e0; palette[1] = e1;
    if (e0 > e1)
    {
        for (int i = 2; i < 8; ++i) palette[i] = ((8 - i) * e0 + (i - 1) * e1) / 7;
    }
    else
    {
        for (int i = 2; i < 6; ++i) palette[i] = ((6 - i) * e0 + (i - 1) * e1) / 5;
        palette[6] = isSigned ? -127 : 0;
        palette[7] = isSigned ? 127 : 255;
    }
    ulong indices = 0;
    for (uint b = 0; b < 6; ++b) indices |= ulong(block[2 + b]) << (8 * b);
    for (uint i = 0; i < 16; ++i) values[i] = palette[(indices >> (3 * i)) & 7];
}

// EAC block for 16 target values. scale/offset describe the decoder:
// value = clamp(base * scale + offset + modifier * multiplier * scale, lo, hi)
// alpha8: scale 1, offset 0, base 0..255; R11 unorm: scale 8, offset 4,
// base 0..255; R11 snorm: scale 8, offset 0, base -127..127.
static void EncodeEAC(thread const int (&target)[16], int scale, int offset, int baseMin, int baseMax,
                      int lo, int hi, device uchar* out)
{
    int vmin = target[0], vmax = target[0];
    for (uint i = 1; i < 16; ++i) { vmin = min(vmin, target[i]); vmax = max(vmax, target[i]); }
    // Constant block (opaque alpha, flat channels): table 13 has a 0 modifier
    // at index 4, so a nearest base reproduces the value directly.
    if (vmin == vmax)
    {
        const int base = clamp(int(round(float(vmin - offset) / float(scale))), baseMin, baseMax);
        int best = base, bestError = 0x7fffffff;
        for (int b = max(baseMin, base - 1); b <= min(baseMax, base + 1); ++b)
        {
            const int e = abs(clamp(b * scale + offset, lo, hi) - vmin);
            if (e < bestError) { bestError = e; best = b; }
        }
        out[0] = uchar(best & 0xff);
        out[1] = uchar((1 << 4) | 13);
        const ulong bits = 0x924924924924ul; // every 3-bit index = 4
        for (uint b = 0; b < 6; ++b) out[2 + b] = uchar(bits >> (8 * (5 - b)));
        return;
    }

    uint bestError = 0xffffffffu;
    int bestBase = 0, bestMult = 1, bestTable = 0;
    for (int t = 0; t < 16 && bestError; ++t)
    {
        const int tmin = kEACTables[t][3], tmax = kEACTables[t][7];
        const float span = float((tmax - tmin) * scale);
        // ceil covers the full range so extremes (alpha 0/255) stay reachable;
        // round is often more precise in between.
        const int coverMult = clamp(int(ceil(float(vmax - vmin) / span)), 1, 15);
        const int nearMult = clamp(int(round(float(vmax - vmin) / span)), 1, 15);
        for (uint candidate = 0; candidate < 2; ++candidate)
        {
            const int m = candidate ? nearMult : coverMult;
            if (candidate && m == coverMult)
                break;
            const float center = 0.5f * float(vmax + vmin) - 0.5f * float((tmax + tmin) * m * scale);
            const int base = clamp(int(round((center - float(offset)) / float(scale))), baseMin, baseMax);
            int levels[8];
            for (uint k = 0; k < 8; ++k)
                levels[k] = clamp(base * scale + offset + kEACTables[t][k] * m * scale, lo, hi);
            uint error = 0;
            for (uint i = 0; i < 16 && error < bestError; ++i)
            {
                uint best = 0xffffffffu;
                for (uint k = 0; k < 8; ++k)
                    best = min(best, uint((levels[k] - target[i]) * (levels[k] - target[i])));
                error += best;
            }
            if (error < bestError) { bestError = error; bestBase = base; bestMult = m; bestTable = t; }
        }
    }
    int levels[8];
    for (uint k = 0; k < 8; ++k)
        levels[k] = clamp(bestBase * scale + offset + kEACTables[bestTable][k] * bestMult * scale, lo, hi);
    // Indices are column-major (x * 4 + y), most significant pixel first.
    ulong bits = 0;
    for (uint x = 0; x < 4; ++x)
        for (uint y = 0; y < 4; ++y)
        {
            const int v = target[y * 4 + x];
            uint best = 0xffffffffu, index = 0;
            for (uint k = 0; k < 8; ++k)
            {
                const uint e = uint((levels[k] - v) * (levels[k] - v));
                if (e < best) { best = e; index = k; }
            }
            bits = (bits << 3) | index;
        }
    out[0] = uchar(bestBase & 0xff);
    out[1] = uchar((bestMult << 4) | bestTable);
    for (uint b = 0; b < 6; ++b) out[2 + b] = uchar(bits >> (8 * (5 - b)));
}

// Pixel i of subblock s for a given flip (row-major pixel index).
static uint SubblockPixel(uint flip, uint s, uint i)
{
    return flip ? (s * 8 + i) : ((i >> 1) * 4 + s * 2 + (i & 1));
}

// Best ETC table and indices for one subblock with a fixed base color. With
// d = pixel - base, adding modifier m to all channels costs
// |d|^2 - 2 m (dx + dy + dz) + 3 m^2 (clamping ignored during the search).
static uint FitETCSubblock(thread const int4 (&px)[16], int3 base, uint flip, uint subblock,
                           thread uint& table, thread uint (&idx)[16])
{
    int sums[8], squares[8];
    for (uint i = 0; i < 8; ++i)
    {
        const int3 d = px[SubblockPixel(flip, subblock, i)].rgb - base;
        sums[i] = d.x + d.y + d.z;
        squares[i] = d.x * d.x + d.y * d.y + d.z * d.z;
    }
    uint bestError = 0xffffffffu;
    for (uint t = 0; t < 8; ++t)
    {
        const int a = kETCTables[t][0], b = kETCTables[t][1];
        uint error = 0;
        for (uint i = 0; i < 8; ++i)
        {
            const int s = sums[i];
            const int e0 = -2 * a * s + 3 * a * a, e1 = -2 * b * s + 3 * b * b;
            const int e2 = 2 * a * s + 3 * a * a, e3 = 2 * b * s + 3 * b * b;
            error += uint(squares[i] + min(min(e0, e1), min(e2, e3)));
        }
        if (error < bestError) { bestError = error; table = t; }
    }
    const int a = kETCTables[table][0], b = kETCTables[table][1];
    for (uint i = 0; i < 8; ++i)
    {
        const int s = sums[i];
        const int e[4] = {-2 * a * s + 3 * a * a, -2 * b * s + 3 * b * b, 2 * a * s + 3 * a * a, 2 * b * s + 3 * b * b};
        uint k = 0;
        for (uint j = 1; j < 4; ++j)
            if (e[j] < e[k]) k = j;
        idx[SubblockPixel(flip, subblock, i)] = k;
    }
    return bestError;
}

// ETC1-compatible ETC2 color block (individual or differential mode).
static void EncodeETC2Color(thread const int4 (&px)[16], device uchar* out)
{
    uint bestError = 0xffffffffu;
    uint bestBytes[4] = {0, 0, 0, 0};
    uint bestIdx[16];
    for (uint flip = 0; flip < 2; ++flip)
    {
        float3 sum[2] = {float3(0), float3(0)};
        for (uint i = 0; i < 16; ++i)
        {
            const uint sub = flip ? ((i >> 2) >> 1) : ((i & 3) >> 1);
            sum[sub] += float3(px[i].rgb);
        }
        const float3 avg0 = sum[0] / 8.0f, avg1 = sum[1] / 8.0f;
        for (uint differential = 0; differential < 2; ++differential)
        {
            int3 base0, base1;
            uint bytes[3];
            if (differential)
            {
                const int3 q0 = clamp(int3(round(avg0 * 31.0f / 255.0f)), 0, 31);
                const int3 q1 = clamp(int3(round(avg1 * 31.0f / 255.0f)), 0, 31);
                const int3 delta = q1 - q0;
                if (any(delta < -4) || any(delta > 3))
                    continue; // individual mode covers this block
                base0 = (q0 << 3) | (q0 >> 2);
                base1 = (q1 << 3) | (q1 >> 2);
                for (uint c = 0; c < 3; ++c) bytes[c] = uint(q0[c] << 3) | uint(delta[c] & 7);
            }
            else
            {
                const int3 q0 = clamp(int3(round(avg0 * 15.0f / 255.0f)), 0, 15);
                const int3 q1 = clamp(int3(round(avg1 * 15.0f / 255.0f)), 0, 15);
                base0 = q0 * 17;
                base1 = q1 * 17;
                for (uint c = 0; c < 3; ++c) bytes[c] = uint(q0[c] << 4) | uint(q1[c]);
            }
            uint table0 = 0, table1 = 0;
            uint idx[16];
            const uint error = FitETCSubblock(px, base0, flip, 0, table0, idx) +
                               FitETCSubblock(px, base1, flip, 1, table1, idx);
            if (error < bestError)
            {
                bestError = error;
                bestBytes[0] = bytes[0]; bestBytes[1] = bytes[1]; bestBytes[2] = bytes[2];
                bestBytes[3] = (table0 << 5) | (table1 << 2) | (differential << 1) | flip;
                for (uint i = 0; i < 16; ++i) bestIdx[i] = idx[i];
            }
        }
    }
    // Modifier order +a, +b, -a, -b is index (msb << 1) | lsb.
    uint indexBits = 0;
    for (uint i = 0; i < 16; ++i)
    {
        const uint x = i & 3, y = i >> 2, p = x * 4 + y;
        indexBits |= ((bestIdx[i] >> 1) << (16 + p)) | ((bestIdx[i] & 1) << p);
    }
    for (uint b = 0; b < 4; ++b) out[b] = uchar(bestBytes[b]);
    for (uint b = 0; b < 4; ++b) out[4 + b] = uchar(indexBits >> (8 * (3 - b)));
}

template<typename T>
static void ReplicateEdge(thread T (&values)[16], uint visibleX, uint visibleY)
{
    if (visibleX >= 4 && visibleY >= 4)
        return;
    visibleX = max(visibleX, 1u);
    visibleY = max(visibleY, 1u);
    for (uint y = 0; y < 4; ++y)
        for (uint x = 0; x < 4; ++x)
            values[y * 4 + x] = values[min(y, visibleY - 1) * 4 + min(x, visibleX - 1)];
}

constant uint cemuBCFormat [[function_constant(0)]];

kernel void transcodeBCToETC(device const uchar* source [[buffer(0)]],
                             device uchar* destination [[buffer(1)]],
                             constant BCTranscodeParams& params [[buffer(2)]],
                             uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.blocksWide || gid.y >= params.blocksHigh)
        return;
    const uint block = gid.y * params.blocksWide + gid.x;
    const uint format = is_function_constant_defined(cemuBCFormat) ? cemuBCFormat : params.format;
    // Hidden texels of edge blocks copy a visible neighbour so the encoder
    // spends its precision on what is sampled.
    const uint visibleX = min(4u, params.width - min(params.width, gid.x * 4));
    const uint visibleY = min(4u, params.height - min(params.height, gid.y * 4));
    if (format <= 2)
    {
        device const uchar* in = source + block * (format == 0 ? 8u : 16u);
        device uchar* out = destination + block * 16u;
        int4 px[16];
        DecodeColorBlock(in + (format == 0 ? 0 : 8), format == 0, px);
        ReplicateEdge(px, visibleX, visibleY);
        int alpha[16];
        if (format == 1)
        {
            for (uint i = 0; i < 16; ++i) alpha[i] = int((in[i / 2] >> (4 * (i & 1))) & 15) * 17;
        }
        else if (format == 2)
        {
            DecodeChannelBlock(in, false, alpha);
        }
        else
        {
            for (uint i = 0; i < 16; ++i) alpha[i] = px[i].a;
        }
        ReplicateEdge(alpha, visibleX, visibleY);
        EncodeEAC(alpha, 1, 0, 0, 255, 0, 255, out);
        EncodeETC2Color(px, out + 8);
        return;
    }
    const bool isSigned = format == 4 || format == 6;
    const uint channels = format <= 4 ? 1u : 2u;
    device const uchar* in = source + block * (channels * 8u);
    device uchar* out = destination + block * (channels * 8u);
    for (uint c = 0; c < channels; ++c)
    {
        int values[16];
        DecodeChannelBlock(in + c * 8u, isSigned, values);
        ReplicateEdge(values, visibleX, visibleY);
        // Convert 8-bit normalized values to the 11-bit EAC domain.
        for (uint i = 0; i < 16; ++i)
            values[i] = isSigned ? int(round(float(values[i]) * 1023.0f / 127.0f))
                                 : int(round(float(values[i]) * 2047.0f / 255.0f));
        if (isSigned)
            EncodeEAC(values, 8, 0, -127, 127, -1023, 1023, out + c * 8u);
        else
            EncodeEAC(values, 8, 4, 0, 255, 0, 2047, out + c * 8u);
    }
}
)";
