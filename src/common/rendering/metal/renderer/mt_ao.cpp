#include "mt_ao.h"
#include "c_cvars.h"
#include "mt_renderbuffers.h"
#include "../system/mt_renderdevice.h"
#include "../shaders/mt_shader.h"
#include "../renderer/mt_debug.h"
#include "flatvertices.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "metal/renderer/mt_compute.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_hwbuffer.h"
#include "metal/textures/mt_texture.h"
#include "matrix.h"
#include "printf.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

EXTERN_CVAR(Int, mt_compute_ao_scale)
EXTERN_CVAR(Bool, mt_compute_ao_normal_upsample)
EXTERN_CVAR(Bool, mt_compute_ao_normal_blur)
EXTERN_CVAR(Bool, mt_compute_ao_fullres_cleanup)
EXTERN_CVAR(Int, mt_compute_ao_blur_passes)
EXTERN_CVAR(Float, mt_compute_ao_combine_smooth)
EXTERN_CVAR(Bool, mt_compute_ao_skip_fullres)
EXTERN_CVAR(Int, mt_compute_ao_atrous_passes)
EXTERN_CVAR(Int, mt_compute_ao_steps)
EXTERN_CVAR(Int, mt_compute_ao_directions)

static const char* SSAO_COMPUTE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct SSAOParams {
    float4x4 invProj;
    float radius;
    float bias;
    float intensity;
    float screenResX;
    float screenResY;
    float zNear;
    float zFar;
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    float uvToViewAX;
    float uvToViewAY;
    float uvToViewBX;
    float uvToViewBY;
    float negInvR2;
    float radiusToScreen;
    float aoMultiplier;
    float visibilityStrength;
    int numDirections;
    int numSteps;
    float maxThickness;
};

struct AOFlags {
    int flipY;
    float invBackingScale;
};
struct AOBlurParams {
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    float blurSharpness;
    float powExponent;
    int normalAware;
    int flipY;
    float maxThickness;
};

struct AOFullresParams {
    float2 sceneScale;
    float2 sceneOffset;
    float2 fullRes;
    float blurSharpness;
    float zNear;
    float zFar;
    int normalAware;
    int atrousStep;
};

#define GOLDEN_ANGLE 2.39996323
#define NUM_SAMPLES 16

float3 ReconstructViewPos(float2 uv, float depth, float4x4 invProj, float2 screenRes) {
    // Map depth from [0,1] to NDC z in [-1,1]
    float zNDC = depth * 2.0 - 1.0;
    float4 ndc = float4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, zNDC, 1.0);
    float4 viewPos = invProj * ndc;
    return viewPos.xyz / viewPos.w;
}

float3 FetchViewPos(float2 uv, float linearDepth, constant SSAOParams &params) {
    float2 uvToViewA = float2(params.uvToViewAX, params.uvToViewAY);
    float2 uvToViewB = float2(params.uvToViewBX, params.uvToViewBY);
    return float3((uvToViewA * uv + uvToViewB) * linearDepth, linearDepth);
}

float LinearizeSceneDepth(float depth, constant SSAOParams &params) {
    float normalizedDepth = clamp(1.0 - depth, 0.0, 1.0);
    float linearizeA = 1.0 / params.zFar - 1.0 / params.zNear;
    float linearizeB = max(1.0 / params.zNear, 1e-8);
    return 1.0 / (normalizedDepth * linearizeA + linearizeB);
}

float LinearizeDepth(float depth, float zNear, float zFar) {
    float normalizedDepth = clamp(1.0 - depth, 0.0, 1.0);
    float linearizeA = 1.0 / zFar - 1.0 / zNear;
    float linearizeB = max(1.0 / zNear, 1e-8);
    return 1.0 / (normalizedDepth * linearizeA + linearizeB);
}

float3 DecodeSceneNormal(float3 encodedNormal) {
    float3 normal = encodedNormal * 2.0 - 1.0;
    if (length(normal) <= 0.1) {
        return float3(0.0);
    }

    // Match the Metal-patched postprocess SSAO shader: the regular GLSL
    // normal.z flip is intentionally suppressed for Metal's view-space setup.
    return normalize(normal);
}

float NormalWeight(float3 centerNormal, float3 sampleNormal) {
    if (all(centerNormal == float3(0.0)) || all(sampleNormal == float3(0.0))) {
        return 0.0;
    }
    // Much sharper normal weight to prevent bleeding across 90-degree edges
    return pow(saturate(dot(centerNormal, sampleNormal)), 8.0);
}

float InterleavedGradientNoise(float2 p) {
    return fract(52.9829189 * fract(dot(p, float2(0.06711056, 0.00583715))));
}

