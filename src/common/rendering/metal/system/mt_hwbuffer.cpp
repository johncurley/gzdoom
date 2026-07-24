#include "i_time.h"
#include <Metal/Metal.hpp>

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
  for (int i = 0; i < 3; i++) {
    if (mBuffers[i]) {
      if (fb && !fb->mIsDestroyed) {
        fb->RecycleBuffer(mBuffers[i]);
      } else {
        mBuffers[i]->release();
      }
      mBuffers[i] = nullptr;
    }
  }
}

MTL::Buffer *MtHardwareDataBuffer::GetBuffer() const {
  return mBuffers[mActiveBufferIndex];
}

void MtHardwareDataBuffer::PrepareForWrite() {
    if (mUsage == BufferUsageType::Static || !mBuffers[1]) {
        auto buffer = GetBuffer();
        if (buffer) {
            this->map = buffer->contents();
            this->mMappedMemory = this->map;
        }
        return;
    }

    uint64_t frame = fb->GetFrameCount();
    if (mLastWriteFrame != frame) {
        int oldIndex = mActiveBufferIndex;
        mActiveBufferIndex = (mActiveBufferIndex + 1) % 3;
        mLastWriteFrame = frame;

        if (mBuffers[oldIndex] && mBuffers[mActiveBufferIndex]) {
            // Update mapping pointers to the new buffer slot
            this->map = mBuffers[mActiveBufferIndex]->contents();
            this->mMappedMemory = this->map;
        }
    }
}

void MtHardwareDataBuffer::BindRange(FRenderState *state, size_t start,
                                     size_t length) {
  auto mt_state = static_cast<MtRenderState *>(state ? state : fb->GetRenderState());
  mt_state->BindBuffer(mBindingPoint, GetBuffer(), (uint32_t)start);
}

void MtHardwareDataBuffer::Upload(size_t offset, size_t size) {
  auto buffer = GetBuffer();
    if (buffer->storageMode() == MTL::StorageModeManaged) {
    if (size > 0 && offset + size <= mBufferSize) {
      buffer->didModifyRange(NS::Range(offset, size));
    }
  }
}

void MtHardwareDataBuffer::SetData(size_t size, const void *data,
                                   BufferUsageType usage) {
  mUsage = usage;
  if (!mBuffers[0] || mBufferSize < size) {
      CreateBuffer(size);
  } else {
      PrepareForWrite();
  }

  auto buffer = GetBuffer();
  if (buffer && data) {
    memcpy(buffer->contents(), data, size);
    if (buffer->storageMode() == MTL::StorageModeManaged)
        buffer->didModifyRange(NS::Range(0, size));
  }
}
void MtHardwareDataBuffer::SetSubData(size_t offset, size_t size,
                                      const void *data) {
  if (mUsage != BufferUsageType::Static)
      PrepareForWrite();
  auto buffer = GetBuffer();
  if (buffer && data) {
    memcpy((uint8_t *)buffer->contents() + offset, data, size);
    if (buffer->storageMode() == MTL::StorageModeManaged)
        buffer->didModifyRange(NS::Range(offset, size));
  }
}
void MtHardwareDataBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtHardwareDataBuffer::Map() {
  auto buffer = GetBuffer();
  if (buffer) {
    this->map = buffer->contents();
    this->mMappedMemory = this->map;
  }
}
void MtHardwareDataBuffer::Unmap() { 
  auto buffer = GetBuffer();
  if (buffer && buffer->storageMode() == MTL::StorageModeManaged) {
      buffer->didModifyRange(NS::Range(0, mBufferSize));
  }
}
void *MtHardwareDataBuffer::Lock(unsigned int size) {
  if (!mBuffers[0] || mBufferSize < size) {
      CreateBuffer(size);
  } else {
      PrepareForWrite();
  }
  Map();
  return this->map;
}
void MtHardwareDataBuffer::Unlock() { Unmap(); }

