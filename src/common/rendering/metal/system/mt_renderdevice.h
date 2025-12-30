#pragma once

#include "engineerrors.h"
#include "gl_sysfb.h"
#include <dispatch/dispatch.h>
#include <mutex>

// Prevent MacTypes.h from defining TimeScale by defining it as a macro
// temporarily
#define TimeScale TimeScale_GZDOOM

// Forward declarations (no Metal headers in public interface unless needed)
namespace MTL {
class Device;
class CommandQueue;
class Buffer;
class Texture;
} // namespace MTL

// Restore TimeScale
#undef TimeScale

namespace CA {
class MetalLayer;
class MetalDrawable;
} // namespace CA

struct FRenderViewpoint;
class MtSamplerManager;
class MtBufferManager;
class MtTextureManager;
class MtShaderManager;
class MtCommandBufferManager;
class MtResourceBindingManager;
class MtPipelineStateManager;
class MtRenderState;
class MtStreamBuffer;
class MtHardwareDataBuffer;
class MtHardwareTexture;
class MtRenderBuffers;
class MtPostprocess;
class SWSceneDrawer;

// Metal device wrapper
struct MetalDevice {
  MTL::Device *device;
  MTL::CommandQueue *commandQueue;
};

class MetalRenderDevice : public SystemBaseFrameBuffer {
  typedef SystemBaseFrameBuffer Super;

public:
  std::shared_ptr<MetalDevice> device;
  std::mutex mRecycleMutex;
  bool mIsDestroyed = false;

  // Resource recycling bin to keep buffers alive until GPU is done
  std::vector<MTL::Buffer *> mBufferRecycleBin[2];
  std::vector<MTL::Texture *> mTextureRecycleBin[2];
  int mCurrentFrameRecycleIndex = 0;

  // Manager accessors
  MtCommandBufferManager *GetCommands() { return mCommands.get(); }
  MtShaderManager *GetShaderManager() { return mShaderManager.get(); }
  MtSamplerManager *GetSamplerManager() { return mSamplerManager.get(); }
  MtBufferManager *GetBufferManager() { return mBufferManager.get(); }
  MtTextureManager *GetTextureManager() { return mTextureManager.get(); }
  MtResourceBindingManager *GetResourceBindingManager() {
    return mResourceBindingManager.get();
  }
  MtPipelineStateManager *GetPipelineStateManager() {
    return mPipelineStateManager.get();
  }
  MtRenderState *GetRenderState() { return mMtRenderState.get(); }
  MtPostprocess *GetPostprocess() { return mPostprocess.get(); }
  MtRenderBuffers *GetBuffers() { return mActiveRenderBuffers; }
  FRenderState *RenderState() override;

  unsigned int GetLightBufferBlockSize() const;

  MetalRenderDevice(void *hMonitor, bool fullscreen);
  ~MetalRenderDevice();
  bool IsMetal() override { return true; }
  bool RenderTextureIsFlipped() const override { return true; }
  bool IsReverseZ() const override { return true; }

  void Update() override;

  void InitializeState() override;
  void SetMode(bool fullscreen, bool hiDPI) override;
  bool CompileNextShader() override;
  void PrecacheMaterial(FMaterial *mat, int translation) override;
  void UpdatePalette() override;
  const char *DeviceName() const override;
  int Backend() override { return 3; } // 0=OpenGL, 1=Vulkan, 2=GLES, 3=Metal
  void SetTextureFilterMode() override;
  void StartPrecaching() override;
  void BeginFrame() override;
  void InitLightmap(int LMTextureSize, int LMTextureCount,
                    TArray<uint16_t> &LMTextureData) override;
  void BlurScene(float amount) override;
  void PostProcessScene(
      bool swscene, int fixedcm, float flash,
      const std::function<void()> &afterBloomDrawEndScene2D) override;
  void AmbientOccludeScene(float m5) override;
  void SetSceneRenderTarget(bool useSSAO) override;
  void SetLevelMesh(hwrenderer::LevelMesh *mesh) override;
  void UpdateShadowMap() override;
  void SetSaveBuffers(bool yes) override;
  void ImageTransitionScene(bool unknown) override;
  void SetActiveRenderTarget() override;

  IHardwareTexture *CreateHardwareTexture(int numchannels) override;
  FMaterial *CreateMaterial(FGameTexture *tex, int scaleflags) override;
  IVertexBuffer *CreateVertexBuffer() override;
  IIndexBuffer *CreateIndexBuffer() override;
  IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo,
                                bool needsresize) override;

  FTexture *WipeStartScreen() override;
  FTexture *WipeEndScreen() override;

  TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type,
                                      float &gamma) override;

  bool GetVSync() { return mVSync; }
  void SetVSync(bool vsync) override;

  void Draw2D() override;

  void WaitForCommands(bool finish) override;
  void RecycleBuffer(MTL::Buffer *buffer);
  void RecycleTexture(MTL::Texture *texture);
  
  dispatch_semaphore_t GetInflightSemaphore() { return mInflightFramesSemaphore; }
  int GetFrameCount();

  CA::MetalDrawable *mCurrentDrawable = nullptr;

private:
  bool mInFrame = false;
  void RenderTextureView(FCanvasTexture *tex,
                         std::function<void(IntRect &)> renderFunc) override;
  void PrintStartupLog();
  void CopyScreenToBuffer(int w, int h, uint8_t *data) override;
  void PresentFrame(void *drawable);

  // Manager instances (following Vulkan pattern)
  std::unique_ptr<MtCommandBufferManager> mCommands;
  std::unique_ptr<MtBufferManager> mBufferManager;
  std::unique_ptr<MtSamplerManager> mSamplerManager;
  std::unique_ptr<MtTextureManager> mTextureManager;
  std::unique_ptr<MtShaderManager> mShaderManager;
  std::unique_ptr<MtRenderBuffers> mScreenBuffers;
  std::unique_ptr<MtRenderBuffers> mSaveBuffers;
  std::unique_ptr<MtPostprocess> mPostprocess;
  std::unique_ptr<MtResourceBindingManager> mResourceBindingManager;
  std::unique_ptr<MtPipelineStateManager> mPipelineStateManager;
  std::unique_ptr<MtRenderState> mMtRenderState;

  MtRenderBuffers *mActiveRenderBuffers = nullptr;

  bool mVSync = false;
  dispatch_semaphore_t mInflightFramesSemaphore;
};

class CMetalError : public CEngineError {
public:
  CMetalError() : CEngineError() {}
  CMetalError(const char *message) : CEngineError(message) {}
};
