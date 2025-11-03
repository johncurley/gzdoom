#pragma once

#include <memory>
#include <unordered_map>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

class MetalRenderDevice;

// Sampler state key for caching
struct MtSamplerKey
{
	int MinFilter = 0;
	int MagFilter = 0;
	int MipFilter = 0;
	int AddressU = 0;
	int AddressV = 0;
	int AddressW = 0;
	float MaxAnisotropy = 1.0f;

	bool operator==(const MtSamplerKey& other) const;
	bool operator!=(const MtSamplerKey& other) const { return !(*this == other); }
};

// Hash function for sampler key
namespace std
{
	template<>
	struct hash<MtSamplerKey>
	{
		size_t operator()(const MtSamplerKey& key) const;
	};
}

// Sampler manager
class MtSamplerManager
{
public:
	MtSamplerManager(MetalRenderDevice* fb);
	~MtSamplerManager();

	// Get or create sampler state
#ifdef __OBJC__
	id<MTLSamplerState> GetSamplerState(const MtSamplerKey& key);
#else
	void* GetSamplerState(const MtSamplerKey& key);
#endif

	// Clear cache
	void ClearCache();

private:
	MetalRenderDevice* fb = nullptr;

#ifdef __OBJC__
	std::unordered_map<MtSamplerKey, id<MTLSamplerState>> mSamplerCache;
#else
	std::unordered_map<MtSamplerKey, void*> mSamplerCache;
#endif
};
