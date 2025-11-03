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
void MtHardwareDataBuffer::BindBase() {}
void MtHardwareDataBuffer::SetData(size_t size, const void* data, BufferUsageType usage) { CreateBuffer(size); if (mBuffer && data) memcpy(((MTL::Buffer*)mBuffer)->contents(), data, size); }
void MtHardwareDataBuffer::SetSubData(size_t offset, size_t size, const void* data) { if (mBuffer && data) memcpy((uint8_t*)((MTL::Buffer*)mBuffer)->contents() + offset, data, size); }
void MtHardwareDataBuffer::Resize(size_t newsize) { CreateBuffer(newsize); }
void MtHardwareDataBuffer::Map() { if (mBuffer) mMappedMemory = ((MTL::Buffer*)mBuffer)->contents(); }
void MtHardwareDataBuffer::Unmap() { mMappedMemory = nullptr; }

void MtHardwareDataBuffer::CreateBuffer(size_t size)
{
	if (mBuffer) ((MTL::Buffer*)mBuffer)->release();
	mBuffer = fb->device->device->newBuffer(size, MTL::ResourceStorageModeShared);
	mBufferSize = size;
}

// MtVertexBuffer
MtVertexBuffer::MtVertexBuffer(MetalRenderDevice* fb) : fb(fb) {}
MtVertexBuffer::~MtVertexBuffer() { if (mBuffer) ((MTL::Buffer*)mBuffer)->release(); }

void MtVertexBuffer::SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute* attrs) {}
void MtVertexBuffer::SetData(size_t size, const void* data, BufferUsageType usage) { CreateBuffer(size); if (mBuffer && data) memcpy(((MTL::Buffer*)mBuffer)->contents(), data, size); }
void MtVertexBuffer::SetSubData(size_t offset, size_t size, const void* data) { if (mBuffer && data) memcpy((uint8_t*)((MTL::Buffer*)mBuffer)->contents() + offset, data, size); }
void MtVertexBuffer::Map() { if (mBuffer) mMappedMemory = ((MTL::Buffer*)mBuffer)->contents(); }
void MtVertexBuffer::Unmap() { mMappedMemory = nullptr; }

void MtVertexBuffer::CreateBuffer(size_t size)
{
	if (mBuffer) ((MTL::Buffer*)mBuffer)->release();
	mBuffer = fb->device->device->newBuffer(size, MTL::ResourceStorageModeShared);
	mBufferSize = size;
}

// MtIndexBuffer
MtIndexBuffer::MtIndexBuffer(MetalRenderDevice* fb) : fb(fb) {}
MtIndexBuffer::~MtIndexBuffer() { if (mBuffer) ((MTL::Buffer*)mBuffer)->release(); }

void MtIndexBuffer::SetData(size_t size, const void* data, BufferUsageType usage) { CreateBuffer(size); if (mBuffer && data) memcpy(((MTL::Buffer*)mBuffer)->contents(), data, size); }
void MtIndexBuffer::SetSubData(size_t offset, size_t size, const void* data) { if (mBuffer && data) memcpy((uint8_t*)((MTL::Buffer*)mBuffer)->contents() + offset, data, size); }
void MtIndexBuffer::Map() { if (mBuffer) mMappedMemory = ((MTL::Buffer*)mBuffer)->contents(); }
void MtIndexBuffer::Unmap() { mMappedMemory = nullptr; }

void MtIndexBuffer::CreateBuffer(size_t size)
{
	if (mBuffer) ((MTL::Buffer*)mBuffer)->release();
	mBuffer = fb->device->device->newBuffer(size, MTL::ResourceStorageModeShared);
	mBufferSize = size;
}
