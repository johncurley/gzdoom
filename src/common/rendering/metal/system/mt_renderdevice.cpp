/*
**  Metal backend
**  Copyright (c) 2025 GZDoom Contributors
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
*/

// Include i_time.h BEFORE Metal headers to avoid TimeScale conflict
#include "i_time.h"
#include "metal/metal_common.h" // New include

// Prevent MacTypes.h from defining TimeScale by defining it as a macro
#define TimeScale TimeScale_GZDOOM

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

// Restore TimeScale
#undef TimeScale

#include "hw_renderstate.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/renderer/mt_resourcebinding.h"
#include "metal/renderer/mt_debug.h"
#include "metal/shaders/mt_shader.h"
#include "metal/textures/mt_sampler.h"
#include "metal/textures/mt_texture.h"
#include "mt_buffer.h"
#include "mt_commandbuffer.h"
#include "mt_hwbuffer.h"
#include "mt_renderdevice.h"

#include "mt_binaryarchive.h"

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

// Max number of frames to queue for rendering
constexpr int MaxFramesInFlight = 2;

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
  mInflightFramesSemaphore = dispatch_semaphore_create(MaxFramesInFlight);
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

  // Safely reset all post-processing backend resources while we are still alive
  PPResource::ResetAll();

  if (mCommands) {
    mCommands->WaitForCommands(true);
  }

  delete mVertexData;
  delete mSkyData;
  delete mViewpoints;
  delete mLights;
  delete mBones;
  mShadowMap.Reset();

  mMtRenderState.reset();
  mPipelineStateManager.reset();
  mResourceBindingManager.reset();
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
      CGSize drawableSize = CGSizeMake(viewSize.width, viewSize.height);
      metalLayer->setDrawableSize(drawableSize);

      // Force internal resolution to match window size immediately to prevent startup squashing
      SetVirtualSize((int)viewSize.width, (int)viewSize.height);
      V_OutputResized((int)viewSize.width, (int)viewSize.height);
      if (mVertexData) mVertexData->OutputResized((int)viewSize.width, (int)viewSize.height);
      SetViewportRects(nullptr);
    } else {
      // Fall back to logical window size which should match configured
      // resolution
      metalLayer->setDrawableSize(CGSizeMake(GetWidth(), GetHeight()));
    }

    // Enable VSync to prevent screen tearing (flashing horizontal lines)
    metalLayer->setDisplaySyncEnabled(true);
    metalLayer->setMaximumDrawableCount(
        (NS::UInteger)mVersionManager.maxDrawableCount);
    metalLayer->setAllowsNextDrawableTimeout(true);

    // Set framebufferOnly to NO to disable aggressive Intel scanout
    // optimizations that can cause "squares" or tiling artifacts in fullscreen.
    metalLayer->setFramebufferOnly(false);

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
  mResourceBindingManager.reset(new MtResourceBindingManager(this));
  mPipelineStateManager.reset(new MtPipelineStateManager(this));
  mShaderManager.reset(new MtShaderManager(this));
  mMtRenderState.reset(new MtRenderState(this));

  FMaterial::SetLayerCallback([](int layer, int translation) -> IHardwareTexture* {
    auto fb = static_cast<MetalRenderDevice*>(screen);
    return fb->GetTextureManager()->GetPaletteTexture(translation, layer == 2);
  });

  mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), mPipelineNbr);
  mSkyData = new FSkyVertexBuffer;
  mViewpoints = new HWViewpointBuffer;
  mLights = new FLightBuffer();
  mBones = new BoneBuffer();

  mFrameCount = 0; // Reset for Startup Lag Guard effectiveness

  Printf(PRINT_LOG,
         TEXTCOLOR_GREEN "Metal renderer initialized successfully!\n");
}

