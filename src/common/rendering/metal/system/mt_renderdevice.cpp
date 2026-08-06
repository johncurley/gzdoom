/*
**  Metal backend
**  Copyright (c) 2025 GZDoom Contributors
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
*/

#include "i_time.h"
#include "metal/metal_common.h"

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <CoreVideo/CoreVideo.h>

#include <chrono>
#include <cmath>

#include "hw_renderstate.h"
#include "i_interface.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_bloom.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/renderer/mt_resourcebinding.h"
#include "metal/renderer/mt_debug.h"
#include "metal/renderer/mt_compute.h"
#include "metal/shaders/mt_shader.h"
#include "metal/textures/mt_sampler.h"
#include "metal/textures/mt_texture.h"
#include "mt_buffer.h"
#include "mt_commandbuffer.h"
#include "mt_hwbuffer.h"
#include "mt_renderdevice.h"

#include "mt_binaryarchive.h"
#include "textures.h"

#include "c_console.h"
#include "c_dispatch.h"
#include "engineerrors.h"
#include "flatvertices.h"
#include "gamestate.h"
#include "hw_bonebuffer.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_lightbuffer.h"
#include "hw_skydome.h"
#include "hw_viewpointuniforms.h"
#include "hw_vrmodes.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "m_png.h"
#include "menu.h"
#include "menustate.h"
#include "r_videoscale.h"
#include "v_draw.h"
#include "v_text.h"
#include "v_video.h"
#include "version.h"

#ifdef __APPLE__
#include "../../platform/posix/cocoa/cocoanativehandle.h"
#endif

// Semaphore is created after version manager initialization so the count
// matches maxDrawableCount exactly. Defined here so it can still be used
// as the timeout guard in BeginFrame.
static constexpr int kDefaultMaxFramesInFlight = 3;

static CVReturn MetalDisplayLinkCallback(CVDisplayLinkRef,
                                         const CVTimeStamp *,
                                         const CVTimeStamp *,
                                         CVOptionFlags,
                                         CVOptionFlags *,
                                         void *displayLinkContext) {
  auto fb = static_cast<MetalRenderDevice *>(displayLinkContext);
  if (fb)
    fb->NotifyDisplayTick();
  return kCVReturnSuccess;
}

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)

CVAR(Bool, mt_debug, false, 0)

void MetalError(const char *text) { throw CMetalError(text); }

void MetalPrintLog(const char *typestr, const std::string &msg) {
  Printf(PRINT_LOG, TEXTCOLOR_RED "[Metal %s] ", typestr);
  Printf(PRINT_LOG, TEXTCOLOR_WHITE "%s\n", msg.c_str());
}

MetalRenderDevice::MetalRenderDevice(void *hMonitor, bool fullscreen)
    : Super(hMonitor, fullscreen) {
  mInflightFramesSemaphore = dispatch_semaphore_create(kDefaultMaxFramesInFlight);
  mDisplayLinkSemaphore = dispatch_semaphore_create(0);
  mPipelineNbr = 3;
  device = std::make_shared<MetalDevice>();
  device->device = MTL::CreateSystemDefaultDevice();

  if (!device->device) {
    MetalError("Failed to create Metal device");
  }

  device->commandQueue = device->device->newCommandQueue();
  if (!device->commandQueue) {
    MetalError("Failed to create Metal command queue");
  }

}

MetalRenderDevice::~MetalRenderDevice() {
  mIsDestroyed = true;
  StopDisplayLink();

  // Safely reset all post-processing backend resources while we are still alive
  PPResource::ResetAll();

  if (mCommands) {
    mCommands->WaitForCommands(true);
  }

  // Destroy native postprocess modules
  if (mBloomModule) { delete mBloomModule; mBloomModule = nullptr; }
  if (mAOModule) { delete mAOModule; mAOModule = nullptr; }

  delete mVertexData;
  delete mSkyData;
  delete mViewpoints;
  delete mLights;
  delete mBones;
  mShadowMap.Reset();

  mMtRenderState.reset();
  mPipelineStateManager.reset();
  mResourceBindingManager.reset();
  mComputeManager.reset();
  mPostprocess.reset();
  mSaveBuffers.reset();
  mScreenBuffers.reset();
  mShaderManager.reset();
  mTextureManager.reset();
  mSamplerManager.reset();
  if (mBufferManager) {
    mBufferManager->Deinit();
    mBufferManager.reset();
  }
  mCommands.reset();

  if (device) {
    if (device->commandQueue) {
      device->commandQueue->release();
    }
    if (device->device) {
      device->device->release();
    }
  }

  for (int i = 0; i < 3; i++) {
    for (auto *buffer : mBufferRecycleBin[i]) {
      buffer->release();
    }
    mBufferRecycleBin[i].clear();

    for (auto *texture : mTextureRecycleBin[i]) {
      texture->release();
    }
    mTextureRecycleBin[i].clear();
  }

  for (auto *buffer : mStagingPool) {
    buffer->release();
  }
  mStagingPool.clear();

  if (mInFrame) {
    dispatch_semaphore_signal(mInflightFramesSemaphore);
  }
  if (mDisplayLinkSemaphore) {
    dispatch_release(mDisplayLinkSemaphore);
    mDisplayLinkSemaphore = nullptr;
  }
  dispatch_release(mInflightFramesSemaphore);
}