kernel void ssao_compute(
    uint2 gid [[thread_position_in_grid]],
    uint2 tid [[thread_position_in_threadgroup]],
    constant SSAOParams &params [[buffer(0)]],
    constant AOFlags &flags [[buffer(1)]],
    texture2d<float, access::sample> ditherTexture [[texture(0)]],
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::write> aoOutput [[texture(2)]],
    texture2d<float, access::sample> normalTexture [[texture(3)]],
    texture2d<float, access::sample> sceneColorTexture [[texture(4)]])
{
    float2 outSize = float2((float)aoOutput.get_width(), (float)aoOutput.get_height());
    if (gid.x >= outSize.x || gid.y >= outSize.y) return;

    // LDS optimization: Cache depth/normals for the threadgroup to save bandwidth
    // For 8x8 threadgroup, we need a slightly larger buffer for sample range
    threadgroup float ldsDepth[16][16];
    threadgroup float3 ldsNormal[16][16];
    
    sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::repeat);
    sampler nearestClampSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);

    float2 pixelCenter = float2(gid) + 0.5;
    float2 halfTexel = 0.5 / outSize;
    float2 uv = pixelCenter / outSize;
    if (flags.flipY == 1) uv.y = 1.0 - uv.y;
    float2 sceneUV = float2(params.offsetX, params.offsetY) + uv * float2(params.scaleX, params.scaleY);
    
    // Initial fetch for LDS
    float centerDepth = depthTexture.sample(nearestClampSampler, sceneUV).r;
    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sceneUV).xyz);
    
    ldsDepth[tid.x + 4][tid.y + 4] = centerDepth;
    ldsNormal[tid.x + 4][tid.y + 4] = centerNormal;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    float centerCoverage = sceneColorTexture.sample(nearestClampSampler, sceneUV).a;
    if (centerCoverage <= 0.0001 || centerDepth <= 0.0001 || all(centerNormal == float3(0.0))) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float centerLinearDepth = LinearizeSceneDepth(centerDepth, params);
    float3 centerViewPos = FetchViewPos(uv, centerLinearDepth, params);

    float2 noiseUV = pixelCenter / 64.0;
    float4 noise = ditherTexture.sample(nearestSampler, noiseUV);
    float ign = InterleavedGradientNoise(pixelCenter);
    float rotation = (noise.x * 2.0 - 1.0) * 3.14159 + ign * 2.39996;
    float stepJitter = fract(noise.z + ign * 0.618033);
    float directionJitter = fract(noise.w + ign * 1.324717);

    float radiusPixels = params.radiusToScreen / max(centerViewPos.z, 1e-5);
    const int numSteps = clamp(params.numSteps, 2, 8); // Capped for Intel
    const int numDirections = clamp(params.numDirections, 2, 6); // Capped for Intel
    float stepSizePixels = radiusPixels / (float(numSteps) + 1.0);
    float minStepPixels = max(stepSizePixels, 1.0);

    float occlusion = 0.0;
    
    for(int directionIndex = 0; directionIndex < numDirections; directionIndex++) {
        float theta = (6.2831853 / float(numDirections)) * (float(directionIndex) + directionJitter) + rotation;
        float2 dir = float2(cos(theta), sin(theta));
        float horizon = 0.0;

        for (int stepIndex = 0; stepIndex < numSteps; stepIndex++) {
            float rayPixels = (float(stepIndex) + stepJitter + 1.0) * minStepPixels;
            float2 sampleUV = uv + dir * rayPixels / outSize;
            sampleUV = clamp(sampleUV, halfTexel, float2(1.0) - halfTexel);
            float2 sampleSceneUV = float2(params.offsetX, params.offsetY) + sampleUV * float2(params.scaleX, params.scaleY);

            // Fast path: if sample is within our LDS tile, use cached data
            float sampleRawDepth;
            float3 sampleNormal;
            
            int2 localOff = int2(floor(dir * rayPixels));
            int2 ldsCoord = int2(tid) + 4 + localOff;
            if (all(ldsCoord >= 0 && ldsCoord < 16)) {
                sampleRawDepth = ldsDepth[ldsCoord.x][ldsCoord.y];
                sampleNormal = ldsNormal[ldsCoord.x][ldsCoord.y];
            } else {
                // Fallback to texture sample if outside tile
                sampleRawDepth = depthTexture.sample(nearestClampSampler, sampleSceneUV).r;
                sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sampleSceneUV).xyz);
            }

            if (sampleRawDepth <= 0.0001 || all(sampleNormal == float3(0.0))) continue;

            float sampleLinearDepth = LinearizeSceneDepth(sampleRawDepth, params);
            float3 sampleViewPos = FetchViewPos(sampleUV, sampleLinearDepth, params);
            float3 sampleVector = sampleViewPos - centerViewPos;
            
            float depthDiff = sampleViewPos.z - centerViewPos.z;
            float thicknessThreshold = params.maxThickness * (1.0 + centerViewPos.z * 0.05);
            if (depthDiff > thicknessThreshold || depthDiff < -params.maxThickness * 0.5) continue;

            float distanceSquare = max(dot(sampleVector, sampleVector), 1e-6);
            float invDistance = rsqrt(distanceSquare);
            float normalAngle = dot(centerNormal, sampleVector) * invDistance;
            
            float sampleHorizon = max(normalAngle - params.bias * 0.5, 0.0);
            float falloff = saturate(distanceSquare * params.negInvR2 + 1.0);
            occlusion += max(sampleHorizon - horizon, 0.0) * falloff;
            horizon = max(horizon, sampleHorizon);
        }
    }

    occlusion *= (params.aoMultiplier * 1.15) / float(numDirections * numSteps);
    float visibility = clamp(1.0 - occlusion * params.visibilityStrength, 0.0, 1.0);
    visibility = visibility * params.intensity + (1.0 - params.intensity);
    
    aoOutput.write(float4(saturate(visibility), centerLinearDepth, 0.0, 1.0), gid);
}

kernel void bilateral_blur(
    uint2 gid [[thread_position_in_grid]],
    constant AOBlurParams &params [[buffer(0)]],
    texture2d<float, access::read> sourceTexture [[texture(0)]],
    texture2d<float, access::write> destTexture [[texture(1)]],
    texture2d<float, access::sample> normalTexture [[texture(2)]])
{
    if (gid.x >= destTexture.get_width() || gid.y >= destTexture.get_height()) return;

    constexpr sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);
    float4 centerSample = sourceTexture.read(gid);
    float center = centerSample.r;
    float depth = centerSample.g;
    if (depth <= 1e-5) {
        destTexture.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float sum = center;
    float totalWeight = 1.0;
    bool useNormals = params.normalAware != 0;
    float2 texSize = float2((float)sourceTexture.get_width(), (float)sourceTexture.get_height());
    float2 centerUV = (float2(gid) + 0.5) / texSize;
    if (params.flipY != 0) {
        centerUV.y = 1.0 - centerUV.y;
    }
    float2 centerSceneUV = float2(params.offsetX, params.offsetY) + centerUV * float2(params.scaleX, params.scaleY);
    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, centerSceneUV).xyz);
    if (all(centerNormal == float3(0.0))) {
        useNormals = false;
    }

    int2 offsets[4] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)
    };

    for(int i = 0; i < 4; i++) {
        uint2 sampleCoord = uint2(int2(gid) + offsets[i]);
        if (sampleCoord.x < sourceTexture.get_width() && sampleCoord.y < sourceTexture.get_height()) {
            float4 sampleValue = sourceTexture.read(sampleCoord);
            float val = sampleValue.r;
            float sampleDepth = sampleValue.g;
            if (sampleDepth <= 1e-5) {
                continue;
            }
            float r = 1.0;
            // Improved bilateral weight: use a more robust sharpness scale
            float depthDelta = abs(sampleDepth - depth) * params.blurSharpness;
            
            // Thickness clamping in blur: prevent background AO from leaking onto foreground
            if (sampleDepth - depth > params.maxThickness) {
                continue;
            }
            
            // Extra sharp falloff for depth to avoid halos bleeding in blur pass
            float weight = exp2(-r * r * 0.5 - depthDelta * depthDelta * 8.0);
            
            if (useNormals) {
                float2 sampleUV = (float2(sampleCoord) + 0.5) / texSize;
                if (params.flipY != 0) {
                    sampleUV.y = 1.0 - sampleUV.y;
                }
                float2 sampleSceneUV = float2(params.offsetX, params.offsetY) + sampleUV * float2(params.scaleX, params.scaleY);
                float3 sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, sampleSceneUV).xyz);
                weight *= mix(0.1, 1.0, NormalWeight(centerNormal, sampleNormal));
            }
            
            sum += val * weight;
            totalWeight += weight;
        }
    }

    float blurred = sum / totalWeight;
    if (params.applyExponent != 0) {
        blurred = pow(saturate(blurred), params.powExponent);
    }
    destTexture.write(float4(blurred, depth, 0, 1.0), gid);
}

