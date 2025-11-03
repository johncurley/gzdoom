#pragma once

#include <cstdint>
#include <memory>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

class MetalRenderDevice;

// Ring buffer for dynamic data (following Vulkan pattern)
class MtStreamBuffer
{
public:
	MtStreamBuffer(MetalRenderDevice* fb, size_t size);
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

	// Reset for new frame
	void Reset();

private:
#ifdef __OBJC__
	id<MTLBuffer> mBuffer = nil;
#else
	void* mBuffer = nullptr;
#endif
	uint32_t mStreamDataOffset = 0;
	size_t mBufferSize = 0;
	MetalRenderDevice* fb = nullptr;
};

// Writer classes for common buffer types
class MtStreamBufferWriter
{
public:
	MtStreamBufferWriter(MetalRenderDevice* fb);

	uint32_t Write(const void* data, size_t size);
	void Reset();

private:
	std::unique_ptr<MtStreamBuffer> mBuffer;
};

class MtMatrixBufferWriter
{
public:
	MtMatrixBufferWriter(MetalRenderDevice* fb);

	uint32_t Write(const void* data, size_t size);
	void Reset();

private:
	std::unique_ptr<MtStreamBuffer> mBuffer;
};
