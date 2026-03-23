/*
**  Metal backend - MTLHeap implementation
**  Copyright (c) 2020-2025 Magnus Norddahl
*/

#include "mt_heap.h"
#include "mt_version.h"
#include <algorithm>

MtHeapManager::MtHeapManager(MTL::Device *device, const MtVersionManager *versionMgr)
    : mDevice(device), mVersionMgr(versionMgr), mSupported(false) {
  if (versionMgr && versionMgr->supportsHeaps && device) {
    mSupported = true;
  }
}

MtHeapManager::~MtHeapManager() {
  for (auto &info : mTextureHeaps) {
    if (info.heap) info.heap->release();
  }
  for (auto &info : mBufferHeaps) {
    if (info.heap) info.heap->release();
  }
}

MTL::Heap *MtHeapManager::CreateTextureHeap(size_t sizeInBytes) {
  if (!mSupported) return nullptr;
  MTL::HeapDescriptor *desc = MTL::HeapDescriptor::alloc()->init();
  desc->setStorageMode(MTL::StorageModePrivate);
  desc->setSize(sizeInBytes);
  desc->setType(MTL::HeapTypeAutomatic);
  MTL::Heap *heap = mDevice->newHeap(desc);
  desc->release();
  if (heap) {
    mTextureHeaps.push_back({heap, sizeInBytes, 0});
    mStats.totalSize += sizeInBytes;
  }
  return heap;
}

MTL::Heap *MtHeapManager::CreateBufferHeap(size_t sizeInBytes) {
  if (!mSupported) return nullptr;
  MTL::HeapDescriptor *desc = MTL::HeapDescriptor::alloc()->init();
  desc->setStorageMode(MTL::StorageModePrivate);
  desc->setSize(sizeInBytes);
  desc->setType(MTL::HeapTypeAutomatic);
  MTL::Heap *heap = mDevice->newHeap(desc);
  desc->release();
  if (heap) {
    mBufferHeaps.push_back({heap, sizeInBytes, 0});
    mStats.totalSize += sizeInBytes;
  }
  return heap;
}

MTL::Texture *MtHeapManager::AllocateTextureFromHeap(MTL::TextureDescriptor *descriptor, MTL::Heap *heap) {
  if (!mSupported || !heap) {
    // Fallback: standard allocation
    return mDevice->newTexture(descriptor);
  }
  MTL::Texture *tex = heap->newTexture(descriptor);
  if (tex) {
    size_t sz = EstimateTextureSize(descriptor);
    mStats.usedSize += sz;
    mStats.peakUsedSize = std::max(mStats.peakUsedSize, mStats.usedSize);
    mStats.textureCount++;
  }
  return tex;
}

MTL::Buffer *MtHeapManager::AllocateBufferFromHeap(size_t length, MTL::ResourceOptions options, MTL::Heap *heap) {
  if (!mSupported || !heap) {
    // Fallback: standard allocation
    return mDevice->newBuffer(length, options);
  }
  MTL::Buffer *buf = heap->newBuffer(length, options);
  if (buf) {
    mStats.usedSize += length;
    mStats.peakUsedSize = std::max(mStats.peakUsedSize, mStats.usedSize);
    mStats.bufferCount++;
  }
  return buf;
}

void MtHeapManager::FreeHeap(MTL::Heap *heap) {
  if (!heap) return;
  heap->release();
  // Remove from tracking vectors
  auto pred = [heap](const HeapInfo &info) { return info.heap == heap; };
  mTextureHeaps.erase(std::remove_if(mTextureHeaps.begin(), mTextureHeaps.end(), pred), mTextureHeaps.end());
  mBufferHeaps.erase(std::remove_if(mBufferHeaps.begin(), mBufferHeaps.end(), pred), mBufferHeaps.end());
}

void MtHeapManager::ResetFrameStats() {
  mStats.usedSize = 0;
  mStats.textureCount = 0;
  mStats.bufferCount = 0;
}

size_t MtHeapManager::EstimateTextureSize(MTL::TextureDescriptor *desc) const {
  // Simple estimation: width * height * pixel bytes * mip levels
  size_t bpp = 4; // Assume 4 bytes per pixel (RGBA8)
  switch (desc->pixelFormat()) {
    case MTL::PixelFormatRGBA8Unorm:
    case MTL::PixelFormatBGRA8Unorm:
      bpp = 4; break;
    case MTL::PixelFormatR8Unorm:
      bpp = 1; break;
    case MTL::PixelFormatR16Float:
      bpp = 2; break;
    case MTL::PixelFormatRGBA16Float:
      bpp = 8; break;
    default:
      bpp = 4; break;
  }
  size_t mipLevels = desc->mipmapLevelCount();
  if (mipLevels < 1) mipLevels = 1;
  return desc->width() * desc->height() * bpp * mipLevels;
}
