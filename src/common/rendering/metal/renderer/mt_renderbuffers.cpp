/*
**  Metal backend - Render buffers
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_renderbuffers.h"
#include "metal/textures/mt_texture.h"
#include "metal/system/mt_renderdevice.h"

MtRenderBuffers::MtRenderBuffers(MetalRenderDevice* fb) : fb(fb) {}

MtRenderBuffers::~MtRenderBuffers()
{
	// unique_ptr will clean up automatically
}

void MtRenderBuffers::Resize(int width, int height)
{
	// Skip if same size
	if (mWidth == width && mHeight == height && mDepthStencilBuffer)
		return;

	mWidth = width;
	mHeight = height;

	// Create depth/stencil buffer
	CreateDepthStencilBuffer(width, height);
}

void MtRenderBuffers::CreateDepthStencilBuffer(int width, int height)
{
	// Release old buffer if it exists
	mDepthStencilBuffer.reset();

	// Create new depth/stencil texture
	mDepthStencilBuffer = std::make_unique<MtTextureImage>(fb);

	// Create Metal texture descriptor
	auto desc = MTL::TextureDescriptor::alloc()->init();
	desc->setWidth(width);
	desc->setHeight(height);
	desc->setPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);  // 32-bit depth + 8-bit stencil
	desc->setUsage(MTL::TextureUsageRenderTarget);  // Used as render target
	desc->setStorageMode(MTL::StorageModePrivate);  // GPU-only memory (fastest)

	// Create the texture
	MTL::Texture* texture = fb->device->device->newTexture(desc);
	desc->release();

	if (!texture)
	{
		throw CMetalError("Failed to create depth/stencil buffer");
	}

	// Store in MtTextureImage
	mDepthStencilBuffer->SetTexture(texture);
	mDepthStencilBuffer->SetWidth(width);
	mDepthStencilBuffer->SetHeight(height);
	mDepthStencilBuffer->SetFormat((int)MTL::PixelFormatDepth32Float_Stencil8);
}