void MetalRenderDevice::InitializeState() {
  static bool first = true;
  if (first) {
    PrintStartupLog();
    first = false;
  }

#ifdef __APPLE__
  CocoaNativeHandle nativeHandle = GetNativeHandle();
  if (nativeHandle.metalLayer) {
    CA::MetalLayer *metalLayer = (CA::MetalLayer *)nativeHandle.metalLayer;
    metalLayer->setDevice(device->device);
    metalLayer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);

    MetalViewSize viewSize = GetMetalViewDrawableSize(nativeHandle.nsWindow);
    // Validate that view size is at least minimum dimensions before using it
    // This prevents incorrect scaling during early window initialization
    if (viewSize.width >= VID_MIN_WIDTH && viewSize.height >= VID_MIN_HEIGHT) {
      // Drawable size must be in physical pixels for 1:1 Retina rendering
      metalLayer->setDrawableSize(CGSizeMake(viewSize.width, viewSize.height));

      // Engine resolution uses logical points — viewport/scissor coordinates
      // from GZDoom are in logical space; MtRenderState scales to physical.
      SetVirtualSize((int)viewSize.logicalWidth, (int)viewSize.logicalHeight);
      V_OutputResized((int)viewSize.logicalWidth, (int)viewSize.logicalHeight);
      if (mVertexData) mVertexData->OutputResized((int)viewSize.logicalWidth, (int)viewSize.logicalHeight);
      SetViewportRects(nullptr);
    } else {
      // Fall back: no Retina information, use engine resolution for both
      metalLayer->setDrawableSize(CGSizeMake(GetWidth(), GetHeight()));
    }

    metalLayer->setDisplaySyncEnabled(mVSync);
    metalLayer->setMaximumDrawableCount(
        (NS::UInteger)mVersionManager.maxDrawableCount);
    metalLayer->setAllowsNextDrawableTimeout(true);

    // framebufferOnly=true lets Metal/IOKit use the fast scanout path on Apple
    // Silicon (TBDR) since we only ever write to the swapchain drawable, never
    // read it back. On Intel/AMD the driver can produce tiling artifacts in
    // fullscreen if this is true, so keep it false for non-TBDR GPUs.
    metalLayer->setFramebufferOnly(mVersionManager.isTBDR);

    if (mVersionManager.presentsWithTransaction) {
      // Synchronize with Cocoa transactions for smoother UI integration
      // Note: presentsWithTransaction is not explicitly in metal-cpp
      // CAMetalLayer class but we can use the selector if we really needed it.
      // For now we rely on the standard presentation.
    }

    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (srgb) {
      metalLayer->setColorspace(srgb);
      CGColorSpaceRelease(srgb);
    }
  }
#endif

  const char *deviceName = device->device->name()->utf8String();
  mVersionManager.Initialize(device->device);

  // Re-create the inflight semaphore now that we know the actual drawable
  // count. On macOS 11+ the layer uses 3 drawables (triple buffering), so the
  // semaphore must allow the same number of frames in flight — a mismatch
  // causes nextDrawable() to block because the CPU races ahead and exhausts
  // the drawable pool before the GPU has finished presenting.
  dispatch_release(mInflightFramesSemaphore);
  mInflightFramesSemaphore = dispatch_semaphore_create(mVersionManager.maxDrawableCount);

  vendorstring = deviceName;
  hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
  glslversion = 4.50f;
  uniformblockalignment = 256;
  maxuniformblock = 65536;

  mCommands.reset(new MtCommandBufferManager(this));
  mSamplerManager.reset(new MtSamplerManager(this));
  mTextureManager.reset(new MtTextureManager(this));
  mBufferManager.reset(new MtBufferManager(this));
  mBufferManager->Init();

  mScreenBuffers.reset(new MtRenderBuffers(this));
  mSaveBuffers.reset(new MtRenderBuffers(this));
  mActiveRenderBuffers = mScreenBuffers.get();

  mPostprocess.reset(new MtPostprocess(this));
  mBinaryArchive.reset(new MtBinaryArchive(this));
  mBinaryArchive->Init();
  mDebugManager.reset(new MtDebugManager(this));
  mComputeManager.reset(new MtComputeManager(this));
  mResourceBindingManager.reset(new MtResourceBindingManager(this));
  mPipelineStateManager.reset(new MtPipelineStateManager(this));
  mShaderManager.reset(new MtShaderManager(this));
  mAOModule = new MtAOModule(this);
  mBloomModule = new MtBloomModule(this);
  mMtRenderState.reset(new MtRenderState(this));

  FMaterial::SetLayerCallback([](int layer, int translation) -> IHardwareTexture* {
    auto fb = static_cast<MetalRenderDevice*>(screen);
    return fb->GetTextureManager()->GetPaletteTexture(translation, layer == 2);
  });

  mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), mPipelineNbr);
  mSkyData = new FSkyVertexBuffer;
  mViewpoints = new HWViewpointBuffer(mPipelineNbr);
  mLights = new FLightBuffer(mPipelineNbr);
  mBones = new BoneBuffer(mPipelineNbr);

  mFrameCount = 0; // Reset for Startup Lag Guard effectiveness

  Printf(PRINT_LOG,
         TEXTCOLOR_GREEN "Metal renderer initialized successfully!\n");
}

