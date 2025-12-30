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
#include "bitmap.h"
#include "image.h"
#include "gamestate.h"
#include "filesystem.h" // For ns_graphics

#include "textures.h"
#include <cmath>

EXTERN_CVAR(Bool, mt_debug)
EXTERN_CVAR(Int, gl_texture_filter)

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

void MtTextureManager::SetLightmap(int LMTextureSize, int LMTextureCount, const TArray<uint16_t>& LMTextureData) {
    if (LMTextureSize <= 0 || LMTextureCount <= 0) {
        if (mt_debug) Printf(PRINT_LOG, "Metal: SetLightmap ignored - invalid dimensions %dx%dx%d\n", LMTextureSize, LMTextureSize, LMTextureCount);
        return;
    }

    int w = LMTextureSize;
    int h = LMTextureSize;
    int count = LMTextureCount;
    int pixelsize = 8; // RGBA16Float

    // Verify data size to prevent overflow
    if (LMTextureData.Size() < (unsigned int)(w * h * count * 3)) {
        Printf(PRINT_LOG, "Metal: SetLightmap error - data size mismatch (%u expected, %u provided)\n", 
               (unsigned int)(w * h * count * 3), LMTextureData.Size());
        return;
    }

    if (mLightmap && mLightmap->GetWidth() == w && 
        mLightmap->GetHeight() == h && 
        mLightmap->GetTexture() && 
        (int)mLightmap->GetTexture()->arrayLength() == count) {
        // Reuse existing
    } else {
        if (mt_debug) Printf(PRINT_LOG, "Metal: Creating Lightmap array %dx%dx%d RGBA16Float\n", w, h, count);
        mLightmap = std::make_unique<MtTextureImage>(fb);
        auto desc = MTL::TextureDescriptor::alloc()->init();
        desc->setWidth(w);
        desc->setHeight(h);
        desc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        desc->setTextureType(MTL::TextureType2DArray);
        desc->setArrayLength(count);
        desc->setMipmapLevelCount(1);
        desc->setUsage(MTL::TextureUsageShaderRead);
        
        // On Intel Macs, Managed mode is often more reliable than Private for array uploads
        desc->setStorageMode(MTL::StorageModeManaged);
        
        MTL::Texture* tex = fb->device->device->newTexture(desc);
        if (!tex) {
            Printf(PRINT_LOG, "Metal: FAILED to create Lightmap texture array!\n");
            desc->release();
            return;
        }
        
        mLightmap->SetTexture(tex);
        mLightmap->SetWidth(w);
        mLightmap->SetHeight(h);
        desc->release();
    }
    
    if (mLightmap->GetTexture()) {
        std::vector<uint16_t> stagingBuffer(w * h * count * 4);
        uint16_t one = 0x3c00; // half-float 1.0
        const uint16_t* src = LMTextureData.Data();
        uint16_t* dst = stagingBuffer.data();
        
        for (int i = 0; i < w * h * count; i++) {
            *(dst++) = *(src++); // R
            *(dst++) = *(src++); // G
            *(dst++) = *(src++); // B
            *(dst++) = one;      // A
        }
        
        for (int i = 0; i < count; i++) {
            MTL::Region region = MTL::Region::Make2D(0, 0, w, h);
            mLightmap->GetTexture()->replaceRegion(region, 0, i, 
                                                  &stagingBuffer[i * w * h * 4], 
                                                  w * pixelsize,
                                                  w * h * pixelsize);
        }
        // Managed textures need to know they were modified if we used replaceRegion on some drivers,
        // though replaceRegion usually handles it. Explicitly calling it for safety.
    }
}

// MtHardwareTexture
MtHardwareTexture::MtHardwareTexture(MetalRenderDevice *fb, int numchannels)
    : fb(fb), mNumChannels(numchannels) {
  mImage = std::make_unique<MtTextureImage>(fb);
}

MtHardwareTexture::~MtHardwareTexture() {
  if (mBackingBuffer) {
    mBackingBuffer->release();
    mBackingBuffer = nullptr;
  }
}

void MtHardwareTexture::AllocateBuffer(int w, int h, int texelsize) {
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
  if (mapCount++ < 20) // Increased limit to see more startup activity
    Printf(PRINT_LOG, "Metal: MapBuffer called for %s, returning %zu byte buffer\n",
           mDebugName.c_str(), mStagingBuffer.size());

  mNeedsUpload = true;
  return mStagingBuffer.data();
}

unsigned int MtHardwareTexture::CreateTexture(unsigned char *buffer, int w,
                                              int h, int texunit, bool mipmap,
                                              const char *name) {
  if (name) mDebugName = name;
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
  if (mImage && mImage->GetTexture()) {
    fb->RecycleTexture(mImage->GetTexture());
    mImage->SetTexture(nullptr);
  }
  if (mBackingBuffer) {
    mBackingBuffer->release();
    mBackingBuffer = nullptr;
  }
  mStagingBuffer.clear();
  mNeedsUpload = false;
}

