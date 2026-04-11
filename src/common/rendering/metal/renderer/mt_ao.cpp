#include "mt_ao.h"
#include "../system/mt_renderdevice.h"
#include "../renderer/mt_debug.h"
#include "printf.h"
#include <chrono>
#include <cstdlib>

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
    texture2d<float, access::sample> ditherTexture [[texture(0)]],
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::write> aoOutput [[texture(2)]])
{
    if (gid.x >= params.screenRes.x || gid.y >= params.screenRes.y) return;

    sampler linearSampler(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::repeat);

    float2 uv = float2(gid) / params.screenRes;
    float centerDepth = depthTexture.read(gid).r; // Read raw depth (Reverse-Z: 1=Near, 0=Far)

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
        float2 sampleUV = (float2(gid) + dir * params.radius) / params.screenRes;
        
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
    texture2d<float, access::read_write> sceneTexture [[texture(1)]])
{
    if (gid.x >= sceneTexture.get_width() || gid.y >= sceneTexture.get_height()) return;

    float ao = aoTexture.read(gid).r;
    float4 scene = sceneTexture.read(gid);

    // Debug preview mode: write AO directly as grayscale to the scene texture
    if (debugMode != 0) {
        float3 gray = float3(ao, ao, ao);
        scene.rgb = gray;
        scene.a = 1.0;
        sceneTexture.write(scene, gid);
        return;
    }

    // Apply AO as a simple multiplicative occlusion factor and clamp to avoid over-darkening
    float occlusionFactor = clamp(ao, 0.25, 1.0);
    scene.rgb *= occlusionFactor;

    sceneTexture.write(scene, gid);
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

    auto combineFunc = library->newFunction(NS::String::string("ssao_combine", NS::UTF8StringEncoding));
    if (combineFunc) {
        combinePSO = deviceObj->newComputePipelineState(combineFunc, (NS::Error**)nullptr);
        combineFunc->release();
    }
    
    library->release();
}

MtAOModule::~MtAOModule() {
    if (ssaoPSO) ssaoPSO->release();
    if (blurPSO) blurPSO->release();
    if (combinePSO) combinePSO->release();
}

void MtAOModule::Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* depthTex, MTL::Texture* aoTex, MTL::Texture* ditherTex, MTL::Texture* combineTex, const SSAOParams& params) {
    if (!ssaoPSO || !blurPSO || !combinePSO || !ditherTex || !combineTex) return;
    auto startTime = std::chrono::high_resolution_clock::now();
    
    auto encoder = cmdBuf->computeCommandEncoder();
    
    // 1. SSAO Pass
    encoder->setComputePipelineState(ssaoPSO);
    encoder->setBytes(&params, sizeof(SSAOParams), 0);
    encoder->setTexture(ditherTex, 0);
    encoder->setTexture(depthTex, 1);
    encoder->setTexture(aoTex, 2);
    
    MTL::Size gridSize = {aoTex->width(), aoTex->height(), 1};
    encoder->dispatchThreads(gridSize, MTL::Size(16, 16, 1));
    
    // Barrier to resolve read-write hazards
    encoder->memoryBarrier(MTL::BarrierScopeTextures);
    
    // 2. Blur Pass
    encoder->setComputePipelineState(blurPSO);
    encoder->setTexture(aoTex, 0);
    encoder->dispatchThreads(gridSize, MTL::Size(16, 16, 1));

    // Barrier before combine
    encoder->memoryBarrier(MTL::BarrierScopeTextures);

    // 3. Combine Pass (Native Metal blend)
    encoder->setComputePipelineState(combinePSO);
    int debugMode = (getenv("GZ_AO_DEBUG") != nullptr) ? 1 : 0;
    encoder->setBytes(&debugMode, sizeof(debugMode), 0);
    encoder->setTexture(aoTex, 0);
    encoder->setTexture(combineTex, 1);
    encoder->dispatchThreads(gridSize, MTL::Size(16, 16, 1));
    
    encoder->endEncoding();
    
    auto endTime = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    if (fb && fb->GetDebugManager()) {
        fb->GetDebugManager()->RecordStall("ao_pass", (float)ms);
    }
}
