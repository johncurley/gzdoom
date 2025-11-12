/*
**  Metal backend - Resource binding management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_resourcebinding.h"
#include "metal/system/mt_renderdevice.h"

MtResourceBindingManager::MtResourceBindingManager(MetalRenderDevice* fb) : fb(fb) {}
MtResourceBindingManager::~MtResourceBindingManager() {}

void MtResourceBindingManager::BindFixedTexture(int slot, void* texture)
{
	if (slot >= (int)mFixedTextures.size())
		mFixedTextures.resize(slot + 1);
	mFixedTextures[slot] = texture;
}

void MtResourceBindingManager::BindPerFrameBuffer(int slot, void* buffer, uint32_t offset)
{
	if (slot >= (int)mPerFrameBuffers.size())
		mPerFrameBuffers.resize(slot + 1);
	mPerFrameBuffers[slot].buffer = buffer;
	mPerFrameBuffers[slot].offset = offset;
}

void MtResourceBindingManager::BindMaterialTexture(int slot, void* texture, void* sampler)
{
	if (slot >= (int)mMaterialTextures.size())
		mMaterialTextures.resize(slot + 1);
	mMaterialTextures[slot].texture = texture;
	mMaterialTextures[slot].sampler = sampler;
}

void MtResourceBindingManager::ApplyBindings(void* encoder, bool vertex, bool fragment)
{
	auto mtlEncoder = (MTL::RenderCommandEncoder*)encoder;
	if (!mtlEncoder) return;

	// Tier 0: Bind fixed textures (shadowmaps, lightmaps, etc.)
	// These are typically bound once and rarely change
	for (size_t i = 0; i < mFixedTextures.size(); i++)
	{
		if (mFixedTextures[i])
		{
			auto texture = (MTL::Texture*)mFixedTextures[i];
			if (vertex)
				mtlEncoder->setVertexTexture(texture, i);
			if (fragment)
				mtlEncoder->setFragmentTexture(texture, i);
		}
	}

	// Tier 1: Bind per-frame buffers (viewpoint, matrices, stream data, lights, bones)
	// These update every frame but use dynamic offsets for efficiency
	for (size_t i = 0; i < mPerFrameBuffers.size(); i++)
	{
		if (mPerFrameBuffers[i].buffer)
		{
			auto buffer = (MTL::Buffer*)mPerFrameBuffers[i].buffer;
			NS::UInteger offset = mPerFrameBuffers[i].offset;

			if (vertex)
				mtlEncoder->setVertexBuffer(buffer, offset, i);
			if (fragment)
				mtlEncoder->setFragmentBuffer(buffer, offset, i);
		}
	}

	// Tier 2: Bind per-material textures (texture layers)
	// These change per-material during rendering
	for (size_t i = 0; i < mMaterialTextures.size(); i++)
	{
		if (mMaterialTextures[i].texture)
		{
			auto texture = (MTL::Texture*)mMaterialTextures[i].texture;
			auto sampler = (MTL::SamplerState*)mMaterialTextures[i].sampler;

			if (vertex)
			{
				mtlEncoder->setVertexTexture(texture, i);
				if (sampler)
					mtlEncoder->setVertexSamplerState(sampler, i);
			}
			if (fragment)
			{
				mtlEncoder->setFragmentTexture(texture, i);
				if (sampler)
					mtlEncoder->setFragmentSamplerState(sampler, i);
			}
		}
	}
}

void MtResourceBindingManager::BeginFrame()
{
	// Clear material textures (Tier 2) - these are rebound every draw
	// Keep fixed textures (Tier 0) and per-frame buffers (Tier 1) intact
	mMaterialTextures.clear();
}

void MtResourceBindingManager::ClearMaterialTextures()
{
	// Clear all material texture bindings (called when switching materials)
	mMaterialTextures.clear();
}
