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
        // World units per AO pixel at unit view depth, used to scale the
        // noise cell size with distance (see NoiseCellSize). <= 0 disables
        // the depth-adaptive path, falling back to the fixed noiseCellSize.
        float pixelWorldScale;
        // Weight of the screen-space decorrelation term mixed into the
        // world-cell jitter (see AoNoise). 0 = pure world-locked noise.
        float screenNoiseMix;
        // screen->stencilValue for this frame. The kernels reject samples
        // whose stencil differs, i.e. samples from another portal layer.
        uint32_t stencilRef;
    };
    bool Render(float m5, int sceneWidth, int sceneHeight, const HWViewpointUniforms* currentViewpoint);
    void Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* depthTex, MTL::Texture* normalTex, MTL::Texture* sceneColorTex, MTL::Texture* aoTex, MTL::Texture* stencilTex, const SSAOParams& params, bool blurAO, bool useFullresCleanup, int algorithm);

private:
    void EnsureTextures(int width, int height);
    // A stencil texture VIEW (X32_Stencil8) over the scene depth/stencil
    // buffer, so the kernels can test portal layers themselves. This replaced
    // a full-screen render pass that materialized the same information into an
    // R8 texture and cost 3.12ms/frame -- more than the AO itself. Cached
    // because newTextureView allocates; rebuilt when the source changes.
    MTL::Texture* EnsureStencilView(MTL::Texture* depthStencilTex);
    void EnsureFullresTextures(int width, int height);
    void EnsureDepthPyramid(int width, int height);
    void Combine(MTL::Texture* aoTex, int sceneWidth, int sceneHeight, bool fullresAO);

    MetalRenderDevice* fb;
    MTL::ComputePipelineState* ssaoPSO = nullptr;
    MTL::ComputePipelineState* ssaoAlchemyPSO = nullptr;
    MTL::ComputePipelineState* depthLinearizePSO = nullptr;
    MTL::ComputePipelineState* ssaoMipPSO = nullptr;
    MTL::ComputePipelineState* blurPSO = nullptr;
    MTL::ComputePipelineState* upsamplePSO = nullptr;
    MTL::ComputePipelineState* atrousPSO = nullptr;
    // The combine pass renders into SceneColor, so its colour attachment format
    // must track mt_hdr_pipeline. The library is retained so the PSO can be
    // rebuilt when the format changes mid-session.
    MTL::RenderPipelineState* combineRenderPSO = nullptr;
    int combineRenderPSOFormat = 0;
    MTL::Library* combineLibrary = nullptr;
    MTL::RenderPipelineState* BuildCombinePipeline(MTL::Library* library,
                                                   MTL::PixelFormat colorFormat);
    bool EnsureCombinePSOFormat(int colorFormat);
    MTL::Texture* mAOTexture = nullptr;
    MTL::Texture* mBlurTexture = nullptr;
    MTL::Texture* mLowresResultTexture = nullptr;
    MTL::Texture* mFullresAOTexture = nullptr;
    MTL::Texture* mFullresTempTexture = nullptr;
    MTL::Texture* mFullresResultTexture = nullptr;
    MTL::Texture* mStencilView = nullptr;
    MTL::Texture* mStencilViewSource = nullptr;
    MTL::Texture* mDepthPyramidTexture = nullptr;
    int mAOWidth = 0;
    int mAOHeight = 0;
    int mAOScale = 1;   // resolved divisor, for the resource registry
    int mFullresWidth = 0;
    int mFullresHeight = 0;
    int mDepthPyramidWidth = 0;
    int mDepthPyramidHeight = 0;
};
