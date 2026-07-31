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
    // Nonzero when an exposure texture is bound to bloom_extract. Must stay
    // in this position -- mt_bloom.metal's BloomParams declares it here, and
    // tools/check_shader_parity.py compares field order.
    float useExposure;
    float sampleWeights[8];
};

class MtBloomModule {
public:
    MtBloomModule(MetalRenderDevice* fb);
    ~MtBloomModule();

    // exposureTex is hw_postprocess.exposure.CameraTexture (1x1 R32f). Pass
    // nullptr only when the camera exposure pass did not run this frame;
    // otherwise the extract diverges from the reference PP bloom path.
    bool Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* sceneColor, float amount,
                 MTL::Texture* exposureTex);

private:
    MetalRenderDevice* fb;

    MTL::ComputePipelineState* extractPSO = nullptr;
    MTL::ComputePipelineState* downsamplePSO = nullptr;
    MTL::ComputePipelineState* blurHPSO = nullptr;
    MTL::ComputePipelineState* blurVPSO = nullptr;
    MTL::ComputePipelineState* combineAllPSO = nullptr;
    MTL::ComputePipelineState* combineRWPSO = nullptr;
    // Tier 1 composite. Keyed on the scene colour format, which mt_hdr_pipeline
    // can change mid-session -- a PSO whose colour attachment format disagrees
    // with the render target is a Metal validation error.
    MTL::RenderPipelineState* compositePSO = nullptr;
    int compositePSOFormat = 0;
    MTL::Function* compositeVertexFn = nullptr;
    MTL::Function* compositeFragmentFn = nullptr;
    MTL::RenderPipelineState* GetCompositePSO(MTL::PixelFormat format);

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
