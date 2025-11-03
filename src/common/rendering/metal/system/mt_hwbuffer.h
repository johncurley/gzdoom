#pragma once

#include "hw_databuffer.h"
#include <cstdint>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

class MetalRenderDevice;

// Metal hardware data buffer
class MtHardwareDataBuffer : public IDataBuffer
{
public:
	MtHardwareDataBuffer(MetalRenderDevice* fb, int bindingpoint, bool ssbo, bool needsresize);
	~MtHardwareDataBuffer();

	void BindRange(FRenderState* state, size_t start, size_t length) override;
	void BindBase() override;

#ifdef __OBJC__
	id<MTLBuffer> GetBuffer() const { return mBuffer; }
#else
	void* GetBuffer() const { return mBuffer; }
#endif

protected:
	void SetData(size_t size, const void* data, BufferUsageType usage) override;
	void SetSubData(size_t offset, size_t size, const void* data) override;
	void Resize(size_t newsize) override;
	void Map() override;
	void Unmap() override;

private:
	void CreateBuffer(size_t size);

#ifdef __OBJC__
	id<MTLBuffer> mBuffer = nil;
#else
	void* mBuffer = nullptr;
#endif
	size_t mBufferSize = 0;
	int mBindingPoint = 0;
	bool mSSBO = false;
	bool mNeedsResize = false;
	MetalRenderDevice* fb = nullptr;
	void* mMappedMemory = nullptr;
};

// Metal vertex buffer
class MtVertexBuffer : public IVertexBuffer
{
public:
	MtVertexBuffer(MetalRenderDevice* fb);
	~MtVertexBuffer();

	void SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute* attrs) override;

#ifdef __OBJC__
	id<MTLBuffer> GetBuffer() const { return mBuffer; }
#else
	void* GetBuffer() const { return mBuffer; }
#endif

protected:
	void SetData(size_t size, const void* data, BufferUsageType usage) override;
	void SetSubData(size_t offset, size_t size, const void* data) override;
	void Map() override;
	void Unmap() override;

private:
	void CreateBuffer(size_t size);

#ifdef __OBJC__
	id<MTLBuffer> mBuffer = nil;
#else
	void* mBuffer = nullptr;
#endif
	size_t mBufferSize = 0;
	MetalRenderDevice* fb = nullptr;
	void* mMappedMemory = nullptr;
};

// Metal index buffer
class MtIndexBuffer : public IIndexBuffer
{
public:
	MtIndexBuffer(MetalRenderDevice* fb);
	~MtIndexBuffer();

#ifdef __OBJC__
	id<MTLBuffer> GetBuffer() const { return mBuffer; }
#else
	void* GetBuffer() const { return mBuffer; }
#endif

protected:
	void SetData(size_t size, const void* data, BufferUsageType usage) override;
	void SetSubData(size_t offset, size_t size, const void* data) override;
	void Map() override;
	void Unmap() override;

private:
	void CreateBuffer(size_t size);

#ifdef __OBJC__
	id<MTLBuffer> mBuffer = nil;
#else
	void* mBuffer = nullptr;
#endif
	size_t mBufferSize = 0;
	MetalRenderDevice* fb = nullptr;
	void* mMappedMemory = nullptr;
};
