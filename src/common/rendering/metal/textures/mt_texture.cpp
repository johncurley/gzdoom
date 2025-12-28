#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/system/mt_commandbuffer.h"
#include "mt_texture.h"
#include "printf.h"
#include "c_cvars.h"

#include "textures.h"
#include <cmath>

EXTERN_CVAR(Bool, mt_debug)

// MtTextureImage
MtTextureImage::MtTextureImage(MetalRenderDevice *fb) : fb(fb) {}

MtTextureImage::~MtTextureImage() {
  if (mTexture) {
    if (fb && !fb->mIsDestroyed) {
      fb->RecycleTexture(mTexture);
    } else {
      mTexture->release();
    }
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
  if (mt_debug)
    Printf(PRINT_LOG, "Metal: AllocateBuffer %s: %dx%d, texelsize=%d\n", mDebugName.c_str(), w, h,
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
      Printf(PRINT_LOG, "Metal: Failed to allocate texture %dx%d\n", w, h);
      return;
    }

    // Store in image
    mImage->SetTexture(texture);
    mImage->SetWidth(w);
    mImage->SetHeight(h);
    mImage->SetFormat((int)format);

    if (mt_debug)
      Printf(PRINT_LOG, "Metal: Texture allocated successfully: %p (%dx%d)\n", texture, w,
             h);
  }

  // Allocate staging buffer for CPU writes. 
  // GZDoom typically provides tightly packed data.
  mBufferPitch = w * texelsize;
  mStagingBuffer.resize(mBufferPitch * h);
  mNeedsUpload = false;
}

uint8_t *MtHardwareTexture::MapBuffer() {
  // For Metal, we use shared storage mode which allows CPU access
  // So we return the staging buffer that will be uploaded later
  if (mStagingBuffer.empty()) {
    Printf(PRINT_LOG, "Metal: Warning - MapBuffer called but staging buffer is empty\n");
    return nullptr;
  }

  static int mapCount = 0;
  if (mapCount++ < 5)
    Printf(PRINT_LOG, "Metal: MapBuffer called, returning %zu byte buffer\n",
           mStagingBuffer.size());

  mNeedsUpload = true;
  return mStagingBuffer.data();
}

unsigned int MtHardwareTexture::CreateTexture(unsigned char *buffer, int w,
                                              int h, int texunit, bool mipmap,
                                              const char *name) {
  if (name) mDebugName = name;
  if (mt_debug)
    Printf(PRINT_LOG, "Metal: CreateTexture %s: %dx%d, mipmap=%d\n", mDebugName.c_str(), w, h,
           mipmap);

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

  int mipLevels = 1;
  if (mipmap) {
      mipLevels = (int)floor(log2(max(w, h))) + 1;
  }
  desc->setMipmapLevelCount(mipLevels);
  desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
  desc->setStorageMode(MTL::StorageModeShared);

  // Create the texture
  MTL::Texture *texture = fb->device->device->newTexture(desc);
  desc->release();

  if (!texture) {
    Printf(PRINT_LOG, "Metal: Failed to create texture %dx%d\n", w, h);
    return 0;
  }

  // Upload data if provided
  if (buffer) {
    MTL::Region region = MTL::Region::Make2D(0, 0, w, h);
    int bytesPerPixel = mNumChannels;
    mBufferPitch = w * bytesPerPixel;
    texture->replaceRegion(region, 0, buffer, mBufferPitch);
    
    if (mipLevels > 1) {
        fb->GetTextureManager()->GenerateMipmaps(texture);
    }
  }

  // Store in image
  mImage->SetTexture(texture);
  mImage->SetWidth(w);
  mImage->SetHeight(h);
  mImage->SetFormat((int)texture->pixelFormat());
  mNeedsUpload = false;

  if (mt_debug)
    Printf(PRINT_LOG, "Metal: Texture created successfully: %p\n", texture);

  return 1; // Success
}

void MtHardwareTexture::CreateWipeTexture(int w, int h, const char *name) {
  if (name) mDebugName = name;
  if (mt_debug) Printf(PRINT_LOG, "Metal: CreateWipeTexture %s (%dx%d)\n", mDebugName.c_str(), w, h);
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
  mImage->SetFormat((int)MTL::PixelFormatBGRA8Unorm);

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
  mNeedsUpload = false;
}

