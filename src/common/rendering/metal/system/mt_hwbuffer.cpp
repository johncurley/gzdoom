#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "hwrenderer/data/hw_renderstate.h"
#include "metal/renderer/mt_renderstate.h"
#include "mt_hwbuffer.h"
#include "mt_renderdevice.h"
#include "printf.h"

// MtHardwareDataBuffer
MtHardwareDataBuffer::MtHardwareDataBuffer(MetalRenderDevice *fb,
                                           int bindingpoint, bool ssbo,
                                           bool needsresize)
    : mBindingPoint(bindingpoint), mSSBO(ssbo), mNeedsResize(needsresize),
      fb(fb) {}

MtHardwareDataBuffer::~MtHardwareDataBuffer() {
  if (mBuffer)
    mBuffer->release();
}

void MtHardwareDataBuffer::BindRange(FRenderState *state, size_t start,
                                     size_t length) {
  static int bindCount = 0;
  if (bindCount++ < 10) {
    Printf("MtHardwareDataBuffer::BindRange: binding to point %d, start=%zu, "
           "length=%zu\n",
           mBindingPoint, start, length);
  }

  auto mt_state = static_cast<MtRenderState *>(state);
  MTL::RenderCommandEncoder *encoder = mt_state->GetEncoder();
  if (encoder) {
    encoder->setVertexBuffer(mBuffer, start, mBindingPoint);
  }
}
void MtHardwareDataBuffer::SetData(size_t size, const void *data,
                                   BufferUsageType usage) {
  CreateBuffer(size);
  if (mBuffer && data)
    memcpy(mBuffer->contents(), data, size);
}
void MtHardwareDataBuffer::SetSubData(size_t offset, size_t size,
                                      const void *data) {
  if (mBuffer && data)
    memcpy((uint8_t *)mBuffer->contents() + offset, data, size);
}
void MtHardwareDataBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtHardwareDataBuffer::Map() {
  if (mBuffer)
    this->map = mBuffer->contents();
}
void MtHardwareDataBuffer::Unmap() { this->map = nullptr; }
void *MtHardwareDataBuffer::Lock(unsigned int size) {
  Map();
  return this->map;
}
void MtHardwareDataBuffer::Unlock() { Unmap(); }

void MtHardwareDataBuffer::CreateBuffer(size_t size) {
  if (mBuffer)
    mBuffer->release();
  mBuffer = fb->device->device->newBuffer(size, MTL::StorageModeShared);
  mBufferSize = size;
  buffersize = size; // Set base class member
}

// MtVertexBuffer
MtVertexBuffer::MtVertexBuffer(MetalRenderDevice *fb) : fb(fb) {}
MtVertexBuffer::~MtVertexBuffer() {
  if (mBuffer)
    mBuffer->release();
}

void MtVertexBuffer::SetFormat(int numBindingPoints, int numAttributes,
                               size_t stride,
                               const FVertexBufferAttribute *attrs) {
  mStride = stride;
  mNumBindingPoints = numBindingPoints;
  mAttributes.assign(attrs, attrs + numAttributes);
  mHasColor = false;

  for (int i = 0; i < numAttributes; ++i) {
    if (attrs[i].location == VATTR_COLOR) {
      mHasColor = true;
      break;
    }
  }

  // Create a simple vertex format ID by combining stride and attribute count
  // This is enough to differentiate between different vertex formats for now
  // TODO: Implement proper vertex format caching like Vulkan
  VertexFormat = (int)stride + (numAttributes << 16);
}
void MtVertexBuffer::SetData(size_t size, const void *data,
                             BufferUsageType usage) {
  CreateBuffer(size);
  if (mBuffer && data)
    memcpy(mBuffer->contents(), data, size);
}
void MtVertexBuffer::SetSubData(size_t offset, size_t size, const void *data) {
  if (mBuffer && data)
    memcpy((uint8_t *)mBuffer->contents() + offset, data, size);
}
void MtVertexBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtVertexBuffer::Map() {
  if (mBuffer)
    this->map = mBuffer->contents();
}
void MtVertexBuffer::Unmap() { this->map = nullptr; }
void *MtVertexBuffer::Lock(unsigned int size) {
  Map();
  return this->map;
}
void MtVertexBuffer::Unlock() { Unmap(); }

void MtVertexBuffer::CreateBuffer(size_t size) {
  if (mBuffer)
    mBuffer->release();
  mBuffer = fb->device->device->newBuffer(size, MTL::StorageModeShared);
  mBufferSize = size;
  buffersize = size; // Set base class member
}

// MtIndexBuffer
MtIndexBuffer::MtIndexBuffer(MetalRenderDevice *fb) : fb(fb) {}
MtIndexBuffer::~MtIndexBuffer() {
  if (mBuffer)
    mBuffer->release();
}

void MtIndexBuffer::SetData(size_t size, const void *data,
                            BufferUsageType usage) {
  CreateBuffer(size);
  if (mBuffer && data)
    memcpy(mBuffer->contents(), data, size);
}
void MtIndexBuffer::SetSubData(size_t offset, size_t size, const void *data) {
  if (mBuffer && data)
    memcpy((uint8_t *)mBuffer->contents() + offset, data, size);
}
void MtIndexBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtIndexBuffer::Map() {
  if (mBuffer)
    this->map = mBuffer->contents();
}
void MtIndexBuffer::Unmap() { this->map = nullptr; }
void *MtIndexBuffer::Lock(unsigned int size) {
  Map();
  return this->map;
}
void MtIndexBuffer::Unlock() { Unmap(); }

void MtIndexBuffer::CreateBuffer(size_t size) {
  if (mBuffer)
    mBuffer->release();
  mBuffer = fb->device->device->newBuffer(size, MTL::StorageModeShared);
  mBufferSize = size;
  buffersize = size; // Set base class member
}
