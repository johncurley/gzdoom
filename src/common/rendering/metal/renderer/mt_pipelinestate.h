#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
#endif

// Pipeline key for caching (similar to Vulkan)
struct MtPipelineKey
{
	int VertexFormat = 0;
	int ShaderKey = 0;
	int BlendMode = 0;
	int DepthFunc = 0;
	int StencilOp = 0;
	int ColorMask = 0;
	int CullMode = 0;
	int DepthClampMode = 0;
	int SampleCount = 1;
	int DrawBufferCount = 1;
	int PixelFormat = 0;
	int DepthStencilFormat = 0;

	bool operator==(const MtPipelineKey& other) const;
	bool operator!=(const MtPipelineKey& other) const { return !(*this == other); }
};

// Hash function for pipeline key caching
namespace std
{
	template<>
	struct hash<MtPipelineKey>
	{
		size_t operator()(const MtPipelineKey& key) const;
	};
}

// Metal pipeline state wrapper
class MtPipelineState
{
public:
#ifdef __OBJC__
	id<MTLRenderPipelineState> pipelineState;
	id<MTLDepthStencilState> depthStencilState;
#else
	void* pipelineState;
	void* depthStencilState;
#endif

	MtPipelineKey Key;
};

// Pipeline state manager
class MtPipelineStateManager
{
public:
	MtPipelineStateManager(class MetalRenderDevice* fb);
	~MtPipelineStateManager();

	// Get or create pipeline state for given key
	MtPipelineState* GetPipelineState(const MtPipelineKey& key);

	// Clear cache
	void ClearCache();

private:
	// Helper methods for pipeline creation
#ifdef __OBJC__
	id<MTLRenderPipelineState> CreateRenderPipelineState(const MtPipelineKey& key);
	id<MTLDepthStencilState> CreateDepthStencilState(const MtPipelineKey& key);
#else
	void* CreateRenderPipelineState(const MtPipelineKey& key);
	void* CreateDepthStencilState(const MtPipelineKey& key);
#endif
	void ConfigureBlendMode(void* colorAttachment, int blendMode);

	class MetalRenderDevice* fb = nullptr;
	std::unordered_map<MtPipelineKey, std::unique_ptr<MtPipelineState>> mPipelineCache;
};
