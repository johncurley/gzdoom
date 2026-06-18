#include <metal_stdlib>
using namespace metal;

// NOTE: The authoritative Metal Bloom shader source is the inline string in
// src/common/rendering/metal/renderer/mt_bloom.cpp (BLOOM_COMPUTE_SOURCE).
// This file is kept in sync as a reference copy.

struct BloomParams {
    float threshold;
    float strength;
    float2 srcRes;
    float2 bloomRes;
    float2 srcScale;
    float2 srcOffset;
    float2 viewportOrigin;
    float sampleWeights[8];
};

struct BloomCompositeParams {
    float strength[4];
    float2 srcRes;
    float2 bloomRes[4];
};

kernel void bloom_extract(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> src [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = params.srcOffset + ((float2(gid) + 0.5) / params.bloomRes) * params.srcScale;
    float4 c = src.sample(s, uv);
    float3 res = max(c.rgb - float3(params.threshold), float3(0.0));
    out.write(float4(res, 1.0), gid);
}

kernel void blur_horizontal(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> inTex [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / params.bloomRes;
    float texelX = 1.0 / params.bloomRes.x;
    float4 sum = float4(0.0);
    sum += inTex.sample(s, uv) * params.sampleWeights[0];
    sum += inTex.sample(s, uv + float2(1.0*texelX, 0.0)) * params.sampleWeights[1];
    sum += inTex.sample(s, uv + float2(-1.0*texelX, 0.0)) * params.sampleWeights[2];
    sum += inTex.sample(s, uv + float2(2.0*texelX, 0.0)) * params.sampleWeights[3];
    sum += inTex.sample(s, uv + float2(-2.0*texelX, 0.0)) * params.sampleWeights[4];
    sum += inTex.sample(s, uv + float2(3.0*texelX, 0.0)) * params.sampleWeights[5];
    sum += inTex.sample(s, uv + float2(-3.0*texelX, 0.0)) * params.sampleWeights[6];
    out.write(sum, gid);
}

kernel void blur_vertical(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> inTex [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / params.bloomRes;
    float texelY = 1.0 / params.bloomRes.y;
    float4 sum = float4(0.0);
    sum += inTex.sample(s, uv) * params.sampleWeights[0];
    sum += inTex.sample(s, uv + float2(0.0, 1.0*texelY)) * params.sampleWeights[1];
    sum += inTex.sample(s, uv + float2(0.0, -1.0*texelY)) * params.sampleWeights[2];
    sum += inTex.sample(s, uv + float2(0.0, 2.0*texelY)) * params.sampleWeights[3];
    sum += inTex.sample(s, uv + float2(0.0, -2.0*texelY)) * params.sampleWeights[4];
    sum += inTex.sample(s, uv + float2(0.0, 3.0*texelY)) * params.sampleWeights[5];
    sum += inTex.sample(s, uv + float2(0.0, -3.0*texelY)) * params.sampleWeights[6];
    out.write(sum, gid);
}

kernel void downsample_box(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> src [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / float2(out.get_width(), out.get_height());
    float4 c = src.sample(s, uv);
    out.write(c, gid);
}

// Compute kernel that writes the bloom contribution (no direct scene writes).
kernel void bloom_combine_contrib(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> bloomTex [[texture(0)]],
    texture2d<float, access::write> outTex [[texture(1)]])
{
    if (gid.x >= outTex.get_width() || gid.y >= outTex.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 bloomUv = (float2(gid) + 0.5) / params.srcRes;
    float4 bloom = bloomTex.sample(s, bloomUv);
    float4 contrib = float4(bloom.rgb * params.strength, 1.0);
    outTex.write(contrib, gid);
}

kernel void bloom_combine_contrib_all(
    uint2 gid [[thread_position_in_grid]],
    constant BloomCompositeParams &params [[buffer(0)]],
    texture2d<float, access::sample> bloom0 [[texture(0)]],
    texture2d<float, access::sample> bloom1 [[texture(1)]],
    texture2d<float, access::sample> bloom2 [[texture(2)]],
    texture2d<float, access::sample> bloom3 [[texture(3)]],
    texture2d<float, access::write> outTex [[texture(4)]])
{
    if (gid.x >= outTex.get_width() || gid.y >= outTex.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 pixel = float2(gid) + 0.5;
    float2 uv = pixel / params.srcRes;
    float3 sum = float3(0.0);
    if (params.strength[0] > 0.0) sum += bloom0.sample(s, uv).rgb * params.strength[0];
    if (params.strength[1] > 0.0) sum += bloom1.sample(s, uv).rgb * params.strength[1];
    if (params.strength[2] > 0.0) sum += bloom2.sample(s, uv).rgb * params.strength[2];
    if (params.strength[3] > 0.0) sum += bloom3.sample(s, uv).rgb * params.strength[3];
    outTex.write(float4(sum, 1.0), gid);
}

// Compute kernel that writes directly into the scene render target (RW BGRA8) on Tier 2 devices.
kernel void bloom_combine_rw(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::read_write> sceneTex [[texture(0)]],
    texture2d<float, access::sample> bloomTex [[texture(1)]])
{
    uint2 sceneGid = gid + uint2(params.viewportOrigin.x, params.viewportOrigin.y);
    if (gid.x >= uint(params.srcRes.x) || gid.y >= uint(params.srcRes.y) ||
        sceneGid.x >= sceneTex.get_width() || sceneGid.y >= sceneTex.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 bloomUv = (float2(gid) + 0.5) / params.srcRes;
    float4 bloom = bloomTex.sample(s, bloomUv);
    float4 scene = sceneTex.read(sceneGid);
    scene.rgb += bloom.rgb * params.strength;
    sceneTex.write(scene, sceneGid);
}

// Fullscreen triangle vertex and composite fragment shader
struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut bloom_vs(uint vid [[vertex_id]]) {
    VSOut out;
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    out.position = float4(pos[vid], 0.0, 1.0);
    // uv in 0..1
    out.uv = out.position.xy * 0.5 + float2(0.5);
    return out;
}

fragment float4 bloom_fs(VSOut in [[stage_in]],
                         constant BloomParams &params [[buffer(0)]],
                         texture2d<float, access::sample> bloomTex [[texture(0)]],
                         sampler samp [[sampler(0)]]) {
    float4 bloom = bloomTex.sample(samp, in.uv);
    // Output bloom contribution; additive blending will be used when rendering.
    return float4(bloom.rgb * params.strength, 1.0);
}