// Defined in renderer/mt_capture.cpp. Bracketed around the whole frame rather
// than hooked into the postprocess chain, because a capture is only useful if
// it contains the frame's complete command stream. Declared here, above
// Update(), because Update() closes the frame and BeginFrame() opens it.
void MtCaptureBeginFrameIfArmed(MetalRenderDevice *fb);
void MtCaptureEndFrameIfCapturing();

void MetalRenderDevice::Update() {
  NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();

  // Normal rendering enters through D_Display(), which calls BeginFrame()
  // before drawing. Wipes call Update() directly in a loop, so start a frame
  // here only for that path instead of pre-acquiring the next drawable at the
  // end of every regular frame.
  if (!mInFrame) {
    BeginFrame();
    if (!mInFrame) {
      pool->release();
      return;
    }
  }

  twoD.Reset();
  Flush3D.Reset();
  Flush3D.Clock();

  // 1. Set target and Draw 2D into PipelineImage[0] (where the scene is now)
  if (mPostprocess) {
    mPostprocess->SetActiveRenderTarget();
  }

  this->Draw2D();

  if (mMtRenderState) {
    mMtRenderState->EndRenderPass();
    mMtRenderState->EndFrame();
  }

  Flush3D.Unclock();

  if (mCurrentDrawable) {
    auto drawableTexture = mCurrentDrawable->texture();
    int width = (int)drawableTexture->width();
    int height = (int)drawableTexture->height();

    // 3. Blit the final result (3D + 2D) from PipelineImage[0] to the swapchain
    if (mPostprocess) {
      // Use LOGICAL dimensions. MtRenderState::ApplyViewport will correctly 
      // scale these to the physical swapchain pixels using the Retina ratio.
      IntRect logicalBox = {0, 0, GetWidth(), GetHeight()};
      mPostprocess->DrawPresentTexture(logicalBox, true, false);

      // Reset viewport/scissor after present blit
      mMtRenderState->SetViewport(0, 0, GetWidth(), GetHeight());
      mMtRenderState->SetScissor(0, 0, GetWidth(), GetHeight());
    }

    if (mMtRenderState)
      mMtRenderState->EndRenderPass();

    PresentFrame(mCurrentDrawable);

    // Force synchronous flush during startup to ensure loading screen
    // and progress bar updates are 100% visible and correctly tiled.
    if (gamestate == GS_STARTUP) {
      mCommands->FlushCommands(true);
    }
  }

  if (mCommands) {
    mCommands->EndFrame();
  }

  if (!mVSync) {
    this->FPSLimit();
  }

  twod->Clear();

  Super::Update();
  pool->release();

  // Release drawable AFTER the pool is popped to ensure any references
  // in the pool (like in RenderPassDescriptors) are already gone.
  if (mCurrentDrawable) {
    mCurrentDrawable->release();
    mCurrentDrawable = nullptr;
  }

  if (mDebugManager)
    mDebugManager->EndFrame();

  mInFrame = false;

  // After the drawable is released and the frame is fully closed out, so the
  // trace contains the present as well as the scene and postprocess work.
  MtCaptureEndFrameIfCapturing();
}

void MetalRenderDevice::PresentFrame(void *drawablePtr) {
  if (!drawablePtr)
    return;

  auto drawable = (CA::MetalDrawable *)drawablePtr;
  void *cmdBufPtr = mCommands->GetRenderCommandBuffer();
  if (!cmdBufPtr)
    return;

  auto commandBuffer = (MTL::CommandBuffer *)cmdBufPtr;
  commandBuffer->presentDrawable(drawable);
}

void MetalRenderDevice::StartDisplayLink() {
#ifdef __APPLE__
  if (mDisplayLink && CVDisplayLinkIsRunning(mDisplayLink))
    return;

  if (!mDisplayLink) {
    CVReturn result = CVDisplayLinkCreateWithActiveCGDisplays(&mDisplayLink);
    if (result != kCVReturnSuccess || !mDisplayLink) {
      Printf(PRINT_LOG, "Metal: Failed to create CVDisplayLink (%d)\n",
             (int)result);
      mDisplayLink = nullptr;
      return;
    }
    CVDisplayLinkSetOutputCallback(mDisplayLink, MetalDisplayLinkCallback,
                                   this);
  }

  CVReturn result = CVDisplayLinkStart(mDisplayLink);
  if (result != kCVReturnSuccess)
    Printf(PRINT_LOG, "Metal: Failed to start CVDisplayLink (%d)\n",
           (int)result);
#endif
}

