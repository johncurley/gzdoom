#include <metal_stdlib>
using namespace metal;

// NOTE: This file is the authoritative Metal Bloom shader source. It is
// compiled into native_shaders.metallib (CMake target metal_native_shaders)
// and loaded by MtShaderManager::LoadNativeLibrary() before any inline
// string. The matching BLOOM_COMPUTE_SOURCE inline string in
// src/common/rendering/metal/renderer/mt_bloom.cpp is only a fallback used
// if the metallib can't be found -- keep it in sync with this file, not the
// other way around. (Same fix already applied to mt_ao.metal 2026-07-10;
// verified 2026-07-14 via tools/check_shader_parity.py that this pair's
// kernel bodies were never actually drifted, only this comment's authority
// direction was wrong.)

struct BloomParams {
    float threshold;
    float strength;
    float2 srcRes;
    float2 bloomRes;
    float2 srcScale;
    float2 srcOffset;
    float2 viewportOrigin;
    // Nonzero when an exposure texture is bound to bloom_extract. When zero
    // the extract runs unexposed, which is only correct if the camera
    // exposure pass did not run this frame.
    float useExposure;
    float sampleWeights[8];
};

struct BloomCompositeParams {
    float strength[4];
    float2 srcRes;
    float2 bloomRes[4];
    float2 viewportOrigin;
};

kernel void bloom_extract(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> src [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]],
    texture2d<float, access::sample> exposureTex [[texture(2)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = params.srcOffset + ((float2(gid) + 0.5) / params.bloomRes) * params.srcScale;
    float4 c = src.sample(s, uv);

    // Match PP bloom extract (shaders/pp/bloomextract.fp) exactly:
    //     max((color + 0.001) * exposureAdjustment - threshold, 0)
    // The bias is applied *before* the exposure multiply and the threshold
    // subtraction *after*, which is not the same as scaling the threshold:
    // the reference scales the bias too. Getting this order wrong changes
    // which pixels survive the extract at all.
    //
    // Omitting the exposure term made compute bloom measurably dimmer than
    // the reference path in dark scenes (verified 2026-07-27: with exposure
    // forced off the two paths produce byte-identical frames, and with it
    // live the compute path was darker across ~3.8% of the frame around
    // light sources, peak -7 luminance).
    float exposureAdjustment = params.useExposure != 0.0
        ? exposureTex.sample(s, float2(0.5)).x
        : 1.0;
    float3 res = max((c.rgb + float3(0.001)) * exposureAdjustment - float3(params.threshold),
                     float3(0.0));
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

// Tier 2: combine every bloom level and update the scene in one read-write pass.
kernel void bloom_combine_rw_all(
    uint2 gid [[thread_position_in_grid]],
    constant BloomCompositeParams &params [[buffer(0)]],
    texture2d<float, access::read_write> sceneTex [[texture(0)]],
    texture2d<float, access::sample> bloom0 [[texture(1)]],
    texture2d<float, access::sample> bloom1 [[texture(2)]],
    texture2d<float, access::sample> bloom2 [[texture(3)]],
    texture2d<float, access::sample> bloom3 [[texture(4)]])
{
    uint2 sceneGid = gid + uint2(params.viewportOrigin.x, params.viewportOrigin.y);
    if (gid.x >= uint(params.srcRes.x) || gid.y >= uint(params.srcRes.y) ||
        sceneGid.x >= sceneTex.get_width() || sceneGid.y >= sceneTex.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / params.srcRes;
    float3 bloom = float3(0.0);
    if (params.strength[0] > 0.0) bloom += bloom0.sample(s, uv).rgb * params.strength[0];
    if (params.strength[1] > 0.0) bloom += bloom1.sample(s, uv).rgb * params.strength[1];
    if (params.strength[2] > 0.0) bloom += bloom2.sample(s, uv).rgb * params.strength[2];
    if (params.strength[3] > 0.0) bloom += bloom3.sample(s, uv).rgb * params.strength[3];
    float4 scene = sceneTex.read(sceneGid);
    scene.rgb += bloom;
    sceneTex.write(scene, sceneGid);
}

struct BloomVSOut {
    float4 position [[position]];
    float2 uv;
};

vertex BloomVSOut bloom_vs(uint vid [[vertex_id]]) {
    BloomVSOut out;
    float2 pos[3] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)
    };
    out.position = float4(pos[vid], 0.0, 1.0);
    out.uv = out.position.xy * 0.5 + float2(0.5);
    return out;
}

fragment float4 bloom_fs(BloomVSOut in [[stage_in]],
                         texture2d<float, access::sample> bloomTex [[texture(0)]],
                         sampler samp [[sampler(0)]]) {
    return float4(bloomTex.sample(samp, in.uv).rgb, 0.0);
}
