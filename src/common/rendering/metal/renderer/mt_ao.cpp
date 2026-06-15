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
};

struct AOFlags {
    int flipY;
    float invBackingScale;
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

float3 DecodeSceneNormal(float3 encodedNormal) {
    float3 normal = encodedNormal * 2.0 - 1.0;
    if (length(normal) <= 0.1) {
        return float3(0.0);
    }

    // Match the Metal-patched postprocess SSAO shader: the regular GLSL
    // normal.z flip is intentionally suppressed for Metal's view-space setup.
    return normalize(normal);
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
    texture2d<float, access::read> sourceTexture [[texture(0)]],
    texture2d<float, access::write> destTexture [[texture(1)]])
{
    if (gid.x >= destTexture.get_width() || gid.y >= destTexture.get_height()) return;

    float4 centerSample = sourceTexture.read(gid);
    float center = centerSample.r;
    float depth = centerSample.g;
    if (depth <= 1e-5) {
        destTexture.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float sum = center;
    float totalWeight = 1.0;

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
            float weight = 1.0 - abs(val - center) * 10.0;
            weight *= exp2(-abs(sampleDepth - depth) * 0.01);
            if (i >= 4) {
                weight *= 0.7071;
            }
            weight = saturate(weight);
            sum += val * weight;
            totalWeight += weight;
        }
    }

    destTexture.write(float4(sum / totalWeight, depth, 0, 1.0), gid);
}