void MetalRenderDevice::StopDisplayLink() {
#ifdef __APPLE__
  if (mDisplayLink) {
    if (CVDisplayLinkIsRunning(mDisplayLink))
      CVDisplayLinkStop(mDisplayLink);
    CVDisplayLinkRelease(mDisplayLink);
    mDisplayLink = nullptr;
  }
#endif
}

void MetalRenderDevice::NotifyDisplayTick() {
#ifdef __APPLE__
  if (mDisplayLinkSemaphore)
    dispatch_semaphore_signal(mDisplayLinkSemaphore);
#endif
}

void MetalRenderDevice::WaitForDisplayTick() {
#ifdef __APPLE__
  if (!mDisplayLinkSemaphore || !mDisplayLink ||
      !CVDisplayLinkIsRunning(mDisplayLink))
    return;

  while (dispatch_semaphore_wait(mDisplayLinkSemaphore,
                                 DISPATCH_TIME_NOW) == 0) {
  }

  if (dispatch_semaphore_wait(
          mDisplayLinkSemaphore,
          dispatch_time(DISPATCH_TIME_NOW, 1000 * NSEC_PER_MSEC)) != 0) {
    if (mDebugManager)
      mDebugManager->RecordStall("displaylink_timeout", 1000.0f);
  }
#endif
}

void MetalRenderDevice::BeginFrame() {
  if (mInFrame)
    return;
  mInFrame = true;

  MtCaptureBeginFrameIfArmed(this);

  SetViewportRects(nullptr);
  mViewpoints->Clear();
  mLights->Clear();
  mBones->Clear();

  // If we already have a drawable, don't acquire another one.
  // This happens during wipes where Update() calls BeginFrame() for us.
  if (!mCurrentDrawable) {
    // Wait for GPU backpressure (MaxFramesInFlight frames allowed). If the
    // wait exceeds the diagnostic threshold, keep waiting for the real
    // completion signal; fabricating a signal lets the CPU outrun the drawable
    // pool and creates large timing spikes.
    {
      auto semWaitStart = std::chrono::high_resolution_clock::now();
      if (dispatch_semaphore_wait(
              mInflightFramesSemaphore,
              dispatch_time(DISPATCH_TIME_NOW, 1000 * NSEC_PER_MSEC)) != 0) {
        float semMs = std::chrono::duration<float, std::milli>(
                          std::chrono::high_resolution_clock::now() - semWaitStart)
                          .count();
        if (mDebugManager)
          mDebugManager->RecordStall("semaphore_timeout", semMs);
        dispatch_semaphore_wait(mInflightFramesSemaphore,
                                DISPATCH_TIME_FOREVER);
      } else {
        float semMs = std::chrono::duration<float, std::milli>(
                          std::chrono::high_resolution_clock::now() - semWaitStart)
                          .count();
        if (semMs >= 2.0f && mDebugManager)
          mDebugManager->RecordStall("semaphore", semMs);
      }
    }

    {
      std::lock_guard<std::mutex> lock(mRecycleMutex);
      mCurrentFrameRecycleIndex = (mCurrentFrameRecycleIndex + 1) % 4;
      for (auto *buffer : mBufferRecycleBin[mCurrentFrameRecycleIndex]) {
        buffer->release();
      }
      mBufferRecycleBin[mCurrentFrameRecycleIndex].clear();

      for (auto *texture : mTextureRecycleBin[mCurrentFrameRecycleIndex]) {
        texture->release();
      }
      mTextureRecycleBin[mCurrentFrameRecycleIndex].clear();
    }

    CocoaNativeHandle nativeHandle = GetNativeHandle();
    if (nativeHandle.metalLayer) {
      CA::MetalLayer *metalLayer = (CA::MetalLayer *)nativeHandle.metalLayer;

      MetalViewSize viewSize = GetMetalViewDrawableSize(nativeHandle.nsWindow);

      if (viewSize.width >= VID_MIN_WIDTH && viewSize.height >= VID_MIN_HEIGHT) {
        // Engine resolution tracks logical points
        int targetWidth = (int)viewSize.logicalWidth;
        int targetHeight = (int)viewSize.logicalHeight;
        int scaledWidth = ViewportScaledWidth(targetWidth, targetHeight);
        int scaledHeight = ViewportScaledHeight(targetWidth, targetHeight);

        // Retina backing scale — used to size the drawable in physical pixels
        float backingScale = (viewSize.logicalWidth > 0)
                               ? (viewSize.width / viewSize.logicalWidth)
                               : 1.0f;
        float targetDrawableW = scaledWidth  * backingScale;
        float targetDrawableH = scaledHeight * backingScale;

        // STARTUP LAG GUARD: Cocoa can report a smaller-than-configured size
        // during the first few frames while layout settles. Don't downscale.
        if (mFrameCount < 10 &&
            (scaledWidth < GetWidth() || scaledHeight < GetHeight())) {
          metalLayer->setDrawableSize(CGSizeMake(GetWidth() * backingScale,
                                                 GetHeight() * backingScale));
        }
        // Normal: resync when logical resolution or backing scale changes
        else if (GetWidth() != scaledWidth || GetHeight() != scaledHeight ||
                 fabsf((float)metalLayer->drawableSize().width  - targetDrawableW) > 0.1f ||
                 fabsf((float)metalLayer->drawableSize().height - targetDrawableH) > 0.1f) {
          SetVirtualSize(scaledWidth, scaledHeight);
          V_OutputResized(scaledWidth, scaledHeight);
          if (mVertexData) mVertexData->OutputResized(scaledWidth, scaledHeight);
          metalLayer->setDrawableSize(CGSizeMake(targetDrawableW, targetDrawableH));
          SetViewportRects(nullptr);
        }
      } else {
        float backingScale = (viewSize.logicalWidth > 0)
                               ? (viewSize.width / viewSize.logicalWidth) : 1.0f;
        float targetDrawableW = GetWidth()  * backingScale;
        float targetDrawableH = GetHeight() * backingScale;
        if (fabsf((float)metalLayer->drawableSize().width - targetDrawableW) > 0.1f)
          metalLayer->setDrawableSize(CGSizeMake(targetDrawableW, targetDrawableH));
      }

      mCurrentDrawable = (CA::MetalDrawable *)metalLayer->nextDrawable();

      if (mCurrentDrawable && mFrameCount < 10) {
        mFrameCount++;
      }
    }

    if (!mCurrentDrawable) {
      mInFrame = false;
      return;
    }

    mCurrentDrawable->retain();
  }

  // Retina Fix: Initialize screen buffers with the PHYSICAL drawable size.
  // This ensures the G-buffer matches the physical resolution of the window.
  auto drawableTexture = mCurrentDrawable->texture();
  int physicalWidth = (int)drawableTexture->width();
  int physicalHeight = (int)drawableTexture->height();

  // Pipeline buffers are physical drawable-sized. Scene buffers use the
  // logical screen viewport, matching the GL/Vulkan postprocess contract.
  mScreenBuffers->BeginFrame(physicalWidth, physicalHeight,
                             mScreenViewport.width, mScreenViewport.height);
  mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH,
                           SAVEPICHEIGHT);

  // Correctly cycle to the next set of buffers for the new frame
  mVertexData->NextPipelineBuffer();

  if (mCommands)
    mCommands->BeginFrame();

  if (mMtRenderState)
    mMtRenderState->BeginFrame();

  if (mResourceBindingManager)
    mResourceBindingManager->BeginFrame();

  if (mDebugManager)
    mDebugManager->BeginFrame();

  // Drain any async texture decompression tasks that completed on background
  // GCD threads and upload their pixel data to the GPU now.
  if (mTextureManager)
    mTextureManager->ProcessAsyncTextureLoads();

  // Set default render target to PipelineImage[0] for 2D/UI drawn before
  // Update()
  if (mPostprocess) {
    mPostprocess->SetActiveRenderTarget();
  }
}

