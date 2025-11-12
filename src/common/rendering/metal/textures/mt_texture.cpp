/*
**  Metal backend - Texture management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_texture.h"
#include "metal/system/mt_renderdevice.h"

// MtTextureImage
MtTextureImage::MtTextureImage(MetalRenderDevice* fb) : fb(fb) {}

MtTextureImage::~MtTextureImage()
{
	if (mTexture)
	{
		((MTL::Texture*)mTexture)->release();
		mTexture = nullptr;
	}
}

// MtHardwareTexture
MtHardwareTexture::MtHardwareTexture(MetalRenderDevice* fb, int numchannels)
	: fb(fb), mNumChannels(numchannels)
{
	mImage = std::make_unique<MtTextureImage>(fb);
}

MtHardwareTexture::~MtHardwareTexture() {}

void MtHardwareTexture::AllocateBuffer(int w, int h, int texelsize) { mStagingBuffer.resize(w * h * texelsize); }
uint8_t* MtHardwareTexture::MapBuffer() { return mStagingBuffer.data(); }
unsigned int MtHardwareTexture::CreateTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, const char* name) { return 0; }
void MtHardwareTexture::Reset() {}

// MtTextureManager
MtTextureManager::MtTextureManager(MetalRenderDevice* fb) : fb(fb) {}
MtTextureManager::~MtTextureManager() {}

void* MtTextureManager::CreateTexture(int width, int height, int format, int mipmaps)
{
	auto desc = MTL::TextureDescriptor::alloc()->init();
	desc->setWidth(width);
	desc->setHeight(height);
	desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
	desc->setMipmapLevelCount(mipmaps);
	desc->setUsage(MTL::TextureUsageShaderRead);

	MTL::Texture* texture = fb->device->device->newTexture(desc);
	desc->release();
	return texture;
}

void* MtTextureManager::CreateTextureFromData(const void* data, int width, int height, int format, int mipmaps)
{
	void* texture = CreateTexture(width, height, format, mipmaps);
	if (texture && data)
	{
		UpdateTexture(texture, 0, data, width * height * 4);
	}
	return texture;
}

void MtTextureManager::UpdateTexture(void* texture, int level, const void* data, size_t size)
{
	if (!texture || !data) return;

	MTL::Texture* mtlTexture = (MTL::Texture*)texture;
	MTL::Region region = MTL::Region(0, 0, mtlTexture->width(), mtlTexture->height());
	mtlTexture->replaceRegion(region, level, data, mtlTexture->width() * 4);
}