kernel void ao_upsample_fullres(
    uint2 gid [[thread_position_in_grid]],
    constant AOFullresParams &params [[buffer(0)]],
    texture2d<float, access::sample> lowresAO [[texture(0)]],
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::sample> normalTexture [[texture(2)]],
    texture2d<float, access::write> outTexture [[texture(3)]])
{
    if (gid.x >= outTexture.get_width() || gid.y >= outTexture.get_height()) return;

    constexpr sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);
    float2 localUV = (float2(gid) + 0.5) / params.fullRes;
    float2 sceneUV = params.sceneOffset + localUV * params.sceneScale;
    float rawDepth = depthTexture.sample(nearestSampler, sceneUV).r;
    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, sceneUV).xyz);
    if (rawDepth <= 0.0001 || all(centerNormal == float3(0.0))) {
        outTexture.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float sceneDepth = LinearizeDepth(rawDepth, params.zNear, params.zFar);
    float2 aoSize = float2((float)lowresAO.get_width(), (float)lowresAO.get_height());
    float2 aoUV = float2(localUV.x, 1.0 - localUV.y);
    float2 aoCoord = aoUV * aoSize - 0.5;
    float2 aoBase = floor(aoCoord);
    float2 aoFrac = fract(aoCoord);
    float combineSharpness = max(params.blurSharpness * 8.0, 0.1);

    float alpha = 0.0;
    float totalWeight = 0.0;
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            float2 tap = float2((float)x, (float)y);
            float2 tapCoord = clamp(aoBase + tap, float2(0.0), aoSize - 1.0);
            float2 tapUV = (tapCoord + 0.5) / aoSize;
            float2 tapLocalUV = float2(tapUV.x, 1.0 - tapUV.y);
            float2 tapSceneUV = params.sceneOffset + tapLocalUV * params.sceneScale;
            float4 sampleAO = lowresAO.sample(nearestSampler, tapUV);
            if (sampleAO.y <= 1e-5) {
                continue;
            }

            float bilinearWeight = ((x == 0) ? (1.0 - aoFrac.x) : aoFrac.x) *
                                   ((y == 0) ? (1.0 - aoFrac.y) : aoFrac.y);
            float depthDelta = (sampleAO.y - sceneDepth) * combineSharpness;
            float weight = bilinearWeight * exp2(-depthDelta * depthDelta * 4.0);
            if (params.normalAware != 0) {
                float3 tapNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, tapSceneUV).xyz);
                weight *= NormalWeight(centerNormal, tapNormal);
            }

            alpha += (1.0 - sampleAO.x) * weight;
            totalWeight += weight;
        }
    }

    alpha = (totalWeight > 1e-5) ? alpha / totalWeight : 0.0;
    outTexture.write(float4(1.0 - alpha, sceneDepth, 0.0, 1.0), gid);
}

kernel void ao_atrous_fullres(
    uint2 gid [[thread_position_in_grid]],
    constant AOFullresParams &params [[buffer(0)]],
    texture2d<float, access::read> sourceTexture [[texture(0)]],
    texture2d<float, access::sample> normalTexture [[texture(1)]],
    texture2d<float, access::write> destTexture [[texture(2)]])
{
    if (gid.x >= destTexture.get_width() || gid.y >= destTexture.get_height()) return;

    constexpr sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);
    float4 centerAO = sourceTexture.read(gid);
    float centerDepth = centerAO.y;
    float2 localUV = (float2(gid) + 0.5) / params.fullRes;
    float2 sceneUV = params.sceneOffset + localUV * params.sceneScale;
    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, sceneUV).xyz);
    if (centerDepth <= 1e-5 || all(centerNormal == float3(0.0))) {
        destTexture.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    int stepSize = max(params.atrousStep, 1);
    int2 offsets[4] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)
    };
    float sum = centerAO.x;
    float totalWeight = 1.0;
    for (int i = 0; i < 4; i++) {
        int2 icoord = int2(gid) + offsets[i] * stepSize;
        if (icoord.x < 0 || icoord.y < 0 ||
            icoord.x >= (int)sourceTexture.get_width() ||
            icoord.y >= (int)sourceTexture.get_height()) {
            continue;
        }

        uint2 sampleCoord = uint2(icoord);
        float4 sampleAO = sourceTexture.read(sampleCoord);
        if (sampleAO.y <= 1e-5) {
            continue;
        }

        float2 sampleLocalUV = (float2(sampleCoord) + 0.5) / params.fullRes;
        float2 sampleSceneUV = params.sceneOffset + sampleLocalUV * params.sceneScale;
        float3 sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, sampleSceneUV).xyz);
        float depthDelta = (sampleAO.y - centerDepth) * max(params.blurSharpness * 4.0, 0.02);
        float weight = 0.75 * exp2(-depthDelta * depthDelta);
        if (params.normalAware != 0) {
            weight *= NormalWeight(centerNormal, sampleNormal);
        }

        sum += sampleAO.x * weight;
        totalWeight += weight;
    }

    destTexture.write(float4(sum / totalWeight, centerDepth, 0.0, 1.0), gid);
}

