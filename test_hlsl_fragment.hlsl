// Test HLSL Pixel Shader
// Demonstrates HLSL → SPIR-V → MSL translation
// Compatible with DX12, Vulkan (via SPIR-V), and Metal (via SPIR-V→MSL)

Texture2D diffuseTexture : register(t0);
SamplerState diffuseSampler : register(s0);

cbuffer MaterialConstants : register(b1)
{
    float4 baseColor;
    float metallic;
    float roughness;
    float2 padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 texcoord : TEXCOORD0;
    float4 color : COLOR;
};

struct PSOutput
{
    float4 color : SV_TARGET0;
};

PSOutput main(PSInput input)
{
    PSOutput output;

    // Sample diffuse texture
    float4 texColor = diffuseTexture.Sample(diffuseSampler, input.texcoord);

    // Simple lighting (Lambertian diffuse)
    float3 lightDir = normalize(float3(1.0, 1.0, 1.0));
    float3 normal = normalize(input.worldNormal);
    float ndotl = max(dot(normal, lightDir), 0.0);

    // Combine texture, vertex color, material color, and lighting
    float3 finalColor = texColor.rgb * input.color.rgb * baseColor.rgb * ndotl;

    // Apply simple tonemapping
    finalColor = finalColor / (finalColor + 1.0);

    output.color = float4(finalColor, texColor.a * input.color.a * baseColor.a);

    return output;
}
