#pragma once

#include "zstring.h"
#include "mt_resources.h"

#include <memory>

namespace MTL {
class Texture;
} // namespace MTL

class MetalRenderDevice;
class MtTextureImage;

// Render buffers (framebuffer targets)
class MtRenderBuffers {
public:
  // tag distinguishes the two live instances in the resource registry: the
  // screen buffers and the savegame-thumbnail buffers go through this same
  // class, and without a prefix the thumbnail's small textures overwrite the
  // screen's entries under identical names. Caught by the registry itself,
  // 2026-08-16, which reported every scene target as 216x162 and STALE.
  MtRenderBuffers(MetalRenderDevice *fb, const char *tag = "screen");
  ~MtRenderBuffers();

  // Get render targets
  void BeginFrame(int width, int height, int sceneWidth, int sceneHeight);

  int GetWidth() const { return mWidth; }
  int GetHeight() const { return mHeight; }
  const char *ResName(int slot) const { return mResNames[slot].GetChars(); }
  // Only the screen buffers track the scene viewport. The savegame-thumbnail
  // instance has its own size and must not be checked against it, or it reports
  // STALE on every frame -- which is how a validator earns being ignored.
  MtSizeRule SceneRule() const;
  enum ResSlot { RES_SceneColor, RES_SceneDepth, RES_SceneNormal,
                 RES_SceneFog, RES_Pipeline0, RES_Pipeline1, RES_Count };

  int GetSceneWidth() const { return mSceneWidth; }
  int GetSceneHeight() const { return mSceneHeight; }
  int GetSceneSamples() const { return mSamples; }

  // Pixel format of SceneColor and the postprocess pipeline images, as an
  // MTL::PixelFormat widened to int (this header stays metal-cpp free; cast at
  // the use site). Never hardcode the format at a use site -- the HDR pipeline
  // toggle switches these between BGRA8Unorm and RGBA16Float at runtime, and a
  // stale constant becomes an attachment/PSO format mismatch.
  int GetSceneColorFormat() const { return mColorFormat; }
  int GetPipelineFormat() const { return mColorFormat; }
  // SceneFog and SceneNormal keep their own formats. SceneFog is 8-bit in the
  // reference too (vk_renderpass.cpp drawBufferFormats), so it does not follow
  // the HDR toggle.
  int GetSceneFogFormat() const { return mFogFormat; }
  int GetSceneNormalFormat() const { return mNormalFormat; }

  std::unique_ptr<MtTextureImage> SceneColor;
  std::unique_ptr<MtTextureImage> SceneDepthStencil;
  std::unique_ptr<MtTextureImage> SceneNormal;
  std::unique_ptr<MtTextureImage> SceneFog;
  std::unique_ptr<MtTextureImage> ShadowMap;

  static const int NumPipelineImages = 2;
  std::unique_ptr<MtTextureImage> PipelineDepthStencil;
  std::unique_ptr<MtTextureImage> PipelineImage[NumPipelineImages];

private:
  FString mResNames[6];
  const char *mTag = "screen";
  void CreatePipelineDepthStencil(int width, int height);
  void CreatePipeline(int width, int height);
  void CreateScene(int width, int height, int samples);
  void CreateSceneColor(int width, int height, int samples);
  void CreateSceneDepthStencil(int width, int height, int samples);
  void CreateSceneFog(int width, int height, int samples);
  void CreateSceneNormal(int width, int height, int samples);
  void CreateShadowMap();

  // Returns the MTL::PixelFormat (as int) the color/pipeline buffers should
  // have right now, per the mt_hdr_pipeline CVAR.
  int DesiredColorFormat() const;

  MetalRenderDevice *fb = nullptr;
  int mColorFormat = 0;  // set on first BeginFrame
  int mNormalFormat = 0;
  int mFogFormat = 0;
  int mWidth = 0;
  int mHeight = 0;
  int mSceneWidth = 0;
  int mSceneHeight = 0;
  int mSamples = 1;
};
