
#pragma once

#include <zvulkan/vulkanobjects.h>
#include "vulkan/textures/vk_imagetransition.h"
#include <list>

class VulkanRenderDevice;
class VkHardwareTexture;
class VkMaterial;
class VkPPTexture;
class VkTextureImage;
enum class PPTextureType;
class PPTexture;

class VkTextureManager
{
public:
	VkTextureManager(VulkanRenderDevice* fb);
	~VkTextureManager();

	void Deinit();

	void BeginFrame();

	void SetLightmap(int LMTextureSize, int LMTextureCount, const TArray<uint16_t>& LMTextureData);

	VkTextureImage* GetTexture(const PPTextureType& type, PPTexture* tex);
	VkFormat GetTextureFormat(PPTexture* texture);

	// Frame graph resource registry name for a PPTextureType, or nullptr if this
	// type isn't a tracked resource (PPTexture/SwapChain/ShadowMap aren't declared
	// yet). Mirrors GetTexture's dispatch so Touch() calls agree with what
	// VkRenderBuffers actually declared.
	const char *GetTextureResourceName(const PPTextureType& type);

	void AddTexture(VkHardwareTexture* texture);
	void RemoveTexture(VkHardwareTexture* texture);

	void AddPPTexture(VkPPTexture* texture);
	void RemovePPTexture(VkPPTexture* texture);

	VulkanImage* GetNullTexture() { return NullTexture.get(); }
	VulkanImageView* GetNullTextureView() { return NullTextureView.get(); }

	VkTextureImage Shadowmap;
	VkTextureImage Lightmap;

private:
	void CreateNullTexture();
	void CreateShadowmap();
	void CreateLightmap();

	VkPPTexture* GetVkTexture(PPTexture* texture);

	VulkanRenderDevice* fb = nullptr;

	std::list<VkHardwareTexture*> Textures;
	std::list<VkPPTexture*> PPTextures;

	std::unique_ptr<VulkanImage> NullTexture;
	std::unique_ptr<VulkanImageView> NullTextureView;
};
