#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "hwrenderer/data/hw_renderstate.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/metal_common.h" // New include
#include "mt_hwbuffer.h"
#include "mt_renderdevice.h"
#include "printf.h"

EXTERN_CVAR(Bool, mt_debug)

// MtHardwareDataBuffer
MtHardwareDataBuffer::MtHardwareDataBuffer(MetalRenderDevice *fb,
                                           int bindingpoint, bool ssbo,
                                           bool needsresize)
    : mBindingPoint(bindingpoint), mSSBO(ssbo), mNeedsResize(needsresize),
      fb(fb) {}

MtHardwareDataBuffer::~MtHardwareDataBuffer() {
  if (mBuffer) {
    if (fb && !fb->mIsDestroyed) {
      fb->RecycleBuffer(mBuffer);
    } else {
      mBuffer->release();
    }
    mBuffer = nullptr;
  }
}

void MtHardwareDataBuffer::BindRange(FRenderState *state, size_t start,
                                     size_t length) {
  if (mt_debug) {
    Printf("MtHardwareDataBuffer::BindRange: bp=%d, start=%zu, len=%zu\n",
           mBindingPoint, start, length);
  }

  auto mt_state = static_cast<MtRenderState *>(state ? state : fb->GetRenderState());
  mt_state->BindBuffer(mBindingPoint, mBuffer, (uint32_t)start);
}

void MtHardwareDataBuffer::Upload(size_t offset, size_t size) {
  if (mBuffer && mBuffer->storageMode() == MTL::StorageModeShared) {
    if (size > 0 && offset + size <= mBufferSize) {
      mBuffer->didModifyRange(NS::Range(offset, size));
    }
  }
}

void MtHardwareDataBuffer::SetData(size_t size, const void *data,
                                   BufferUsageType usage) {
  CreateBuffer(size);
  if (mBuffer && data) {
    memcpy(mBuffer->contents(), data, size);
    if (mBuffer->storageMode() == MTL::StorageModeShared)
        mBuffer->didModifyRange(NS::Range(0, size));
  }
}
void MtHardwareDataBuffer::SetSubData(size_t offset, size_t size,
                                      const void *data) {
  if (mBuffer && data) {
    memcpy((uint8_t *)mBuffer->contents() + offset, data, size);
    if (mBuffer->storageMode() == MTL::StorageModeShared)
        mBuffer->didModifyRange(NS::Range(offset, size));
  }
}
void MtHardwareDataBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtHardwareDataBuffer::Map() {
  if (mBuffer) {
    this->map = mBuffer->contents();
    this->mMappedMemory = this->map;
  }
}
void MtHardwareDataBuffer::Unmap() { 
  // For Metal StorageModeShared, we keep the buffer mapped as it's persistently accessible.
  // Clearing this->map here would cause crashes in the generic HW renderer logic.
}
void *MtHardwareDataBuffer::Lock(unsigned int size) {
  if (!mBuffer) CreateBuffer(size);
  Map();
  return this->map;
}
void MtHardwareDataBuffer::Unlock() { Unmap(); }

void MtHardwareDataBuffer::CreateBuffer(size_t size) {
  if (mBuffer) {
    if (fb && !fb->mIsDestroyed) {
      fb->RecycleBuffer(mBuffer);
    } else {
      mBuffer->release();
    }
  }
  if (size == 0) size = 16;
  mBuffer = fb->device->device->newBuffer(size, MTL::StorageModeShared);
  if (!mBuffer) return;
  
  mBufferSize = size;
  buffersize = size; // Set base class member
  this->map = mBuffer->contents();
  this->mMappedMemory = this->map;
}

