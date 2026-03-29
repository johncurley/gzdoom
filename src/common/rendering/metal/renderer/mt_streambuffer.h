#pragma once

#include "hw_renderstate.h"
#include "matrix.h"
#include "metal/shaders/mt_shader.h"
#include <cstdint>
#include <memory>

#define TimeScale TimeScale_GZDOOM
namespace MTL {
class Buffer;
} // namespace MTL
#undef TimeScale

class MetalRenderDevice;

// Ring buffer for dynamic data (following Vulkan pattern)
class MtStreamBuffer {
public:
  MtStreamBuffer(MetalRenderDevice *fb, size_t structSize, size_t capacity = 0);
  ~MtStreamBuffer();

  // Allocate from ring buffer
  uint32_t NextStreamDataBlock();

  // Get buffer and offset
  MTL::Buffer *GetBuffer() const { return mBuffers[mBufferIndex]; }
  uint32_t GetOffset() const { return mStreamDataOffset; }

  // Get CPU-accessible buffer pointer for writing
  uint8_t *GetBufferPointer() const;

  // Reset for new frame and cycle to next buffer
  void Reset();
  uint32_t GetStreamDataOffset() const { return mStreamDataOffset; }

private:
  MTL::Buffer *mBuffers[3] = { nullptr, nullptr, nullptr };
  int mBufferIndex = 0;
  uint32_t mBlockSize = 0;
  uint32_t mStreamDataOffset = 0xffffffff;
  size_t mBufferSize = 0;
  MetalRenderDevice *fb = nullptr;
};

// Writer classes for common buffer types
class MtStreamBufferWriter {
public:
  MtStreamBufferWriter(MetalRenderDevice *fb);

  bool Write(const StreamData &data);
  void Reset();
  void BeginFrame() { 
    mBuffer->Reset(); 
    mDataIndex = MAX_STREAM_DATA - 1;
    mStreamDataOffset = mBuffer->GetStreamDataOffset();
  }

  uint32_t DataIndex() const;
  uint32_t StreamDataOffset() const;

  // Get underlying buffer for binding
  MTL::Buffer *GetBuffer() const {
    return mBuffer ? mBuffer->GetBuffer() : nullptr;
  }

private:
  std::unique_ptr<MtStreamBuffer> mBuffer;
  StreamData mLastData = {};
  uint32_t mDataIndex = MAX_STREAM_DATA - 1;
  uint32_t mStreamDataOffset = 0;
};

class MtMatrixBufferWriter {
public:
  MtMatrixBufferWriter(MetalRenderDevice *fb);

  bool Write(const VSMatrix &modelMatrix, bool modelMatrixEnabled,
             const VSMatrix &textureMatrix, bool textureMatrixEnabled);
  void Reset();
  void BeginFrame() { 
    mBuffer->Reset(); 
    mOffset = mBuffer->GetStreamDataOffset();
  }

  uint32_t Offset() const;

  // Get underlying buffer for binding
  MTL::Buffer *GetBuffer() const {
    return mBuffer ? mBuffer->GetBuffer() : nullptr;
  }

private:
  std::unique_ptr<MtStreamBuffer> mBuffer;
  MatricesUBO mMatrices = {};
  VSMatrix mIdentityMatrix;
  uint32_t mOffset = 0;
};
