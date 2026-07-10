#pragma once

#include <Metal/Metal.hpp>
#include <vector>

class MetalRenderDevice;

// Bloom parameters
struct BloomParams {
    float threshold;
    float strength;
    float srcRes[2];
    float bloomRes[2];
    float srcScale[2];
    float srcOffset[2];
    float viewportOrigin[2];
    float sampleWeights[8];
};

class MtBloomModule {
public:
    MtBloomModule(MetalRenderDevice* fb);
    ~MtBloomModule();

    bool Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* sceneColor, float amount);

private:
    MetalRenderDevice* fb;

    MTL::ComputePipelineState* extractPSO = nullptr;
    MTL::ComputePipelineState* downsamplePSO = nullptr;
    MTL::ComputePipelineState* blurHPSO = nullptr;
    MTL::ComputePipelineState* blurVPSO = nullptr;
    MTL::ComputePipelineState* combineAllPSO = nullptr;
    MTL::ComputePipelineState* combineRWPSO = nullptr;
    MTL::RenderPipelineState* compositePSO = nullptr;

    std::vector<MTL::Texture*> mDownsampledTextures; // Mip chain for blur
    std::vector<MTL::Texture*> mDownsampledTempTextures; // ping-pong temps per level
    MTL::Texture* mTempBlurTexture = nullptr; // For ping-pong blurring

    // Cached bloom ping-pong textures
    MTL::Texture* mBloomA = nullptr;
    MTL::Texture* mBloomB = nullptr;
    int mCachedBloomW = 0;
    int mCachedBloomH = 0;

    // Full-resolution high-precision bloom contribution
    MTL::Texture* mCompositeTex = nullptr;
    int mCompositeW = 0;
    int mCompositeH = 0;

    void CreateTextures(int width, int height, MTL::PixelFormat format);
    void ReleaseTextures();
};