void MtHardwareDataBuffer::CreateBuffer(size_t size) {
  if (size == 0) size = 16;
  
  // Only triple-buffer internally for Stream/Mappable types. 
  // Persistent usage (lights/bones) is double-buffered by the engine's manager classes.
  bool useInternalRing = (mUsage == BufferUsageType::Stream || mUsage == BufferUsageType::Mappable);
  int count = useInternalRing ? 3 : 1;

  // Check if we need to recreate
  if (mBuffers[0] && mBufferSize >= size) {
      if (useInternalRing && !mBuffers[1]) {
          // Upgrade to dynamic - fall through to recreation
      } else {
          return;
      }
  }

  for (int i = 0; i < 3; i++) {
    if (mBuffers[i]) {
      if (fb && !fb->mIsDestroyed) {
        fb->RecycleBuffer(mBuffers[i]);
      }
      else {
        mBuffers[i]->release();
      }
      mBuffers[i] = nullptr;
    }
    if (i < count) {
        mBuffers[i] = fb->device->device->newBuffer(size, fb->mVersionManager.GetDynamicStorageMode());
    }
  }
  
  mActiveBufferIndex = 0;
  mBufferSize = size;
  buffersize = size; // Set base class member
  auto buffer = GetBuffer();
  if (buffer) {
      this->map = buffer->contents();
      this->mMappedMemory = this->map;
  }
}

// MtVertexBuffer
MtVertexBuffer::MtVertexBuffer(MetalRenderDevice *fb) : fb(fb) {}
MtVertexBuffer::~MtVertexBuffer() {
  for (int i = 0; i < 3; i++) {
    if (mBuffers[i]) {
      if (fb && !fb->mIsDestroyed) {
        fb->RecycleBuffer(mBuffers[i]);
      } else {
        mBuffers[i]->release();
      }
      mBuffers[i] = nullptr;
    }
  }
}

MTL::Buffer *MtVertexBuffer::GetBuffer() const {
  return mBuffers[mActiveBufferIndex];
}

void MtVertexBuffer::PrepareForWrite() {
    if (mUsage == BufferUsageType::Static || !mBuffers[1]) {
        auto buffer = GetBuffer();
        if (buffer) {
            this->map = buffer->contents();
            this->mMappedMemory = this->map;
        }
        return;
    }

    uint64_t frame = fb->GetFrameCount();
    if (mLastWriteFrame != frame) {
        int oldIndex = mActiveBufferIndex;
        mActiveBufferIndex = (mActiveBufferIndex + 1) % 3;
        mLastWriteFrame = frame;

        if (mBuffers[oldIndex] && mBuffers[mActiveBufferIndex]) {
            // Update mapping pointers to the new buffer slot
            this->map = mBuffers[mActiveBufferIndex]->contents();
            this->mMappedMemory = this->map;
        }
    }
}

void MtVertexBuffer::SetFormat(int numBindingPoints, int numAttributes,
                               size_t stride,
                               const FVertexBufferAttribute *attrs) {
  mStride = stride;
  mNumBindingPoints = numBindingPoints;
  mAttributes.assign(attrs, attrs + numAttributes);
  mHasColor = false;
  mHasNormal = false;

  // Mirrors Vulkan's VkRenderPassManager::GetVertexFormat (vk_renderpass.cpp),
  // which sets UseVertexData bit 0 for VATTR_COLOR and bit 1 for VATTR_NORMAL
  // -- the shared main.vp vertex shader branches on both bits independently
  // ("useVertexData & 1", "useVertexData & 2"). This code previously only
  // tracked VATTR_COLOR, so ApplyStreamData() never set bit 1 and Metal
  // always took the fallback-normal branch even when the vertex buffer had
  // real per-vertex normals.
  for (int i = 0; i < numAttributes; ++i) {
    if (attrs[i].location == VATTR_COLOR) {
      mHasColor = true;
    } else if (attrs[i].location == VATTR_NORMAL) {
      mHasNormal = true;
    }
  }

  // Create a simple vertex format ID by combining stride and attribute count
  // This is enough to differentiate between different vertex formats for now
  // TODO: Implement proper vertex format caching like Vulkan
  VertexFormat = (int)stride + (numAttributes << 16);
}

