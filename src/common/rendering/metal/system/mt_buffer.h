#pragma once

#include <memory>
#include <vector>
#include <list>

namespace MTL {
class Buffer;
}

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
	MTL::Buffer* CreateBuffer(size_t size, unsigned int options);
	MTL::Buffer* CreateBufferWithData(const void* data, size_t size, unsigned int options);

	IVertexBuffer* CreateVertexBuffer();
	IIndexBuffer* CreateIndexBuffer();
	IDataBuffer* CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize);

	// Ring buffer allocation (for dynamic data)
	struct RingBufferAllocation
	{
		MTL::Buffer* buffer;
		uint32_t offset;
		void* data;
	};

	RingBufferAllocation AllocateRingBuffer(size_t size);

	// Frame management
	void BeginFrame();
	void EndFrame();

	// Statistics
	size_t GetAllocatedMemory() const { return mAllocatedMemory; }

	std::unique_ptr<MtStreamBuffer> MatrixBuffer;
	std::unique_ptr<MtStreamBuffer> StreamBuffer;

	std::unique_ptr<IIndexBuffer> FanToTrisIndexBuffer;

	MtHardwareDataBuffer* ViewpointUBO = nullptr;
	MtHardwareDataBuffer* LightBufferSSO = nullptr;
	MtHardwareDataBuffer* BoneBufferSSO = nullptr;

private:
	void CreateFanToTrisIndexBuffer();

	MetalRenderDevice* fb = nullptr;
	size_t mAllocatedMemory = 0;

	// Ring buffers
	static const size_t kRingBufferSize = 8 * 1024 * 1024; // 8MB
	std::vector<MTL::Buffer*> mRingBuffers;
	size_t mCurrentRingBuffer = 0;
	size_t mRingBufferOffset = 0;
};