struct AOCombineParams {
    int debugMode;
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    float blurSharpness;
    float zNear;
    float zFar;
    float combineSmooth;
    int fullresAO;
};

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut ssao_combine_vs(uint vid [[vertex_id]]) {
    VSOut out;
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    out.position = float4(pos[vid], 0.0, 1.0);
    out.uv = out.position.xy * 0.5 + float2(0.5);
    return out;
}

fragment float4 ssao_combine_fs(VSOut in [[stage_in]],
                                constant AOCombineParams &params [[buffer(0)]],
                                texture2d<float, access::sample> aoTexture [[texture(0)]],
                                texture2d<float, access::sample> fogTexture [[texture(1)]],
                                texture2d<float, access::sample> normalTexture [[texture(2)]],
                                texture2d<float, access::sample> depthTexture [[texture(3)]])
{
    constexpr sampler linearSampler(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    constexpr sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);

    float2 aoUV = float2(in.uv.x, 1.0 - in.uv.y);
    float2 fogUV = float2(params.offsetX, params.offsetY) + in.uv * float2(params.scaleX, params.scaleY);
    float4 ssao = aoTexture.sample(linearSampler, aoUV);
    float4 fogSample = fogTexture.sample(nearestSampler, fogUV);
    float3 sceneNormal = normalTexture.sample(linearSampler, fogUV).xyz;
    float attenuation = ssao.x;
    float rawSceneDepth = depthTexture.sample(nearestSampler, fogUV).r;
    float normalizedDepth = clamp(1.0 - rawSceneDepth, 0.0, 1.0);
    float sceneDepth = 1.0 / (normalizedDepth * (1.0 / params.zFar - 1.0 / params.zNear) + max(1.0 / params.zNear, 1e-8));
    // Improved depth mask: softer falloff for distant objects
    float depthSignal = 1.0 - exp2(-sceneDepth * 0.005);
    float depthMask = saturate(depthSignal);
    float3 decodedNormal = sceneNormal * 2.0 - 1.0;

    if (params.debugMode == 0) {
        if (rawSceneDepth <= 0.0001 || length(decodedNormal) <= 0.1) {
            return float4(fogSample.rgb, 0.0);
        }

        if (params.fullresAO != 0) {
            float aoAlpha = (1.0 - ssao.x) * depthMask;
            // Less aggressive smoothstep to preserve subtle AO while still killing speckles
            aoAlpha *= smoothstep(0.002, 0.020, aoAlpha);
            return float4(fogSample.rgb, aoAlpha);
        }

        float centerAlpha = (1.0 - attenuation) * saturate(1.0 - exp2(-ssao.y * 0.005));
        float2 aoTexel = 1.0 / float2((float)aoTexture.get_width(), (float)aoTexture.get_height());
        float4 ssaoL = aoTexture.sample(linearSampler, aoUV + float2(-aoTexel.x, 0.0));
        float4 ssaoR = aoTexture.sample(linearSampler, aoUV + float2( aoTexel.x, 0.0));
        float4 ssaoU = aoTexture.sample(linearSampler, aoUV + float2(0.0, -aoTexel.y));
        float4 ssaoD = aoTexture.sample(linearSampler, aoUV + float2(0.0,  aoTexel.y));
        float centerDepth = max(ssao.y, 1e-5);
        float blurSharpness = max(params.blurSharpness * 4.0, 0.02);
        float4 taps[4] = { ssaoL, ssaoR, ssaoU, ssaoD };
        float alphaSum = centerAlpha;
        float weightSum = 1.0;
        for (int i = 0; i < 4; i++) {
            if (taps[i].y <= 1e-5) {
                continue;
            }
            float tapAlpha = (1.0 - taps[i].x) * saturate(1.0 - exp2(-taps[i].y * 0.005));
            float depthDelta = (taps[i].y - centerDepth) * blurSharpness;
            float weight = exp2(-0.35 - depthDelta * depthDelta);
            alphaSum += tapAlpha * weight;
            weightSum += weight;
        }
        float aoAlpha = mix(centerAlpha, alphaSum / weightSum, saturate(params.combineSmooth));
        // Depth-weighted neighbor average — prevents stair-edge bleeding
        float neighborAlpha = 0.0;
        float neighborWeight = 0.0;
        float4 neighborVals[4] = { ssaoL, ssaoR, ssaoU, ssaoD };
        for (int n = 0; n < 4; n++) {
            if (neighborVals[n].y <= 1e-5) continue;
            float nAlpha = (1.0 - neighborVals[n].x) * saturate(1.0 - exp2(-neighborVals[n].y * 0.005));
            float nDepthDelta = abs(neighborVals[n].y - centerDepth) * blurSharpness;
            float nWeight = exp2(-0.35 - nDepthDelta * nDepthDelta);
            neighborAlpha += nAlpha * nWeight;
            neighborWeight += nWeight;
        }
        neighborAlpha = (neighborWeight > 1e-5) ? neighborAlpha / neighborWeight : 0.0;
        
        // Speckle removal: pull weak isolated pixels toward neighbor average.
        // A single bright pixel surrounded by darker neighbors is noise.
        if (aoAlpha < neighborAlpha * 0.85 && neighborAlpha > 0.005) {
            aoAlpha = mix(aoAlpha, neighborAlpha, 0.8);
        }
        // Hard floor: no pixel can be drastically brighter than its neighbors
        aoAlpha = max(aoAlpha, neighborAlpha * 0.3);
        
        // Multi-bounce AO approximation (Jimenez 2016)
        // Helps darken corners while preventing the "glow" artifact around edges.
        float3 albedo = float3(0.5); // Assume neutral albedo
        float3 a = 2.0 * albedo - 0.33;
        float3 b = -4.8 * albedo + 0.64;
        float3 c = 2.8 * albedo + 0.69;
        float3 multiBounce = max(aoAlpha, ((a * aoAlpha + b) * aoAlpha + c) * aoAlpha);
        aoAlpha = multiBounce.x;

        aoAlpha *= smoothstep(0.001, 0.015, aoAlpha);
        return float4(fogSample.rgb, aoAlpha);
    }
    else if (params.debugMode < 3)
        return float4(float3(attenuation), 1.0);
    else if (params.debugMode == 3)
        return float4(ssao.yyy / 1000.0, 1.0);
    else if (params.debugMode == 4)
        return float4(sceneNormal, 1.0);
    else if (params.debugMode == 5)
        return float4(float3(ssao.x), 1.0);
    else if (params.debugMode == 6)
        return float4(float3(depthSignal), 1.0);
    else if (params.debugMode == 7)
        return float4(float3(step(1e-5, ssao.y)), 1.0);
    else if (params.debugMode == 8)
        return float4(float3(depthMask), 1.0);
    else
        return float4(ssao.xyz, 1.0);
}

