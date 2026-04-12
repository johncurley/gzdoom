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
};

class MtBloomModule {
public:
    MtBloomModule(MetalRenderDevice* fb);
    ~MtBloomModule();

    void Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* sceneColor, float amount);

private:
    MetalRenderDevice* fb;

    MTL::ComputePipelineState* extractPSO = nullptr;
    MTL::ComputePipelineState* blurHPSO = nullptr;
    MTL::ComputePipelineState* blurVPSO = nullptr;
    MTL::ComputePipelineState* combinePSO = nullptr;

    std::vector<MTL::Texture*> mDownsampledTextures; // Mip chain for blur
    MTL::Texture* mTempBlurTexture = nullptr; // For ping-pong blurring

    // Cached bloom ping-pong textures
    MTL::Texture* mBloomA = nullptr;
    MTL::Texture* mBloomB = nullptr;
    int mCachedBloomW = 0;
    int mCachedBloomH = 0;

    void CreateTextures(int width, int height, MTL::PixelFormat format);
    void ReleaseTextures();
};
