/*
**  Metal backend - Buffer management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_buffer.h"
#include "mt_renderdevice.h"

MtBufferManager::MtBufferManager(MetalRenderDevice* fb)
	: fb(fb)
{
	// Pre-allocate ring buffers
	for (int i = 0; i < 3; i++) // Triple buffering
	{
		MTL::Buffer* ringBuffer = fb->device->device->newBuffer(
			kRingBufferSize,
			MTL::ResourceStorageModeShared
		);
		if (ringBuffer)
		{
			mRingBuffers.push_back(ringBuffer);
			mAllocatedMemory += kRingBufferSize;
		}
	}
}

MtBufferManager::~MtBufferManager()
{
	// Release all ring buffers
	for (auto buffer : mRingBuffers)
	{
		if (buffer)
			buffer->release();
	}
	mRingBuffers.clear();
}

MTL::Buffer* MtBufferManager::CreateBuffer(size_t size, MTL::ResourceOptions options)
{
	MTL::Buffer* buffer = fb->device->device->newBuffer(size, options);
	if (buffer)
	{
		mAllocatedMemory += size;
	}
	return buffer;
}

MTL::Buffer* MtBufferManager::CreateBufferWithData(const void* data, size_t size, MTL::ResourceOptions options)
{
	MTL::Buffer* buffer = fb->device->device->newBuffer(data, size, options);
	if (buffer)
	{
		mAllocatedMemory += size;
	}
	return buffer;
}

MtBufferManager::RingBufferAllocation MtBufferManager::AllocateRingBuffer(size_t size)
{
	RingBufferAllocation alloc;

	if (mRingBuffers.empty())
	{
		alloc.buffer = nullptr;
		alloc.offset = 0;
		alloc.data = nullptr;
		return alloc;
	}

	// Get current ring buffer
	MTL::Buffer* buffer = mRingBuffers[mCurrentRingBuffer];

	// Check if we need to wrap around
	if (mRingBufferOffset + size > kRingBufferSize)
	{
		// Move to next ring buffer
		mCurrentRingBuffer = (mCurrentRingBuffer + 1) % mRingBuffers.size();
		mRingBufferOffset = 0;
		buffer = mRingBuffers[mCurrentRingBuffer];
	}

	// Allocate from current position
	alloc.buffer = buffer;
	alloc.offset = (uint32_t)mRingBufferOffset;
	alloc.data = (uint8_t*)buffer->contents() + mRingBufferOffset;

	// Advance offset (with alignment)
	size_t alignedSize = (size + 255) & ~255; // 256-byte alignment
	mRingBufferOffset += alignedSize;

	return alloc;
}

void MtBufferManager::BeginFrame()
{
	// Ring buffer wraps automatically in AllocateRingBuffer
}

void MtBufferManager::EndFrame()
{
	// Nothing needed for now
}
