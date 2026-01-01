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

#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_renderstate.h"
#include "hw_renderstate.h"
#include "metal/renderer/mt_resourcebinding.h"
#include "metal/shaders/mt_shader.h"
#include "metal/textures/mt_sampler.h"
#include "metal/textures/mt_texture.h"
#include "mt_buffer.h"
#include "mt_commandbuffer.h"
#include "mt_hwbuffer.h"
#include "mt_renderdevice.h"

#include "mt_binaryarchive.h"

#include "m_png.h"
#include "r_videoscale.h"
#include "v_video.h"
#include "c_dispatch.h"
#include "engineerrors.h"
#include "flatvertices.h"
#include "hw_bonebuffer.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_lightbuffer.h"
#include "hw_skydome.h"
#include "gamestate.h"
#include "hw_vrmodes.h"
#include "hw_viewpointuniforms.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "v_draw.h"
#include "v_text.h"
#include "version.h"

#ifdef __APPLE__
#include <zwidget/window/cocoanativehandle.h>
#endif

// Max number of frames to queue for rendering
constexpr int MaxFramesInFlight = 2;

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)

CVAR(Bool, mt_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

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

    MetalViewSize viewSize = GetMetalViewDrawableSize(m_window);
    if (viewSize.width > 0 && viewSize.height > 0) {
      CGSize drawableSize = CGSizeMake(viewSize.width, viewSize.height);
      metalLayer->setDrawableSize(drawableSize);
    } else {
      metalLayer->setDrawableSize(CGSizeMake(GetWidth(), GetHeight()));
    }
    metalLayer->setDisplaySyncEnabled(false);
    metalLayer->setMaximumDrawableCount((NS::UInteger)mVersionManager.maxDrawableCount);
    metalLayer->setAllowsNextDrawableTimeout(true);

    if (mVersionManager.presentsWithTransaction) {
        // Synchronize with Cocoa transactions for smoother UI integration
        // Note: presentsWithTransaction is not explicitly in metal-cpp CAMetalLayer class
        // but we can use the selector if we really needed it. 
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
  
  if (mt_debug) {
      Printf(PRINT_LOG, "Metal: Tuning for OS %d.%d: maxDrawableCount=%d, presentsWithTransaction=%d\n", 
             mVersionManager.osMajor, mVersionManager.osMinor, mVersionManager.maxDrawableCount, (int)mVersionManager.presentsWithTransaction);
  }

  vendorstring = deviceName;
  hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
  glslversion = 4.50f;
  uniformblockalignment = 256;
  maxuniformblock = 65536;

  Printf(PRINT_LOG, TEXTCOLOR_BLUE "Initializing Metal renderer managers...\n");
  
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

  Printf(PRINT_LOG, TEXTCOLOR_GREEN "Metal renderer initialized successfully!\n");
}

void MetalRenderDevice::Update() {
  NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();

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
      IntRect physicalBox = { 0, 0, width, height };
      mPostprocess->DrawPresentTexture(physicalBox, true, false);
      
      // Reset viewport/scissor after present blit
      mMtRenderState->SetViewport(0, 0, width, height);
      mMtRenderState->SetScissor(0, 0, width, height);
    }

    if (mMtRenderState)
      mMtRenderState->EndRenderPass();

    // 4. Frame pacing and presentation
    if (!mVSync) {
        this->FPSLimit();
    }

    PresentFrame(mCurrentDrawable);
    
    // During startup, flush immediately to keep CPU/GPU in sync for the progress bar
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

  mInFrame = false;

  // CRITICAL: If we are in a wipe, the engine calls Update() in a loop without calling BeginFrame().
  // We must ensure the next frame is initialized here.
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
  if (mInFrame) return;
  mInFrame = true;

  if (mCurrentDrawable)
    return;

  SetViewportRects(nullptr);
  mViewpoints->Clear();
  mLights->Clear();
  mBones->Clear();

  // Wait for GPU backpressure (MaxFramesInFlight frames allowed)
  dispatch_semaphore_wait(mInflightFramesSemaphore, DISPATCH_TIME_FOREVER);

  {
    std::lock_guard<std::mutex> lock(mRecycleMutex);
    mCurrentFrameRecycleIndex = (mCurrentFrameRecycleIndex + 1) % 3;
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

    MetalViewSize viewSize = GetMetalViewDrawableSize(m_window);
    if (viewSize.width > 0 && viewSize.height > 0) {
      CGSize drawableSize = CGSizeMake(viewSize.width, viewSize.height);
      if (metalLayer->drawableSize().width != drawableSize.width ||
          metalLayer->drawableSize().height != drawableSize.height) {
        metalLayer->setDrawableSize(drawableSize);
      }
    }

    mCurrentDrawable = metalLayer->nextDrawable();
    if (mCurrentDrawable) {
      mCurrentDrawable->retain();
      if (mt_debug) {
        auto tex = mCurrentDrawable->texture();
        Printf(PRINT_LOG, "Metal: Acquired drawable %p (%lu x %lu). ClientSize: %d x %d\n", mCurrentDrawable, 
               tex->width(), tex->height(), GetWidth(), GetHeight());
      }
    }
  }

  mScreenBuffers->BeginFrame(GetWidth(), GetHeight(), GetWidth(), GetHeight());
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

  // Set default render target to PipelineImage[0] for 2D/UI drawn before Update()
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
}

