// Test HLSL Vertex Shader
// Demonstrates HLSL → SPIR-V → MSL translation
// Compatible with DX12, Vulkan (via SPIR-V), and Metal (via SPIR-V→MSL)

cbuffer SceneConstants : register(b0)
{
    float4x4 modelMatrix;
    float4x4 viewMatrix;
    float4x4 projectionMatrix;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 texcoord : TEXCOORD0;
    float4 color : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    // Transform position to clip space
    float4 worldPos = mul(float4(input.position, 1.0), modelMatrix);
    float4 viewPos = mul(worldPos, viewMatrix);
    output.position = mul(viewPos, projectionMatrix);

    // Transform normal to world space
    output.worldNormal = mul(input.normal, (float3x3)modelMatrix);

    // Pass through texture coordinates and color
    output.texcoord = input.texcoord;
    output.color = input.color;

    return output;
}