// MtVertexBuffer
MtVertexBuffer::MtVertexBuffer(MetalRenderDevice *fb) : fb(fb) {}
MtVertexBuffer::~MtVertexBuffer() {
  if (mBuffer) {
    if (fb && !fb->mIsDestroyed) {
      fb->RecycleBuffer(mBuffer);
    } else {
      mBuffer->release();
    }
    mBuffer = nullptr;
  }
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

void MtVertexBuffer::Upload(size_t offset, size_t size) {
  if (mBuffer && mBuffer->storageMode() == MTL::StorageModeShared) {
    if (size > 0 && offset + size <= mBufferSize) {
      mBuffer->didModifyRange(NS::Range(offset, size));
    }
  }
}

void MtVertexBuffer::SetData(size_t size, const void *data,
                             BufferUsageType usage) {
  if (mt_debug) {
    Printf("Metal: MtVertexBuffer::SetData size=%zu data=%p usage=%d\n", size,
           data, (int)usage);
  }
  CreateBuffer(size);
  if (mBuffer && data) {
    memcpy(mBuffer->contents(), data, size);
    if (mBuffer->storageMode() == MTL::StorageModeShared)
        mBuffer->didModifyRange(NS::Range(0, size));
  }
}
void MtVertexBuffer::SetSubData(size_t offset, size_t size, const void *data) {
  if (mBuffer && data) {
    memcpy((uint8_t *)mBuffer->contents() + offset, data, size);
    if (mBuffer->storageMode() == MTL::StorageModeShared)
        mBuffer->didModifyRange(NS::Range(offset, size));
  }
}
void MtVertexBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtVertexBuffer::Map() {
  if (mBuffer) {
    this->map = mBuffer->contents();
    this->mMappedMemory = this->map;
  }
}
void MtVertexBuffer::Unmap() { }
void *MtVertexBuffer::Lock(unsigned int size) {
  if (!mBuffer) CreateBuffer(size);
  Map();
  return this->map;
}
void MtVertexBuffer::Unlock() { Unmap(); }

void MtVertexBuffer::CreateBuffer(size_t size) {
  if (mt_debug) {
    Printf("Metal: MtVertexBuffer::CreateBuffer size=%zu\n", size);
  }
  if (mBuffer) {
    if (fb && !fb->mIsDestroyed) {
      fb->RecycleBuffer(mBuffer);
    } else {
      mBuffer->release();
    }
  }
  if (size == 0) size = 16;
  mBuffer = fb->device->device->newBuffer(size, MTL::StorageModeShared);
  if (!mBuffer) return;

  mBufferSize = size;
  buffersize = size; // Set base class member
  this->map = mBuffer->contents();
  this->mMappedMemory = this->map;
}

// MtIndexBuffer
MtIndexBuffer::MtIndexBuffer(MetalRenderDevice *fb) : fb(fb) {}
MtIndexBuffer::~MtIndexBuffer() {
  if (mBuffer) {
    if (fb && !fb->mIsDestroyed) {
      fb->RecycleBuffer(mBuffer);
    } else {
      mBuffer->release();
    }
    mBuffer = nullptr;
  }
}

void MtIndexBuffer::Upload(size_t offset, size_t size) {
  if (mBuffer && mBuffer->storageMode() == MTL::StorageModeShared) {
    if (size > 0 && offset + size <= mBufferSize) {
      mBuffer->didModifyRange(NS::Range(offset, size));
    }
  }
}

void MtIndexBuffer::SetData(size_t size, const void *data,
                            BufferUsageType usage) {
  CreateBuffer(size);
  if (mBuffer && data) {
    memcpy(mBuffer->contents(), data, size);
    if (mBuffer->storageMode() == MTL::StorageModeShared)
        mBuffer->didModifyRange(NS::Range(0, size));
  }
}
void MtIndexBuffer::SetSubData(size_t offset, size_t size, const void *data) {
  if (mBuffer && data) {
    memcpy((uint8_t *)mBuffer->contents() + offset, data, size);
    if (mBuffer->storageMode() == MTL::StorageModeShared)
        mBuffer->didModifyRange(NS::Range(offset, size));
  }
}
void MtIndexBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtIndexBuffer::Map() {
  if (mBuffer) {
    this->map = mBuffer->contents();
    this->mMappedMemory = this->map;
  }
}
void MtIndexBuffer::Unmap() { }
void *MtIndexBuffer::Lock(unsigned int size) {
  if (!mBuffer) CreateBuffer(size);
  Map();
  return this->map;
}
void MtIndexBuffer::Unlock() { Unmap(); }

void MtIndexBuffer::CreateBuffer(size_t size) {
  if (mBuffer) {
    if (fb && !fb->mIsDestroyed) {
      fb->RecycleBuffer(mBuffer);
    } else {
      mBuffer->release();
    }
  }
  if (size == 0) size = 16;
  mBuffer = fb->device->device->newBuffer(size, MTL::StorageModeShared);
  if (!mBuffer) return;

  mBufferSize = size;
  buffersize = size; // Set base class member
  this->map = mBuffer->contents();
  this->mMappedMemory = this->map;
}
