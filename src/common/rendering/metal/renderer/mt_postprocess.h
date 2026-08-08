#pragma once

#include "intrect.h"
#include <functional>
#include <memory>

class MetalRenderDevice;
class MtTextureImage;
struct HWViewpointUniforms;

namespace MTL {
class Texture;
} // namespace MTL

// Post-processing effects
class MtPostprocess {
public:
  MtPostprocess(MetalRenderDevice *fb);
  ~MtPostprocess();

  // Post-processing operations
  void BlurScene(float amount);
  void AmbientOccludeScene(float m5, const HWViewpointUniforms* currentViewpoint);
  void UpdateShadowMap();
  void ImageTransitionScene(bool undefinedSrcLayout);
  void BlitSceneToPostprocess();
  void BlitCurrentToImage(MTL::Texture *dstimage);
  void SetSceneRenderTarget(bool useSSAO);
  // Drop the cached palette-tonemap lookup texture so it is rebuilt from the
  // current game palette. Mirrors VkPostprocess::ClearTonemapPalette.
  void ClearTonemapPalette();
  // Undo the render-target and draw-buffer state that the mid-scene AO
  // postprocess passes leave behind. See the definition for why this matters.
  void RestoreSceneRenderTargetAfterAO();

  // Scene rendering
  void SetActiveRenderTarget();
  void PostProcessScene(bool swscene, int fixedcm, float flash,
                        const std::function<void()> &afterBloomDrawEndScene2D);
  void DrawPresentTexture(IntRect box, bool applyGamma, bool screenshot);
  MTL::Texture *GetCurrentTexture();

  int mCurrentPipelineImage = 0;

private:
  MetalRenderDevice *fb = nullptr;
};
