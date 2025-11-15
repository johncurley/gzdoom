/*
**  Metal backend - Hardware buffers
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_hwbuffer.h"
#include "mt_renderdevice.h"

// MtHardwareDataBuffer
MtHardwareDataBuffer::MtHardwareDataBuffer(MetalRenderDevice* fb, int bindingpoint, bool ssbo, bool needsresize)
	: fb(fb), mBindingPoint(bindingpoint), mSSBO(ssbo), mNeedsResize(needsresize) {}

MtHardwareDataBuffer::~MtHardwareDataBuffer()
{
	if (mBuffer) ((MTL::Buffer*)mBuffer)->release();
}

void MtHardwareDataBuffer::BindRange(FRenderState* state, size_t start, size_t length) {}
void MtHardwareDataBuffer::SetData(size_t size, const void* data, BufferUsageType usage) { CreateBuffer(size); if (mBuffer && data) memcpy(((MTL::Buffer*)mBuffer)->contents(), data, size); }
void MtHardwareDataBuffer::SetSubData(size_t offset, size_t size, const void* data) { if (mBuffer && data) memcpy((uint8_t*)((MTL::Buffer*)mBuffer)->contents() + offset, data, size); }
void MtHardwareDataBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtHardwareDataBuffer::Map() { if (mBuffer) map = ((MTL::Buffer*)mBuffer)->contents(); }
void MtHardwareDataBuffer::Unmap() { map = nullptr; }
void* MtHardwareDataBuffer::Lock(unsigned int size) { Map(); return map; }
void MtHardwareDataBuffer::Unlock() { Unmap(); }

void MtHardwareDataBuffer::CreateBuffer(size_t size)
{
	if (mBuffer) ((MTL::Buffer*)mBuffer)->release();
	mBuffer = fb->device->device->newBuffer(size, MTL::ResourceStorageModeShared);
	mBufferSize = size;
	buffersize = size;  // Set base class member
}

// MtVertexBuffer
MtVertexBuffer::MtVertexBuffer(MetalRenderDevice* fb) : fb(fb) {}
MtVertexBuffer::~MtVertexBuffer() { if (mBuffer) ((MTL::Buffer*)mBuffer)->release(); }

void MtVertexBuffer::SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute* attrs)
{
	mStride = stride;
	mNumBindingPoints = numBindingPoints;

	// Create a simple vertex format ID by combining stride and attribute count
	// This is enough to differentiate between different vertex formats for now
	// TODO: Implement proper vertex format caching like Vulkan
	VertexFormat = (int)stride + (numAttributes << 16);
}
void MtVertexBuffer::SetData(size_t size, const void* data, BufferUsageType usage) { CreateBuffer(size); if (mBuffer && data) memcpy(((MTL::Buffer*)mBuffer)->contents(), data, size); }
void MtVertexBuffer::SetSubData(size_t offset, size_t size, const void* data) { if (mBuffer && data) memcpy((uint8_t*)((MTL::Buffer*)mBuffer)->contents() + offset, data, size); }
void MtVertexBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtVertexBuffer::Map() { if (mBuffer) map = ((MTL::Buffer*)mBuffer)->contents(); }
void MtVertexBuffer::Unmap() { map = nullptr; }
void* MtVertexBuffer::Lock(unsigned int size) { Map(); return map; }
void MtVertexBuffer::Unlock() { Unmap(); }

void MtVertexBuffer::CreateBuffer(size_t size)
{
	if (mBuffer) ((MTL::Buffer*)mBuffer)->release();
	mBuffer = fb->device->device->newBuffer(size, MTL::ResourceStorageModeShared);
	mBufferSize = size;
	buffersize = size;  // Set base class member
}

// MtIndexBuffer
MtIndexBuffer::MtIndexBuffer(MetalRenderDevice* fb) : fb(fb) {}
MtIndexBuffer::~MtIndexBuffer() { if (mBuffer) ((MTL::Buffer*)mBuffer)->release(); }

void MtIndexBuffer::SetData(size_t size, const void* data, BufferUsageType usage) { CreateBuffer(size); if (mBuffer && data) memcpy(((MTL::Buffer*)mBuffer)->contents(), data, size); }
void MtIndexBuffer::SetSubData(size_t offset, size_t size, const void* data) { if (mBuffer && data) memcpy((uint8_t*)((MTL::Buffer*)mBuffer)->contents() + offset, data, size); }
void MtIndexBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtIndexBuffer::Map() { if (mBuffer) map = ((MTL::Buffer*)mBuffer)->contents(); }
void MtIndexBuffer::Unmap() { map = nullptr; }
void* MtIndexBuffer::Lock(unsigned int size) { Map(); return map; }
void MtIndexBuffer::Unlock() { Unmap(); }

void MtIndexBuffer::CreateBuffer(size_t size)
{
	if (mBuffer) ((MTL::Buffer*)mBuffer)->release();
	mBuffer = fb->device->device->newBuffer(size, MTL::ResourceStorageModeShared);
	mBufferSize = size;
	buffersize = size;  // Set base class member
}
