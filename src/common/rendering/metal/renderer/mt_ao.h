#pragma once
#include <Metal/Metal.hpp>

class MetalRenderDevice;
struct HWViewpointUniforms;

class MtAOModule {
public:
    MtAOModule(MetalRenderDevice* fb);
    ~MtAOModule();
    struct SSAOParams {
        // Affine view-space -> world-space transform for this frame's
        // camera (rotation-transpose in the upper-left 3x3, camera world
        // position in the translation column, (0,0,0,1) bottom row).
        // Built CPU-side in Render() from the caller's own currentViewpoint
        // -- NOT a general projective inverse, so no inversion can fail.
        // Used to world-lock the per-pixel jitter noise (see AGENTS.md);
        // repurposes the field that used to hold an inverse-projection
        // matrix for the now-dead ReconstructViewPos helper (confirmed via
        // grep: zero live call sites, all sample kernels use FetchViewPos).
        float viewToWorld[16];
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
        float fadeStartDistance;
        float fadeEndDistance;
        // World-space grid cell size (map units) the noise hash quantizes
        // to -- analogous to the old dither texture's implicit tiling
        // frequency. See mt_compute_ao_noise_cellsize's doc comment
        // (mt_postprocess.cpp).
        float noiseCellSize;
        // 0 = normal AO. Nonzero renders a world-locked-noise diagnostic
        // straight to aoOutput (viewable via the existing gl_ssao_debug 2
        // raw-AO display) instead of running real AO math -- see
        // mt_compute_ao_worldpos_debug's doc comment (mt_postprocess.cpp).
        int debugMode;
    };
    bool Render(float m5, int sceneWidth, int sceneHeight, const HWViewpointUniforms* currentViewpoint);
    void Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* depthTex, MTL::Texture* normalTex, MTL::Texture* sceneColorTex, MTL::Texture* aoTex, MTL::Texture* ditherTex, MTL::Texture* fogTex, MTL::Texture* combineTex, MTL::Texture* coverageTex, const SSAOParams& params, bool blurAO, bool useFullresCleanup, int algorithm);

private:
    void EnsureTextures(int width, int height);
    void EnsureFullresTextures(int width, int height);
    void EnsureDepthPyramid(int width, int height);
    void CreateDitherTexture();
    void CreateCoverageMaskPipeline();
    void RenderCoverageMask(MTL::Texture* depthStencilTex, int stencilValue);
    void Combine(MTL::Texture* aoTex, int sceneWidth, int sceneHeight, bool fullresAO);

    MetalRenderDevice* fb;
    MTL::ComputePipelineState* ssaoPSO = nullptr;
    MTL::ComputePipelineState* ssaoAlchemyPSO = nullptr;
    MTL::ComputePipelineState* depthLinearizePSO = nullptr;
    MTL::ComputePipelineState* ssaoMipPSO = nullptr;
    MTL::ComputePipelineState* blurPSO = nullptr;
    MTL::ComputePipelineState* upsamplePSO = nullptr;
    MTL::ComputePipelineState* atrousPSO = nullptr;
    MTL::ComputePipelineState* combinePSO = nullptr;
    MTL::RenderPipelineState* combineRenderPSO = nullptr;
    MTL::RenderPipelineState* coverageMaskPSO = nullptr;
    MTL::Texture* mAOTexture = nullptr;
    MTL::Texture* mBlurTexture = nullptr;
    MTL::Texture* mLowresResultTexture = nullptr;
    MTL::Texture* mFullresAOTexture = nullptr;
    MTL::Texture* mFullresTempTexture = nullptr;
    MTL::Texture* mFullresResultTexture = nullptr;
    MTL::Texture* mDitherTexture = nullptr;
    MTL::Texture* mCoverageMask = nullptr;
    MTL::Texture* mDepthPyramidTexture = nullptr;
    int mAOWidth = 0;
    int mAOHeight = 0;
    int mFullresWidth = 0;
    int mFullresHeight = 0;
    int mDepthPyramidWidth = 0;
    int mDepthPyramidHeight = 0;
};