void MtVertexBuffer::Upload(size_t offset, size_t size) {
  auto buffer = GetBuffer();
    if (buffer->storageMode() == MTL::StorageModeManaged) {
    if (size > 0 && offset + size <= mBufferSize) {
      buffer->didModifyRange(NS::Range(offset, size));
    }
  }
}

void MtVertexBuffer::SetData(size_t size, const void *data,
                             BufferUsageType usage) {
  mUsage = usage;
  if (!mBuffers[0] || mBufferSize < size) {
      CreateBuffer(size);
  } else {
      PrepareForWrite();
  }

  auto buffer = GetBuffer();
  if (buffer && data) {
    memcpy(buffer->contents(), data, size);
    if (buffer->storageMode() == MTL::StorageModeManaged)
        buffer->didModifyRange(NS::Range(0, size));
  }
}
void MtVertexBuffer::SetSubData(size_t offset, size_t size, const void *data) {
  if (mUsage != BufferUsageType::Static)
      PrepareForWrite();
  auto buffer = GetBuffer();
  if (buffer && data) {
    memcpy((uint8_t *)buffer->contents() + offset, data, size);
    if (buffer->storageMode() == MTL::StorageModeManaged)
        buffer->didModifyRange(NS::Range(offset, size));
  }
}
void MtVertexBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtVertexBuffer::Map() {
  auto buffer = GetBuffer();
  if (buffer) {
    this->map = buffer->contents();
    this->mMappedMemory = this->map;
  }
}
void MtVertexBuffer::Unmap() { 
  auto buffer = GetBuffer();
  if (buffer && buffer->storageMode() == MTL::StorageModeManaged) {
      buffer->didModifyRange(NS::Range(0, mBufferSize));
  }
}
void *MtVertexBuffer::Lock(unsigned int size) {
  if (!mBuffers[0] || mBufferSize < size) {
      CreateBuffer(size);
  } else {
      PrepareForWrite();
  }
  Map();
  return this->map;
}
void MtVertexBuffer::Unlock() { Unmap(); }

void MtVertexBuffer::CreateBuffer(size_t size) {
  if (size == 0) size = 16;

  bool useInternalRing = (mUsage == BufferUsageType::Stream || mUsage == BufferUsageType::Mappable);
  int count = useInternalRing ? 3 : 1;

  if (mBuffers[0] && mBufferSize >= size) {
      if (useInternalRing && !mBuffers[1]) {
          // Upgrade
      } else {
          return;
      }
  }

  for (int i = 0; i < 3; i++) {
    if (mBuffers[i]) {
      if (fb && !fb->mIsDestroyed) {
        fb->RecycleBuffer(mBuffers[i]);
      } else {
        mBuffers[i]->release();
      }
      mBuffers[i] = nullptr;
    }
    if (i < count) {
        mBuffers[i] = fb->device->device->newBuffer(size, fb->mVersionManager.GetDynamicStorageMode());
    }
  }

  mActiveBufferIndex = 0;
  mBufferSize = size;
  buffersize = size; // Set base class member
  auto buffer = GetBuffer();
  if (buffer) {
      this->map = buffer->contents();
      this->mMappedMemory = this->map;
  }
}

// MtIndexBuffer
MtIndexBuffer::MtIndexBuffer(MetalRenderDevice *fb) : fb(fb) {}
MtIndexBuffer::~MtIndexBuffer() {
  for (int i = 0; i < 3; i++) {
    if (mBuffers[i]) {
      if (fb && !fb->mIsDestroyed) {
        fb->RecycleBuffer(mBuffers[i]);
      }
      else {
        mBuffers[i]->release();
      }
      mBuffers[i] = nullptr;
    }
  }
}

MTL::Buffer *MtIndexBuffer::GetBuffer() const {
  if (mUsage == BufferUsageType::Static) return mBuffers[0];
  return mBuffers[mActiveBufferIndex];
}

