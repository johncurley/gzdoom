/*
**  Metal backend
**  Copyright (c) 2025 GZDoom Contributors
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
*/

// Include i_time.h BEFORE Metal headers to avoid TimeScale conflict
// i_time.h declares: extern double TimeScale
// MacTypes.h (included by Metal) declares: typedef SInt32 TimeScale
#include "i_time.h"

// Prevent MacTypes.h from defining TimeScale by defining it as a macro
// temporarily
#define TimeScale TimeScale_GZDOOM

// Define private implementation for metal-cpp (only in this file!)
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
#include "metal/renderer/mt_resourcebinding.h"
#include "metal/shaders/mt_shader.h"
#include "metal/textures/mt_sampler.h"
#include "metal/textures/mt_texture.h"
#include "mt_buffer.h"
#include "mt_commandbuffer.h"
#include "mt_hwbuffer.h"
#include "mt_renderdevice.h"

#include "m_png.h"
#include "r_videoscale.h"
#include "v_video.h"
// i_time.h included earlier to avoid TimeScale conflict
#include "c_dispatch.h"
#include "engineerrors.h"
#include "flatvertices.h"
#include "hw_bonebuffer.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_lightbuffer.h"
#include "hw_skydome.h"
#include "hw_vrmodes.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "v_draw.h"
#include "v_text.h"
#include "version.h"

// For accessing CocoaNativeHandle
#ifdef __APPLE__
#include <zwidget/window/cocoanativehandle.h>
#endif

// Max number of frames to queue for rendering
constexpr int MaxFramesInFlight = 3;

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)

CVAR(Bool, mt_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

void MetalError(const char *text) { throw CMetalError(text); }

void MetalPrintLog(const char *typestr, const std::string &msg) {
  Printf(TEXTCOLOR_RED "[Metal %s] ", typestr);
  Printf(TEXTCOLOR_WHITE "%s\n", msg.c_str());
}

MetalRenderDevice::MetalRenderDevice(void *hMonitor, bool fullscreen)
    : Super(hMonitor, fullscreen) {
  // Create the semaphore to control frame pacing
  mInflightFramesSemaphore = dispatch_semaphore_create(MaxFramesInFlight);

  // Create Metal device
  device = std::make_shared<MetalDevice>();
  device->device = MTL::CreateSystemDefaultDevice();

  if (!device->device) {
    MetalError("Failed to create Metal device - Metal may not be supported on "
               "this system");
  }

  // Create command queue
  device->commandQueue = device->device->newCommandQueue();
  if (!device->commandQueue) {
    MetalError("Failed to create Metal command queue");
  }

  // Note: Metal layer setup is deferred to InitializeState() after window is
  // set
}

MetalRenderDevice::~MetalRenderDevice() {
  // Wait for GPU to finish all work
  if (mCommands) {
    mCommands->WaitForCommands(true);
  }

  // Clean up resources
  delete mVertexData;
  delete mSkyData;
  delete mViewpoints;
  delete mLights;
  delete mBones;
  mShadowMap.Reset();

  // Release managers (in reverse order)
  mRenderState.reset();
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

  // Release Metal device objects
  if (device) {
    if (device->commandQueue) {
      device->commandQueue->release();
      device->commandQueue = nullptr;
    }
    if (device->device) {
      device->device->release();
      device->device = nullptr;
    }
  }

  // Release the semaphore
  dispatch_release(mInflightFramesSemaphore);
}

void MetalRenderDevice::InitializeState() {
  static bool first = true;
  if (first) {
    PrintStartupLog();
    first = false;
  }

  // Set up Metal layer now that window is available
#ifdef __APPLE__
  CocoaNativeHandle nativeHandle = GetNativeHandle();
  if (nativeHandle.metalLayer) {
    CA::MetalLayer *metalLayer = (CA::MetalLayer *)nativeHandle.metalLayer;
    metalLayer->setDevice(device->device);
    metalLayer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);

    // VSync will be set later via SetVSync()
    metalLayer->setDisplaySyncEnabled(false);

    Printf("Metal layer configured successfully\n");
  } else {
    MetalError("Failed to get Metal layer from window");
  }
#endif

  // Set vendor string from device
  const char *deviceName = device->device->name()->utf8String();
  vendorstring = deviceName;

  // Set capabilities (Metal supports shader storage buffers via argument
  // buffers)
  hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
  glslversion = 4.50f; // Report GLSL 4.5 for shader compatibility

  // Metal has specific alignment requirements
  uniformblockalignment = 256; // Metal constant buffer alignment
  maxuniformblock = 65536;     // Metal max constant buffer size

  // Initialize managers in dependency order (following Vulkan pattern)
  // Order matters! Some managers depend on others being initialized first

  Printf(TEXTCOLOR_BLUE "Initializing Metal renderer...\n");

  // 1. Command buffer manager (needed by everything)
  mCommands.reset(new MtCommandBufferManager(this));
  Printf("  - Command buffer manager initialized\n");

  // 2. Sampler manager
  mSamplerManager.reset(new MtSamplerManager(this));
  Printf("  - Sampler manager initialized\n");

  // 3. Texture manager
  mTextureManager.reset(new MtTextureManager(this));
  Printf("  - Texture manager initialized\n");

  // 4. Buffer manager (with initialization)
  mBufferManager.reset(new MtBufferManager(this));
  mBufferManager->Init();
  Printf("  - Buffer manager initialized\n");

  // 5. Render buffers (screen and save)
  mScreenBuffers.reset(new MtRenderBuffers(this));
  mSaveBuffers.reset(new MtRenderBuffers(this));
  mActiveRenderBuffers = mScreenBuffers.get();
  Printf("  - Render buffers initialized\n");

  // 6. Post-processing
  mPostprocess.reset(new MtPostprocess(this));
  Printf("  - Post-process initialized\n");

  // 7. Resource binding manager (three-tier strategy)
  mResourceBindingManager.reset(new MtResourceBindingManager(this));
  Printf("  - Resource binding manager initialized\n");

  // 8. Pipeline state manager (needs shader manager)
  mPipelineStateManager.reset(new MtPipelineStateManager(this));
  Printf("  - Pipeline state manager initialized\n");

  // 9. Shader manager (GLSL→SPIR-V→MSL pipeline)
  mShaderManager.reset(new MtShaderManager(this));
  Printf("  - Shader manager initialized\n");

  // 10. Render state (core state machine)
  mRenderState.reset(new MtRenderState(this));
  Printf("  - Render state initialized\n");

  // Initialize vertex data, lights, etc. (same as Vulkan)
  mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
  mSkyData = new FSkyVertexBuffer;
  mViewpoints = new HWViewpointBuffer;
  mLights = new FLightBuffer();
  mBones = new BoneBuffer();

  Printf(TEXTCOLOR_GREEN "Metal renderer initialized successfully!\n");
  Printf("  Device: %s\n", deviceName);
}

