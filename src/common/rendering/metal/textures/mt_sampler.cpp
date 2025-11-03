/*
**  Metal backend - Sampler management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_sampler.h"
#include "mt_renderdevice.h"

bool MtSamplerKey::operator==(const MtSamplerKey& other) const
{
	return MinFilter == other.MinFilter &&
		MagFilter == other.MagFilter &&
		MipFilter == other.MipFilter &&
		AddressU == other.AddressU &&
		AddressV == other.AddressV &&
		AddressW == other.AddressW &&
		MaxAnisotropy == other.MaxAnisotropy;
}

size_t std::hash<MtSamplerKey>::operator()(const MtSamplerKey& key) const
{
	size_t hash = 0;
	hash ^= std::hash<int>()(key.MinFilter);
	hash ^= std::hash<int>()(key.MagFilter) << 1;
	hash ^= std::hash<int>()(key.MipFilter) << 2;
	hash ^= std::hash<int>()(key.AddressU) << 3;
	hash ^= std::hash<int>()(key.AddressV) << 4;
	hash ^= std::hash<int>()(key.AddressW) << 5;
	hash ^= std::hash<float>()(key.MaxAnisotropy);
	return hash;
}

MtSamplerManager::MtSamplerManager(MetalRenderDevice* fb)
	: fb(fb)
{
}

MtSamplerManager::~MtSamplerManager()
{
	ClearCache();
}

MTL::SamplerState* MtSamplerManager::GetSamplerState(const MtSamplerKey& key)
{
	// Check cache
	auto it = mSamplerCache.find(key);
	if (it != mSamplerCache.end())
	{
		return it->second;
	}

	// Create new sampler state
	auto desc = MTL::SamplerDescriptor::alloc()->init();

	// Set filter modes (stub - proper mapping needed)
	desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
	desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
	desc->setMipFilter(MTL::SamplerMipFilterLinear);

	// Set address modes (stub - proper mapping needed)
	desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
	desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
	desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);

	// Set anisotropy
	desc->setMaxAnisotropy(static_cast<NS::UInteger>(key.MaxAnisotropy));

	MTL::SamplerState* samplerState = fb->device->device->newSamplerState(desc);
	desc->release();

	if (samplerState)
	{
		mSamplerCache[key] = samplerState;
	}

	return samplerState;
}

void MtSamplerManager::ClearCache()
{
	for (auto& pair : mSamplerCache)
	{
		if (pair.second)
			pair.second->release();
	}
	mSamplerCache.clear();
}
