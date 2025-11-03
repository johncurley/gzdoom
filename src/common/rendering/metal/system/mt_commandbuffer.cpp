/*
**  Metal backend
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_commandbuffer.h"
#include "mt_renderdevice.h"

MtCommandBufferManager::MtCommandBufferManager(MetalRenderDevice* fb)
	: fb(fb)
{
}

MtCommandBufferManager::~MtCommandBufferManager()
{
}

MTL::CommandBuffer* MtCommandBufferManager::GetRenderCommandBuffer()
{
	if (!mCurrentCommandBuffer)
	{
		mCurrentCommandBuffer = fb->device->commandQueue->commandBuffer();
		if (!mCurrentCommandBuffer)
		{
			throw CMetalError("Failed to create command buffer");
		}
		mCurrentCommandBuffer->retain(); // Keep alive
	}
	return mCurrentCommandBuffer;
}

MTL::CommandBuffer* MtCommandBufferManager::GetBlitCommandBuffer()
{
	// For now, reuse the same command buffer
	// In the future, we could optimize by using separate blit command encoders
	return GetRenderCommandBuffer();
}

void MtCommandBufferManager::FlushCommands()
{
	if (mCurrentCommandBuffer)
	{
		mCurrentCommandBuffer->commit();
		mCurrentCommandBuffer->release();
		mCurrentCommandBuffer = nullptr;
	}
}

void MtCommandBufferManager::WaitForCommands(bool finish)
{
	if (mCurrentCommandBuffer)
	{
		if (finish)
		{
			mCurrentCommandBuffer->waitUntilCompleted();
		}
		mCurrentCommandBuffer->release();
		mCurrentCommandBuffer = nullptr;
	}
}

void MtCommandBufferManager::BeginFrame()
{
	mFrameIndex++;

	// Create new command buffer for this frame
	if (mCurrentCommandBuffer)
	{
		mCurrentCommandBuffer->release();
	}
	mCurrentCommandBuffer = fb->device->commandQueue->commandBuffer();
	if (mCurrentCommandBuffer)
	{
		mCurrentCommandBuffer->retain();
	}
}

void MtCommandBufferManager::EndFrame()
{
	// Commit the frame's command buffer
	if (mCurrentCommandBuffer)
	{
		mCurrentCommandBuffer->commit();
		// Don't release yet - let the next frame or wait handle it
	}
}

void MtCommandBufferManager::AddCompletedHandler(MTL::CommandBuffer* cmdBuffer, std::function<void()> handler)
{
	if (!cmdBuffer || !handler)
		return;

	// Store handler and attach completion block
	auto handlerPtr = new std::function<void()>(handler);

	cmdBuffer->addCompletedHandler(^(MTL::CommandBuffer* buffer) {
		(*handlerPtr)();
		delete handlerPtr;
	});
}
