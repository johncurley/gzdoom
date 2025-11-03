/*
**  Metal backend - Render buffers
**  Copyright (c) 2025 GZDoom Contributors
*/

#include "mt_renderbuffers.h"
#include "mt_texture.h"
#include "mt_renderdevice.h"

MtRenderBuffers::MtRenderBuffers(MetalRenderDevice* fb) : fb(fb) {}
MtRenderBuffers::~MtRenderBuffers() {}

void MtRenderBuffers::Resize(int width, int height)
{
	mWidth = width;
	mHeight = height;
	// TODO: Recreate textures
}
