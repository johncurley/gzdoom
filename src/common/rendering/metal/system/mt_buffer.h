#pragma once

#include <memory>
#include <vector>
#include <list>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
#endif

class MetalRenderDevice;
class MtHardwareDataBuffer;
class MtStreamBuffer;
class IIndexBuffer;
class IVertexBuffer;
class IDataBuffer;

// Buffer manager - handles buffer allocation and ring buffers
class MtBufferManager
{
public:
	MtBufferManager(MetalRenderDevice* fb);
	~MtBufferManager();

	void Init();
	void Deinit();

	// Create buffers
#ifdef __OBJC__
	id<MTLBuffer> CreateBuffer(size_t size, MTLResourceOptions options);
	id<MTLBuffer> CreateBufferWithData(const void* data, size_t size, MTLResourceOptions options);
#else
	void* CreateBuffer(size_t size, unsigned int options);
	void* CreateBufferWithData(const void* data, size_t size, unsigned int options);
#endif

	IVertexBuffer* CreateVertexBuffer();
	IIndexBuffer* CreateIndexBuffer();
	IDataBuffer* CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize);

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

	// Uniform buffers (exposed for resource binding)
	MtHardwareDataBuffer* ViewpointUBO = nullptr;
	MtHardwareDataBuffer* LightBufferSSO = nullptr;
	MtHardwareDataBuffer* LightNodes = nullptr;
	MtHardwareDataBuffer* LightLines = nullptr;
	MtHardwareDataBuffer* LightList = nullptr;
	MtHardwareDataBuffer* BoneBufferSSO = nullptr;

	std::unique_ptr<MtStreamBuffer> MatrixBuffer;
	std::unique_ptr<MtStreamBuffer> StreamBuffer;

	std::unique_ptr<IIndexBuffer> FanToTrisIndexBuffer;

private:
	void CreateFanToTrisIndexBuffer();

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
