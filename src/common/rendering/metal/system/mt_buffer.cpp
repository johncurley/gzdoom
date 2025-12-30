#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "hwrenderer/data/shaderuniforms.h"
#include "metal/renderer/mt_streambuffer.h"
#include "mt_buffer.h"
#include "mt_hwbuffer.h"
#include "mt_renderdevice.h"
#include "tarray.h"

MtBufferManager::MtBufferManager(MetalRenderDevice *fb) : fb(fb) {
  // Pre-allocate ring buffers
  for (int i = 0; i < 3; i++) // Triple buffering
  {
    MTL::Buffer *ringBuffer = fb->device->device->newBuffer(
        kRingBufferSize, MTL::ResourceStorageModeShared);
    if (ringBuffer) {
      mRingBuffers.push_back(ringBuffer);
      mAllocatedMemory += kRingBufferSize;
    }
  }
}

MtBufferManager::~MtBufferManager() {
  // Release all ring buffers
  for (auto buffer : mRingBuffers) {
    if (buffer)
      buffer->release();
  }
  mRingBuffers.clear();
}

MTL::Buffer *MtBufferManager::CreateBuffer(size_t size, unsigned int options) {
  MTL::Buffer *buffer =
      fb->device->device->newBuffer(size, (MTL::ResourceOptions)options);
  if (buffer) {
    mAllocatedMemory += size;
  }
  return buffer;
}

MTL::Buffer *MtBufferManager::CreateBufferWithData(const void *data, size_t size,
                                            unsigned int options) {
  MTL::Buffer *buffer =
      fb->device->device->newBuffer(data, size, (MTL::ResourceOptions)options);
  if (buffer) {
    mAllocatedMemory += size;
  }
  return buffer;
}

MtBufferManager::RingBufferAllocation
MtBufferManager::AllocateRingBuffer(size_t size) {
  RingBufferAllocation alloc;

  if (mRingBuffers.empty()) {
    alloc.buffer = nullptr;
    alloc.offset = 0;
    alloc.data = nullptr;
    return alloc;
  }

  // Get current ring buffer
  MTL::Buffer *buffer = mRingBuffers[mCurrentRingBuffer];

  // Check if we need to wrap around
  if (mRingBufferOffset + size > kRingBufferSize) {
    // Move to next ring buffer
    mCurrentRingBuffer = (mCurrentRingBuffer + 1) % mRingBuffers.size();
    mRingBufferOffset = 0;
    buffer = mRingBuffers[mCurrentRingBuffer];
  }

  // Allocate from current position
  alloc.buffer = buffer;
  alloc.offset = (uint32_t)mRingBufferOffset;
  alloc.data = (uint8_t *)buffer->contents() + mRingBufferOffset;

  // Advance offset (with alignment)
  size_t alignedSize = (size + 255) & ~255; // 256-byte alignment
  mRingBufferOffset += alignedSize;

  return alloc;
}

void MtBufferManager::Init() {
  // Create stream buffers for matrices and per-draw uniforms
  MatrixBuffer.reset(new MtStreamBuffer(fb, sizeof(MatricesUBO)));
  StreamBuffer.reset(new MtStreamBuffer(fb, sizeof(StreamUBO)));

  // Create fan to triangle index buffer
  CreateFanToTrisIndexBuffer();

  // Create constant dummy buffer for unused attributes
  DummyBuffer = fb->device->device->newBuffer(256, MTL::StorageModeShared);
  if (DummyBuffer) {
      memset(DummyBuffer->contents(), 0, 256);
  }
}

void MtBufferManager::Deinit() {
  if (DummyBuffer) {
      DummyBuffer->release();
      DummyBuffer = nullptr;
  }
  // Clean up stream buffers
  MatrixBuffer.reset();
  StreamBuffer.reset();
  FanToTrisIndexBuffer.reset();
}

IVertexBuffer *MtBufferManager::CreateVertexBuffer() {
  return new MtVertexBuffer(fb);
}

IIndexBuffer *MtBufferManager::CreateIndexBuffer() {
  return new MtIndexBuffer(fb);
}

IDataBuffer *MtBufferManager::CreateDataBuffer(int bindingpoint, bool ssbo,
                                               bool needsresize) {
  auto buffer = new MtHardwareDataBuffer(fb, bindingpoint, ssbo, needsresize);
  switch (bindingpoint)
  {
  case VIEWPOINT_BINDINGPOINT: ViewpointUBO = buffer; break;
  case LIGHTBUF_BINDINGPOINT: LightBufferSSO = buffer; break;
  case BONEBUF_BINDINGPOINT: BoneBufferSSO = buffer; break;
  }
  return buffer;
}

void MtBufferManager::CreateFanToTrisIndexBuffer() {
  // Create index buffer for converting triangle fans to triangle lists
  // Needed because Metal doesn't support triangle fans natively
  TArray<uint32_t> data;
  for (int i = 2; i < 1000; i++) {
    data.Push(0);
    data.Push(i - 1);
    data.Push(i);
  }

  FanToTrisIndexBuffer.reset(CreateIndexBuffer());
  FanToTrisIndexBuffer->SetData(sizeof(uint32_t) * data.Size(), data.Data(),
                                BufferUsageType::Static);
}

void MtBufferManager::BeginFrame() {
  // Ring buffer wraps automatically in AllocateRingBuffer
}

void MtBufferManager::EndFrame() {
  // Nothing needed for now
}