bool MetalRenderDevice::CompileNextShader() {
  if (mShaderManager)
    return mShaderManager->CompileNextShader();
  return false;
}

void MetalRenderDevice::SetVSync(bool vsync) {
  mVSync = vsync;
#ifdef __APPLE__
  CocoaNativeHandle nativeHandle = GetNativeHandle();
  if (nativeHandle.metalLayer) {
    CA::MetalLayer *metalLayer = (CA::MetalLayer *)nativeHandle.metalLayer;
    metalLayer->setDisplaySyncEnabled(vsync);
  }
  StopDisplayLink();
#endif
}

void MetalRenderDevice::SetViewportRects(IntRect *bounds) {
  Super::SetViewportRects(bounds);

  // The base framebuffer may scale mScreenViewport/mSceneViewport to the
  // physical output letterbox for GL-style rendering. Metal keeps scene
  // textures in logical space and scales only when presenting to the drawable,
  // so rebuild the internal viewports from logical dimensions every frame.
  // mOutputLetterbox from the base class remains physical.
  if (bounds)
    return;

  int logicalWidth = GetWidth();
  int logicalHeight = GetHeight();
  if (logicalWidth <= 0 || logicalHeight <= 0)
    return;

  mScreenViewport.left = 0;
  mScreenViewport.top = 0;
  mScreenViewport.width = logicalWidth;
  mScreenViewport.height = logicalHeight;

  if (sysCallbacks.GetSceneRect)
    mSceneViewport = sysCallbacks.GetSceneRect();
  else
    mSceneViewport = mScreenViewport;
}

void MetalRenderDevice::SetMode(bool fullscreen, bool hiDPI) {
  Super::SetMode(fullscreen, hiDPI);
  if (mVertexData) mVertexData->OutputResized(GetWidth(), GetHeight());

  // Keep the startup lag guard for initial window creation, but do not
  // re-enable it after explicit mode switches; doing so can keep the previous
  // fullscreen/windowed logical size alive for several frames.
  mFrameCount = mScreenBuffers ? 10 : 0;

  // Force Metal layer drawable size update after mode change
#ifdef __APPLE__
  CocoaNativeHandle nativeHandle = GetNativeHandle();
  if (nativeHandle.metalLayer) {
    CA::MetalLayer *metalLayer = (CA::MetalLayer *)nativeHandle.metalLayer;

    // Use physical resolution for the drawable size to ensure 1:1 pixel mapping
    MetalViewSize viewSize = GetMetalViewDrawableSize(nativeHandle.nsWindow);
    if (viewSize.width >= VID_MIN_WIDTH && viewSize.height >= VID_MIN_HEIGHT) {
        metalLayer->setDrawableSize(CGSizeMake(viewSize.width, viewSize.height));
    } else {
        metalLayer->setDrawableSize(CGSizeMake(GetWidth(), GetHeight()));
    }
  }
#endif
}