void MetalRenderDevice::Update() {
  BeginFrame();

  twoD.Reset();
  Flush3D.Reset();

  // Get the Metal layer and next drawable
  CocoaNativeHandle nativeHandle = GetNativeHandle();
  if (!nativeHandle.metalLayer) {
    Super::Update();
    return;
  }

  CA::MetalLayer *metalLayer = (CA::MetalLayer *)nativeHandle.metalLayer;
  CA::MetalDrawable *drawable = metalLayer->nextDrawable();

  if (!drawable) {
    if (mt_debug)
      Printf("Warning: Failed to get next Metal drawable\n");
    Super::Update();
    return;
  }

  // Get the drawable's texture and set it as our render target
  MTL::Texture *drawableTexture = drawable->texture();
  if (drawableTexture && mRenderState && mScreenBuffers) {
    int width = drawableTexture->width();
    int height = drawableTexture->height();

    // Ensure render buffers are sized correctly
    mScreenBuffers->BeginFrame(width, height, width, height);

    // Get depth/stencil buffer
    MtTextureImage *depthStencil = mScreenBuffers->SceneDepthStencil.get();
    MTL::Texture *depthStencilTexture =
        depthStencil ? (MTL::Texture *)depthStencil->GetTexture() : nullptr;

    // Set render target to drawable texture with depth/stencil
    mRenderState->SetRenderTarget(
        drawableTexture,     // Color attachment (raw MTL::Texture*)
        depthStencilTexture, // Depth/stencil attachment
        width, height, (int)MTL::PixelFormatBGRA8Unorm,
        1 // No MSAA yet
    );
  }

  Flush3D.Clock();
  Draw2D();
  Flush3D.Unclock();

  // End the render pass (finishes encoding)
  if (mRenderState)
    mRenderState->EndRenderPass();

  // Present the frame to screen
  PresentFrame(drawable);

  Super::Update();
}

void MetalRenderDevice::PresentFrame(void *drawablePtr) {
  if (!drawablePtr)
    return;

  auto drawable = (CA::MetalDrawable *)drawablePtr;

  // Get current command buffer
  void *cmdBufPtr = mCommands->GetRenderCommandBuffer();
  if (!cmdBufPtr)
    return;

  auto commandBuffer = (MTL::CommandBuffer *)cmdBufPtr;

  // Signal the semaphore when the command buffer finishes execution
  mCommands->AddCompletedHandler(commandBuffer, [this]() {
    dispatch_semaphore_signal(mInflightFramesSemaphore);
  });

  // Present drawable when command buffer completes
  commandBuffer->presentDrawable(drawable);

  // Submit the command buffer
  commandBuffer->commit();
}

