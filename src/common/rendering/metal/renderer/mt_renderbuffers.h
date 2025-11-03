#pragma once

#include <memory>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

class MetalRenderDevice;
class MtTextureImage;

// Render buffers (framebuffer targets)
class MtRenderBuffers
{
public:
	MtRenderBuffers(MetalRenderDevice* fb);
	~MtRenderBuffers();

	// Get render targets
	MtTextureImage* GetColorBuffer() { return mColorBuffer.get(); }
	MtTextureImage* GetDepthStencilBuffer() { return mDepthStencilBuffer.get(); }

	// Resize buffers
	void Resize(int width, int height);

	// Get dimensions
	int GetWidth() const { return mWidth; }
	int GetHeight() const { return mHeight; }

private:
	MetalRenderDevice* fb = nullptr;
	std::unique_ptr<MtTextureImage> mColorBuffer;
	std::unique_ptr<MtTextureImage> mDepthStencilBuffer;
	int mWidth = 0;
	int mHeight = 0;
};
