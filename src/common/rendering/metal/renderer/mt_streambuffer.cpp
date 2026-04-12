#include "i_time.h"
#include <Metal/Metal.hpp>

#include "hw_renderstate.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "i_system.h"
#include "matrix.h"
#include "metal/system/mt_renderdevice.h"
#include "mt_streambuffer.h"
#include "c_cvars.h"
#include "printf.h"
#include <memory>

EXTERN_CVAR(Bool, mt_debug)

MtStreamBuffer::MtStreamBuffer(MetalRenderDevice *fb, size_t structSize, size_t capacity)
    : fb(fb) {
  // Metal requires 256-byte alignment for buffer offsets
  mBlockSize = (static_cast<uint32_t>(structSize) + 255) & ~255;

  // Use provided capacity or fallback to 4MB if not specified
  if (capacity > 0)
      mBufferSize = mBlockSize * capacity;
  else
      mBufferSize = 4 * 1024 * 1024;

  for (int i = 0; i < 3; i++) {
    mBuffers[i] = fb->device->device->newBuffer(mBufferSize, fb->mVersionManager.GetDynamicStorageMode());
    if (!mBuffers[i]) {
      I_FatalError(
          "MtStreamBuffer: Failed to allocate Metal buffer of size %zu bytes",
          mBufferSize);
    }
    // Debug: log requested vs actual buffer storage mode (always logged)
    Printf(PRINT_LOG, "MtStreamBuffer: requested storage %d, buffer storage %d\n",
           (int)fb->mVersionManager.GetDynamicStorageMode(), (int)mBuffers[i]->storageMode());
  }
}

MtStreamBuffer::~MtStreamBuffer() {
  // Release Metal buffers
  for (int i = 0; i < 3; i++) {
    if (mBuffers[i]) {
      mBuffers[i]->release();
      mBuffers[i] = nullptr;
    }
  }
}

void MtStreamBuffer::Reset() {
  mStreamDataOffset = 0xffffffff;
  mBufferIndex = (mBufferIndex + 1) % 3;
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
  auto buf = mBuffers[mBufferIndex];
  if (!buf)
    return nullptr;

  // Metal buffers with StorageModeShared are CPU-accessible
  return (uint8_t *)buf->contents();
}

// ============================================================================
// Stream Buffer Writers
// ============================================================================

MtStreamBufferWriter::MtStreamBufferWriter(MetalRenderDevice *fb)
    : mBuffer(std::make_unique<MtStreamBuffer>(fb, sizeof(StreamData) *
                                                       MAX_STREAM_DATA, 0)) {}

bool MtStreamBufferWriter::Write(const StreamData &data) {
  // Check if current data matches the last written data in the SAME block
  if (mDataIndex < MAX_STREAM_DATA && mStreamDataOffset != 0xffffffff) {
    if (memcmp(&mLastData, &data, sizeof(StreamData)) == 0) {
      return true;
    }
  }

  // If we are at the end of a block or first write, advance to next block
  if (mDataIndex >= MAX_STREAM_DATA - 1 || mStreamDataOffset == 0xffffffff) {
    mDataIndex = 0;
    mStreamDataOffset = mBuffer->NextStreamDataBlock();
    if (mStreamDataOffset == 0xffffffff)
      return false;
  } else {
    mDataIndex++;
  }

  // Write to Metal buffer's CPU-accessible memory
  uint8_t *ptr = mBuffer->GetBufferPointer();
  if (ptr) {
    size_t offset = mStreamDataOffset + sizeof(StreamData) * mDataIndex;
    memcpy(ptr + offset, &data, sizeof(StreamData));
    static bool __mt_streambuffer_logged = false;
    if (!__mt_streambuffer_logged) {
      Printf(PRINT_LOG, "MtStreamBuffer::Write: buffer storage %d, offset %u\n", (int)mBuffer->GetBuffer()->storageMode(), (unsigned)offset);
      __mt_streambuffer_logged = true;
    }
    if (mBuffer->GetBuffer()->storageMode() == MTL::StorageModeManaged) {
      if (mt_debug) Printf(PRINT_LOG, "MtStreamBuffer::Write: calling didModifyRange for offset %u\n", (unsigned)offset);
      mBuffer->GetBuffer()->didModifyRange(NS::Range(offset, sizeof(StreamData)));
    }
    mLastData = data;
  }

  return true;
}

void MtStreamBufferWriter::Reset() {
  mDataIndex = MAX_STREAM_DATA - 1;
  mBuffer->Reset();
  mStreamDataOffset = mBuffer->GetStreamDataOffset();
}

uint32_t MtStreamBufferWriter::DataIndex() const { return mDataIndex; }

uint32_t MtStreamBufferWriter::StreamDataOffset() const {
  return mStreamDataOffset;
}

/////////////////////////////////////////////////////////////////////////////

MtMatrixBufferWriter::MtMatrixBufferWriter(MetalRenderDevice *fb)
    : mBuffer(std::make_unique<MtStreamBuffer>(fb, sizeof(MatricesUBO), 4096)) {
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
      static bool __mt_matrixbuffer_logged = false;
      if (!__mt_matrixbuffer_logged) {
        Printf(PRINT_LOG, "MtMatrixBufferWriter::Write: buffer storage %d, mOffset %u\n", (int)mBuffer->GetBuffer()->storageMode(), (unsigned)mOffset);
        __mt_matrixbuffer_logged = true;
      }
      if (mBuffer->GetBuffer()->storageMode() == MTL::StorageModeManaged) {
        if (mt_debug) Printf(PRINT_LOG, "MtMatrixBufferWriter::Write: calling didModifyRange for mOffset %u\n", (unsigned)mOffset);
        mBuffer->GetBuffer()->didModifyRange(NS::Range(mOffset, sizeof(MatricesUBO)));
      }
    }
  }

  return true;
}

void MtMatrixBufferWriter::Reset() {
  mBuffer->Reset();
  mOffset = mBuffer->GetStreamDataOffset();
}

uint32_t MtMatrixBufferWriter::Offset() const { return mOffset; }