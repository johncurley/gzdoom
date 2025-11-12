#pragma once

#include <cstdint>
#include <memory>
#include "matrix.h"
#include "hw_renderstate.h"
#include "metal/shaders/mt_shader.h"

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
#endif

class MetalRenderDevice;

// Ring buffer for dynamic data (following Vulkan pattern)
class MtStreamBuffer
{
public:
	MtStreamBuffer(MetalRenderDevice* fb, size_t structSize);
	~MtStreamBuffer();

	// Allocate from ring buffer
	uint32_t NextStreamDataBlock();

	// Get buffer and offset
#ifdef __OBJC__
	id<MTLBuffer> GetBuffer() const { return mBuffer; }
#else
	void* GetBuffer() const { return mBuffer; }
#endif
	uint32_t GetOffset() const { return mStreamDataOffset; }

	// Get CPU-accessible buffer pointer for writing
	uint8_t* GetBufferPointer() const;

	// Reset for new frame
	void Reset() { mStreamDataOffset = 0; }

private:
#ifdef __OBJC__
	id<MTLBuffer> mBuffer = nil;
#else
	void* mBuffer = nullptr;
#endif
	uint32_t mBlockSize = 0;
	uint32_t mStreamDataOffset = 0;
	size_t mBufferSize = 0;
	MetalRenderDevice* fb = nullptr;
};

// Writer classes for common buffer types
class MtStreamBufferWriter
{
public:
	MtStreamBufferWriter(MetalRenderDevice* fb);

	bool Write(const StreamData& data);
	void Reset();

	uint32_t DataIndex() const;
	uint32_t StreamDataOffset() const;

	// Get underlying buffer for binding
#ifdef __OBJC__
	id<MTLBuffer> GetBuffer() const { return mBuffer ? mBuffer->GetBuffer() : nil; }
#else
	void* GetBuffer() const { return mBuffer ? mBuffer->GetBuffer() : nullptr; }
#endif

private:
	std::unique_ptr<MtStreamBuffer> mBuffer;
	uint32_t mDataIndex = MAX_STREAM_DATA - 1;
	uint32_t mStreamDataOffset = 0;
};

class MtMatrixBufferWriter
{
public:
	MtMatrixBufferWriter(MetalRenderDevice* fb);

	bool Write(const VSMatrix& modelMatrix, bool modelMatrixEnabled, const VSMatrix& textureMatrix, bool textureMatrixEnabled);
	void Reset();

	uint32_t Offset() const;

	// Get underlying buffer for binding
#ifdef __OBJC__
	id<MTLBuffer> GetBuffer() const { return mBuffer ? mBuffer->GetBuffer() : nil; }
#else
	void* GetBuffer() const { return mBuffer ? mBuffer->GetBuffer() : nullptr; }
#endif

private:
	std::unique_ptr<MtStreamBuffer> mBuffer;
	MatricesUBO mMatrices = {};
	VSMatrix mIdentityMatrix;
	uint32_t mOffset = 0;
};