struct AOCombineParams {
    int debugMode;
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
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
                                texture2d<float, access::sample> normalTexture [[texture(2)]])
{
    constexpr sampler linearSampler(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    constexpr sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);

    float2 aoUV = float2(in.uv.x, 1.0 - in.uv.y);
    float2 fogUV = float2(params.offsetX, params.offsetY) + in.uv * float2(params.scaleX, params.scaleY);
    float4 ssao = aoTexture.sample(linearSampler, aoUV);
    float4 fogSample = fogTexture.sample(nearestSampler, fogUV);
    float3 sceneNormal = normalTexture.sample(linearSampler, fogUV).xyz;
    float attenuation = ssao.x;
    float depthSignal = 1.0 - exp2(-ssao.y * 0.01);
    float depthMask = saturate(depthSignal);

    if (params.debugMode == 0) {
        float aoAlpha = (1.0 - attenuation) * depthMask;
        float2 aoTexel = 1.0 / float2((float)aoTexture.get_width(), (float)aoTexture.get_height());
        float4 ssaoL = aoTexture.sample(linearSampler, aoUV + float2(-aoTexel.x, 0.0));
        float4 ssaoR = aoTexture.sample(linearSampler, aoUV + float2( aoTexel.x, 0.0));
        float4 ssaoU = aoTexture.sample(linearSampler, aoUV + float2(0.0, -aoTexel.y));
        float4 ssaoD = aoTexture.sample(linearSampler, aoUV + float2(0.0,  aoTexel.y));
        float neighborAlpha =
            ((1.0 - ssaoL.x) * saturate(1.0 - exp2(-ssaoL.y * 0.01)) +
             (1.0 - ssaoR.x) * saturate(1.0 - exp2(-ssaoR.y * 0.01)) +
             (1.0 - ssaoU.x) * saturate(1.0 - exp2(-ssaoU.y * 0.01)) +
             (1.0 - ssaoD.x) * saturate(1.0 - exp2(-ssaoD.y * 0.01))) * 0.25;
        if (aoAlpha < 0.20) {
            aoAlpha = min(aoAlpha, neighborAlpha * 3.0);
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
)";

MtAOModule::MtAOModule(MetalRenderDevice* device) : fb(device) {
    auto shaderManager = fb->GetShaderManager();
    ssaoPSO = shaderManager->CreateComputePipeline("ssao_compute", SSAO_COMPUTE_SOURCE, "SSAO ssao_compute");
    blurPSO = shaderManager->CreateComputePipeline("bilateral_blur", SSAO_COMPUTE_SOURCE, "SSAO bilateral_blur");

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
    if (combinePSO) combinePSO->release();
    if (combineRenderPSO) combineRenderPSO->release();
    if (mAOTexture) mAOTexture->release();
    if (mBlurTexture) mBlurTexture->release();
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

bool MtAOModule::Render(float m5, int sceneWidth, int sceneHeight) {
    if (!ssaoPSO || !blurPSO || !combineRenderPSO || !mDitherTexture || !fb->GetBuffers())
        return false;

    auto buffers = fb->GetBuffers();
    if (!buffers->SceneDepthStencil || !buffers->SceneNormal || !buffers->SceneFog ||
        !buffers->SceneDepthStencil->GetTexture() || !buffers->SceneNormal->GetTexture() ||
        !buffers->SceneFog->GetTexture()) {
        return false;
    }

    const int aoScale = clamp((int)mt_compute_ao_scale, 2, 4);
    EnsureTextures((sceneWidth + aoScale - 1) / aoScale, (sceneHeight + aoScale - 1) / aoScale);
    if (!mAOTexture || !mBlurTexture)
        return false;

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
    switch (gl_ssao) {
    default:
    case 1: params.visibilityStrength = 2.35f; params.numDirections = 4; break;
    case 2: params.visibilityStrength = 2.75f; params.numDirections = 4; break;
    case 3: params.visibilityStrength = 3.15f; params.numDirections = 5; break;
    }

    auto cmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
    if (!cmdBuf)
        return false;

    fb->GetRenderState()->EndRenderPass();
    const bool blurAO = gl_ssao_debug < 2;
    auto aoStart = std::chrono::high_resolution_clock::now();
    Execute(cmdBuf, buffers->SceneDepthStencil->GetTexture(),
            buffers->SceneNormal->GetTexture(), buffers->SceneColor->GetTexture(),
            mAOTexture, mDitherTexture,
            buffers->SceneFog->GetTexture(), nullptr, params, blurAO);
    Combine(mAOTexture, sceneWidth, sceneHeight);
    auto aoEnd = std::chrono::high_resolution_clock::now();
    if (fb->GetComputeManager()) {
        float ms = std::chrono::duration<float, std::milli>(aoEnd - aoStart).count();
        fb->GetComputeManager()->RecordTiming(
            HWComputeEffect::AmbientOcclusion, ms);
    }
    return true;
}

void MtAOModule::Combine(MTL::Texture* aoTex, int sceneWidth, int sceneHeight) {
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
    } combineParams = {};
    combineParams.debugMode = gl_ssao_debug;
    auto sceneScale = fb->SceneScale();
    auto sceneOffset = fb->SceneOffset();
    combineParams.scaleX = sceneScale.X;
    combineParams.scaleY = sceneScale.Y;
    combineParams.offsetX = sceneOffset.X;
    combineParams.offsetY = sceneOffset.Y;

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
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0,
                            (NS::UInteger)3);
}

void MtAOModule::Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* depthTex, MTL::Texture* normalTex, MTL::Texture* sceneColorTex, MTL::Texture* aoTex, MTL::Texture* ditherTex, MTL::Texture* fogTex, MTL::Texture* combineTex, const SSAOParams& params, bool blurAO) {
    // Execute SSAO + blur compute passes. Final compositing into the scene is
    // intentionally omitted here (the engine postprocess handles SSAO combine)
    if (!ssaoPSO || (blurAO && (!blurPSO || !mBlurTexture)) || !depthTex || !normalTex || !sceneColorTex || !ditherTex || !aoTex) return;
    
    auto encoder = cmdBuf->computeCommandEncoder();
    if (!encoder) return;

    // 1. SSAO Pass
    encoder->setComputePipelineState(ssaoPSO);
    encoder->setBytes(&params, sizeof(SSAOParams), 0);
    encoder->setTexture(ditherTex, 0);
    encoder->setTexture(depthTex, 1);
    encoder->setTexture(aoTex, 2);
    encoder->setTexture(normalTex, 3);
    encoder->setTexture(sceneColorTex, 4);

    // Determine whether internal render textures are inverted.
    struct AOFlags { int flipY; float invBackingScale; } aoFlags;
    aoFlags.flipY = fb->RenderTextureIsFlipped() ? 1 : 0;
    aoFlags.invBackingScale = aoFlags.flipY ? params.screenResY / (float)aoTex->height() : 1.0f;
    encoder->setBytes(&aoFlags, sizeof(aoFlags), 1);

    MTL::Size gridSize = { (NS::UInteger)aoTex->width(), (NS::UInteger)aoTex->height(), 1 };
    encoder->dispatchThreads(gridSize, MTL::Size(16, 16, 1));
    
    // Barrier to resolve read-write hazards
    encoder->memoryBarrier(MTL::BarrierScopeTextures);
    
    if (blurAO) {
        // 2. Blur Pass
        encoder->setComputePipelineState(blurPSO);
        encoder->setTexture(aoTex, 0);
        encoder->setTexture(mBlurTexture, 1);
        encoder->dispatchThreads(gridSize, MTL::Size(16, 16, 1));

        // 3. Second bilateral pass back into the AO texture. This costs a
        // small amount of compute time but removes the residual stable dither
        // speckle that otherwise reads as snow in motion.
        encoder->memoryBarrier(MTL::BarrierScopeTextures);
        encoder->setTexture(mBlurTexture, 0);
        encoder->setTexture(aoTex, 1);
        encoder->dispatchThreads(gridSize, MTL::Size(16, 16, 1));
    }

    // Barrier and finish compute work. Composite is done by engine postprocess.
    encoder->memoryBarrier(MTL::BarrierScopeTextures);
    encoder->endEncoding();
}
