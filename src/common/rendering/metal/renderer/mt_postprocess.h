#pragma once

#include "intrect.h"
#include <functional>
#include <memory>

class MetalRenderDevice;
class MtTextureImage;

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
  void AmbientOccludeScene(float m5);
  void UpdateShadowMap();
  void ImageTransitionScene(bool undefinedSrcLayout);
  void BlitSceneToPostprocess();
  void BlitCurrentToImage(MTL::Texture *dstimage);
  void SetSceneRenderTarget(bool useSSAO);

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