kernel void ssao_combine(
    uint2 gid [[thread_position_in_grid]],
    constant int &debugMode [[buffer(0)]],
    texture2d<float, access::read> aoTexture [[texture(0)]],
    texture2d<float, access::read> fogTexture [[texture(1)]],
    texture2d<float, access::read_write> sceneTexture [[texture(2)]])
{
    if (gid.x >= sceneTexture.get_width() || gid.y >= sceneTexture.get_height()) return;

    float4 ss = aoTexture.read(gid);
    float ao = ss.r;
    float4 scene = sceneTexture.read(gid);
    float4 fog = fogTexture.read(gid);

    // Debug modes mirror shaders/pp/ssaocombine.fp
    if (debugMode == 0) {
        // Emulate original: composite fog over scene using src alpha = 1 - ao
        float srcAlpha = 1.0 - ao;
        float3 result = fog.rgb * srcAlpha + scene.rgb * (1.0 - srcAlpha);
        scene.rgb = result;
        scene.a = 1.0;
        sceneTexture.write(scene, gid);
        return;
    }
    else if (debugMode < 3) {
        float3 gray = float3(ao, ao, ao);
        scene.rgb = gray;
        scene.a = 1.0;
        sceneTexture.write(scene, gid);
        return;
    }
    else if (debugMode == 3) {
        float depthVal = ss.g / 1000.0; // if AO stores viewZ in .g this shows depth
        scene.rgb = float3(depthVal, depthVal, depthVal);
        scene.a = 1.0;
        sceneTexture.write(scene, gid);
        return;
    }
    else {
        scene.rgb = ss.rgb;
        scene.a = 1.0;
        sceneTexture.write(scene, gid);
        return;
    }
}
)";

MtAOModule::MtAOModule(MetalRenderDevice* device) : fb(device) {
    auto shaderManager = fb->GetShaderManager();
    ssaoPSO = shaderManager->CreateComputePipeline("ssao_compute", SSAO_COMPUTE_SOURCE, "SSAO ssao_compute");
    blurPSO = shaderManager->CreateComputePipeline("bilateral_blur", SSAO_COMPUTE_SOURCE, "SSAO bilateral_blur");
    upsamplePSO = shaderManager->CreateComputePipeline("ao_upsample_fullres", SSAO_COMPUTE_SOURCE, "SSAO fullres upsample");
    atrousPSO = shaderManager->CreateComputePipeline("ao_atrous_fullres", SSAO_COMPUTE_SOURCE, "SSAO fullres atrous");

    auto createCombinePipeline = [&](MTL::Library* library) -> MTL::RenderPipelineState* {
        if (!library)
            return nullptr;
        auto vert = library->newFunction(NS::String::string("ssao_combine_vs", NS::UTF8StringEncoding));
        auto frag = library->newFunction(NS::String::string("ssao_combine_fs", NS::UTF8StringEncoding));
        if (!vert || !frag) {
            if (vert) vert->release();
            if (frag) frag->release();
            return nullptr;
        }

        auto desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vert);
        desc->setFragmentFunction(frag);
        desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        desc->colorAttachments()->object(0)->setBlendingEnabled(true);
        desc->colorAttachments()->object(0)->setRgbBlendOperation(MTL::BlendOperationAdd);
        desc->colorAttachments()->object(0)->setAlphaBlendOperation(MTL::BlendOperationAdd);
        desc->colorAttachments()->object(0)->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
        desc->colorAttachments()->object(0)->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        desc->colorAttachments()->object(0)->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        desc->colorAttachments()->object(0)->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        desc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
        desc->setStencilAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);

        NS::Error* error = nullptr;
        auto pso = fb->device->device->newRenderPipelineState(desc, &error);
        if (!pso && error) {
            Printf(PRINT_LOG, "Metal: Failed to create SSAO combine render pipeline: %s\n",
                   error->localizedDescription()->utf8String());
            error->release();
        }

        desc->release();
        vert->release();
        frag->release();
        return pso;
    };

    combineRenderPSO = createCombinePipeline(shaderManager->LoadNativeLibrary());
    if (!combineRenderPSO) {
        auto sourceString = NS::String::string(SSAO_COMPUTE_SOURCE, NS::UTF8StringEncoding);
        auto compileOptions = MTL::CompileOptions::alloc()->init();
        compileOptions->setLanguageVersion(MTL::LanguageVersion2_0);
        NS::Error* error = nullptr;
        auto library = fb->device->device->newLibrary(sourceString, compileOptions, &error);
        compileOptions->release();
        if (library) {
            combineRenderPSO = createCombinePipeline(library);
            library->release();
        } else if (error) {
            Printf(PRINT_LOG, "Metal: Failed to compile SSAO combine fallback library: %s\n",
                   error->localizedDescription()->utf8String());
            error->release();
        }
    }

    CreateDitherTexture();
}

MtAOModule::~MtAOModule() {
    if (ssaoPSO) ssaoPSO->release();
    if (blurPSO) blurPSO->release();
    if (upsamplePSO) upsamplePSO->release();
    if (atrousPSO) atrousPSO->release();
    if (combinePSO) combinePSO->release();
    if (combineRenderPSO) combineRenderPSO->release();
    if (mAOTexture) mAOTexture->release();
    if (mBlurTexture) mBlurTexture->release();
    if (mFullresAOTexture) mFullresAOTexture->release();
    if (mFullresTempTexture) mFullresTempTexture->release();
    if (mDitherTexture) mDitherTexture->release();
}