void MtHardwareTexture::CreateImage(FTexture *tex, int translation, int flags) {
  if (tex) {
      char buf[32];
      snprintf(buf, sizeof(buf), "Tex_%p", tex);
      mDebugName = buf;
  }
  if (mt_debug)
    Printf(PRINT_LOG, 
        "Metal: CreateImage %s (translation=%d, flags=%d). HWCanvas=%d\n",
        mDebugName.c_str(), translation, flags, tex ? tex->isHardwareCanvas() : -1);

  if (!tex) return;

  if (!tex->isHardwareCanvas()) {
    // If we already have a texture, don't recreate it unless the size changed
    if (mImage->GetTexture()) {
        if (mImage->GetWidth() == tex->GetWidth() && mImage->GetHeight() == tex->GetHeight()) {
            if (mt_debug) Printf(PRINT_LOG, "Metal: CreateImage - reusing existing texture %p\n", mImage->GetTexture());
            return;
        }
        Reset();
    }

    // Regular texture - get pixel data from game texture and upload to GPU
    FTextureBuffer texbuffer =
        tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
    bool indexed = flags & CTF_Indexed;
    int numChannels = (flags & CTF_Indexed) ? 1 : 4;
    
    // GZDoom's CreateTexBuffer can return 3-channel RGB for some images
    // We need to detect this to avoid distortion
    int w = texbuffer.mWidth;
    int h = texbuffer.mHeight;

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
    int mipLevels = 1;
    if (!(flags & CTF_Indexed) && w > 1 && h > 1) {
        mipLevels = (int)floor(log2(max(w, h))) + 1;
    }
    desc->setMipmapLevelCount(mipLevels);
    desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    desc->setStorageMode(MTL::StorageModeShared);

    // Create the texture
    MTL::Texture *texture = fb->device->device->newTexture(desc);
    desc->release();

    if (!texture) {
      Printf(PRINT_LOG, "Metal: Failed to create GPU texture %dx%d\n", w, h);
      return;
    }

    if (mt_debug) {
        Printf(PRINT_LOG, "Metal: Created GPU texture %p (%dx%d), format=%llu, storageMode=%llu, mips=%d\n", 
               texture, w, h, (unsigned long long)texture->pixelFormat(), (unsigned long long)texture->storageMode(), mipLevels);
    }

    // Upload texture data
    if (texbuffer.mBuffer) {
      MTL::Region region = MTL::Region::Make2D(0, 0, w, h);
      mBufferPitch = w * numChannels;
      
      texture->replaceRegion(region, 0, texbuffer.mBuffer, mBufferPitch);

      if (mt_debug)
        Printf(PRINT_LOG, "Metal: Uploaded %d bytes to GPU texture %p (mips=%d)\n", 
               (int)(mBufferPitch * h), texture, mipLevels);
      
      if (mipLevels > 1) {
          fb->GetTextureManager()->GenerateMipmaps(texture);
      }
    }

    // Store in image
    mImage->SetTexture(texture);
    mImage->SetWidth(w);
    mImage->SetHeight(h);
    mImage->SetFormat((int)format);
    mNeedsUpload = false;
  } else {
    // Hardware canvas (render target) - create empty texture for rendering
    int w = tex->GetWidth();
    int h = tex->GetHeight();

    if (mImage->GetTexture()) {
        if (mImage->GetWidth() == w && mImage->GetHeight() == h) {
            return;
        }
        Reset();
    }

    MTL::PixelFormat format =
        tex->IsHDR() ? MTL::PixelFormatRGBA32Float : MTL::PixelFormatBGRA8Unorm;

    if (mt_debug)
      Printf(PRINT_LOG, "Metal: Creating hardware canvas: %dx%d\n", w, h);

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
      Printf(PRINT_LOG, "Metal: Failed to create hardware canvas %dx%d\n", w, h);
      return;
    }

    if (mt_debug) {
        Printf(PRINT_LOG, "Metal: Created HWCanvas texture %p (%dx%d), format=%llu, storageMode=%llu\n", 
               texture, w, h, (unsigned long long)texture->pixelFormat(), (unsigned long long)texture->storageMode());
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

void MtTextureManager::GenerateMipmaps(MTL::Texture *texture) {
  if (!texture || texture->mipmapLevelCount() <= 1)
    return;

  // Use a dedicated command buffer for mipmap generation.
  // Metal guarantees that command buffers committed to the same queue execute in order.
  // By committing this now, we ensure it's submitted before the main render buffer.
  MTL::CommandBuffer *cmdBuf = fb->device->commandQueue->commandBuffer();
  if (!cmdBuf) return;

  auto blitEncoder = cmdBuf->blitCommandEncoder();
  if (blitEncoder) {
      blitEncoder->generateMipmaps(texture);
      blitEncoder->endEncoding();
  }
  cmdBuf->commit();
  // We do not wait here to avoid CPU stalls; the GPU will handle the ordering.
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

IHardwareTexture *MtTextureManager::GetPaletteTexture(int translation, bool highlight) {
  uint32_t key = (translation << 1) | (highlight ? 1 : 0);
  auto it = mPaletteTextures.find(key);
  if (it != mPaletteTextures.end())
    return it->second.get();

  auto hwTex = std::make_unique<MtHardwareTexture>(fb, 4);
  
  // Translation table from palette
  FRemapTable *remap = GPalette.GetTranslation(GetTranslationType(translation), GetTranslationIndex(translation));
  const PalEntry *palette = remap ? remap->Palette : GPalette.BaseColors;

  PalEntry colors[256];
  for (int i = 0; i < 256; i++) {
      colors[i] = palette[i];
      if (highlight) {
          // Highlight logic matching Vulkan/GL
          colors[i].r = (colors[i].r + 255) / 2;
          colors[i].g = (colors[i].g + 255) / 2;
          colors[i].b = (colors[i].b + 255) / 2;
      }
  }

  hwTex->CreateTexture((unsigned char *)colors, 256, 1, 0, false, "PaletteTexture");
  
  auto ptr = hwTex.get();
  mPaletteTextures[key] = std::move(hwTex);
  return ptr;
}

MtPPTexture::MtPPTexture(MetalRenderDevice *fb, PPTexture *texture) : fb(fb) {
  MTL::PixelFormat format = MTL::PixelFormatInvalid; // Initialize to an invalid but known value
  int bytesPerPixel = 4;
  switch (texture->Format) {
  case PixelFormat::Rgba8:
    format = MTL::PixelFormatBGRA8Unorm;
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

  if (mTexture) {

    if (fb && !fb->mIsDestroyed) {

      fb->RecycleTexture(mTexture);

    } else {

      mTexture->release();

    }

    mTexture = nullptr;

  }

}


