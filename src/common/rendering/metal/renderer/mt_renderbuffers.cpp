/*
**  Metal backend - Render Buffers
*/

#include "i_time.h"

#include "../mt_system_wrapper.h"

#include "mt_renderbuffers.h"
#include "mt_resources.h"
#include <string.h>
#include "c_cvars.h"
#include "../system/mt_renderdevice.h"
#include "../textures/mt_texture.h"
#include "printf.h"

EXTERN_CVAR(Int, gl_shadowmap_quality)

// Half-float scene colour / postprocess pipeline, matching the OpenGL and
// Vulkan backends (both use RGBA16F). Metal has historically used BGRA8Unorm,
// which clamps scene colour to [0,1]; the reference shaders deliberately carry
// up to 1.4 out of ProcessMaterialLight, and the bloom extract threshold of 1.0
// assumes that headroom exists. Off by default until the visual A/B lands.
CVAR(Bool, mt_hdr_pipeline, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

MtRenderBuffers::MtRenderBuffers(MetalRenderDevice *fb, const char *tag)
    : fb(fb), mTag(tag ? tag : "screen") {
  static const char *kBase[RES_Count] = {"SceneColor",       "SceneDepthStencil",
                                         "SceneNormal",      "SceneFog",
                                         "PipelineImage[0]", "PipelineImage[1]"};
  for (int i = 0; i < RES_Count; ++i)
    mResNames[i].Format("%s.%s", mTag, kBase[i]);
}

MtRenderBuffers::~MtRenderBuffers() {}

MtSizeRule MtRenderBuffers::SceneRule() const {
  return strcmp(mTag, "screen") == 0 ? MtSizeRule::Full()
                                     : MtSizeRule::Absolute();
}

int MtRenderBuffers::DesiredColorFormat() const {
  return (int)(mt_hdr_pipeline ? MTL::PixelFormatRGBA16Float
                               : MTL::PixelFormatBGRA8Unorm);
}

void MtRenderBuffers::BeginFrame(int width, int height, int sceneWidth,
                                 int sceneHeight) {
  // A mid-session mt_hdr_pipeline toggle changes the format of every colour
  // buffer, so it forces the same recreation path a resize does. Metal keeps
  // the outgoing textures alive for any command buffer still referencing them.
  const int colorFormat = DesiredColorFormat();
  const bool formatChanged = (colorFormat != mColorFormat);
  mColorFormat = colorFormat;

  if (mWidth != width || mHeight != height || formatChanged) {
    mWidth = width;
    mHeight = height;
    CreatePipeline(width, height);
    CreatePipelineDepthStencil(width, height);
  }

  if (mSceneWidth != sceneWidth || mSceneHeight != sceneHeight ||
      formatChanged) {
    mSceneWidth = sceneWidth;
    mSceneHeight = sceneHeight;
    CreateScene(sceneWidth, sceneHeight, 1); // samples=1 for now
  }

  // Touch only. The frame boundary is MetalRenderDevice::BeginFrame -- this
  // function runs again for every render-to-texture, so advancing the frame here
  // made a mid-frame dump measure itself against whatever small target rendered
  // last.
  for (int i = 0; i < RES_Count; ++i)
    MtResources().Touch(ResName(i));
}

// Name a render target so it is identifiable in a GPU frame capture. Without
// this every texture shows in Xcode as an anonymous MTLTexture-<n>, and reading
// a specific buffer's contents out of a trace means guessing among forty
// numbered blobs. Added 2026-08-06 alongside the PP encoder labels, for the
// same reason: the AO composite investigation needs a readable capture. Costs
// one string per target at allocation, which is never in a hot path.
static void LabelTexture(MTL::Texture *texture, const char *name) {
  if (!texture || !name)
    return;
  texture->setLabel(
      NS::String::string(name, NS::StringEncoding::UTF8StringEncoding));
}

void MtRenderBuffers::CreatePipelineDepthStencil(int width, int height) {
  PipelineDepthStencil = std::make_unique<MtTextureImage>(fb);

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height);
  desc->setPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);

  if (fb->mVersionManager.supportsMemoryless) {
    desc->setUsage(MTL::TextureUsageRenderTarget);
    desc->setStorageMode(MTL::StorageModeMemoryless);
  } else {
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModePrivate);
  }

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  if (!texture) {
    Printf(PRINT_LOG,
           "Metal: Failed to create PipelineDepthStencil texture!\n");
  }
  LabelTexture(texture, "PipelineDepthStencil");
  PipelineDepthStencil->SetTexture(texture);
  PipelineDepthStencil->SetWidth(width);
  PipelineDepthStencil->SetHeight(height);
  desc->release();
}

void MtRenderBuffers::CreatePipeline(int width, int height) {
  for (int i = 0; i < NumPipelineImages; ++i) {
    PipelineImage[i] = std::make_unique<MtTextureImage>(fb);

    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(width);
    desc->setHeight(height);
    desc->setPixelFormat((MTL::PixelFormat)mColorFormat);
    auto usage = MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
    if (fb->mVersionManager.supportsReadWriteBGRA8)
      usage = usage | MTL::TextureUsageShaderWrite;
    desc->setUsage(usage);
    desc->setStorageMode(MTL::StorageModePrivate);

    MTL::Texture *texture = fb->device->device->newTexture(desc);
    {
      FString n;
      n.Format("PipelineImage[%d]", i);
      LabelTexture(texture, n.GetChars());
    }
    PipelineImage[i]->SetTexture(texture);
    PipelineImage[i]->SetWidth(width);
    PipelineImage[i]->SetHeight(height);
    // Pipeline images track the WINDOW, not the scene viewport, so they are
    // Fixed rather than SceneFull -- at screenblocks < 11 the two differ and a
    // SceneFull rule would report a false STALE on every frame.
    {
      MtResources().Declare({ResName(i == 0 ? RES_Pipeline0 : RES_Pipeline1),
                             "MtRenderBuffers", width, height, 1,
                             mColorFormat, MtSizeRule::Absolute(), false},
                            texture);
    }
    desc->release();
  }
}

