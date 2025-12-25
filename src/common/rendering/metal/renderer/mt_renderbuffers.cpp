#include "mt_renderbuffers.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/textures/mt_texture.h"
#include "printf.h" // New include

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

// FORCE RECOMPILE: December 25 V3 Cache Force
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
      Printf("Metal: Failed to create PipelineDepthStencil texture!\n");
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
    if (i == 0) Printf("Metal: Created PipelineImage[0] at %p\n", texture);
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
