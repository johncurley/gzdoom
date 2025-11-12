#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct type_MaterialConstants
{
    float4 baseColor;
    float metallic;
    float roughness;
    float2 padding;
};

struct main0_out
{
    float4 out_var_SV_TARGET0 [[color(0)]];
};

struct main0_in
{
    float3 in_var_NORMAL [[user(locn0)]];
    float2 in_var_TEXCOORD0 [[user(locn1)]];
    float4 in_var_COLOR [[user(locn2)]];
};

fragment main0_out main0(main0_in in [[stage_in]], constant type_MaterialConstants& MaterialConstants [[buffer(0)]], texture2d<float> diffuseTexture [[texture(0)]], sampler diffuseSampler [[sampler(0)]])
{
    main0_out out = {};
    float4 _42 = diffuseTexture.sample(diffuseSampler, in.in_var_TEXCOORD0);
    float3 _54 = ((_42.xyz * in.in_var_COLOR.xyz) * MaterialConstants.baseColor.xyz) * precise::max(dot(fast::normalize(in.in_var_NORMAL), fast::normalize(float3(1.0))), 0.0);
    out.out_var_SV_TARGET0 = float4(_54 / (_54 + float3(1.0)), (_42.w * in.in_var_COLOR.w) * MaterialConstants.baseColor.w);
    return out;
}

