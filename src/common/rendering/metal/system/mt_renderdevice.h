#pragma once

#include "engineerrors.h"
#include "gl_sysfb.h"
#include "hwrenderer/data/hw_viewpointuniforms.h"
#include "mt_ao.h"

#ifdef __APPLE__
#include <CoreVideo/CVDisplayLink.h>
#include <atomic>
#include <dispatch/dispatch.h>
#endif

#include <mutex>

#include "mt_version.h"

#ifdef __APPLE__

// Forward declarations (no Metal headers in public interface unless needed)
namespace MTL {
class Device;
class CommandQueue;
class Buffer;
class Texture;
} // namespace MTL

namespace CA {class MetalLayer;
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
class MtBloomModule;
class MtBinaryArchive;
class MtDebugManager;
class MtComputeManager;
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
  MtVersionManager mVersionManager;
  std::mutex mRecycleMutex;

  // Inflight-semaphore balance audit. The wait happens in BeginFrame() and is
  // guarded twice (early-out on mInFrame, and only when acquiring a drawable);
  // the signal happens in MtCommandBufferManager::EndFrame(), which every
  // Update() reaches unconditionally. If those two counts diverge, the
  // semaphore gains permits it never had to earn, backpressure decays, the CPU
  // runs ahead and nextDrawable() starves -- which is the stall actually
  // observed. Signalled from Metal's completion-handler thread, hence atomic.
  std::atomic<uint64_t> mSemWaits{0};
  std::atomic<uint64_t> mSemSignals{0};

  // Drawable lifecycle audit. nextDrawable() blocks for a full second when no
  // drawable is free, and the open question is why: a drawable acquired but
  // never presented (outstanding climbs and stays up) behaves differently from
  // one presented but retired late by the compositor (latency spikes while
  // outstanding stays bounded). Those are different bugs with different fixes,
  // and only the presented-handler can tell them apart.
  //
  // Presented handlers fire on a system thread, hence atomics throughout.
  std::atomic<int> mDrawablesOutstanding{0};
  std::atomic<int> mDrawablesPeakOutstanding{0};
  std::atomic<uint64_t> mDrawablesAcquired{0};
  std::atomic<uint64_t> mDrawablesPresented{0};
  std::atomic<uint64_t> mPresentLatencyUsTotal{0};
  std::atomic<uint64_t> mPresentLatencyUsMax{0};

  void RecordDrawableAcquired() {
    const int now = mDrawablesOutstanding.fetch_add(1, std::memory_order_relaxed) + 1;
    mDrawablesAcquired.fetch_add(1, std::memory_order_relaxed);
    int peak = mDrawablesPeakOutstanding.load(std::memory_order_relaxed);
    while (now > peak &&
           !mDrawablesPeakOutstanding.compare_exchange_weak(peak, now,
                                                            std::memory_order_relaxed)) {
    }
  }

  void RecordDrawablePresented(uint64_t latencyUs) {
    mDrawablesOutstanding.fetch_sub(1, std::memory_order_relaxed);
    mDrawablesPresented.fetch_add(1, std::memory_order_relaxed);
    mPresentLatencyUsTotal.fetch_add(latencyUs, std::memory_order_relaxed);
    uint64_t prev = mPresentLatencyUsMax.load(std::memory_order_relaxed);
    while (latencyUs > prev &&
           !mPresentLatencyUsMax.compare_exchange_weak(prev, latencyUs,
                                                       std::memory_order_relaxed)) {
    }
  }

  int GetDrawablesOutstanding() const { return mDrawablesOutstanding.load(std::memory_order_relaxed); }
  int GetDrawablesPeak() const { return mDrawablesPeakOutstanding.load(std::memory_order_relaxed); }
  uint64_t GetDrawablesAcquired() const { return mDrawablesAcquired.load(std::memory_order_relaxed); }
  uint64_t GetDrawablesPresented() const { return mDrawablesPresented.load(std::memory_order_relaxed); }
  uint64_t GetPresentLatencyUsTotal() const { return mPresentLatencyUsTotal.load(std::memory_order_relaxed); }
  uint64_t GetPresentLatencyUsMax() const { return mPresentLatencyUsMax.load(std::memory_order_relaxed); }

  void RecordSemWait() { mSemWaits.fetch_add(1, std::memory_order_relaxed); }
  void RecordSemSignal() { mSemSignals.fetch_add(1, std::memory_order_relaxed); }
  uint64_t GetSemWaits() const { return mSemWaits.load(std::memory_order_relaxed); }
  uint64_t GetSemSignals() const { return mSemSignals.load(std::memory_order_relaxed); }

