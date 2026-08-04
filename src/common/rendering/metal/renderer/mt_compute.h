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

  // New infrastructure helpers
  MTL::ComputeCommandEncoder* BeginComputePass(MTL::CommandBuffer* cmdBuf);
  void Dispatch(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pso, 
                int width, int height, int threadGroupWidth = 16, int threadGroupHeight = 16);
  void EndComputePass(MTL::ComputeCommandEncoder* encoder);

private:
  MetalRenderDevice *fb = nullptr;
};

// Dispatch `gridThreads` threads, choosing the dispatch call this GPU actually
// supports.
//
// dispatchThreads() -- the one that takes a thread count and splits it into
// partial threadgroups for you -- requires non-uniform threadgroup support,
// which is MTLGPUFamilyCommon3/Mac2. The Intel HD 6000 reference machine
// reports Common3 = no, Mac2 = no, supportsNonUniformThreadgroups = false, so
// every dispatchThreads() call on it was outside the API contract. What a
// non-conforming driver does with it is undefined: rounding the grid up is
// harmless here (every kernel bounds-checks against its output extent), but
// truncating to whole threadgroups leaves the right and bottom edges of the
// output texture unwritten. On a 45x25 bloom level with 16x16 groups that is
// 13 of 45 columns and 9 of 25 rows of stale content, which then gets blurred
// and upscale-replaced down the pyramid.
//
// So: use dispatchThreads only where it is actually supported, and otherwise
// round up to whole threadgroups and let the kernels' existing bounds checks
// discard the overhang. Both branches dispatch at least the requested threads
// and never fewer.
void MtDispatchThreads(MTL::ComputeCommandEncoder *encoder,
                       const MetalRenderDevice *fb,
                       MTL::Size gridThreads, MTL::Size threadsPerGroup);
