/*
**  Metal backend - Stream buffers
**  Copyright (c) 2025 GZDoom Contributors
*/

#include "mt_streambuffer.h"
#include "metal/system/mt_renderdevice.h"
#include "i_system.h"
#include <Metal/Metal.hpp>

MtStreamBuffer::MtStreamBuffer(MetalRenderDevice* fb, size_t structSize)
	: mBlockSize(static_cast<uint32_t>(structSize)), fb(fb)
{
	// Allocate 8MB ring buffer (following Vulkan pattern)
	mBufferSize = 8 * 1024 * 1024;

	// Create MTLBuffer with MTLResourceStorageModeShared for CPU-GPU shared memory
	// This allows direct CPU writes without explicit synchronization
	mBuffer = (void*)fb->device->device->newBuffer(mBufferSize, MTL::ResourceStorageModeShared);

	if (!mBuffer)
	{
		I_FatalError("MtStreamBuffer: Failed to allocate Metal buffer of size %zu bytes", mBufferSize);
	}
}

MtStreamBuffer::~MtStreamBuffer()
{
	// Release Metal buffer
	if (mBuffer)
	{
		((MTL::Buffer*)mBuffer)->release();
		mBuffer = nullptr;
	}
}

uint32_t MtStreamBuffer::NextStreamDataBlock()
{
	mStreamDataOffset += mBlockSize;
	if (mStreamDataOffset + mBlockSize >= mBufferSize)
	{
		mStreamDataOffset = 0;
		return 0xffffffff; // Buffer full, need to wait
	}
	return mStreamDataOffset;
}

uint8_t* MtStreamBuffer::GetBufferPointer() const
{
	if (!mBuffer)
		return nullptr;

	// Metal buffers with StorageModeShared are CPU-accessible
	// contents() returns a pointer to the buffer's CPU-side memory
	auto buffer = (MTL::Buffer*)mBuffer;
	return (uint8_t*)buffer->contents();
}
