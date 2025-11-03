#pragma once

#include "hw_texcontainer.h"
#include <memory>
#include <vector>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

class MetalRenderDevice;
class FGameTexture;

// Metal texture image
class MtTextureImage
{
public:
	MtTextureImage(MetalRenderDevice* fb);
	~MtTextureImage();

#ifdef __OBJC__
	id<MTLTexture> GetTexture() const { return mTexture; }
	void SetTexture(id<MTLTexture> texture) { mTexture = texture; }
#else
	void* GetTexture() const { return mTexture; }
	void SetTexture(void* texture) { mTexture = texture; }
#endif

	int GetWidth() const { return mWidth; }
	int GetHeight() const { return mHeight; }
	int GetFormat() const { return mFormat; }

private:
#ifdef __OBJC__
	id<MTLTexture> mTexture = nil;
#else
	void* mTexture = nullptr;
#endif
	int mWidth = 0;
	int mHeight = 0;
	int mFormat = 0;
	MetalRenderDevice* fb = nullptr;
};

// Metal hardware texture
class MtHardwareTexture : public IHardwareTexture
{
public:
	MtHardwareTexture(MetalRenderDevice* fb, int numchannels);
	~MtHardwareTexture();

	void CreateTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, const char* name) override;
	void AllocateBuffer(int w, int h, int texelsize) override;
	uint8_t* MapBuffer() override;

	unsigned int CreateTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, int translation, const char* name) override;
	void Reset() override;

	MtTextureImage* GetImage() { return mImage.get(); }

private:
	std::unique_ptr<MtTextureImage> mImage;
	MetalRenderDevice* fb = nullptr;
	int mNumChannels = 0;
	std::vector<uint8_t> mStagingBuffer;
};

// Texture manager
class MtTextureManager
{
public:
	MtTextureManager(MetalRenderDevice* fb);
	~MtTextureManager();

	// Create textures
#ifdef __OBJC__
	id<MTLTexture> CreateTexture(int width, int height, int format, int mipmaps);
	id<MTLTexture> CreateTextureFromData(const void* data, int width, int height, int format, int mipmaps);
#else
	void* CreateTexture(int width, int height, int format, int mipmaps);
	void* CreateTextureFromData(const void* data, int width, int height, int format, int mipmaps);
#endif

	// Update texture data
#ifdef __OBJC__
	void UpdateTexture(id<MTLTexture> texture, int level, const void* data, size_t size);
#else
	void UpdateTexture(void* texture, int level, const void* data, size_t size);
#endif

private:
	MetalRenderDevice* fb = nullptr;
};