  bool mIsDestroyed = false;

  // Resource recycling bin to keep buffers alive until GPU is done
  std::vector<MTL::Buffer *> mBufferRecycleBin[4];
  std::vector<MTL::Texture *> mTextureRecycleBin[4];
  std::vector<MTL::Buffer *> mStagingPool;
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
  MtBinaryArchive *GetBinaryArchive() { return mBinaryArchive.get(); }
  MtDebugManager *GetDebugManager() { return mDebugManager.get(); }
  MtComputeManager *GetComputeManager() { return mComputeManager.get(); }
  MtRenderBuffers *GetBuffers() { return mActiveRenderBuffers; }
  FRenderState *RenderState() override;
  MtAOModule* mAOModule = nullptr;
  MtBloomModule* mBloomModule = nullptr;

  unsigned int GetLightBufferBlockSize() const;

  MetalRenderDevice(void *hMonitor, bool fullscreen);
  ~MetalRenderDevice();
  bool IsMetal() override { return true; }
  bool RenderTextureIsFlipped() const override { return false; }
  bool UseBottomLeft2DProjection() const override { return true; }
  bool IsReverseZ() const override { return true; }
  float GetZNear() const override { return mZNear; }
  float GetZFar() const override { return mZFar; }

  void Update() override;

  void InitializeState() override;
  void SetMode(bool fullscreen, bool hiDPI) override;
  void SetViewportRects(IntRect *bounds) override;
  int GetClientWidth() override;
  int GetClientHeight() override;
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
  void AmbientOccludeScene(float m5, const HWViewpointUniforms* currentViewpoint) override;
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
  void NotifyDisplayTick();

  void Draw2D() override;

  void WaitForCommands(bool finish) override;
  void RecycleBuffer(MTL::Buffer *buffer);
  void RecycleTexture(MTL::Texture *texture);
  
  // Get a staging buffer from the pool or create a new one
  MTL::Buffer* GetStagingBuffer(size_t size);

#ifdef __APPLE__
  dispatch_semaphore_t GetInflightSemaphore() {
    return mInflightFramesSemaphore;
  }
#endif
  int GetFrameCount();

  CA::MetalDrawable *mCurrentDrawable = nullptr;

  MTL::SharedEvent *mGpuEvent = nullptr;
  uint64_t mNextEventValue = 1;
  float mZNear = 5.0f;
  float mZFar = 65536.0f;
  HWViewpointUniforms mLastSceneViewpoint;

private:
  bool mInFrame = false;
  void RenderTextureView(FCanvasTexture *tex,
                         std::function<void(IntRect &)> renderFunc) override;
  void PrintStartupLog();
  void CopyScreenToBuffer(int w, int h, uint8_t *data) override;
  void PresentFrame(void *drawable);
  void StartDisplayLink();
  void StopDisplayLink();
  void WaitForDisplayTick();

  // Manager instances (following Vulkan pattern)
  std::unique_ptr<MtCommandBufferManager> mCommands;
  std::unique_ptr<MtBufferManager> mBufferManager;
  std::unique_ptr<MtSamplerManager> mSamplerManager;
  std::unique_ptr<MtTextureManager> mTextureManager;
  std::unique_ptr<MtShaderManager> mShaderManager;
  std::unique_ptr<MtRenderBuffers> mScreenBuffers;
  std::unique_ptr<MtRenderBuffers> mSaveBuffers;
  std::unique_ptr<MtPostprocess> mPostprocess;
  std::unique_ptr<MtBinaryArchive> mBinaryArchive;
  std::unique_ptr<MtDebugManager> mDebugManager;
  std::unique_ptr<MtComputeManager> mComputeManager;
  std::unique_ptr<MtResourceBindingManager> mResourceBindingManager;
  std::unique_ptr<MtPipelineStateManager> mPipelineStateManager;
  std::unique_ptr<MtRenderState> mMtRenderState;

  MtRenderBuffers *mActiveRenderBuffers = nullptr;

  bool mVSync = false;
  int mFrameCount = 0;
#ifdef __APPLE__
  dispatch_semaphore_t mInflightFramesSemaphore;
  dispatch_semaphore_t mDisplayLinkSemaphore = nullptr;
  CVDisplayLinkRef mDisplayLink = nullptr;
#else
  void* mInflightFramesSemaphore = nullptr;
#endif
};

#endif // __APPLE__

class CMetalError : public CEngineError {
public:
  CMetalError() : CEngineError() {}
  CMetalError(const char *message) : CEngineError(message) {}
};
