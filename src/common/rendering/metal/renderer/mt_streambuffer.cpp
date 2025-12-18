#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "i_system.h"
#include "metal/system/mt_renderdevice.h"
#include "mt_streambuffer.h"

MtStreamBuffer::MtStreamBuffer(MetalRenderDevice *fb, size_t structSize)
    : mBlockSize(static_cast<uint32_t>(structSize)), fb(fb) {
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
  mStreamDataOffset += mBlockSize;
  if (mStreamDataOffset + mBlockSize >= mBufferSize) {
    mStreamDataOffset = 0;
    return 0xffffffff; // Buffer full, need to wait
  }
  return mStreamDataOffset;
}

uint8_t *MtStreamBuffer::GetBufferPointer() const {
  if (!mBuffer)
    return nullptr;

  // Metal buffers with StorageModeShared are CPU-accessible
  // contents() returns a pointer to the buffer's CPU-side memory
  return (uint8_t *)mBuffer->contents();
}
