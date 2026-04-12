#include <metal_stdlib>
using namespace metal;

struct SSAOParams {
    float4x4 proj;
    float radius;
    float bias;
    float intensity;
    float2 screenRes;
};

// --- Dither Texture ---
// Used for rotating the sample pattern
texture2d<float, access::read> ditherTexture [[texture(0)]];
// Depth buffer
texture2d<float, access::read> depthTexture [[texture(1)]];
// Output AO texture
texture2d<float, access::write> aoOutput [[texture(2)]];

sampler linearSampler(mag_filter::filter_linear, min_filter::filter_linear, address::clamp_to_edge);
sampler nearestSampler(mag_filter::filter_nearest, min_filter::filter_nearest, address::repeat);

#define GOLDEN_ANGLE 2.39996323

kernel void ssao_compute(
    uint2 gid [[thread_position_in_grid]],
    constant SSAOParams &params [[buffer(0)]])
{
    if (gid.x >= params.screenRes.x || gid.y >= params.screenRes.y) return;

    float depth = depthTexture.read(gid).r;
    if (depth >= 0.9999) { 
        aoOutput.write(float4(1.0), gid); 
        return; 
    }

    // Generate random rotation from 4x4 dither texture
    float2 noiseUV = float2(gid % 4) / 4.0;
    float rotation = ditherTexture.sample(nearestSampler, noiseUV).r * 6.283185; 

    float2 samples[8] = {
        float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0),
        float2(0.707, 0.707), float2(-0.707, -0.707), float2(0.707, -0.707), float2(-0.707, 0.707)
    };

    float occlusion = 0.0;
    for(int i = 0; i < 8; i++) {
        float2 dir = samples[i];
        float2 rotatedDir = float2(dir.x * cos(rotation) - dir.y * sin(rotation),
                                   dir.x * sin(rotation) + dir.y * cos(rotation));
        
        float2 sampleUV = (float2(gid) + rotatedDir * params.radius) / params.screenRes;
        float sampleDepth = depthTexture.sample(linearSampler, sampleUV).r;
        
        // Simple comparison: if sample is behind center, it's occluded
        if (sampleDepth < depth - params.bias) {
            occlusion += 1.0;
        }
    }

    float visibility = 1.0 - (occlusion / 8.0) * params.intensity;
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
