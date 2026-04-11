#pragma once
#include <Metal/Metal.hpp>

class MetalRenderDevice;

class MtAOModule {
public:
    MtAOModule(MetalRenderDevice* fb);
    ~MtAOModule();
    struct SSAOParams {
        float invProj[16];
        float radius;
        float bias;
        float intensity;
        float screenRes[2];
    };
    void Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* depthTex, MTL::Texture* aoTex, MTL::Texture* ditherTex, MTL::Texture* fogTex, MTL::Texture* combineTex, const SSAOParams& params);

private:
    MetalRenderDevice* fb;
    MTL::ComputePipelineState* ssaoPSO = nullptr;
    MTL::ComputePipelineState* blurPSO = nullptr;
    MTL::ComputePipelineState* combinePSO = nullptr;
};