void MtAOModule::CreateDitherTexture() {
    if (mDitherTexture)
        return;

    std::mt19937 generator(1337);
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
    std::vector<int16_t> randomValues(64 * 64 * 4);
    for (int i = 0; i < 64 * 64; i++) {
        float angle = 2.0f * 3.14159265359f * distribution(generator);
        randomValues[i * 4 + 0] = (int16_t)clamp(cosf(angle) * 32767.0f, -32768.0f, 32767.0f);
        randomValues[i * 4 + 1] = (int16_t)clamp(sinf(angle) * 32767.0f, -32768.0f, 32767.0f);
        randomValues[i * 4 + 2] = (int16_t)clamp(distribution(generator) * 32767.0f, -32768.0f, 32767.0f);
        randomValues[i * 4 + 3] = (int16_t)clamp(distribution(generator) * 32767.0f, -32768.0f, 32767.0f);
    }

    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(64);
    desc->setHeight(64);
    desc->setPixelFormat(MTL::PixelFormatRGBA16Snorm);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(fb->mVersionManager.GetDynamicStorageMode());
    mDitherTexture = fb->device->device->newTexture(desc);
    desc->release();

    if (mDitherTexture) {
        auto region = MTL::Region::Make2D(0, 0, 64, 64);
        mDitherTexture->replaceRegion(region, 0, randomValues.data(), 64 * 8);
    }
}

void MtAOModule::EnsureTextures(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);

    if (mAOTexture && mBlurTexture && mAOWidth == width && mAOHeight == height)
        return;

    if (mAOTexture) {
        mAOTexture->release();
        mAOTexture = nullptr;
    }
    if (mBlurTexture) {
        mBlurTexture->release();
        mBlurTexture = nullptr;
    }

    auto compute = fb->GetComputeManager();
    if (!compute)
        return;

    const auto usage = (MTL::TextureUsage)(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    mAOTexture = compute->CreateTexture(width, height, MTL::PixelFormatRG16Float,
                                        usage, MTL::StorageModePrivate);
    mBlurTexture = compute->CreateTexture(width, height, MTL::PixelFormatRG16Float,
                                          usage, MTL::StorageModePrivate);

    mAOWidth = width;
    mAOHeight = height;
}

void MtAOModule::EnsureFullresTextures(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);

    if (mFullresAOTexture && mFullresTempTexture &&
        mFullresWidth == width && mFullresHeight == height)
        return;

    if (mFullresAOTexture) {
        mFullresAOTexture->release();
        mFullresAOTexture = nullptr;
    }
    if (mFullresTempTexture) {
        mFullresTempTexture->release();
        mFullresTempTexture = nullptr;
    }

    auto compute = fb->GetComputeManager();
    if (!compute)
        return;

    const auto usage = (MTL::TextureUsage)(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    mFullresAOTexture = compute->CreateTexture(width, height, MTL::PixelFormatRG16Float,
                                               usage, MTL::StorageModePrivate);
    mFullresTempTexture = compute->CreateTexture(width, height, MTL::PixelFormatRG16Float,
                                                 usage, MTL::StorageModePrivate);
    mFullresWidth = width;
    mFullresHeight = height;
}

bool MtAOModule::Render(float m5, int sceneWidth, int sceneHeight) {
    if (!ssaoPSO || !blurPSO || !combineRenderPSO || !mDitherTexture || !fb->GetBuffers())
        return false;

    auto buffers = fb->GetBuffers();
    if (!buffers->SceneDepthStencil || !buffers->SceneNormal || !buffers->SceneFog ||
        !buffers->SceneDepthStencil->GetTexture() || !buffers->SceneNormal->GetTexture() ||
        !buffers->SceneFog->GetTexture()) {
        return false;
    }

    int aoScale = (int)mt_compute_ao_scale;
    // Intel integrated GPUs default to quarter-res to stay within 16ms budget.
    // Apple Silicon and discrete GPUs can handle half-res comfortably.
    if (fb->mVersionManager.architecture == MtGPUArchitecture::Intel && aoScale < 4)
        aoScale = 4;
    aoScale = clamp(aoScale, 2, 4);
    EnsureTextures((sceneWidth + aoScale - 1) / aoScale, (sceneHeight + aoScale - 1) / aoScale);
    if (!mAOTexture || !mBlurTexture)
        return false;
    
    // Automatically enable fullres cleanup for High quality settings if not explicitly set
    bool useFullresCleanup = mt_compute_ao_fullres_cleanup && !mt_compute_ao_skip_fullres;
    if (gl_ssao >= 3 && mt_compute_ao_atrous_passes >= 0 && !mt_compute_ao_skip_fullres) {
        useFullresCleanup = true;
    }
    useFullresCleanup = useFullresCleanup && upsamplePSO && atrousPSO;

    if (useFullresCleanup) {
        EnsureFullresTextures(sceneWidth, sceneHeight);
        if (!mFullresAOTexture || !mFullresTempTexture)
            return false;
    }

    VSMatrix invProj;
    if (!fb->mLastSceneViewpoint.mProjectionMatrix.inverseMatrix(invProj))
        return false;

    SSAOParams params = {};
    const auto *matrix = invProj.get();
    for (int i = 0; i < 16; i++)
        params.invProj[i] = (float)matrix[i];

    float aoStrength = gl_ssao_strength;
    switch (gl_ssao) {
    default:
    case 1: aoStrength *= 0.85f; break;
    case 2: break;
    case 3: aoStrength *= 1.15f; break;
    }

    params.radius = gl_ssao_radius;
    params.bias = clamp((float)gl_ssao_bias, 0.0f, 1.0f);
    params.intensity = clamp(aoStrength, 0.0f, 1.0f);
    params.screenResX = (float)sceneWidth;
    params.screenResY = (float)sceneHeight;
    params.zNear = fb->GetZNear();
    params.zFar = fb->GetZFar();
    auto sceneScale = fb->SceneScale();
    auto sceneOffset = fb->SceneOffset();
    params.scaleX = sceneScale.X;
    params.scaleY = sceneScale.Y;
    params.offsetX = sceneOffset.X;
    params.offsetY = sceneOffset.Y;

    float tanHalfFovy = 1.0f / m5;
    float invFocalLenX = tanHalfFovy * (sceneWidth / (float)sceneHeight);
    float invFocalLenY = tanHalfFovy;
    float r2 = std::max(gl_ssao_radius * gl_ssao_radius, 1.0f);
    params.uvToViewAX = 2.0f * invFocalLenX;
    params.uvToViewAY = 2.0f * invFocalLenY;
    params.uvToViewBX = -invFocalLenX;
    params.uvToViewBY = -invFocalLenY;
    params.negInvR2 = -1.0f / r2;
    params.radiusToScreen = gl_ssao_radius * 0.5f / tanHalfFovy * (float)mAOHeight;
    params.aoMultiplier = 1.0f / std::max(1.0f - params.bias, 1e-5f);
    
    // Quality-based defaults:
    // Low: 4x4, Med: 4x4 (stronger), High: 5x4
    switch (gl_ssao) {
    default:
    case 1: params.visibilityStrength = 2.35f; params.numDirections = 4; params.numSteps = 4; break;
    case 2: params.visibilityStrength = 2.75f; params.numDirections = 4; params.numSteps = 4; break;
    case 3: params.visibilityStrength = 3.15f; params.numDirections = 6; params.numSteps = 8; break;
    }

    // Override with CVARs if they are > 0
    if (mt_compute_ao_directions > 0) params.numDirections = mt_compute_ao_directions;
    if (mt_compute_ao_steps > 0) params.numSteps = mt_compute_ao_steps;
    
    // Thickness heuristic: prevent background from occluding foreground.
    // 1.5-2.0 units is usually a good balance for GZDoom scale.
    params.maxThickness = 1.25f;

    auto cmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
    if (!cmdBuf)
        return false;

    fb->GetRenderState()->EndRenderPass();
    const bool blurAO = gl_ssao_debug < 2;
    auto aoStart = std::chrono::high_resolution_clock::now();
    Execute(cmdBuf, buffers->SceneDepthStencil->GetTexture(),
            buffers->SceneNormal->GetTexture(), buffers->SceneColor->GetTexture(),
            mAOTexture, mDitherTexture,
            buffers->SceneFog->GetTexture(), nullptr, params, blurAO,
            useFullresCleanup);
    MTL::Texture *combineAO = (useFullresCleanup && mFullresResultTexture) ? mFullresResultTexture :
        (mLowresResultTexture ? mLowresResultTexture : mAOTexture);
    Combine(combineAO, sceneWidth, sceneHeight, combineAO == mFullresResultTexture);
    auto aoEnd = std::chrono::high_resolution_clock::now();
    if (fb->GetComputeManager()) {
        float ms = std::chrono::duration<float, std::milli>(aoEnd - aoStart).count();
        fb->GetComputeManager()->RecordTiming(
            HWComputeEffect::AmbientOcclusion, ms);
    }
    return true;
}