void MetalRenderDevice::PrintStartupLog() {
  const char *deviceName = device->device->name()->utf8String();
  Printf(PRINT_LOG, TEXTCOLOR_CYAN "Metal Renderer for GZDoom\n");
  Printf(PRINT_LOG, "  Device: %s\n", deviceName);
  Printf(PRINT_LOG, "  Architecture: %s\n", mVersionManager.GetArchName());
  Printf(PRINT_LOG, "  OS Version: macOS %d.%d.%d\n", mVersionManager.osMajor,
         mVersionManager.osMinor, mVersionManager.osPatch);
  Printf(PRINT_LOG, "  API Version: Metal %d.%d\n",
         mVersionManager.metalVersion / 10, mVersionManager.metalVersion % 10);
  Printf(PRINT_LOG, "  Backend: Native Metal (metal-cpp)\n");
  Printf(PRINT_LOG, "\n");
}

const char *MetalRenderDevice::DeviceName() const {
  if (device && device->device)
    return device->device->name()->utf8String();
  return "Metal Device";
}

int MetalRenderDevice::GetClientWidth() {
#ifdef __APPLE__
  CocoaNativeHandle nativeHandle = GetNativeHandle();
  MetalViewSize viewSize = GetMetalViewDrawableSize(nativeHandle.nsWindow);
  if (viewSize.logicalWidth >= VID_MIN_WIDTH)
    return (int)viewSize.logicalWidth;
#endif
  return Super::GetClientWidth();
}

int MetalRenderDevice::GetClientHeight() {
#ifdef __APPLE__
  CocoaNativeHandle nativeHandle = GetNativeHandle();
  MetalViewSize viewSize = GetMetalViewDrawableSize(nativeHandle.nsWindow);
  if (viewSize.logicalHeight >= VID_MIN_HEIGHT)
    return (int)viewSize.logicalHeight;
#endif
  return Super::GetClientHeight();
}

FRenderState *MetalRenderDevice::RenderState() {
  return static_cast<FRenderState *>(mMtRenderState.get());
}

void MetalRenderDevice::WaitForCommands(bool finish) {
  if (mCommands)
    mCommands->WaitForCommands(finish);
}

void MetalRenderDevice::RecycleBuffer(MTL::Buffer *buffer) {
  if (buffer) {
    if (mIsDestroyed) {
      buffer->release();
      return;
    }
    
    try {
        std::lock_guard<std::mutex> lock(mRecycleMutex);
        
        // Return suitable buffers to the staging pool instead of releasing them
        if (buffer->storageMode() == MTL::StorageModeShared && mStagingPool.size() < 32) {
            mStagingPool.push_back(buffer);
        } else {
            mBufferRecycleBin[mCurrentFrameRecycleIndex].push_back(buffer);
        }
    } catch (const std::system_error& e) {
        // Mutex is invalid or destroyed - just release the buffer
        buffer->release();
    }
  }
}

MTL::Buffer* MetalRenderDevice::GetStagingBuffer(size_t size) {
    try {
        std::lock_guard<std::mutex> lock(mRecycleMutex);
        
        // Find a buffer in the pool that fits the requested size
        for (size_t i = 0; i < mStagingPool.size(); ++i) {
            if (mStagingPool[i]->length() >= size) {
                auto buf = mStagingPool[i];
                mStagingPool.erase(mStagingPool.begin() + i);
                return buf;
            }
        }
    } catch (const std::system_error& e) {
        // Fall through to creation if lock fails
    }
    
    // Fallback: create a new one
    return device->device->newBuffer(size, MTL::StorageModeShared);
}

void MetalRenderDevice::RecycleTexture(MTL::Texture *texture) {
  if (texture) {
    if (mIsDestroyed) {
      texture->release();
      return;
    }
    try {
        std::lock_guard<std::mutex> lock(mRecycleMutex);
        mTextureRecycleBin[mCurrentFrameRecycleIndex].push_back(texture);
    } catch (const std::system_error& e) {
        texture->release();
    }
  }
}

int MetalRenderDevice::GetFrameCount() {
  return mCommands ? mCommands->GetFrameIndex() : 0;
}

unsigned int MetalRenderDevice::GetLightBufferBlockSize() const { return 256; }