void MtHardwareTexture::CreateImage(FTexture *tex, int translation, int flags) {
  if (tex) {
      char buf[128];
      const char* lumpName = "";
      if (tex->GetSourceLump() >= 0) {
          lumpName = fileSystem.GetFileFullName(tex->GetSourceLump());
      }
      snprintf(buf, sizeof(buf), "Tex_%p_Lump_%d_%s", tex, tex->GetSourceLump(), lumpName ? lumpName : "");
      mDebugName = buf;
  }
  
  if (!tex) return;

  // Detect UI and Startup textures to disable mipmaps.
  bool isStartupState = (gamestate == GS_STARTUP);
  bool isSpecialTex = (tex->GetSourceLump() == -1);
  const char* name = mDebugName.c_str();
  
  int ns = tex->GetSourceLump() >= 0 ? fileSystem.GetFileNamespace(tex->GetSourceLump()) : -1;
  bool isGraphicsNS = (ns == FileSys::ns_graphics);

  bool isUITexture = (isGraphicsNS ||
                      strstr(name, "STARTUP") || strstr(name, "BOOTLOGO") || 
                      strstr(name, "M_") || strstr(name, "ST_") || strstr(name, "WI_") ||
                      strstr(name, "TITLE") || strstr(name, "INTER") || 
                      strstr(name, "HELP") || strstr(name, "CREDIT") || strstr(name, "CONBACK") ||
                      strstr(name, "Font") || strstr(name, "FONT"));

  bool disableMips = isStartupState || isSpecialTex || isUITexture;

  if (mt_debug) {
      Printf(PRINT_LOG, "Metal: CreateImage %s (Lump %d). State: %d, ns: %d, isUI: %d, disableMips: %d\n", 
             name, tex->GetSourceLump(), (int)gamestate, ns, (int)isUITexture, (int)disableMips);
  }

  if (!tex->isHardwareCanvas()) {
    // Get pixel data from game texture using standard engine path
    FTextureBuffer texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
    if (!texbuffer.mBuffer) return;

    bool indexed = (flags & CTF_Indexed) != 0;
    int bytesPerPixel = indexed ? 1 : 4;
    int expectedW = texbuffer.mWidth;
    int expectedH = texbuffer.mHeight;
    int pitch = expectedW * bytesPerPixel;

    // Calculate desired mip levels upfront to ensure reuse logic is consistent
    int desiredMipLevels = 1;
    bool wantMipmap = (gl_texture_filter > 0) && !indexed && expectedW > 1 && expectedH > 1 && !disableMips;
    if (wantMipmap) {
        desiredMipLevels = (int)floor(log2(max(expectedW, expectedH))) + 1;
    }

    // If we already have a texture, don't recreate it unless the size OR mip count changed.
    if (mImage->GetTexture()) {
        if (mImage->GetWidth() == expectedW && 
            mImage->GetHeight() == expectedH &&
            (int)mImage->GetTexture()->mipmapLevelCount() == desiredMipLevels) {
            // Texture matches configuration.
        } else {
            if (mt_debug) Printf(PRINT_LOG, "Metal: Resetting texture %s (size mismatch or mip change)\n", name);
            Reset();
        }
    }

    MTL::Texture *texture = nullptr;
    MTL::PixelFormat format = indexed ? MTL::PixelFormatR8Unorm : MTL::PixelFormatBGRA8Unorm;

    if (!mImage->GetTexture()) {
        auto desc = MTL::TextureDescriptor::alloc()->init();
        desc->setWidth(expectedW);
        desc->setHeight(expectedH);
        desc->setPixelFormat(format);
        desc->setMipmapLevelCount(desiredMipLevels);
        desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
        desc->setStorageMode(MTL::StorageModeShared);

        texture = fb->device->device->newTexture(desc);
        mBufferPitch = pitch;
        
        desc->release();

        if (!texture) {
          Printf(PRINT_LOG, "Metal: Failed to create GPU texture %dx%d\n", expectedW, expectedH);
          return;
        }
        
        mImage->SetTexture(texture);
        mImage->SetWidth(expectedW);
        mImage->SetHeight(expectedH);
        mImage->SetFormat((int)format);
        
        if (mt_debug) Printf(PRINT_LOG, "Metal: Created NEW texture %p for %s (%dx%d, mips:%d)\n", texture, name, expectedW, expectedH, desiredMipLevels);
    } else {
        texture = mImage->GetTexture();
    }

    mBufferPitch = pitch;
    MTL::Region region = MTL::Region::Make2D(0, 0, expectedW, expectedH);
    
    if (mt_debug) Printf(PRINT_LOG, "Metal: Uploading to %s (%dx%d, pitch:%d)\n", name, expectedW, expectedH, pitch);
    texture->replaceRegion(region, 0, texbuffer.mBuffer, pitch);

    static_cast<MtRenderState*>(fb->RenderState())->MarkAsFilled(texture);

    if (desiredMipLevels > 1) {
        fb->GetTextureManager()->GenerateMipmaps(texture);
    }

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

  // Ensure any active render pass is ended before creating a blit encoder.
  // Metal does not allow multiple encoders to be active on the same command buffer.
  auto renderState = static_cast<MtRenderState*>(fb->RenderState());
  if (renderState) {
      renderState->EndRenderPass();
  }

  // Use the active render command buffer to ensure correct ordering and zero stalls.
  auto cmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
  if (!cmdBuf) return;

  auto blitEncoder = cmdBuf->blitCommandEncoder();
  if (blitEncoder) {
      blitEncoder->generateMipmaps(texture);
      blitEncoder->endEncoding();
  }
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