void MetalRenderDevice::PrintStartupLog() {
  const char *deviceName = device->device->name()->utf8String();
  Printf(PRINT_LOG, TEXTCOLOR_CYAN "Metal Renderer for GZDoom\n");
  Printf(PRINT_LOG, "  Device: %s\n", deviceName);
  Printf(PRINT_LOG, "  Architecture: %s\n", mVersionManager.GetArchName());
  Printf(PRINT_LOG, "  OS Version: macOS %d.%d.%d\n", mVersionManager.osMajor, mVersionManager.osMinor, mVersionManager.osPatch);
  Printf(PRINT_LOG, "  API Version: Metal %d.%d\n", mVersionManager.metalVersion / 10, mVersionManager.metalVersion % 10);
  Printf(PRINT_LOG, "  Backend: Native Metal (metal-cpp)\n");
  Printf(PRINT_LOG, "\n");
}

const char *MetalRenderDevice::DeviceName() const {
  if (device && device->device)
    return device->device->name()->utf8String();
  return "Metal Device";
}

FRenderState *MetalRenderDevice::RenderState() { return static_cast<FRenderState*>(mMtRenderState.get()); }

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
    std::lock_guard<std::mutex> lock(mRecycleMutex);
    mBufferRecycleBin[mCurrentFrameRecycleIndex].push_back(buffer);
  }
}

void MetalRenderDevice::RecycleTexture(MTL::Texture *texture) {
  if (texture) {
    if (mIsDestroyed) {
        texture->release();
        return;
    }
    std::lock_guard<std::mutex> lock(mRecycleMutex);
    mTextureRecycleBin[mCurrentFrameRecycleIndex].push_back(texture);
  }
}

int MetalRenderDevice::GetFrameCount() {
    return mCommands ? mCommands->GetFrameIndex() : 0;
}

unsigned int MetalRenderDevice::GetLightBufferBlockSize() const {
  return 256;
}

void MetalRenderDevice::PrecacheMaterial(FMaterial *mat, int translation) {}
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
  if (mt_debug) Printf(PRINT_LOG, "Metal: PostProcessScene\n");
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
  if (mt_debug) Printf(PRINT_LOG, "Metal: SetSceneRenderTarget\n");
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
  if (mt_debug) Printf(PRINT_LOG, "Metal: SetActiveRenderTarget (SceneColor)\n");
  mActiveRenderBuffers = mScreenBuffers.get();
  auto tex = mActiveRenderBuffers->SceneColor->GetTexture();
  mMtRenderState->SetRenderTarget(
      tex,
      mActiveRenderBuffers->SceneDepthStencil->GetTexture(),
      mActiveRenderBuffers->GetWidth(), mActiveRenderBuffers->GetHeight(),
      (int)MTL::PixelFormatRGBA16Float, 1);
}
void MetalRenderDevice::Draw2D() {
  if (mPostprocess) {
    mPostprocess->SetActiveRenderTarget();
  }
  
  if (mt_debug) {
      Printf(PRINT_LOG, "Metal: Draw2D with %d commands\n", twod->mData.Size());
  }

  // Force disable culling and depth for 2D pass to avoid winding/occlusion issues
  mMtRenderState->SetCulling(Cull_None);
  mMtRenderState->EnableDepthTest(false);
  mMtRenderState->SetDepthMask(false);

  // No local pool here - it causes encoders created inside ::Draw2D to be 
  // destroyed before endEncoding is called when this local pool is released.
  ::Draw2D(twod, static_cast<FRenderState&>(*mMtRenderState));
}
void MetalRenderDevice::RenderTextureView(
    FCanvasTexture *tex, std::function<void(IntRect &)> renderFunc) {
  auto baseLayer = static_cast<MtHardwareTexture *>(tex->GetHardwareTexture(0, 0));
  auto image = baseLayer->GetImage();
  auto depthStencil = baseLayer->GetDepthStencil(tex);
  auto oldTarget = mMtRenderState->GetRenderTarget();
  mMtRenderState->EndRenderPass();
  mMtRenderState->SetRenderTarget(
      image->GetTexture(), 
      depthStencil ? (MTL::Texture*)depthStencil->GetTexture() : nullptr,
      image->GetWidth(), image->GetHeight(), 
      image->GetFormat(), 1);
  IntRect bounds;
  bounds.left = bounds.top = 0;
  bounds.width = min(tex->GetWidth(), image->GetWidth());
  bounds.height = min(tex->GetHeight(), image->GetHeight());
  if (mt_debug) Printf(PRINT_LOG, "Metal: RenderTextureView bounds %d,%d %dx%d\n", bounds.left, bounds.top, bounds.width, bounds.height);
  
  mMtRenderState->SetInRenderTextureView(true);
  renderFunc(bounds);
  mMtRenderState->SetInRenderTextureView(false);

  mMtRenderState->EndRenderPass();
  mMtRenderState->SetRenderTarget(
      oldTarget.Image, oldTarget.DepthStencil, 
      oldTarget.Width, oldTarget.Height, 
      oldTarget.Format, oldTarget.Samples);
  tex->SetUpdated(true);
}
void MetalRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data) {
  auto srcTex = mPostprocess->GetCurrentTexture();
  if (!srcTex)
    return;
  size_t dataSize = w * h * 4;
  MTL::Buffer *stagingBuffer = device->device->newBuffer(dataSize, MTL::ResourceStorageModeShared);
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
  if (mt_debug) Printf(PRINT_LOG, "Metal: WipeStartScreen capture starting.\n");
  SetViewportRects(nullptr);
  auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
  auto systex = static_cast<MtHardwareTexture *>(tex->GetSystemTexture());
  systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");
  return tex;
}
FTexture *MetalRenderDevice::WipeEndScreen() {
  if (mt_debug) Printf(PRINT_LOG, "Metal: WipeEndScreen capture starting.\n");
  GetPostprocess()->SetActiveRenderTarget();
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
  IntRect box = { 0, 0, w, h };
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