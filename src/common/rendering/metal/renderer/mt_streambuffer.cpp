/*
**  Metal backend - Stream buffers
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_streambuffer.h"
#include "mt_renderdevice.h"

MtStreamBuffer::MtStreamBuffer(MetalRenderDevice* fb, size_t size)
	: fb(fb), mBufferSize(size)
{
	mBuffer = fb->device->device->newBuffer(size, MTL::ResourceStorageModeShared);
}

MtStreamBuffer::~MtStreamBuffer()
{
	if (mBuffer) ((MTL::Buffer*)mBuffer)->release();
}

uint32_t MtStreamBuffer::NextStreamDataBlock()
{
	uint32_t offset = mStreamDataOffset;

	// Advance with alignment
	mStreamDataOffset += 256; // 256-byte alignment

	// Wrap around if needed
	if (mStreamDataOffset >= mBufferSize)
	{
		mStreamDataOffset = 0;
	}

	return offset;
}

void MtStreamBuffer::Reset()
{
	mStreamDataOffset = 0;
}