void MetalRenderDevice::PrecacheMaterial(FMaterial *mat, int translation) {
  if (mat->Source()->GetUseType() == ETextureType::SWCanvas)
    return;

  // Ensure all layers of the material are created and uploaded
  int numLayers = mat->NumLayers();
  int scaleFlags = mat->GetScaleFlags();
  bool indexed = (scaleFlags & CTF_Indexed) != 0;

  if (indexed)
    numLayers = 3;

  for (int i = 0; i < numLayers; i++) {
    int trans = (indexed && i > 0) ? translation : ((i == 0) ? translation : 0);
    MaterialLayerInfo *layerInfo = nullptr;
    auto hwTexture = mat->GetLayer(i, trans, &layerInfo);
    if (hwTexture) {
      auto mtHwTexture = static_cast<MtHardwareTexture *>(hwTexture);
      FTexture *tex = (layerInfo && layerInfo->layerTexture)
                          ? layerInfo->layerTexture
                          : nullptr;
      if (!tex && i == 0)
        tex = mat->Source()->GetTexture();

      if (tex && mTextureManager) {
        bool wantMipmap = !(layerInfo && (layerInfo->scaleFlags & CTF_Indexed));
        mTextureManager->QueueHardwareTextureLoad(
            mtHwTexture, tex, trans,
            layerInfo ? layerInfo->scaleFlags : scaleFlags,
            wantMipmap);
      }
    }
  }
}
void MetalRenderDevice::UpdatePalette() {}
void MetalRenderDevice::SetTextureFilterMode() {}
void MetalRenderDevice::StartPrecaching() {}
void MetalRenderDevice::InitLightmap(int LMTextureSize, int LMTextureCount,
                                     TArray<uint16_t> &LMTextureData) {
  if (mTextureManager) {
    mTextureManager->SetLightmap(LMTextureSize, LMTextureCount, LMTextureData);
  }
}
void MetalRenderDevice::BlurScene(float amount) {
  if (mPostprocess)
    mPostprocess->BlurScene(amount);
}
void MetalRenderDevice::PostProcessScene(
    bool swscene, int fixedcm, float flash,
    const std::function<void()> &afterBloomDrawEndScene2D) {
  if (mPostprocess) {
    if (!swscene)
      mPostprocess->BlitSceneToPostprocess();
    mPostprocess->PostProcessScene(swscene, fixedcm, flash,
                                   afterBloomDrawEndScene2D);
  }
}
void MetalRenderDevice::AmbientOccludeScene(float m5, const HWViewpointUniforms* currentViewpoint) {
  if (mPostprocess)
    mPostprocess->AmbientOccludeScene(m5, currentViewpoint);
}
void MetalRenderDevice::SetSceneRenderTarget(bool useSSAO) {
  if (mPostprocess)
    mPostprocess->SetSceneRenderTarget(useSSAO);
}
void MetalRenderDevice::SetLevelMesh(hwrenderer::LevelMesh *mesh) {}
void MetalRenderDevice::UpdateShadowMap() {
  if (mPostprocess)
    mPostprocess->UpdateShadowMap();
}
void MetalRenderDevice::SetSaveBuffers(bool yes) {
  mActiveRenderBuffers = yes ? mSaveBuffers.get() : mScreenBuffers.get();
}
void MetalRenderDevice::ImageTransitionScene(bool unknown) {}
void MetalRenderDevice::SetActiveRenderTarget() {
  mActiveRenderBuffers = mScreenBuffers.get();
  auto tex = mActiveRenderBuffers->SceneColor->GetTexture();
  mMtRenderState->SetRenderTarget(
      tex, nullptr, // Disable depth for 2D pass
      mActiveRenderBuffers->GetWidth(), mActiveRenderBuffers->GetHeight(),
      mActiveRenderBuffers->GetSceneColorFormat(), 1);

  // Mark as filled so the renderer doesn't clear it during secondary passes
  // (like 2D)
  mMtRenderState->MarkAsFilled(tex);
}
extern int paused;
void MetalRenderDevice::Draw2D() {
  if (mPostprocess) {
    mPostprocess->SetActiveRenderTarget();
  }

  // Explicitly set viewport for 2D pass
  mMtRenderState->SetViewport(0, 0, GetWidth(), GetHeight());

  // Force disable culling and depth for 2D pass to avoid winding/occlusion
  // issues
  mMtRenderState->SetCulling(Cull_None);
  mMtRenderState->EnableDepthTest(false);
  mMtRenderState->SetDepthMask(false);

  // Reset Scissor to full screen (disable scissoring) to ensure UI is not
  // clipped by previous passes
  mMtRenderState->SetScissor(0, 0, -1, -1);

  ::Draw2D(twod, static_cast<FRenderState &>(*mMtRenderState));
}

