#pragma once

#include <memory>
#include <vector>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

class MetalRenderDevice;

// Buffer manager - handles buffer allocation and ring buffers
class MtBufferManager
{
public:
	MtBufferManager(MetalRenderDevice* fb);
	~MtBufferManager();

	// Create buffers
#ifdef __OBJC__
	id<MTLBuffer> CreateBuffer(size_t size, MTLResourceOptions options);
	id<MTLBuffer> CreateBufferWithData(const void* data, size_t size, MTLResourceOptions options);
#else
	void* CreateBuffer(size_t size, unsigned int options);
	void* CreateBufferWithData(const void* data, size_t size, unsigned int options);
#endif

	// Ring buffer allocation (for dynamic data)
	struct RingBufferAllocation
	{
#ifdef __OBJC__
		id<MTLBuffer> buffer;
#else
		void* buffer;
#endif
		uint32_t offset;
		void* data;
	};

	RingBufferAllocation AllocateRingBuffer(size_t size);

	// Frame management
	void BeginFrame();
	void EndFrame();

	// Statistics
	size_t GetAllocatedMemory() const { return mAllocatedMemory; }

private:
	MetalRenderDevice* fb = nullptr;
	size_t mAllocatedMemory = 0;

	// Ring buffers
	static const size_t kRingBufferSize = 8 * 1024 * 1024; // 8MB
#ifdef __OBJC__
	std::vector<id<MTLBuffer>> mRingBuffers;
#else
	std::vector<void*> mRingBuffers;
#endif
	size_t mCurrentRingBuffer = 0;
	size_t mRingBufferOffset = 0;
};
