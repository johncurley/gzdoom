#pragma once

#include "hwrenderer/postprocessing/hw_compute.h"

#include <Metal/Metal.hpp>

class MetalRenderDevice;

class MtComputeManager {
public:
  explicit MtComputeManager(MetalRenderDevice *fb);

  MTL::Texture *CreateTexture(int width, int height, MTL::PixelFormat format,
                              MTL::TextureUsage usage,
                              MTL::StorageMode storageMode);
  MTL::Texture *EnsureTexture(MTL::Texture *&texture, int &cachedWidth,
                              int &cachedHeight, int width, int height,
                              MTL::PixelFormat format,
                              MTL::TextureUsage usage,
                              MTL::StorageMode storageMode);
  void ReleaseTexture(MTL::Texture *&texture, int &cachedWidth,
                      int &cachedHeight);
  void RecordTiming(HWComputeEffect effect, float durationMs);

private:
  MetalRenderDevice *fb = nullptr;
};
