#include <metal_stdlib>
using namespace metal;

fragment float4 compositor_fs(
    float2 uv [[stage_in]],
    texture2d<float> sceneColor [[texture(0)]],
    texture2d<float> aoTexture [[texture(1)]],
    texture2d<float> depthTexture [[texture(2)]],
    sampler s [[sampler(0)]])
{
    float3 color = sceneColor.sample(s, uv).rgb;
    float ao = aoTexture.sample(s, uv).r; 

    // Simple physical blend: AO darkens the ambient term, 
    // keeping direct light areas untouched.
    return float4(color * (1.0 - ao), 1.0);
}
