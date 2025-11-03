#pragma once

#include <memory>
#include <vector>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

class MetalRenderDevice;

// Three-tier resource binding strategy (adapted from Vulkan)
// Tier 0: Fixed textures (shadowmap, lightmap, RTX)
// Tier 1: Per-frame buffers (viewpoint, matrices, stream data, lights, bones)
// Tier 2: Per-material textures (texture layers)

class MtResourceBindingManager
{
public:
	MtResourceBindingManager(MetalRenderDevice* fb);
	~MtResourceBindingManager();

	// Tier 0: Fixed textures (bind once at init)
#ifdef __OBJC__
	void BindFixedTexture(int slot, id<MTLTexture> texture);
#else
	void BindFixedTexture(int slot, void* texture);
#endif

	// Tier 1: Per-frame buffers (update offsets per-frame)
#ifdef __OBJC__
	void BindPerFrameBuffer(int slot, id<MTLBuffer> buffer, uint32_t offset);
#else
	void BindPerFrameBuffer(int slot, void* buffer, uint32_t offset);
#endif

	// Tier 2: Per-material textures (bind per-material)
#ifdef __OBJC__
	void BindMaterialTexture(int slot, id<MTLTexture> texture, id<MTLSamplerState> sampler);
#else
	void BindMaterialTexture(int slot, void* texture, void* sampler);
#endif

	// Apply bindings to encoder
#ifdef __OBJC__
	void ApplyBindings(id<MTLRenderCommandEncoder> encoder, bool vertex, bool fragment);
#else
	void ApplyBindings(void* encoder, bool vertex, bool fragment);
#endif

	// Reset for new frame
	void BeginFrame();

private:
	MetalRenderDevice* fb = nullptr;

	// Tier 0: Fixed textures
#ifdef __OBJC__
	std::vector<id<MTLTexture>> mFixedTextures;
#else
	std::vector<void*> mFixedTextures;
#endif

	// Tier 1: Per-frame buffers
	struct BufferBinding
	{
#ifdef __OBJC__
		id<MTLBuffer> buffer = nil;
#else
		void* buffer = nullptr;
#endif
		uint32_t offset = 0;
	};
	std::vector<BufferBinding> mPerFrameBuffers;

	// Tier 2: Per-material textures
	struct TextureBinding
	{
#ifdef __OBJC__
		id<MTLTexture> texture = nil;
		id<MTLSamplerState> sampler = nil;
#else
		void* texture = nullptr;
		void* sampler = nullptr;
#endif
	};
	std::vector<TextureBinding> mMaterialTextures;
};
