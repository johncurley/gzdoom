#include "mt_compute.h"

#include "../system/mt_renderdevice.h"
#include "mt_debug.h"

#include <algorithm>

MtComputeManager::MtComputeManager(MetalRenderDevice *renderDevice)
    : fb(renderDevice) {}

MTL::Texture *MtComputeManager::CreateTexture(int width, int height,
                                              MTL::PixelFormat format,
                                              MTL::TextureUsage usage,
                                              MTL::StorageMode storageMode) {
  if (!fb || !fb->device || !fb->device->device)
    return nullptr;

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth((NS::UInteger)std::max(width, 1));
  desc->setHeight((NS::UInteger)std::max(height, 1));
  desc->setPixelFormat(format);
  desc->setUsage(usage);
  desc->setStorageMode(storageMode);
  auto texture = fb->device->device->newTexture(desc);
  desc->release();
  return texture;
}

MTL::Texture *MtComputeManager::EnsureTexture(MTL::Texture *&texture,
                                              int &cachedWidth,
                                              int &cachedHeight, int width,
                                              int height,
                                              MTL::PixelFormat format,
                                              MTL::TextureUsage usage,
                                              MTL::StorageMode storageMode) {
  width = std::max(width, 1);
  height = std::max(height, 1);
  if (texture && cachedWidth == width && cachedHeight == height &&
      texture->pixelFormat() == format) {
    return texture;
  }

  if (texture) {
    texture->release();
    texture = nullptr;
  }

  texture = CreateTexture(width, height, format, usage, storageMode);
  cachedWidth = texture ? width : 0;
  cachedHeight = texture ? height : 0;
  return texture;
}

void MtComputeManager::ReleaseTexture(MTL::Texture *&texture, int &cachedWidth,
                                      int &cachedHeight) {
  if (texture) {
    texture->release();
    texture = nullptr;
  }
  cachedWidth = 0;
  cachedHeight = 0;
}

void MtComputeManager::RecordTiming(HWComputeEffect effect, float durationMs) {
  if (!fb || !fb->GetDebugManager())
    return;

  switch (effect) {
  case HWComputeEffect::AmbientOcclusion:
    fb->GetDebugManager()->RecordMetric(MtMetric::ComputeAO, durationMs);
    break;
  case HWComputeEffect::Bloom:
    fb->GetDebugManager()->RecordMetric(MtMetric::ComputeBloom, durationMs);
    break;
  }
}

MTL::ComputeCommandEncoder *
MtComputeManager::BeginComputePass(MTL::CommandBuffer *cmdBuf) {
  if (!cmdBuf)
    return nullptr;
  return cmdBuf->computeCommandEncoder();
}

void MtComputeManager::Dispatch(MTL::ComputeCommandEncoder *encoder,
                                MTL::ComputePipelineState *pso, int width,
                                int height, int threadGroupWidth,
                                int threadGroupHeight) {
  if (!encoder || !pso)
    return;

  encoder->setComputePipelineState(pso);
  MTL::Size gridSize = {(NS::UInteger)width, (NS::UInteger)height, 1};
  MTL::Size threadGroupSize = {(NS::UInteger)threadGroupWidth,
                               (NS::UInteger)threadGroupHeight, 1};
  encoder->dispatchThreads(gridSize, threadGroupSize);
}

void MtComputeManager::EndComputePass(MTL::ComputeCommandEncoder *encoder) {
  if (encoder) {
    encoder->endEncoding();
  }
}
