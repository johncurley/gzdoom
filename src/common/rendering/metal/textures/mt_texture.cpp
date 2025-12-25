#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/system/mt_renderdevice.h"
#include "mt_texture.h"
#include "printf.h"

#include "textures.h"
#include <cmath>

// MtTextureImage
MtTextureImage::MtTextureImage(MetalRenderDevice *fb) : fb(fb) {}

MtTextureImage::~MtTextureImage() {
  if (mTexture) {
    fb->RecycleTexture(mTexture);
    mTexture = nullptr;
  }
}

// MtHardwareTexture
MtHardwareTexture::MtHardwareTexture(MetalRenderDevice *fb, int numchannels)
    : fb(fb), mNumChannels(numchannels) {
  mImage = std::make_unique<MtTextureImage>(fb);
}

MtHardwareTexture::~MtHardwareTexture() {}

void MtHardwareTexture::AllocateBuffer(int w, int h, int texelsize) {
  static int allocCount = 0;
  if (allocCount++ < 10)
    Printf("Metal: AllocateBuffer called: %dx%d, texelsize=%d\n", w, h,
           texelsize);

  // Check if we need to recreate the texture
  if (mImage->GetTexture() &&
      (mImage->GetWidth() != w || mImage->GetHeight() != h)) {
    Reset();
  }

  // Create the Metal texture if it doesn't exist
  if (!mImage->GetTexture()) {
    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(w);
    desc->setHeight(h);

    // Determine pixel format based on texelsize
    MTL::PixelFormat format;
    switch (texelsize) {
    case 1:
      format = MTL::PixelFormatR8Unorm;
      break;
    case 2:
      format = MTL::PixelFormatRG8Unorm;
      break;
    case 3:
    case 4:
    default:
      format = MTL::PixelFormatBGRA8Unorm;
      break;
    }

    desc->setPixelFormat(format);
    desc->setMipmapLevelCount(1); // No mipmaps for AllocateBuffer textures
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared); // Shared for CPU access

    // Create the texture
    MTL::Texture *texture = fb->device->device->newTexture(desc);
    desc->release();

    if (!texture) {
      Printf("Metal: Failed to allocate texture %dx%d\n", w, h);
      return;
    }

    // Store in image
    mImage->SetTexture(texture);
    mImage->SetWidth(w);
    mImage->SetHeight(h);
    mImage->SetFormat((int)format);

    if (allocCount <= 10)
      Printf("Metal: Texture allocated successfully: %p (%dx%d)\n", texture, w,
             h);
  }

  // Allocate staging buffer for CPU writes
  mStagingBuffer.resize(w * h * texelsize);
}

uint8_t *MtHardwareTexture::MapBuffer() {
  // For Metal, we use shared storage mode which allows CPU access
  // So we return the staging buffer that will be uploaded later
  if (mStagingBuffer.empty()) {
    Printf("Metal: Warning - MapBuffer called but staging buffer is empty\n");
    return nullptr;
  }

  static int mapCount = 0;
  if (mapCount++ < 5)
    Printf("Metal: MapBuffer called, returning %zu byte buffer\n",
           mStagingBuffer.size());

  return mStagingBuffer.data();
}

unsigned int MtHardwareTexture::CreateTexture(unsigned char *buffer, int w,
                                              int h, int texunit, bool mipmap,
                                              const char *name) {
  static int texCount = 0;
  if (texCount++ < 10)
    Printf("Metal: CreateTexture called: %dx%d, mipmap=%d, name=%s\n", w, h,
           mipmap, name ? name : "null");

  // Create Metal texture descriptor
  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(w);
  desc->setHeight(h);

  // Determine pixel format based on channel count
  switch (mNumChannels) {
  case 1:
    desc->setPixelFormat(MTL::PixelFormatR8Unorm);
    break;
  case 2:
    desc->setPixelFormat(MTL::PixelFormatRG8Unorm);
    break;
  case 3:
  case 4:
  default:
    desc->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    break;
  }

  desc->setMipmapLevelCount(
      mipmap ? 1 + (int)std::floor(std::log2(std::max(w, h))) : 1);
  desc->setUsage(MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModeShared);

  // Create the texture
  MTL::Texture *texture = fb->device->device->newTexture(desc);
  desc->release();

  if (!texture) {
    Printf("Metal: Failed to create texture %dx%d\n", w, h);
    return 0;
  }

  // Upload data if provided
  if (buffer) {
    MTL::Region region = MTL::Region::Make2D(0, 0, w, h);
    int bytesPerPixel = mNumChannels;
    texture->replaceRegion(region, 0, buffer, w * bytesPerPixel);
  }

  // Store in image
  mImage->SetTexture(texture);
  mImage->SetWidth(w);
  mImage->SetHeight(h);
  mImage->SetFormat((int)desc->pixelFormat());

  if (texCount <= 10)
    Printf("Metal: Texture created successfully: %p\n", texture);

  return 1; // Success
}

