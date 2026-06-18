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
    int applyExponent;
    int normalAware;
    int flipY;
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
    float normalDot = saturate(dot(centerNormal, sampleNormal) * 0.5 + 0.5);
    return normalDot * normalDot * normalDot * normalDot;
}

float InterleavedGradientNoise(float2 p) {
    return fract(52.9829189 * fract(dot(p, float2(0.06711056, 0.00583715))));
}

kernel void ssao_compute(
    uint2 gid [[thread_position_in_grid]],
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

    sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::repeat);
    sampler nearestClampSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);

    float2 pixelCenter = float2(gid) + 0.5;
    float2 halfTexel = 0.5 / outSize;
    float2 uv = pixelCenter / outSize;
    if (flags.flipY == 1) uv.y = 1.0 - uv.y;
    float2 sceneUV = float2(params.offsetX, params.offsetY) + uv * float2(params.scaleX, params.scaleY);
    float centerCoverage = sceneColorTexture.sample(nearestClampSampler, sceneUV).a;
    if (centerCoverage <= 0.0001) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float centerDepth = depthTexture.sample(nearestClampSampler, sceneUV).r;

    if (centerDepth <= 0.0001) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return; 
    }

    float centerLinearDepth = LinearizeSceneDepth(centerDepth, params);
    float3 centerViewPos = FetchViewPos(uv, centerLinearDepth, params);
    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sceneUV).xyz);
    if (all(centerNormal == float3(0.0))) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float2 noiseUV = pixelCenter / 64.0; // Assuming 64x64 noise texture
    float4 noise = ditherTexture.sample(nearestSampler, noiseUV);
    float ign = InterleavedGradientNoise(pixelCenter);
    float rotation = atan2(noise.y, noise.x);
    float stepJitter = fract(noise.z + ign * 0.754877666);
    float directionJitter = fract(noise.w + ign * 0.569840296);

    float radiusPixels = params.radiusToScreen / max(centerViewPos.z, 1e-5);
    const int numSteps = 4;
    const int numDirections = clamp(params.numDirections, 4, 5);
    float stepSizePixels = radiusPixels / (float(numSteps) + 1.0);
    float minStepPixels = max(stepSizePixels, 1.0);

    float occlusion = 0.0;
    
    for(int directionIndex = 0; directionIndex < numDirections; directionIndex++) {
        float theta = (6.28318530718 / float(numDirections)) * (float(directionIndex) + directionJitter * 0.25) + rotation;
        float2 dir = float2(cos(theta), sin(theta));
        float horizon = 0.0;

        for (int stepIndex = 0; stepIndex < numSteps; stepIndex++) {
            float rayPixels = (float(stepIndex) + stepJitter + 1.0) * minStepPixels;
            float2 sampleUV = uv + dir * rayPixels / outSize;
            sampleUV = clamp(sampleUV, halfTexel, float2(1.0) - halfTexel);
            float2 sampleSceneUV = float2(params.offsetX, params.offsetY) + sampleUV * float2(params.scaleX, params.scaleY);

            float sampleCoverage = sceneColorTexture.sample(nearestClampSampler, sampleSceneUV).a;
            if (sampleCoverage <= 0.0001) {
                continue;
            }

            float sampleRawDepth = depthTexture.sample(nearestClampSampler, sampleSceneUV).r;
            if (sampleRawDepth <= 0.0001) {
                continue;
            }

            float3 sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sampleSceneUV).xyz);
            if (all(sampleNormal == float3(0.0))) {
                continue;
            }

            float sampleLinearDepth = LinearizeSceneDepth(sampleRawDepth, params);
            float3 sampleViewPos = FetchViewPos(sampleUV, sampleLinearDepth, params);
            float3 sampleVector = sampleViewPos - centerViewPos;
            float distanceSquare = max(dot(sampleVector, sampleVector), 1e-6);
            float invDistance = rsqrt(distanceSquare);
            float normalAngle = dot(centerNormal, sampleVector) * invDistance;
            float sampleHorizon = max(normalAngle - params.bias, 0.0);
            float falloff = saturate(distanceSquare * params.negInvR2 + 1.0);
            occlusion += max(sampleHorizon - horizon, 0.0) * falloff;
            horizon = max(horizon, sampleHorizon);
        }
    }

    occlusion *= params.aoMultiplier / float(numDirections * numSteps);
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

    int2 offsets[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };

    for(int i = 0; i < 8; i++) {
        uint2 sampleCoord = uint2(int2(gid) + offsets[i]);
        if (sampleCoord.x < sourceTexture.get_width() && sampleCoord.y < sourceTexture.get_height()) {
            float4 sampleValue = sourceTexture.read(sampleCoord);
            float val = sampleValue.r;
            float sampleDepth = sampleValue.g;
            if (sampleDepth <= 1e-5) {
                continue;
            }
            float r = (i >= 4) ? 1.41421356 : 1.0;
            float deltaZ = (sampleDepth - depth) * params.blurSharpness;
            float weight = exp2(-r * r * 0.22222222 - deltaZ * deltaZ);
            if (i >= 4) {
                weight *= 0.7071;
            }
            if (useNormals) {
                float2 sampleUV = (float2(sampleCoord) + 0.5) / texSize;
                if (params.flipY != 0) {
                    sampleUV.y = 1.0 - sampleUV.y;
                }
                float2 sampleSceneUV = float2(params.offsetX, params.offsetY) + sampleUV * float2(params.scaleX, params.scaleY);
                float3 sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, sampleSceneUV).xyz);
                weight *= mix(0.35, 1.0, NormalWeight(centerNormal, sampleNormal));
            }
            weight = saturate(weight);
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
    float combineSharpness = max(params.blurSharpness * 4.0, 0.02);

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
            float weight = bilinearWeight * exp2(-depthDelta * depthDelta);
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
    int2 offsets[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };
    float sum = centerAO.x;
    float totalWeight = 1.0;
    for (int i = 0; i < 8; i++) {
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
        float spatial = (i >= 4) ? 0.5 : 0.75;
        float weight = spatial * exp2(-depthDelta * depthDelta);
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
    float depthSignal = 1.0 - exp2(-sceneDepth * 0.01);
    float depthMask = saturate(depthSignal);
    float3 decodedNormal = sceneNormal * 2.0 - 1.0;

    if (params.debugMode == 0) {
        if (rawSceneDepth <= 0.0001 || length(decodedNormal) <= 0.1) {
            return float4(fogSample.rgb, 0.0);
        }

        if (params.fullresAO != 0) {
            float aoAlpha = (1.0 - ssao.x) * depthMask;
            aoAlpha *= smoothstep(0.020, 0.100, aoAlpha);
            return float4(fogSample.rgb, aoAlpha);
        }

        float centerAlpha = (1.0 - attenuation) * saturate(1.0 - exp2(-ssao.y * 0.01));
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
            float tapAlpha = (1.0 - taps[i].x) * saturate(1.0 - exp2(-taps[i].y * 0.01));
            float depthDelta = (taps[i].y - centerDepth) * blurSharpness;
            float weight = exp2(-0.35 - depthDelta * depthDelta);
            alphaSum += tapAlpha * weight;
            weightSum += weight;
        }
        float aoAlpha = mix(centerAlpha, alphaSum / weightSum, saturate(params.combineSmooth));
        float neighborAlpha =
            ((1.0 - ssaoL.x) * saturate(1.0 - exp2(-ssaoL.y * 0.01)) +
             (1.0 - ssaoR.x) * saturate(1.0 - exp2(-ssaoR.y * 0.01)) +
             (1.0 - ssaoU.x) * saturate(1.0 - exp2(-ssaoU.y * 0.01)) +
             (1.0 - ssaoD.x) * saturate(1.0 - exp2(-ssaoD.y * 0.01))) * 0.25;
        if (aoAlpha < neighborAlpha * 0.6 && neighborAlpha > 0.005) {
            aoAlpha = mix(aoAlpha, neighborAlpha, 0.65);
        }
        aoAlpha *= smoothstep(0.020, 0.100, aoAlpha);
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