void MtAOModule::Combine(MTL::Texture* aoTex, int sceneWidth, int sceneHeight, bool fullresAO) {
    if (!aoTex || !combineRenderPSO || !fb->GetBuffers())
        return;

    auto buffers = fb->GetBuffers();
    auto renderState = fb->GetRenderState();
    renderState->SetRenderTarget(buffers->SceneColor->GetTexture(),
                                 buffers->SceneDepthStencil->GetTexture(),
                                 buffers->SceneColor->GetWidth(),
                                 buffers->SceneColor->GetHeight(),
                                 (int)MTL::PixelFormatBGRA8Unorm, 1);
    renderState->EnableDrawBuffers(1, false);
    const auto& viewport = screen->mSceneViewport;
    renderState->SetViewport(viewport.left, viewport.top, viewport.width, viewport.height);
    renderState->SetScissor(viewport.left, viewport.top, viewport.width, viewport.height);
    renderState->BeginRenderPass();

    auto encoder = renderState->GetEncoder();
    if (!encoder)
        return;

    struct AOCombineParams {
        int debugMode;
        float scaleX;
        float scaleY;
        float offsetX;
        float offsetY;
        float blurSharpness;
        float zNear;
        float zFar;
        float combineSmooth;
        int fullresAO;
    } combineParams = {};
    combineParams.debugMode = gl_ssao_debug;
    auto sceneScale = fb->SceneScale();
    auto sceneOffset = fb->SceneOffset();
    combineParams.scaleX = sceneScale.X;
    combineParams.scaleY = sceneScale.Y;
    combineParams.offsetX = sceneOffset.X;
    combineParams.offsetY = sceneOffset.Y;
    combineParams.blurSharpness = 1.0f / std::max((float)gl_ssao_blur, 0.1f);
    combineParams.zNear = fb->GetZNear();
    combineParams.zFar = fb->GetZFar();
    combineParams.combineSmooth = clamp((float)mt_compute_ao_combine_smooth, 0.0f, 1.0f);
    combineParams.fullresAO = fullresAO ? 1 : 0;

    renderState->SetVertexBuffer(screen->mVertexData);
    auto vb = dynamic_cast<MtVertexBuffer *>(screen->mVertexData->GetBufferObjects().first);
    if (vb) {
        MtPipelineKey ppKey;
        ppKey.VertexFormat = vb->VertexFormat;
        renderState->SetPipelineKey(ppKey);
        encoder->setVertexBuffer(vb->GetBuffer(), 0, 0);
        encoder->useResource(vb->GetBuffer(), MTL::ResourceUsageRead, MTL::RenderStageVertex);
    }

    encoder->setRenderPipelineState(combineRenderPSO);
    encoder->setDepthStencilState(fb->GetPipelineStateManager()->GetPPStencilState());
    encoder->setStencilReferenceValue(screen->stencilValue);
    encoder->setCullMode(MTL::CullModeNone);
    encoder->setFragmentBytes(&combineParams, sizeof(combineParams), 0);
    encoder->setFragmentTexture(aoTex, 0);
    encoder->setFragmentTexture(buffers->SceneFog->GetTexture(), 1);
    encoder->setFragmentTexture(buffers->SceneNormal->GetTexture(), 2);
    encoder->setFragmentTexture(buffers->SceneDepthStencil->GetTexture(), 3);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0,
                            (NS::UInteger)3);
}