void MtIndexBuffer::PrepareForWrite() {
    if (mUsage == BufferUsageType::Static || !mBuffers[1]) {
        auto buffer = GetBuffer();
        if (buffer) {
            this->map = buffer->contents();
            this->mMappedMemory = this->map;
        }
        return;
    }

    uint64_t frame = fb->GetFrameCount();
    if (mLastWriteFrame != frame) {
        int oldIndex = mActiveBufferIndex;
        mActiveBufferIndex = (mActiveBufferIndex + 1) % 3;
        mLastWriteFrame = frame;

        if (mBuffers[oldIndex] && mBuffers[mActiveBufferIndex]) {
            // Update mapping pointers to the new buffer slot
            this->map = mBuffers[mActiveBufferIndex]->contents();
            this->mMappedMemory = this->map;
        }
    }
}

void MtIndexBuffer::Upload(size_t offset, size_t size) {
  auto buffer = GetBuffer();
    if (buffer->storageMode() == MTL::StorageModeManaged) {
    if (size > 0 && offset + size <= mBufferSize) {
      buffer->didModifyRange(NS::Range(offset, size));
    }
  }
}

void MtIndexBuffer::SetData(size_t size, const void *data,
                            BufferUsageType usage) {
  mUsage = usage;
  if (!mBuffers[0] || mBufferSize < size) {
      CreateBuffer(size);
  } else {
      PrepareForWrite();
  }

  auto buffer = GetBuffer();
  if (buffer && data) {
    memcpy(buffer->contents(), data, size);
    if (buffer->storageMode() == MTL::StorageModeManaged)
        buffer->didModifyRange(NS::Range(0, size));
  }
}
void MtIndexBuffer::SetSubData(size_t offset, size_t size, const void *data) {
  if (mUsage != BufferUsageType::Static)
      PrepareForWrite();
  auto buffer = GetBuffer();
  if (buffer && data) {
    memcpy((uint8_t *)buffer->contents() + offset, data, size);
    if (buffer->storageMode() == MTL::StorageModeManaged)
        buffer->didModifyRange(NS::Range(offset, size));
  }
}
void MtIndexBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtIndexBuffer::Map() {
  auto buffer = GetBuffer();
  if (buffer) {
    this->map = buffer->contents();
    this->mMappedMemory = this->map;
  }
}
void MtIndexBuffer::Unmap() {
  auto buffer = GetBuffer();
  if (buffer && buffer->storageMode() == MTL::StorageModeManaged) {
      buffer->didModifyRange(NS::Range(0, mBufferSize));
  }
}
void *MtIndexBuffer::Lock(unsigned int size) {
  if (!mBuffers[0] || mBufferSize < size) {
      CreateBuffer(size);
  } else {
      PrepareForWrite();
  }
  Map();
  return this->map;
}
void MtIndexBuffer::Unlock() { Unmap(); }

void MtIndexBuffer::CreateBuffer(size_t size) {
  if (size == 0) size = 16;

  bool useInternalRing = (mUsage == BufferUsageType::Stream || mUsage == BufferUsageType::Mappable);
  int count = useInternalRing ? 3 : 1;

  if (mBuffers[0] && mBufferSize >= size) {
      if (useInternalRing && !mBuffers[1]) {
          // Upgrade
      } else {
          return;
      }
  }

  for (int i = 0; i < 3; i++) {
    if (mBuffers[i]) {
      if (fb && !fb->mIsDestroyed) {
        fb->RecycleBuffer(mBuffers[i]);
      } else {
        mBuffers[i]->release();
      }
      mBuffers[i] = nullptr;
    }
    if (i < count) {
        mBuffers[i] = fb->device->device->newBuffer(size, fb->mVersionManager.GetDynamicStorageMode());
    }
  }

  mActiveBufferIndex = 0;
  mBufferSize = size;
  buffersize = size; // Set base class member
  auto buffer = GetBuffer();
  if (buffer) {
      this->map = buffer->contents();
      this->mMappedMemory = this->map;
  }
}