void MtHardwareTexture::CreateWipeTexture(int w, int h, const char *name) {
  Reset();

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(w);
  desc->setHeight(h);
  desc->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
  desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
  desc->setStorageMode(MTL::StorageModePrivate);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  desc->release();

  mImage->SetTexture(texture);
  mImage->SetWidth(w);
  mImage->SetHeight(h);
  mImage->SetFormat((int)MTL::PixelFormatRGBA8Unorm);

  fb->GetPostprocess()->BlitCurrentToImage(texture);
}

MtTextureImage *MtHardwareTexture::GetDepthStencil(FCanvasTexture *tex) {
  if (!tex->isHardwareCanvas())
    return nullptr;

  auto &depthStencils = fb->GetTextureManager()->mCanvasDepthStencils;

  auto it = depthStencils.find(tex);
  if (it == depthStencils.end() || it->second->GetWidth() != tex->GetWidth() ||
      it->second->GetHeight() != tex->GetHeight()) {
    auto ds = std::make_unique<MtTextureImage>(fb);
    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(tex->GetWidth());
    desc->setHeight(tex->GetHeight());
    desc->setPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
    desc->setUsage(MTL::TextureUsageRenderTarget);
    desc->setStorageMode(MTL::StorageModePrivate);

    MTL::Texture *texture = fb->device->device->newTexture(desc);
    ds->SetTexture(texture);
    ds->SetWidth(tex->GetWidth());
    ds->SetHeight(tex->GetHeight());
    desc->release();

    depthStencils[tex] = std::move(ds);
    return depthStencils[tex].get();
  }

  return it->second.get();
}

void MtHardwareTexture::Reset() {
  if (mImage) {
    mImage->SetTexture(nullptr);
  }
  mStagingBuffer.clear();
}

void MtHardwareTexture::CreateImage(FTexture *tex, int translation, int flags) {
  static int createImageCount = 0;
  if (mt_debug || createImageCount++ < 10)
    Printf(
        "Metal: CreateImage called for texture %p (translation=%d, flags=%d). HWCanvas=%d\n",
        tex, translation, flags, tex ? tex->isHardwareCanvas() : -1);

  if (!tex) return;

  if (!tex->isHardwareCanvas()) {
    // Regular texture - get pixel data from game texture and upload to GPU
    FTextureBuffer texbuffer =
        tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
    bool indexed = flags & CTF_Indexed;
    int numChannels = indexed ? 1 : 4;
    int w = texbuffer.mWidth;
    int h = texbuffer.mHeight;

    if (createImageCount <= 10)
      Printf("Metal: Creating GPU texture from buffer: %dx%d, %d channels\n", w,
             h, numChannels);

    // Create Metal texture descriptor
    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(w);
    desc->setHeight(h);

    // Determine pixel format
    MTL::PixelFormat format;
    switch (numChannels) {
    case 1:
      format = MTL::PixelFormatR8Unorm;
      break;
    case 2:
      format = MTL::PixelFormatRG8Unorm;
      break;
    case 3:
    case 4:
    default:
      format = MTL::PixelFormatBGRA8Unorm;
      break;
    }

    desc->setPixelFormat(format);
    desc->setMipmapLevelCount(
        !indexed ? 1 + (int)std::floor(std::log2(std::max(w, h))) : 1);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);

    // Create the texture
    MTL::Texture *texture = fb->device->device->newTexture(desc);
    desc->release();

    if (!texture) {
      Printf("Metal: Failed to create GPU texture %dx%d\n", w, h);
      return;
    }

    // Upload texture data
    if (texbuffer.mBuffer) {
      MTL::Region region = MTL::Region::Make2D(0, 0, w, h);
      texture->replaceRegion(region, 0, texbuffer.mBuffer, w * numChannels);

      if (createImageCount <= 10)
        Printf("Metal: Uploaded %d bytes to GPU texture %p\n",
               w * h * numChannels, texture);
    }

    // Store in image
    mImage->SetTexture(texture);
    mImage->SetWidth(w);
    mImage->SetHeight(h);
    mImage->SetFormat((int)format);
  } else {
    // Hardware canvas (render target) - create empty texture for rendering
    int w = tex->GetWidth();
    int h = tex->GetHeight();
    MTL::PixelFormat format =
        tex->IsHDR() ? MTL::PixelFormatRGBA32Float : MTL::PixelFormatRGBA8Unorm;

    if (createImageCount <= 10)
      Printf("Metal: Creating hardware canvas: %dx%d\n", w, h);

    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(w);
    desc->setHeight(h);
    desc->setPixelFormat(format);
    desc->setMipmapLevelCount(1);
    desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    desc->setStorageMode(
        MTL::StorageModePrivate); // GPU-only for render targets

    MTL::Texture *texture = fb->device->device->newTexture(desc);
    desc->release();

    if (!texture) {
      Printf("Metal: Failed to create hardware canvas %dx%d\n", w, h);
      return;
    }

    mImage->SetTexture(texture);
    mImage->SetWidth(w);
    mImage->SetHeight(h);
    mImage->SetFormat((int)format);
  }
}

