#pragma once

#include <memory>
#include <functional>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
#endif

class MetalRenderDevice;

// Command buffer manager
class MtCommandBufferManager
{
public:
	MtCommandBufferManager(MetalRenderDevice* fb);
	~MtCommandBufferManager();

	// Get command buffers for different operations
#ifdef __OBJC__
	id<MTLCommandBuffer> GetRenderCommandBuffer();
	id<MTLCommandBuffer> GetBlitCommandBuffer();
	id<MTLCommandBuffer> GetCurrentCommandBuffer() { return mCurrentCommandBuffer; }
#else
	void* GetRenderCommandBuffer();
	void* GetBlitCommandBuffer();
	void* GetCurrentCommandBuffer() { return mCurrentCommandBuffer; }
#endif

	// Submit and wait
	void FlushCommands();
	void WaitForCommands(bool finish);

	// Frame synchronization
	void BeginFrame();
	void EndFrame();

	// Add completion handler
#ifdef __OBJC__
	void AddCompletedHandler(id<MTLCommandBuffer> cmdBuffer, std::function<void()> handler);
#else
	void AddCompletedHandler(void* cmdBuffer, std::function<void()> handler);
#endif

private:
	MetalRenderDevice* fb = nullptr;

#ifdef __OBJC__
	id<MTLCommandBuffer> mCurrentCommandBuffer = nil;
#else
	void* mCurrentCommandBuffer = nullptr;
#endif

	int mFrameIndex = 0;
};
