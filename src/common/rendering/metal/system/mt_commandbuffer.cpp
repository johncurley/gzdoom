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

void* MtCommandBufferManager::GetRenderCommandBuffer()
{
	if (!mCurrentCommandBuffer)
	{
		MTL::CommandBuffer* cmdBuf = fb->device->commandQueue->commandBuffer();
		if (!cmdBuf)
		{
			throw CMetalError("Failed to create command buffer");
		}
		cmdBuf->retain(); // Keep alive
		mCurrentCommandBuffer = cmdBuf;
	}
	return mCurrentCommandBuffer;
}

void* MtCommandBufferManager::GetBlitCommandBuffer()
{
	// For now, reuse the same command buffer
	// In the future, we could optimize by using separate blit command encoders
	return GetRenderCommandBuffer();
}

void MtCommandBufferManager::FlushCommands()
{
	if (mCurrentCommandBuffer)
	{
		MTL::CommandBuffer* cmdBuf = (MTL::CommandBuffer*)mCurrentCommandBuffer;
		cmdBuf->commit();
		cmdBuf->release();
		mCurrentCommandBuffer = nullptr;
	}
}

void MtCommandBufferManager::WaitForCommands(bool finish)
{
	if (mCurrentCommandBuffer)
	{
		MTL::CommandBuffer* cmdBuf = (MTL::CommandBuffer*)mCurrentCommandBuffer;
		if (finish)
		{
			cmdBuf->waitUntilCompleted();
		}
		cmdBuf->release();
		mCurrentCommandBuffer = nullptr;
	}
}

void MtCommandBufferManager::BeginFrame()
{
	mFrameIndex++;

	// Create new command buffer for this frame
	if (mCurrentCommandBuffer)
	{
		((MTL::CommandBuffer*)mCurrentCommandBuffer)->release();
	}
	MTL::CommandBuffer* cmdBuf = fb->device->commandQueue->commandBuffer();
	if (cmdBuf)
	{
		cmdBuf->retain();
		mCurrentCommandBuffer = cmdBuf;
	}
	else
	{
		mCurrentCommandBuffer = nullptr;
	}
}

void MtCommandBufferManager::EndFrame()
{
	// Commit the frame's command buffer
	if (mCurrentCommandBuffer)
	{
		((MTL::CommandBuffer*)mCurrentCommandBuffer)->commit();
		// Don't release yet - let the next frame or wait handle it
	}
}

void MtCommandBufferManager::AddCompletedHandler(void* cmdBuffer, std::function<void()> handler)
{
	if (!cmdBuffer || !handler)
		return;

	// Store handler and attach completion block
	auto handlerPtr = new std::function<void()>(handler);
	MTL::CommandBuffer* mtlCmdBuf = (MTL::CommandBuffer*)cmdBuffer;

	mtlCmdBuf->addCompletedHandler(^(MTL::CommandBuffer* buffer) {
		(*handlerPtr)();
		delete handlerPtr;
	});
}
