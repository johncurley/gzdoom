/*
**  Metal backend - Resource binding management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_resourcebinding.h"
#include "mt_renderdevice.h"

MtResourceBindingManager::MtResourceBindingManager(MetalRenderDevice* fb) : fb(fb) {}
MtResourceBindingManager::~MtResourceBindingManager() {}

void MtResourceBindingManager::BindFixedTexture(int slot, MTL::Texture* texture)
{
	if (slot >= mFixedTextures.size())
		mFixedTextures.resize(slot + 1);
	mFixedTextures[slot] = texture;
}

void MtResourceBindingManager::BindPerFrameBuffer(int slot, MTL::Buffer* buffer, uint32_t offset)
{
	if (slot >= mPerFrameBuffers.size())
		mPerFrameBuffers.resize(slot + 1);
	mPerFrameBuffers[slot].buffer = buffer;
	mPerFrameBuffers[slot].offset = offset;
}

void MtResourceBindingManager::BindMaterialTexture(int slot, MTL::Texture* texture, MTL::SamplerState* sampler)
{
	if (slot >= mMaterialTextures.size())
		mMaterialTextures.resize(slot + 1);
	mMaterialTextures[slot].texture = texture;
	mMaterialTextures[slot].sampler = sampler;
}

void MtResourceBindingManager::ApplyBindings(MTL::RenderCommandEncoder* encoder, bool vertex, bool fragment)
{
	// TODO: Apply bindings to encoder
}

void MtResourceBindingManager::BeginFrame()
{
	// Reset per-frame state
}
