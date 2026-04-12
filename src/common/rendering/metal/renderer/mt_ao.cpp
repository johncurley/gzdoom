#include "mt_ao.h"
#include "mt_renderbuffers.h"
#include "../system/mt_renderdevice.h"
#include "../renderer/mt_debug.h"
#include "printf.h"
#include <chrono>
#include <cstdlib>

EXTERN_CVAR(Bool, mt_debug)

static const char* SSAO_COMPUTE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct SSAOParams {
    float4x4 invProj;
    float radius;
    float bias;
    float intensity;
    float2 screenRes;
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

kernel void ssao_compute(
    uint2 gid [[thread_position_in_grid]],
    constant SSAOParams &params [[buffer(0)]],
    constant AOFlags &flags [[buffer(1)]],
    texture2d<float, access::sample> ditherTexture [[texture(0)]],
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::write> aoOutput [[texture(2)]])
{
    float2 outSize = float2((float)aoOutput.get_width(), (float)aoOutput.get_height());
    if (gid.x >= outSize.x || gid.y >= outSize.y) return;

    sampler linearSampler(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::repeat);

    float2 uv = float2(gid) / outSize;
    if (flags.flipY == 1) uv.y = 1.0 - uv.y;
    float centerDepth = depthTexture.sample(linearSampler, uv).r; // Read depth via sampling (handles differing resolutions)

    if (centerDepth >= 0.9999 || centerDepth <= 0.0001) { 
        aoOutput.write(float4(1.0), gid); 
        return; 
    }

    float3 centerViewPos = ReconstructViewPos(uv, centerDepth, params.invProj, params.screenRes);

    float2 noiseUV = float2(gid) / 64.0; // Assuming 64x64 noise texture
    float4 noise = ditherTexture.sample(nearestSampler, noiseUV);
    float rotation = noise.x * 6.283185;
    float jitter = noise.y;

    float occlusion = 0.0;
    
    for(int i = 0; i < NUM_SAMPLES; i++) {
        float r = sqrt((float(i) + jitter) / float(NUM_SAMPLES));
        float theta = float(i) * GOLDEN_ANGLE + rotation;
        
        float2 dir = float2(cos(theta), sin(theta)) * r;
        
        // Sample position around the center pixel
        float2 sampleCoord = float2(gid) + dir * params.radius;
        float2 sampleUV = sampleCoord / outSize;
        if (flags.flipY == 1) sampleUV.y = 1.0 - sampleUV.y;
        
        float sampleRawDepth = depthTexture.sample(linearSampler, sampleUV).r;
        
        // Reconstruct view position for the sampled point
        float3 sampleViewPos = ReconstructViewPos(sampleUV, sampleRawDepth, params.invProj, params.screenRes);

        // Occlusion test in view space
        float dist = length(centerViewPos - sampleViewPos);
        float rangeCheck = smoothstep(0.0, 1.0, params.radius / dist);

        // Check if sample is behind the center pixel based on view Z (Reverse-Z)
        if (sampleViewPos.z > centerViewPos.z + params.bias) { // Greater Z means closer to camera in Reverse-Z
            occlusion += rangeCheck;
        }
    }

    float visibility = 1.0 - (occlusion / float(NUM_SAMPLES)) * params.intensity;
    visibility = max(visibility, 0.2); // Don't let AO go completely black
    
    aoOutput.write(float4(saturate(visibility), 0, 0, 1.0), gid);
}

kernel void bilateral_blur(
    uint2 gid [[thread_position_in_grid]],
    texture2d<float, access::read_write> aoTexture [[texture(0)]])
{
    if (gid.x >= aoTexture.get_width() || gid.y >= aoTexture.get_height()) return;

    float center = aoTexture.read(gid).r;
    float sum = center;
    float totalWeight = 1.0;

    int2 offsets[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};

    for(int i = 0; i < 4; i++) {
        uint2 sampleCoord = uint2(int2(gid) + offsets[i]);
        if (sampleCoord.x < aoTexture.get_width() && sampleCoord.y < aoTexture.get_height()) {
            float val = aoTexture.read(sampleCoord).r;
            float weight = 1.0 - abs(val - center) * 10.0;
            weight = saturate(weight);
            sum += val * weight;
            totalWeight += weight;
        }
    }

    aoTexture.write(float4(sum / totalWeight, 0, 0, 1.0), gid);
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
    auto deviceObj = fb->device->device;

    auto sourceString = NS::String::string(SSAO_COMPUTE_SOURCE, NS::UTF8StringEncoding);

    NS::Error* error = nullptr;
    auto library = deviceObj->newLibrary(sourceString, nullptr, &error);

    if (!library) {
        if (error) {
            Printf(PRINT_BOLD, "Error: Failed to compile Metal AO library: %s\n", error->localizedDescription()->utf8String());
        } else {
            Printf(PRINT_BOLD, "Error: Failed to compile Metal AO library (unknown error).\n");
        }
        return;
    }
    
    auto ssaoFunc = library->newFunction(NS::String::string("ssao_compute", NS::UTF8StringEncoding));
    if (ssaoFunc) {
        ssaoPSO = deviceObj->newComputePipelineState(ssaoFunc, (NS::Error**)nullptr);
        ssaoFunc->release();
    }
    
    auto blurFunc = library->newFunction(NS::String::string("bilateral_blur", NS::UTF8StringEncoding));
    if (blurFunc) {
        blurPSO = deviceObj->newComputePipelineState(blurFunc, (NS::Error**)nullptr);
        blurFunc->release();
    }

    // ssao_combine (compute) intentionally not created: the engine postprocess
    // provides a fragment-based SSAO combine that avoids writing to the scene
    // color from compute shaders (some hardware doesn't support read-write on
    // BGRA8Unorm). Rely on the engine path for final compositing.
    
    library->release();
}

MtAOModule::~MtAOModule() {
    if (ssaoPSO) ssaoPSO->release();
    if (blurPSO) blurPSO->release();
    if (combinePSO) combinePSO->release();
}

void MtAOModule::Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* depthTex, MTL::Texture* aoTex, MTL::Texture* ditherTex, MTL::Texture* fogTex, MTL::Texture* combineTex, const SSAOParams& params) {
    // Execute SSAO + blur compute passes. Final compositing into the scene is
    // intentionally omitted here (the engine postprocess handles SSAO combine)
    if (!ssaoPSO || !blurPSO || !ditherTex || !aoTex) return;
    auto startTime = std::chrono::high_resolution_clock::now();
    
    auto encoder = cmdBuf->computeCommandEncoder();
    if (!encoder) return;

    // Diagnostics: log sizes and params to help trace inversion/scaling issues
    if (mt_debug) {
        Printf(PRINT_HIGH, "MtAOModule::Execute: depth=%dx%d ao=%dx%d params.screenRes=%dx%d viewport=(virt:%dx%d phys:%dx%d)\n",
               (int)depthTex->width(), (int)depthTex->height(), (int)aoTex->width(), (int)aoTex->height(),
               (int)params.screenRes[0], (int)params.screenRes[1], fb->GetWidth(), fb->GetHeight(), fb->GetBuffers()->GetWidth(), fb->GetBuffers()->GetHeight());
    }

    // 1. SSAO Pass
    encoder->setComputePipelineState(ssaoPSO);
    encoder->setBytes(&params, sizeof(SSAOParams), 0);
    encoder->setTexture(ditherTex, 0);
    encoder->setTexture(depthTex, 1);
    encoder->setTexture(aoTex, 2);

    // Provisional flip fix: if AO compute expects logical params but AO texture is physical-sized,
    // supply a small uniform that shaders can use to flip Y or adjust uv mapping.
    struct AOFlags { int flipY; float invBackingScale; } aoFlags;
    aoFlags.flipY = (((int)aoTex->height() == fb->GetBuffers()->GetHeight()) && ((int)params.screenRes[1] != (int)aoTex->height())) ? 1 : 0;
    aoFlags.invBackingScale = aoFlags.flipY ? (float)params.screenRes[1] / (float)aoTex->height() : 1.0f;
    encoder->setBytes(&aoFlags, sizeof(aoFlags), 1);

    MTL::Size gridSize = { (NS::UInteger)aoTex->width(), (NS::UInteger)aoTex->height(), 1 };
    encoder->dispatchThreads(gridSize, MTL::Size(16, 16, 1));
    
    // Barrier to resolve read-write hazards
    encoder->memoryBarrier(MTL::BarrierScopeTextures);
    
    // 2. Blur Pass
    encoder->setComputePipelineState(blurPSO);
    encoder->setTexture(aoTex, 0);
    encoder->dispatchThreads(gridSize, MTL::Size(16, 16, 1));

    // Barrier and finish compute work. Composite is done by engine postprocess.
    encoder->memoryBarrier(MTL::BarrierScopeTextures);
    encoder->endEncoding();
    
    auto endTime = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    if (fb && fb->GetDebugManager()) {
        fb->GetDebugManager()->RecordStall("ao_pass", (float)ms);
    }
}
