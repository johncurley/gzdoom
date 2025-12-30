#include "mt_renderbuffers.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/textures/mt_texture.h"
#include "printf.h" // New include
#include "c_cvars.h"

EXTERN_CVAR(Int, gl_shadowmap_quality)

#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

MtRenderBuffers::MtRenderBuffers(MetalRenderDevice *fb) : fb(fb) {}

MtRenderBuffers::~MtRenderBuffers() {}

void MtRenderBuffers::BeginFrame(int width, int height, int sceneWidth,
                                 int sceneHeight) {
  if (mWidth != width || mHeight != height) {
    mWidth = width;
    mHeight = height;
    CreatePipeline(width, height);
    CreatePipelineDepthStencil(width, height);
  }

  if (mSceneWidth != sceneWidth || mSceneHeight != sceneHeight) {
    mSceneWidth = sceneWidth;
    mSceneHeight = sceneHeight;
    CreateScene(sceneWidth, sceneHeight, 1); // samples=1 for now
  }
}

// FORCE RECOMPILE: December 25 V8 Final Audit Build
void MtRenderBuffers::CreatePipelineDepthStencil(int width, int height) {
  PipelineDepthStencil = std::make_unique<MtTextureImage>(fb);

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height);
  desc->setPixelFormat(MTL::PixelFormatDepth32Float_Stencil8); 
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModePrivate);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  if (!texture) {
      Printf(PRINT_LOG, "Metal: Failed to create PipelineDepthStencil texture!\n");
  }
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
    desc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModePrivate);

    MTL::Texture *texture = fb->device->device->newTexture(desc);
    if (i == 0) Printf(PRINT_LOG, "Metal: Created PipelineImage[0] at %p\n", texture);
    PipelineImage[i]->SetTexture(texture);
    PipelineImage[i]->SetWidth(width);
    PipelineImage[i]->SetHeight(height);
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

  // Default to 1024 for now, engine will resize via BeginFrame if needed (though ShadowMap quality is usually fixed)
  int quality = gl_shadowmap_quality; 
  if (quality <= 0) quality = 1024;

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(quality);
  desc->setHeight(1024);
  // GZDoom 1D shadow maps store squared distance as data, must use R32Float.
  desc->setPixelFormat(MTL::PixelFormatR32Float); 
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModePrivate);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  if (texture) {
      Printf(PRINT_LOG, "Metal: Created ShadowMap texture %p (%dx1024) R32Float\n", texture, quality);
  } else {
      Printf(PRINT_LOG, "Metal: FAILED to create ShadowMap texture!\n");
  }
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
  desc->setPixelFormat(MTL::PixelFormatRGBA16Float);
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModePrivate);
  desc->setSampleCount(samples);
  if (samples > 1)
    desc->setTextureType(MTL::TextureType2DMultisample);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  SceneColor->SetTexture(texture);
  SceneColor->SetWidth(width);
  SceneColor->SetHeight(height);
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
  SceneDepthStencil->SetTexture(texture);
  SceneDepthStencil->SetWidth(width);
  SceneDepthStencil->SetHeight(height);
  desc->release();
}

void MtRenderBuffers::CreateSceneNormal(int width, int height, int samples) {
  SceneNormal = std::make_unique<MtTextureImage>(fb);

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height);
  desc->setPixelFormat(
      MTL::PixelFormatBGRA8Unorm); // Match Vulkan A2R10G10B10 if possible, but
                                   // BGRA8 is safe for now
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModePrivate);
  desc->setSampleCount(samples);
  if (samples > 1)
    desc->setTextureType(MTL::TextureType2DMultisample);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  SceneNormal->SetTexture(texture);
  SceneNormal->SetWidth(width);
  SceneNormal->SetHeight(height);
  desc->release();
}

void MtRenderBuffers::CreateSceneFog(int width, int height, int samples) {
  SceneFog = std::make_unique<MtTextureImage>(fb);

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height);
  desc->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModePrivate);
  desc->setSampleCount(samples);
  if (samples > 1)
    desc->setTextureType(MTL::TextureType2DMultisample);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  SceneFog->SetTexture(texture);
  SceneFog->SetWidth(width);
  SceneFog->SetHeight(height);
  desc->release();
}
