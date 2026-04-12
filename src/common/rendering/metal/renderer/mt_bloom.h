#pragma once

#include <Metal/Metal.hpp>
#include <vector>

class MetalRenderDevice;

// Bloom parameters
struct BloomParams {
    float threshold;
    float intensity;
    float screenRes[2];
};

class MtBloomModule {
public:
    MtBloomModule(MetalRenderDevice* fb);
    ~MtBloomModule();

    void Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* sceneColor, const BloomParams& params);

private:
    MetalRenderDevice* fb;

    MTL::ComputePipelineState* mBrightPassPSO = nullptr;
    MTL::ComputePipelineState* mDownsamplePSO = nullptr;
    MTL::ComputePipelineState* mHorizontalBlurPSO = nullptr;
    MTL::ComputePipelineState* mVerticalBlurPSO = nullptr;
    MTL::ComputePipelineState* mCombinePSO = nullptr;

    std::vector<MTL::Texture*> mDownsampledTextures; // Mip chain for blur
    MTL::Texture* mTempBlurTexture = nullptr; // For ping-pong blurring

    void CreateTextures(int width, int height);
    void ReleaseTextures();
};
