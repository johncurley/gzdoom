#pragma once

#include "hwrenderer/data/buffers.h"
#include <cstdint>

#define TimeScale TimeScale_GZDOOM
namespace MTL {
class Buffer;
} // namespace MTL
#undef TimeScale

class MetalRenderDevice;

// Metal hardware data buffer
class MtHardwareDataBuffer : public IDataBuffer {
public:
  MtHardwareDataBuffer(MetalRenderDevice *fb, int bindingpoint, bool ssbo,
                       bool needsresize);
  ~MtHardwareDataBuffer();

  void BindRange(FRenderState *state, size_t start, size_t length) override;
  void Upload(size_t offset, size_t size) override;

  MTL::Buffer *GetBuffer() const;

protected:
  void PrepareForWrite();
  void SetData(size_t size, const void *data, BufferUsageType usage) override;
  void SetSubData(size_t offset, size_t size, const void *data) override;
  void Resize(size_t newsize) override;
  void Map() override;
  void Unmap() override;
  void *Lock(unsigned int size) override;
  void Unlock() override;

private:
  void CreateBuffer(size_t size);

  MTL::Buffer *mBuffers[3] = { nullptr, nullptr, nullptr };
  int mActiveBufferIndex = 0;
  uint64_t mLastWriteFrame = 0xFFFFFFFFFFFFFFFF;
  size_t mBufferSize = 0;
  int mBindingPoint = 0;
  bool mSSBO = false;
  bool mNeedsResize = false;
  BufferUsageType mUsage = BufferUsageType::Static;
  MetalRenderDevice *fb = nullptr;
  void *mMappedMemory = nullptr;
};

// Metal vertex buffer
class MtVertexBuffer : public IVertexBuffer {
public:
  MtVertexBuffer(MetalRenderDevice *fb);
  ~MtVertexBuffer();

  void SetFormat(int numBindingPoints, int numAttributes, size_t stride,
                 const FVertexBufferAttribute *attrs) override;
  void Upload(size_t offset, size_t size) override;

  MTL::Buffer *GetBuffer() const;

  // Vertex format info (needed for binding)
  size_t GetStride() const { return mStride; }
  int GetBindingPoints() const { return mNumBindingPoints; }
  int GetNumAttributes() const { return (int)mAttributes.size(); }
  const FVertexBufferAttribute *GetAttributes() const {
    return mAttributes.data();
  }

  // Vertex format identifier (for pipeline state selection)
  int VertexFormat = -1;
  bool HasColor() const { return mHasColor; }

protected:
  void PrepareForWrite();
  void SetData(size_t size, const void *data, BufferUsageType usage) override;
  void SetSubData(size_t offset, size_t size, const void *data) override;
  void Resize(size_t newsize) override;
  void Map() override;
  void Unmap() override;
  void *Lock(unsigned int size) override;
  void Unlock() override;

private:
  void CreateBuffer(size_t size);

  MTL::Buffer *mBuffers[3] = { nullptr, nullptr, nullptr };
  int mActiveBufferIndex = 0;
  uint64_t mLastWriteFrame = 0xFFFFFFFFFFFFFFFF;
  size_t mBufferSize = 0;
  size_t mStride = 0;
  int mNumBindingPoints = 0;
  std::vector<FVertexBufferAttribute> mAttributes;
  bool mHasColor = false;
  BufferUsageType mUsage = BufferUsageType::Static;
  MetalRenderDevice *fb = nullptr;
  void *mMappedMemory = nullptr;
};

// Metal index buffer
class MtIndexBuffer : public IIndexBuffer {
public:
  MtIndexBuffer(MetalRenderDevice *fb);
  ~MtIndexBuffer();

  MTL::Buffer *GetBuffer() const;
  void Upload(size_t offset, size_t size) override;

  void *Lock(unsigned int size) override;
  void Unlock() override;

protected:
  void PrepareForWrite();
  void SetData(size_t size, const void *data, BufferUsageType usage) override;
  void SetSubData(size_t offset, size_t size, const void *data) override;
  void Resize(size_t newsize) override;
  void Map() override;
  void Unmap() override;

private:
  void CreateBuffer(size_t size);

  MTL::Buffer *mBuffers[3] = { nullptr, nullptr, nullptr };
  int mActiveBufferIndex = 0;
  uint64_t mLastWriteFrame = 0xFFFFFFFFFFFFFFFF;
  size_t mBufferSize = 0;
  BufferUsageType mUsage = BufferUsageType::Static;
  MetalRenderDevice *fb = nullptr;
  void *mMappedMemory = nullptr;
};