void MetalRenderDevice::RenderTextureView(
    FCanvasTexture *tex, std::function<void(IntRect &)> renderFunc) {
  auto baseLayer =
      static_cast<MtHardwareTexture *>(tex->GetHardwareTexture(0, 0));
  auto image = baseLayer->GetImage();
  auto depthStencil = baseLayer->GetDepthStencil(tex);
  auto oldTarget = mMtRenderState->GetRenderTarget();
  mMtRenderState->EndRenderPass();
  mMtRenderState->SetRenderTarget(
      image->GetTexture(),
      depthStencil ? (MTL::Texture *)depthStencil->GetTexture() : nullptr,
      image->GetWidth(), image->GetHeight(), image->GetFormat(), 1);
  IntRect bounds;
  bounds.left = bounds.top = 0;
  bounds.width = min(tex->GetWidth(), image->GetWidth());
  bounds.height = min(tex->GetHeight(), image->GetHeight());

  mMtRenderState->SetInRenderTextureView(true);
  renderFunc(bounds);
  mMtRenderState->SetInRenderTextureView(false);

  mMtRenderState->EndRenderPass();
  mMtRenderState->SetRenderTarget(oldTarget.Image, oldTarget.DepthStencil,
                                  oldTarget.Width, oldTarget.Height,
                                  oldTarget.Format, oldTarget.Samples);
  tex->SetUpdated(true);
}
void MetalRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data) {
  auto srcTex = mPostprocess->GetCurrentTexture();
  if (!srcTex)
    return;
  // The pipeline images are BGRA8Unorm normally and RGBA16Float when
  // mt_hdr_pipeline is on, so neither the stride nor the unpack can be
  // hardcoded.
  const bool halfFloat = srcTex->pixelFormat() == MTL::PixelFormatRGBA16Float;
  const size_t bytesPerPixel = halfFloat ? 8 : 4;
  const size_t bytesPerRow = (size_t)w * bytesPerPixel;
  const size_t dataSize = bytesPerRow * (size_t)h;
  MTL::Buffer *stagingBuffer =
      device->device->newBuffer(dataSize, MTL::ResourceStorageModeShared);
  auto cmdBuffer = mCommands->GetRenderCommandBuffer();
  auto blitEncoder = cmdBuffer->blitCommandEncoder();
  MTL::Size sourceSize = MTL::Size(w, h, 1);
  blitEncoder->copyFromTexture(srcTex, 0, 0, MTL::Origin(0, 0, 0), sourceSize,
                               stagingBuffer, 0, bytesPerRow, dataSize);
  blitEncoder->endEncoding();
  mCommands->WaitForCommands(true);
  uint8_t *dest = data;
  if (halfFloat) {
    // RGBA16Float is linear-ish scene colour above 1.0; the screenshot is 8-bit
    // so clamp rather than tonemap -- the present pass has already run.
    const __fp16 *pixels = (const __fp16 *)stagingBuffer->contents();
    for (int i = 0; i < w * h; i++) {
      for (int c = 0; c < 3; c++) {
        float v = (float)pixels[i * 4 + c];
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        dest[i * 3 + c] = (uint8_t)(v * 255.0f + 0.5f);
      }
    }
  } else {
    // BGRA8Unorm stores B in byte 0 and R in byte 2, but the caller tags the
    // result SS_RGB. Read R from byte 2, not byte 0.
    const uint8_t *pixels = (const uint8_t *)stagingBuffer->contents();
    for (int i = 0; i < w * h; i++) {
      dest[i * 3 + 0] = pixels[i * 4 + 2];
      dest[i * 3 + 1] = pixels[i * 4 + 1];
      dest[i * 3 + 2] = pixels[i * 4 + 0];
    }
  }
  stagingBuffer->release();
}
FTexture *MetalRenderDevice::WipeStartScreen() {
  auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
  auto systex = static_cast<MtHardwareTexture *>(tex->GetSystemTexture());
  systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");
  return tex;
}

FTexture *MetalRenderDevice::WipeEndScreen() {
  // Ensure the current frame (including 2D) is fully rendered to PipelineImage[0]
  mPostprocess->SetActiveRenderTarget();
  Draw2D();
  twod->Clear();

  auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
  auto systex = static_cast<MtHardwareTexture *>(tex->GetSystemTexture());
  systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");
  return tex;
}
TArray<uint8_t> MetalRenderDevice::GetScreenshotBuffer(int &pitch,
                                                       ESSType &color_type,
                                                       float &gamma) {
  int w = SCREENWIDTH;
  int h = SCREENHEIGHT;
  IntRect box = {0, 0, w, h};
  mPostprocess->DrawPresentTexture(box, true, true);
  TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
  CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());
  pitch = w * 3;
  color_type = SS_RGB;
  gamma = 1.0f;
  return ScreenshotBuffer;
}
IHardwareTexture *MetalRenderDevice::CreateHardwareTexture(int numchannels) {
  return new MtHardwareTexture(this, numchannels);
}
FMaterial *MetalRenderDevice::CreateMaterial(FGameTexture *tex,
                                             int scaleflags) {
  return new FMaterial(tex, scaleflags);
}
IVertexBuffer *MetalRenderDevice::CreateVertexBuffer() {
  return mBufferManager->CreateVertexBuffer();
}
IIndexBuffer *MetalRenderDevice::CreateIndexBuffer() {
  return mBufferManager->CreateIndexBuffer();
}
IDataBuffer *MetalRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo,
                                                 bool needsresize) {
  return mBufferManager->CreateDataBuffer(bindingpoint, ssbo, needsresize);
}
