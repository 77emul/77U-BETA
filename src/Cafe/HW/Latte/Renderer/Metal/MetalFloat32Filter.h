#pragma once

// A deliberately bounded fallback: one mip, matching bilinear min/mag, no
// anisotropy, normalized 2D/array samples. Gather performs no float filtering;
// interpolation is float32 ALU. The original sampler retains address modes,
// swizzle, border colour and (for depth) per-texel comparison semantics.
inline constexpr const char* MetalFloat32FilterSource = R"MSL(
float cemuGatherBilinear(float4 taps, float2 f) {
    return mix(mix(taps.w, taps.z, f.x), mix(taps.x, taps.y, f.x), f.y);
}
float4 cemuBilinear32(texture2d<float> tex, sampler s, float2 uv) {
    float2 f = fract(uv * float2(tex.get_width(), tex.get_height()) - 0.5);
    return float4(cemuGatherBilinear(tex.gather(s, uv, int2(0), component::x), f),
                  cemuGatherBilinear(tex.gather(s, uv, int2(0), component::y), f),
                  cemuGatherBilinear(tex.gather(s, uv, int2(0), component::z), f),
                  cemuGatherBilinear(tex.gather(s, uv, int2(0), component::w), f));
}
float4 cemuBilinear32(texture2d_array<float> tex, sampler s, float2 uv, uint slice) {
    float2 f = fract(uv * float2(tex.get_width(), tex.get_height()) - 0.5);
    return float4(cemuGatherBilinear(tex.gather(s, uv, slice, int2(0), component::x), f),
                  cemuGatherBilinear(tex.gather(s, uv, slice, int2(0), component::y), f),
                  cemuGatherBilinear(tex.gather(s, uv, slice, int2(0), component::z), f),
                  cemuGatherBilinear(tex.gather(s, uv, slice, int2(0), component::w), f));
}
float4 cemuBilinear32(depth2d<float> tex, sampler s, float2 uv, float reference) {
    float2 f = fract(uv * float2(tex.get_width(), tex.get_height()) - 0.5);
    return float4(cemuGatherBilinear(tex.gather_compare(s, uv, reference), f));
}
float4 cemuBilinear32(depth2d_array<float> tex, sampler s, float2 uv, uint slice, float reference) {
    float2 f = fract(uv * float2(tex.get_width(), tex.get_height()) - 0.5);
    return float4(cemuGatherBilinear(tex.gather_compare(s, uv, slice, reference), f));
}
)MSL";