void MtAOModule::Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* depthTex, MTL::Texture* normalTex, MTL::Texture* sceneColorTex, MTL::Texture* aoTex, MTL::Texture* ditherTex, MTL::Texture* fogTex, MTL::Texture* combineTex, const SSAOParams& params, bool blurAO, bool useFullresCleanup) {
    if (!ssaoPSO || (blurAO && (!blurPSO || !mBlurTexture)) || !depthTex || !normalTex || !sceneColorTex || !ditherTex || !aoTex) return;
    mFullresResultTexture = nullptr;
    mLowresResultTexture = aoTex;
    
    auto encoder = cmdBuf->computeCommandEncoder();
    if (!encoder) return;

    // 1. SSAO Pass (LDS Optimized)
    encoder->setComputePipelineState(ssaoPSO);
    encoder->setBytes(&params, sizeof(SSAOParams), 0);
    encoder->setTexture(ditherTex, 0);
    encoder->setTexture(depthTex, 1);
    encoder->setTexture(aoTex, 2);
    encoder->setTexture(normalTex, 3);
    encoder->setTexture(sceneColorTex, 4);

    struct AOFlags { int flipY; float invBackingScale; } aoFlags;
    aoFlags.flipY = fb->RenderTextureIsFlipped() ? 1 : 0;
    aoFlags.invBackingScale = aoFlags.flipY ? params.screenResY / (float)aoTex->height() : 1.0f;
    encoder->setBytes(&aoFlags, sizeof(aoFlags), 1);

    MTL::Size gridSize = { (NS::UInteger)aoTex->width(), (NS::UInteger)aoTex->height(), 1 };
    encoder->dispatchThreads(gridSize, MTL::Size(8, 8, 1));
    
    if (blurAO) {
        encoder->memoryBarrier(MTL::BarrierScopeTextures);

        struct AOBlurParams {
            float scaleX, scaleY, offsetX, offsetY;
            float blurSharpness, powExponent;
            int normalAware, flipY;
            float maxThickness;
            int applyExponent; // Moved to end for alignment safety
        } blurParams = {};
        blurParams.scaleX = params.scaleX;
        blurParams.scaleY = params.scaleY;
        blurParams.offsetX = params.offsetX;
        blurParams.offsetY = params.offsetY;
        blurParams.blurSharpness = 1.0f / std::max((float)gl_ssao_blur, 0.1f);
        blurParams.powExponent = std::max((float)gl_ssao_exponent, 0.1f);
        blurParams.normalAware = mt_compute_ao_normal_blur ? 1 : 0;
        blurParams.flipY = aoFlags.flipY;
        blurParams.maxThickness = params.maxThickness;

        encoder->setComputePipelineState(blurPSO);
        const int blurPasses = clamp((int)mt_compute_ao_blur_passes, 1, 4);
        MTL::Texture *src = aoTex;
        MTL::Texture *dst = mBlurTexture;
        for (int pass = 0; pass < blurPasses; pass++) {
            if (pass > 0) encoder->memoryBarrier(MTL::BarrierScopeTextures);
            dst = (src == aoTex) ? mBlurTexture : aoTex;
            blurParams.applyExponent = (pass == blurPasses - 1) ? 1 : 0;
            encoder->setBytes(&blurParams, sizeof(blurParams), 0);
            encoder->setTexture(src, 0);
            encoder->setTexture(dst, 1);
            encoder->setTexture(normalTex, 2);
            encoder->dispatchThreads(gridSize, MTL::Size(8, 8, 1));
            src = dst;
        }
        mLowresResultTexture = src;
    }

    // Fullres cleanup decision was computed in Render() — just verify textures exist
    useFullresCleanup = useFullresCleanup && upsamplePSO && atrousPSO && mFullresAOTexture && mFullresTempTexture;

    if (useFullresCleanup) {
        encoder->memoryBarrier(MTL::BarrierScopeTextures);
        struct AOFullresParamsCPU {
            float sceneScale[2], sceneOffset[2], fullRes[2];
            float blurSharpness, zNear, zFar;
            int normalAware, atrousStep;
        } fullresParams = {};
        fullresParams.sceneScale[0] = params.scaleX;
        fullresParams.sceneScale[1] = params.scaleY;
        fullresParams.sceneOffset[0] = params.offsetX;
        fullresParams.sceneOffset[1] = params.offsetY;
        fullresParams.fullRes[0] = (float)mFullresWidth;
        fullresParams.fullRes[1] = (float)mFullresHeight;
        fullresParams.blurSharpness = 1.0f / std::max((float)gl_ssao_blur, 0.1f);
        fullresParams.zNear = params.zNear;
        fullresParams.zFar = params.zFar;
        fullresParams.normalAware = mt_compute_ao_normal_upsample ? 1 : 0;

        MTL::Size fullGrid = { (NS::UInteger)mFullresWidth, (NS::UInteger)mFullresHeight, 1 };
        encoder->setComputePipelineState(upsamplePSO);
        encoder->setBytes(&fullresParams, sizeof(fullresParams), 0);
        encoder->setTexture(mLowresResultTexture ? mLowresResultTexture : aoTex, 0);
        encoder->setTexture(depthTex, 1);
        encoder->setTexture(normalTex, 2);
        encoder->setTexture(mFullresAOTexture, 3);
        encoder->dispatchThreads(fullGrid, MTL::Size(8, 8, 1));
        mFullresResultTexture = mFullresAOTexture;

        int atrousPasses = clamp((int)mt_compute_ao_atrous_passes, 0, 3);
        if (gl_ssao >= 3 && atrousPasses == 0) atrousPasses = 1;

        MTL::Texture *srcPass = mFullresAOTexture;
        MTL::Texture *dstPass = mFullresTempTexture;
        for (int pass = 0; pass < atrousPasses; pass++) {
            fullresParams.atrousStep = 1 << pass;
            encoder->memoryBarrier(MTL::BarrierScopeTextures);
            encoder->setComputePipelineState(atrousPSO);
            encoder->setBytes(&fullresParams, sizeof(fullresParams), 0);
            encoder->setTexture(srcPass, 0);
            encoder->setTexture(normalTex, 1);
            encoder->setTexture(dstPass, 2);
            encoder->dispatchThreads(fullGrid, MTL::Size(8, 8, 1));
            mFullresResultTexture = dstPass;
            MTL::Texture *tmp = srcPass;
            srcPass = dstPass;
            dstPass = tmp;
        }
    }

    encoder->endEncoding();
}
