#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct type_SceneConstants
{
    float4x4 modelMatrix;
    float4x4 viewMatrix;
    float4x4 projectionMatrix;
};

struct main0_out
{
    float3 out_var_NORMAL [[user(locn0)]];
    float2 out_var_TEXCOORD0 [[user(locn1)]];
    float4 out_var_COLOR [[user(locn2)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 in_var_POSITION [[attribute(0)]];
    float3 in_var_NORMAL [[attribute(1)]];
    float2 in_var_TEXCOORD0 [[attribute(2)]];
    float4 in_var_COLOR [[attribute(3)]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant type_SceneConstants& SceneConstants [[buffer(0)]])
{
    main0_out out = {};
    out.gl_Position = ((float4(in.in_var_POSITION, 1.0) * SceneConstants.modelMatrix) * SceneConstants.viewMatrix) * SceneConstants.projectionMatrix;
    out.out_var_NORMAL = float3x3(float4(SceneConstants.modelMatrix[0][0], SceneConstants.modelMatrix[1][0], SceneConstants.modelMatrix[2][0], SceneConstants.modelMatrix[3][0]).xyz, float4(SceneConstants.modelMatrix[0][1], SceneConstants.modelMatrix[1][1], SceneConstants.modelMatrix[2][1], SceneConstants.modelMatrix[3][1]).xyz, float4(SceneConstants.modelMatrix[0][2], SceneConstants.modelMatrix[1][2], SceneConstants.modelMatrix[2][2], SceneConstants.modelMatrix[3][2]).xyz) * in.in_var_NORMAL;
    out.out_var_TEXCOORD0 = in.in_var_TEXCOORD0;
    out.out_var_COLOR = in.in_var_COLOR;
    return out;
}