void MtRenderBuffers::CreateScene(int width, int height, int samples) {
  CreateSceneColor(width, height, samples);
  CreateSceneDepthStencil(width, height, samples);
  CreateSceneNormal(width, height, samples);
  CreateSceneFog(width, height, samples);
  CreateShadowMap();
}

void MtRenderBuffers::CreateShadowMap() {
  ShadowMap = std::make_unique<MtTextureImage>(fb);

  int quality = gl_shadowmap_quality;
  if (quality <= 0)
    quality = 1024;

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(quality);
  desc->setHeight(1024);
  desc->setPixelFormat(MTL::PixelFormatR32Float);
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModePrivate);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  if (!texture) {
    Printf(PRINT_LOG, "Metal: FAILED to create ShadowMap texture!\n");
  }
  LabelTexture(texture, "ShadowMap");
  ShadowMap->SetTexture(texture);
  ShadowMap->SetWidth(quality);
  ShadowMap->SetHeight(1024);
  desc->release();
}

void MtRenderBuffers::CreateSceneColor(int width, int height, int samples) {
  SceneColor = std::make_unique<MtTextureImage>(fb);

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height);
  desc->setPixelFormat((MTL::PixelFormat)mColorFormat);
  // Allow compute shaders (e.g. bloom combine) to write into the scene color texture only on devices that support read-write BGRA8
  if (fb->mVersionManager.supportsReadWriteBGRA8) {
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
  } else {
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  }
  desc->setStorageMode(MTL::StorageModePrivate);
  desc->setSampleCount(samples);
  if (samples > 1)
    desc->setTextureType(MTL::TextureType2DMultisample);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  LabelTexture(texture, "SceneColor");
  SceneColor->SetTexture(texture);
  SceneColor->SetWidth(width);
  SceneColor->SetHeight(height);
  MtResources().Declare({ResName(RES_SceneColor), "MtRenderBuffers", width, height, samples,
                         mColorFormat, SceneRule(), false},
                        texture);
  desc->release();
}

void MtRenderBuffers::CreateSceneDepthStencil(int width, int height,
                                              int samples) {
  SceneDepthStencil = std::make_unique<MtTextureImage>(fb);

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height);
  desc->setPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);

  desc->setStorageMode(MTL::StorageModePrivate);

  desc->setSampleCount(samples);
  if (samples > 1)
    desc->setTextureType(MTL::TextureType2DMultisample);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  LabelTexture(texture, "SceneDepthStencil");
  SceneDepthStencil->SetTexture(texture);
  SceneDepthStencil->SetWidth(width);
  SceneDepthStencil->SetHeight(height);
  MtResources().Declare({ResName(RES_SceneDepth), "MtRenderBuffers", width, height,
                         samples, (int)MTL::PixelFormatDepth32Float_Stencil8,
                         SceneRule(), false},
                        texture);
  desc->release();
}

void MtRenderBuffers::CreateSceneNormal(int width, int height, int samples) {
  SceneNormal = std::make_unique<MtTextureImage>(fb);

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height);

  MTL::PixelFormat format = MTL::PixelFormatRGBA8Unorm;
  if (fb->mVersionManager.supportsRGB10A2) {
    format = MTL::PixelFormatRGB10A2Unorm;
  }
  mNormalFormat = (int)format;

  desc->setPixelFormat(format);
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModePrivate);
  desc->setSampleCount(samples);
  if (samples > 1)
    desc->setTextureType(MTL::TextureType2DMultisample);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  LabelTexture(texture, "SceneNormal");
  SceneNormal->SetTexture(texture);
  SceneNormal->SetWidth(width);
  SceneNormal->SetHeight(height);
  MtResources().Declare({ResName(RES_SceneNormal), "MtRenderBuffers", width, height,
                         samples, mNormalFormat, SceneRule(), false},
                        texture);
  desc->release();
}

void MtRenderBuffers::CreateSceneFog(int width, int height, int samples) {
  SceneFog = std::make_unique<MtTextureImage>(fb);

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height);
  // 8-bit in the reference too -- does not follow mt_hdr_pipeline.
  mFogFormat = (int)MTL::PixelFormatBGRA8Unorm;
  desc->setPixelFormat((MTL::PixelFormat)mFogFormat);
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModePrivate);
  desc->setSampleCount(samples);
  if (samples > 1)
    desc->setTextureType(MTL::TextureType2DMultisample);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  LabelTexture(texture, "SceneFog");
  SceneFog->SetTexture(texture);
  SceneFog->SetWidth(width);
  SceneFog->SetHeight(height);
  MtResources().Declare({ResName(RES_SceneFog), "MtRenderBuffers", width, height, samples,
                         mFogFormat, SceneRule(), false},
                        texture);
  desc->release();
}
