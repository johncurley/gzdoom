/*
**  Metal backend - Texture management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_texture.h"
#include "mt_renderdevice.h"

// MtTextureImage
MtTextureImage::MtTextureImage(MetalRenderDevice* fb) : fb(fb) {}
MtTextureImage::~MtTextureImage() { if (mTexture) mTexture->release(); }

// MtHardwareTexture
MtHardwareTexture::MtHardwareTexture(MetalRenderDevice* fb, int numchannels)
	: fb(fb), mNumChannels(numchannels)
{
	mImage = std::make_unique<MtTextureImage>(fb);
}

MtHardwareTexture::~MtHardwareTexture() {}

void MtHardwareTexture::CreateTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, const char* name) {}
void MtHardwareTexture::AllocateBuffer(int w, int h, int texelsize) { mStagingBuffer.resize(w * h * texelsize); }
uint8_t* MtHardwareTexture::MapBuffer() { return mStagingBuffer.data(); }
unsigned int MtHardwareTexture::CreateTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, int translation, const char* name) { return 0; }
void MtHardwareTexture::Reset() {}

// MtTextureManager
MtTextureManager::MtTextureManager(MetalRenderDevice* fb) : fb(fb) {}
MtTextureManager::~MtTextureManager() {}

MTL::Texture* MtTextureManager::CreateTexture(int width, int height, int format, int mipmaps)
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

MTL::Texture* MtTextureManager::CreateTextureFromData(const void* data, int width, int height, int format, int mipmaps)
{
	MTL::Texture* texture = CreateTexture(width, height, format, mipmaps);
	if (texture && data)
	{
		UpdateTexture(texture, 0, data, width * height * 4);
	}
	return texture;
}

void MtTextureManager::UpdateTexture(MTL::Texture* texture, int level, const void* data, size_t size)
{
	if (!texture || !data) return;

	MTL::Region region = MTL::Region(0, 0, texture->width(), texture->height());
	texture->replaceRegion(region, level, data, texture->width() * 4);
}