// MtTextureManager
MtTextureManager::MtTextureManager(MetalRenderDevice *fb) : fb(fb) {}
MtTextureManager::~MtTextureManager() {}

MTL::Texture *MtTextureManager::CreateTexture(int width, int height, int format,
                                              int mipmaps) {
  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height);
  desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  desc->setMipmapLevelCount(mipmaps);
  desc->setUsage(MTL::TextureUsageShaderRead);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  desc->release();
  return texture;
}

MTL::Texture *MtTextureManager::CreateTextureFromData(const void *data,
                                                      int width, int height,
                                                      int format, int mipmaps) {
  MTL::Texture *texture = CreateTexture(width, height, format, mipmaps);
  if (texture && data) {
    UpdateTexture(texture, 0, data, width * height * 4);
  }
  return texture;
}

void MtTextureManager::UpdateTexture(MTL::Texture *texture, int level,
                                     const void *data, size_t size) {
  if (!texture || !data)
    return;

  MTL::Region region =
      MTL::Region::Make2D(0, 0, texture->width(), texture->height());
  texture->replaceRegion(region, level, data, texture->width() * 4);
}

MTL::Texture *MtTextureManager::GetPPTexture(PPTexture *texture) {
  auto it = mPPTextures.find(texture);
  if (it != mPPTextures.end())
    return it->second;

  if (texture->Backend == nullptr) {
    texture->Backend = std::make_unique<MtPPTexture>(fb, texture);
  }
  mPPTextures[texture] =
      static_cast<MtPPTexture *>(texture->Backend.get())->mTexture;
  return mPPTextures[texture];
}

MtPPTexture::MtPPTexture(MetalRenderDevice *fb, PPTexture *texture) : fb(fb) {
  MTL::PixelFormat format = MTL::PixelFormatInvalid; // Initialize to an invalid but known value
  int bytesPerPixel = 4;
  switch (texture->Format) {
  case PixelFormat::Rgba8:
    format = MTL::PixelFormatRGBA8Unorm;
    bytesPerPixel = 4;
    break;
  case PixelFormat::Rgba16f:
    format = MTL::PixelFormatRGBA16Float;
    bytesPerPixel = 8;
    break;
  case PixelFormat::R32f:
    format = MTL::PixelFormatR32Float;
    bytesPerPixel = 4;
    break;
  case PixelFormat::Rg16f:
    format = MTL::PixelFormatRG16Float;
    bytesPerPixel = 4;
    break;
  case PixelFormat::Rgba16_snorm:
    format = MTL::PixelFormatRGBA16Snorm;
    bytesPerPixel = 8;
    break;
  default:
    format = MTL::PixelFormatRGBA8Unorm;
    bytesPerPixel = 4;
    break;
  }

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(texture->Width);
  desc->setHeight(texture->Height);
  desc->setPixelFormat(format);
  desc->setMipmapLevelCount(1);
  desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);

  if (texture->Data) {
    desc->setStorageMode(MTL::StorageModeShared);
  } else {
    desc->setStorageMode(MTL::StorageModePrivate);
  }

  mTexture = fb->device->device->newTexture(desc);
  desc->release();

  if (texture->Data && mTexture) {
    MTL::Region region =
        MTL::Region::Make2D(0, 0, texture->Width, texture->Height);
    mTexture->replaceRegion(region, 0, texture->Data.get(),
                            texture->Width * bytesPerPixel);
  }
}

MtPPTexture::~MtPPTexture() {
  // Base class MtTextureImage handles recycling mTexture.
  // No need to recycle again here.
}