void MetalRenderDevice::BeginFrame() {
  // Wait for a free slot in the queue
  dispatch_semaphore_wait(mInflightFramesSemaphore, DISPATCH_TIME_FOREVER);

  if (mCommands)
    mCommands->BeginFrame();

  if (mRenderState)
    mRenderState->BeginFrame();

  if (mResourceBindingManager)
    mResourceBindingManager->BeginFrame();
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

void MetalRenderDevice::PrintStartupLog() {
  const char *deviceName = device->device->name()->utf8String();

  Printf(TEXTCOLOR_CYAN "Metal Renderer for GZDoom\n");
  Printf("  Device: %s\n", deviceName);
  Printf("  API Version: Metal 2.0+\n");
  Printf("  Backend: Native Metal (metal-cpp)\n");
  Printf("\n");
}

const char *MetalRenderDevice::DeviceName() const {
  if (device && device->device)
    return device->device->name()->utf8String();
  return "Metal Device";
}

FRenderState *MetalRenderDevice::RenderState() { return mRenderState.get(); }

void MetalRenderDevice::WaitForCommands(bool finish) {
  if (mCommands)
    mCommands->WaitForCommands(finish);
}

unsigned int MetalRenderDevice::GetLightBufferBlockSize() const {
  // Metal uses 256-byte alignment for constant buffers
  return 256;
}

// Stubs for methods that will be implemented later
void MetalRenderDevice::PrecacheMaterial(FMaterial *mat, int translation) {}
void MetalRenderDevice::UpdatePalette() {}
void MetalRenderDevice::SetTextureFilterMode() {}
void MetalRenderDevice::StartPrecaching() {}
void MetalRenderDevice::InitLightmap(int LMTextureSize, int LMTextureCount,
                                     TArray<uint16_t> &LMTextureData) {}
void MetalRenderDevice::BlurScene(float amount) {
  if (mPostprocess)
    mPostprocess->BlurScene(amount);
}
void MetalRenderDevice::PostProcessScene(
    bool swscene, int fixedcm, float flash,
    const std::function<void()> &afterBloomDrawEndScene2D) {
  if (mPostprocess)
    mPostprocess->PostProcessScene(swscene, fixedcm, flash,
                                   afterBloomDrawEndScene2D);
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
void MetalRenderDevice::SetActiveRenderTarget() {}
void MetalRenderDevice::Draw2D() {
  static int debugCount = 0;
  if (debugCount++ < 5) {
    Printf("Draw2D: twod has %d vertices, %d indices, %d commands\n",
           twod->mVertices.Size(), twod->mIndices.Size(), twod->mData.Size());
  }
  ::Draw2D(twod, *mRenderState);
}
void MetalRenderDevice::RenderTextureView(
    FCanvasTexture *tex, std::function<void(IntRect &)> renderFunc) {}
void MetalRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data) {
  auto srcTex = mPostprocess->GetCurrentTexture();
  if (!srcTex)
    return;

  // Create a staging buffer
  size_t dataSize = w * h * 4; // Assuming RGBA8
  MTL::Buffer *stagingBuffer =
      device->device->newBuffer(dataSize, MTL::ResourceStorageModeShared);

  auto cmdBuffer = mCommands->GetRenderCommandBuffer();
  auto blitEncoder = cmdBuffer->blitCommandEncoder();

  MTL::Size sourceSize = MTL::Size(w, h, 1);
  blitEncoder->copyFromTexture(srcTex, 0, 0, MTL::Origin(0, 0, 0), sourceSize,
                               stagingBuffer, 0, w * 4, w * h * 4);
  blitEncoder->endEncoding();

  // Submit and wait
  mCommands->WaitForCommands(true);

  // Copy from staging buffer to data (and convert RGBA8 to RGB8 if needed, but
  // GZDoom wants RGB8 for screenshots)
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

  IntRect box;
  box.left = 0;
  box.top = 0;
  box.width = w;
  box.height = h;
  mPostprocess->DrawPresentTexture(box, true, true);

  TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
  CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());

  pitch = w * 3;
  color_type = SS_RGB;
  gamma = 1.0f;
  return ScreenshotBuffer;
}

// Hardware object creation
IHardwareTexture *MetalRenderDevice::CreateHardwareTexture(int numchannels) {
  return new MtHardwareTexture(this, numchannels);
}

FMaterial *MetalRenderDevice::CreateMaterial(FGameTexture *tex,
                                             int scaleflags) {
  return new FMaterial(tex, scaleflags);
}

IVertexBuffer *MetalRenderDevice::CreateVertexBuffer() {
  return new MtVertexBuffer(this);
}

IIndexBuffer *MetalRenderDevice::CreateIndexBuffer() {
  return new MtIndexBuffer(this);
}

IDataBuffer *MetalRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo,
                                                 bool needsresize) {
  return new MtHardwareDataBuffer(this, bindingpoint, ssbo, needsresize);
}
