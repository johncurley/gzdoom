#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "hw_renderstate.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "i_system.h"
#include "matrix.h"
#include "metal/system/mt_renderdevice.h"
#include "mt_streambuffer.h"
#include <memory>

MtStreamBuffer::MtStreamBuffer(MetalRenderDevice *fb, size_t structSize)
    : fb(fb) {
  // Metal requires 256-byte alignment for buffer offsets
  mBlockSize = (static_cast<uint32_t>(structSize) + 255) & ~255;

  // Allocate 8MB ring buffer (following Vulkan pattern)
  mBufferSize = 8 * 1024 * 1024;

  // Create MTLBuffer with StorageModeShared for CPU-GPU shared memory
  // This allows direct CPU writes without explicit synchronization
  mBuffer = fb->device->device->newBuffer(mBufferSize, MTL::StorageModeShared);

  if (!mBuffer) {
    I_FatalError(
        "MtStreamBuffer: Failed to allocate Metal buffer of size %zu bytes",
        mBufferSize);
  }
}

MtStreamBuffer::~MtStreamBuffer() {
  // Release Metal buffer
  if (mBuffer) {
    mBuffer->release();
    mBuffer = nullptr;
  }
}

uint32_t MtStreamBuffer::NextStreamDataBlock() {
  if (mStreamDataOffset == 0xffffffff) {
    mStreamDataOffset = 0;
    return 0;
  }
  uint32_t nextOffset = mStreamDataOffset + mBlockSize;
  if (nextOffset + mBlockSize > mBufferSize) {
    mStreamDataOffset = 0xffffffff; // Reset state
    return 0xffffffff; // Buffer full
  }
  mStreamDataOffset = nextOffset;
  return mStreamDataOffset;
}

uint8_t *MtStreamBuffer::GetBufferPointer() const {
  if (!mBuffer)
    return nullptr;

  // Metal buffers with StorageModeShared are CPU-accessible
  // contents() returns a pointer to the buffer's CPU-side memory
  return (uint8_t *)mBuffer->contents();
}

// ============================================================================
// Stream Buffer Writers
// ============================================================================

MtStreamBufferWriter::MtStreamBufferWriter(MetalRenderDevice *fb)
    : mBuffer(std::make_unique<MtStreamBuffer>(fb, sizeof(StreamData) *
                                                       MAX_STREAM_DATA)) {}

bool MtStreamBufferWriter::Write(const StreamData &data) {
  mDataIndex++;
  if (mDataIndex == MAX_STREAM_DATA || mStreamDataOffset == 0xffffffff) {
    mDataIndex = 0;
    mStreamDataOffset = mBuffer->NextStreamDataBlock();
    if (mStreamDataOffset == 0xffffffff)
      return false;
  }

  // Write to Metal buffer's CPU-accessible memory
  uint8_t *ptr = mBuffer->GetBufferPointer();
  if (ptr) {
    memcpy(ptr + mStreamDataOffset + sizeof(StreamData) * mDataIndex, &data,
           sizeof(StreamData));
  }

  return true;
}

void MtStreamBufferWriter::Reset() {
  mDataIndex = MAX_STREAM_DATA - 1;
  mStreamDataOffset = 0xffffffff; // Force NextStreamDataBlock() on first write
  mBuffer->Reset();
}

uint32_t MtStreamBufferWriter::DataIndex() const { return mDataIndex; }

uint32_t MtStreamBufferWriter::StreamDataOffset() const {
  return mStreamDataOffset;
}

/////////////////////////////////////////////////////////////////////////////

MtMatrixBufferWriter::MtMatrixBufferWriter(MetalRenderDevice *fb)
    : mBuffer(std::make_unique<MtStreamBuffer>(fb, sizeof(MatricesUBO))) {
  mIdentityMatrix.loadIdentity();
}

template <typename T>
static void BufferedSet(bool &modified, T &dst, const T &src) {
  if (dst == src)
    return;
  dst = src;
  modified = true;
}

static void BufferedSet(bool &modified, VSMatrix &dst, const VSMatrix &src) {
  if (memcmp(dst.get(), src.get(), sizeof(FLOATTYPE) * 16) == 0)
    return;
  dst = src;
  modified = true;
}

bool MtMatrixBufferWriter::Write(const VSMatrix &modelMatrix,
                                 bool modelMatrixEnabled,
                                 const VSMatrix &textureMatrix,
                                 bool textureMatrixEnabled) {
  bool modified = (mOffset == 0xffffffff); // always modified first call after reset

  if (modelMatrixEnabled) {
    BufferedSet(modified, mMatrices.ModelMatrix, modelMatrix);
    if (modified)
      mMatrices.NormalModelMatrix.computeNormalMatrix(modelMatrix);
  } else {
    BufferedSet(modified, mMatrices.ModelMatrix, mIdentityMatrix);
    BufferedSet(modified, mMatrices.NormalModelMatrix, mIdentityMatrix);
  }

  if (textureMatrixEnabled) {
    BufferedSet(modified, mMatrices.TextureMatrix, textureMatrix);
  } else {
    BufferedSet(modified, mMatrices.TextureMatrix, mIdentityMatrix);
  }

  if (modified) {
    mOffset = mBuffer->NextStreamDataBlock();
    if (mOffset == 0xffffffff)
      return false;

    // Write to Metal buffer's CPU-accessible memory
    uint8_t *ptr = mBuffer->GetBufferPointer();
    if (ptr) {
      memcpy(ptr + mOffset, &mMatrices, sizeof(MatricesUBO));
    }
  }

  return true;
}

void MtMatrixBufferWriter::Reset() {
  mOffset = 0xffffffff;
  mBuffer->Reset();
}

uint32_t MtMatrixBufferWriter::Offset() const { return mOffset; }
