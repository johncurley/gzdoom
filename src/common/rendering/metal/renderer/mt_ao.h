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
        int numSteps;
        float maxThickness;
    };
    bool Render(float m5, int sceneWidth, int sceneHeight);
    void Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* depthTex, MTL::Texture* normalTex, MTL::Texture* sceneColorTex, MTL::Texture* aoTex, MTL::Texture* ditherTex, MTL::Texture* fogTex, MTL::Texture* combineTex, const SSAOParams& params, bool blurAO);

private:
    void EnsureTextures(int width, int height);
    void EnsureFullresTextures(int width, int height);
    void CreateDitherTexture();
    void Combine(MTL::Texture* aoTex, int sceneWidth, int sceneHeight, bool fullresAO);

    MetalRenderDevice* fb;
    MTL::ComputePipelineState* ssaoPSO = nullptr;
    MTL::ComputePipelineState* blurPSO = nullptr;
    MTL::ComputePipelineState* upsamplePSO = nullptr;
    MTL::ComputePipelineState* atrousPSO = nullptr;
    MTL::ComputePipelineState* combinePSO = nullptr;
    MTL::RenderPipelineState* combineRenderPSO = nullptr;
    MTL::Texture* mAOTexture = nullptr;
    MTL::Texture* mBlurTexture = nullptr;
    MTL::Texture* mLowresResultTexture = nullptr;
    MTL::Texture* mFullresAOTexture = nullptr;
    MTL::Texture* mFullresTempTexture = nullptr;
    MTL::Texture* mFullresResultTexture = nullptr;
    MTL::Texture* mDitherTexture = nullptr;
    int mAOWidth = 0;
    int mAOHeight = 0;
    int mFullresWidth = 0;
    int mFullresHeight = 0;
};