void MetalRenderDevice::Update() {
  NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();

  twoD.Reset();
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

    // 4. Frame pacing and presentation
    if (!mVSync) {
      this->FPSLimit();
    }

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

  // CRITICAL: If we are in a wipe, the engine calls Update() in a loop without
  // calling BeginFrame(). We must ensure the next frame is initialized here.
  this->BeginFrame();
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

void MetalRenderDevice::BeginFrame() {
  if (mInFrame)
    return;
  mInFrame = true;

  if (mCurrentDrawable)
    return;

  // Process any completed async texture loads
  if (mTextureManager) {
    mTextureManager->ProcessAsyncTextureLoads();
  }

  SetViewportRects(nullptr);
  mViewpoints->Clear();
  mLights->Clear();
  mBones->Clear();

  // Wait for GPU backpressure (MaxFramesInFlight frames allowed)
  if (dispatch_semaphore_wait(
          mInflightFramesSemaphore,
          dispatch_time(DISPATCH_TIME_NOW, 1000 * NSEC_PER_MSEC)) != 0) {
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
      // Sync GZDoom's internal resolution with the window size
      int targetWidth = (int)viewSize.width;
      int targetHeight = (int)viewSize.height;
      int scaledWidth = ViewportScaledWidth(targetWidth, targetHeight);
      int scaledHeight = ViewportScaledHeight(targetWidth, targetHeight);

      // STARTUP LAG GUARD:
      // If the view reports a smaller size than our current configured
      // resolution during startup, it is likely a Cocoa layout lag. We MUST NOT
      // downscale the engine, or we get a low-res stretched frame. Instead, we
      // force the Main Layer to match our higher internal resolution.
      if (mFrameCount < 10 &&
          (scaledWidth < GetWidth() || scaledHeight < GetHeight())) {
        metalLayer->setDrawableSize(CGSizeMake(GetWidth(), GetHeight()));
      }
      // Normal operation: Sync if different (handles Resizing and startup
      // Upscaling)
      else if (GetWidth() != scaledWidth || GetHeight() != scaledHeight) {
        SetVirtualSize(scaledWidth, scaledHeight);
        V_OutputResized(scaledWidth, scaledHeight);
        if (mVertexData) mVertexData->OutputResized(scaledWidth, scaledHeight);

        // Ensure drawable size matches the NEW resolution
        metalLayer->setDrawableSize(CGSizeMake(scaledWidth, scaledHeight));
      }
    } else {
      metalLayer->setDrawableSize(CGSizeMake(GetWidth(), GetHeight()));
    }

    // Ensure we have a valid drawable for this frame
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

  // Retina Fix: Initialize screen buffers with the PHYSICAL drawable size.
  // This ensures the G-buffer matches the physical resolution of the window.
  auto drawableTexture = mCurrentDrawable->texture();
  int physicalWidth = (int)drawableTexture->width();
  int physicalHeight = (int)drawableTexture->height();

  mScreenBuffers->BeginFrame(physicalWidth, physicalHeight, physicalWidth, physicalHeight);
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
#endif
}

void MetalRenderDevice::SetMode(bool fullscreen, bool hiDPI) {
  Super::SetMode(fullscreen, hiDPI);
  if (mVertexData) mVertexData->OutputResized(GetWidth(), GetHeight());

  mFrameCount = 0; // Re-trigger Lag Guard for the new mode

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

      if (tex) {
        mtHwTexture->CreateImage(
            tex, trans, layerInfo ? layerInfo->scaleFlags : scaleFlags);
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
void MetalRenderDevice::AmbientOccludeScene(float m5) {
  if (mPostprocess)
    mPostprocess->AmbientOccludeScene(m5);
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
      (int)MTL::PixelFormatBGRA8Unorm, 1);

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

  // Set up 2D projection matrix
  {
    HWViewpointUniforms matrices;
    matrices.mViewMatrix.loadIdentity();
    matrices.mNormalViewMatrix.loadIdentity();
    matrices.mViewHeight = 0;
    matrices.mGlobVis = 1.f;
    matrices.mPalLightLevels = 0;
    matrices.mClipLine.X = -10000000.0f;
    matrices.mShadowmapFilter = 0;
    matrices.mLightBlendMode = 0;
    // Use Y-up ortho matrix (0 at bottom) to counteract vertex shader flip.
    matrices.mProjectionMatrix.ortho(0, (float)GetWidth(), 0,
                                     (float)GetHeight(), -1.0f, 1.0f);
    matrices.CalcDependencies();
    mViewpoints->SetViewpoint(*mMtRenderState, &matrices);
  }

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
  size_t dataSize = w * h * 4;
  MTL::Buffer *stagingBuffer =
      device->device->newBuffer(dataSize, MTL::ResourceStorageModeShared);
  auto cmdBuffer = mCommands->GetRenderCommandBuffer();
  auto blitEncoder = cmdBuffer->blitCommandEncoder();
  MTL::Size sourceSize = MTL::Size(w, h, 1);
  blitEncoder->copyFromTexture(srcTex, 0, 0, MTL::Origin(0, 0, 0), sourceSize,
                               stagingBuffer, 0, w * 4, w * h * 4);
  blitEncoder->endEncoding();
  mCommands->WaitForCommands(true);
  uint8_t *pixels = (uint8_t *)stagingBuffer->contents();
  uint8_t *dest = data;
  for (int i = 0; i < w * h; i++) {
    dest[i * 3 + 0] = pixels[i * 4 + 0];
    dest[i * 3 + 1] = pixels[i * 4 + 1];
    dest[i * 3 + 2] = pixels[i * 4 + 2];
  }
  stagingBuffer->release();
}
FTexture *MetalRenderDevice::WipeStartScreen() {
  SetViewportRects(nullptr);
  auto tex =
      new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
  auto systex = static_cast<MtHardwareTexture *>(tex->GetSystemTexture());
  systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height,
                            "WipeStartScreen");
  return tex;
}
FTexture *MetalRenderDevice::WipeEndScreen() {
  GetPostprocess()->SetActiveRenderTarget();
  Draw2D();
  twod->Clear();
  auto tex =
      new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
  auto systex = static_cast<MtHardwareTexture *>(tex->GetSystemTexture());
  systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height,
                            "WipeEndScreen");
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