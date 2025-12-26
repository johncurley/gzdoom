#pragma once

#include <memory>

#define TimeScale TimeScale_GZDOOM
namespace MTL {
class Texture;
} // namespace MTL
#undef TimeScale

class MetalRenderDevice;
class MtTextureImage;

// Render buffers (framebuffer targets)
class MtRenderBuffers {
public:
  MtRenderBuffers(MetalRenderDevice *fb);
  ~MtRenderBuffers();

  // Get render targets
  void BeginFrame(int width, int height, int sceneWidth, int sceneHeight);

  int GetWidth() const { return mWidth; }
  int GetHeight() const { return mHeight; }
  int GetSceneWidth() const { return mSceneWidth; }
  int GetSceneHeight() const { return mSceneHeight; }
  int GetSceneSamples() const { return mSamples; }

  std::unique_ptr<MtTextureImage> SceneColor;
  std::unique_ptr<MtTextureImage> SceneDepthStencil;
  std::unique_ptr<MtTextureImage> SceneNormal;
  std::unique_ptr<MtTextureImage> SceneFog;
  std::unique_ptr<MtTextureImage> ShadowMap;

  static const int NumPipelineImages = 2;
  std::unique_ptr<MtTextureImage> PipelineDepthStencil;
  std::unique_ptr<MtTextureImage> PipelineImage[NumPipelineImages];

private:
  void CreatePipelineDepthStencil(int width, int height);
  void CreatePipeline(int width, int height);
  void CreateScene(int width, int height, int samples);
  void CreateSceneColor(int width, int height, int samples);
  void CreateSceneDepthStencil(int width, int height, int samples);
  void CreateSceneFog(int width, int height, int samples);
  void CreateSceneNormal(int width, int height, int samples);
  void CreateShadowMap();

  MetalRenderDevice *fb = nullptr;
  int mWidth = 0;
  int mHeight = 0;
  int mSceneWidth = 0;
  int mSceneHeight = 0;
  int mSamples = 1;
};
