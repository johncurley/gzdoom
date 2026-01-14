#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "bitmap.h"
#include "c_cvars.h"
#include "filesystem.h" // For ns_graphics
#include "gamestate.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "image.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_renderdevice.h"
#include "mt_texture.h"
#include "printf.h"

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

void MtTextureManager::SetLightmap(int LMTextureSize, int LMTextureCount,
                                   const TArray<uint16_t> &LMTextureData) {
  if (LMTextureSize <= 0 || LMTextureCount <= 0) {
    return;
  }

  int w = LMTextureSize;
  int h = LMTextureSize;
  int count = LMTextureCount;
  int pixelsize = 8; // RGBA16Float

  // Verify data size to prevent overflow
  if (LMTextureData.Size() < (unsigned int)(w * h * count * 3)) {
    Printf(PRINT_LOG,
           "Metal: SetLightmap error - data size mismatch (%u expected, %u "
           "provided)\n",
           (unsigned int)(w * h * count * 3), LMTextureData.Size());
    return;
  }

  if (mLightmap && mLightmap->GetWidth() == w && mLightmap->GetHeight() == h &&
      mLightmap->GetTexture() &&
      (int)mLightmap->GetTexture()->arrayLength() == count) {
    // Reuse existing
  } else {
    mLightmap = std::make_unique<MtTextureImage>(fb);
    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(w);
    desc->setHeight(h);
    desc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    desc->setTextureType(MTL::TextureType2DArray);
    desc->setArrayLength(count);
    desc->setMipmapLevelCount(1);
    desc->setUsage(MTL::TextureUsageShaderRead);

    // Use Private storage mode for best sampling performance when using blit updates
    desc->setStorageMode(MTL::StorageModePrivate);

    MTL::Texture *tex = fb->device->device->newTexture(desc);
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
    int w = mLightmap->GetWidth();
    int h = mLightmap->GetHeight();
    int count = (int)mLightmap->GetTexture()->arrayLength();
    size_t totalBytes = (size_t)w * h * count * 8; // RGBA16Float

    MTL::Buffer *mtlStaging = fb->device->device->newBuffer(totalBytes, MTL::ResourceStorageModeShared);
    if (mtlStaging) {
      uint16_t *dst = (uint16_t *)mtlStaging->contents();
      uint16_t one = 0x3c00; // half-float 1.0
      const uint16_t *src = LMTextureData.Data();

      for (int i = 0; i < w * h * count; i++) {
        *(dst++) = *(src++); // R
        *(dst++) = *(src++); // G
        *(dst++) = *(src++); // B
        *(dst++) = one;      // A
      }

      auto cmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
      if (cmdBuf) {
        auto blit = cmdBuf->blitCommandEncoder();
        for (int i = 0; i < count; i++) {
          blit->copyFromBuffer(mtlStaging, (size_t)i * w * h * 8, w * 8, w * h * 8,
                               MTL::Size::Make(w, h, 1), mLightmap->GetTexture(), i, 0,
                               MTL::Origin::Make(0, 0, 0));
        }
        blit->endEncoding();
        cmdBuf->commit();
        cmdBuf->waitUntilCompleted();
      }
      fb->RecycleBuffer(mtlStaging);
    }
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
  Reset(); // Always reset/orphan for safety with linear textures

  // Create the Metal texture if it doesn't exist
  if (!mImage->GetTexture()) {
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

    // For UI/2D, we use a tiled Private texture for maximum scanout stability,
    // but we use a synchronized blit path to prevent tiling artifacts.
    size_t alignedPitch = (w * texelsize + 1023) & ~1023;
    size_t totalSize = alignedPitch * h;

    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(w);
    desc->setHeight(h);
    desc->setPixelFormat(format);
    desc->setMipmapLevelCount(1);
    desc->setStorageMode(MTL::StorageModePrivate);
    desc->setUsage(MTL::TextureUsageShaderRead);

    MTL::Texture *texture = fb->device->device->newTexture(desc);
    mImage->SetTexture(texture);
    mImage->SetWidth(w);
    mImage->SetHeight(h);
    mImage->SetFormat((int)format);
    mBufferPitch = (int)alignedPitch;
    desc->release();
  }

  mNeedsUpload = false;
}

uint8_t *MtHardwareTexture::MapBuffer() {
  if (mStagingBuffer.empty()) {
    return nullptr;
  }

  mNeedsUpload = true;
  return mStagingBuffer.data();
}

unsigned int MtHardwareTexture::CreateTexture(unsigned char *buffer, int w,
                                              int h, int texunit, bool mipmap,
                                              const char *name) {
  if (name)
    mDebugName = name;
  Reset();

  MTL::PixelFormat format;
  int bpp = mNumChannels;
  switch (mNumChannels) {
  case 1:
    format = MTL::PixelFormatR8Unorm;
    break;
  case 2:
    format = MTL::PixelFormatRG8Unorm;
    break;
  default:
    format = MTL::PixelFormatBGRA8Unorm;
    bpp = 4;
    break;
  }

  if (!mipmap) {
    // UI/2D path: Optimized for fast uploads
    auto storageMode = fb->mVersionManager.GetDynamicStorageMode();
    size_t alignedPitch = (w * bpp + 1023) & ~1023; // Keep alignment for safety, though less critical for Managed

    if (!mImage->GetTexture() || mImage->GetWidth() != w || mImage->GetHeight() != h) {
      Reset();
      auto desc = MTL::TextureDescriptor::alloc()->init();
      desc->setWidth(w);
      desc->setHeight(h);
      desc->setPixelFormat(format);
      desc->setMipmapLevelCount(1);
      desc->setStorageMode(storageMode);
      desc->setUsage(MTL::TextureUsageShaderRead);

      MTL::Texture *texture = fb->device->device->newTexture(desc);
      mImage->SetTexture(texture);
      mImage->SetWidth(w);
      mImage->SetHeight(h);
      mImage->SetFormat((int)format);
      
      // For Managed/Shared, we can often pack tighter, but keeping alignment is safe
      mBufferPitch = (storageMode == MTL::StorageModePrivate) ? (int)alignedPitch : (w * bpp);
      
      desc->release();
    }

    MTL::Texture *texture = mImage->GetTexture();

    if (buffer) {
      // Direct upload via replaceRegion
      MTL::Region region = MTL::Region::Make2D(0, 0, w, h);
      // If the input buffer is packed (row = w * bpp), pass that as bytesPerRow
      // CreateTexture is typically called with tightly packed buffers from the engine
      texture->replaceRegion(region, 0, buffer, w * bpp);
    }
  } else {
    // Standard path for MIPmapped world textures
    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(w);
    desc->setHeight(h);
    desc->setPixelFormat(format);
    int mipLevels = (int)floor(log2(max(w, h))) + 1;
    desc->setMipmapLevelCount(mipLevels);
    desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    desc->setStorageMode(MTL::StorageModePrivate);

    MTL::Texture *texture = fb->device->device->newTexture(desc);
    mImage->SetTexture(texture);
    mImage->SetWidth(w);
    mImage->SetHeight(h);
    mImage->SetFormat((int)format);
    desc->release();

    if (buffer) {
      size_t alignedPitch = (w * bpp + 1023) & ~1023;
      size_t totalSize = alignedPitch * h;
      MTL::Buffer *staging = fb->device->device->newBuffer(totalSize, MTL::StorageModeShared);
      if (staging) {
        uint8_t *dst = (uint8_t *)staging->contents();
        for (int y = 0; y < h; y++) {
          memcpy(dst + y * alignedPitch, buffer + y * w * bpp, w * bpp);
        }

        auto cmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
        if (cmdBuf) {
          auto blit = cmdBuf->blitCommandEncoder();
          blit->copyFromBuffer(staging, 0, alignedPitch, 0,
                               MTL::Size::Make(w, h, 1), texture, 0, 0,
                               MTL::Origin::Make(0, 0, 0));
          if (mipLevels > 1)
            blit->generateMipmaps(texture);
          blit->endEncoding();
          cmdBuf->commit();
        }
        fb->RecycleBuffer(staging);
      }
    }
  }

  return 1;
}

void MtHardwareTexture::CreateWipeTexture(int w, int h, const char *name) {
  if (name)
    mDebugName = name;
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

    if (fb->mVersionManager.supportsMemoryless) {
      desc->setStorageMode(MTL::StorageModeMemoryless);
    } else {
      desc->setStorageMode(MTL::StorageModePrivate);
    }

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
    fb->RecycleBuffer(mBackingBuffer); // Explicitly recycle backing buffer
    mBackingBuffer = nullptr;
  }
  mStagingBuffer.clear();
  mNeedsUpload = false;
}

void MtHardwareTexture::CreateImage(FTexture *tex, int translation, int flags) {
  if (tex) {
    char buf[128];
    const char *lumpName = "";
    if (tex->GetSourceLump() >= 0) {
      lumpName = fileSystem.GetFileFullName(tex->GetSourceLump());
    }
    snprintf(buf, sizeof(buf), "Tex_%p_Lump_%d_%s", tex, tex->GetSourceLump(),
             lumpName ? lumpName : "");
    mDebugName = buf;
  }

  if (!tex)
    return;

  // Detect UI and Startup textures to disable mipmaps.
  bool isStartupState = (gamestate == GS_STARTUP);
  bool isSpecialTex = (tex->GetSourceLump() == -1);
  const char *name = mDebugName.c_str();

  int ns = tex->GetSourceLump() >= 0
               ? fileSystem.GetFileNamespace(tex->GetSourceLump())
               : -1;
  bool isGraphicsNS = (ns == FileSys::ns_graphics);

  bool isUITexture =
      (isGraphicsNS || strstr(name, "STARTUP") || strstr(name, "BOOTLOGO") ||
       strstr(name, "M_") || strstr(name, "ST_") || strstr(name, "WI_") ||
       strstr(name, "TITLE") || strstr(name, "INTER") || strstr(name, "HELP") ||
       strstr(name, "CREDIT") || strstr(name, "CONBACK") ||
       strstr(name, "Font") || strstr(name, "FONT"));

  bool disableMips = isStartupState || isSpecialTex || isUITexture ||
                     (fb->GetFrameCount() < 100);

  if (!tex->isHardwareCanvas()) {
    // Get pixel data from game texture using standard engine path
    FTextureBuffer texbuffer =
        tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
    if (!texbuffer.mBuffer)
      return;

    bool indexed = (flags & CTF_Indexed) != 0;
    int bytesPerPixel = indexed ? 1 : 4;
    int expectedW = texbuffer.mWidth;
    int expectedH = texbuffer.mHeight;
    int pitch = expectedW * bytesPerPixel;

    // Calculate desired mip levels upfront to ensure reuse logic is consistent
    int desiredMipLevels = 1;
    bool wantMipmap = (gl_texture_filter > 0) && !indexed && expectedW > 1 &&
                      expectedH > 1 && !disableMips;
    if (wantMipmap) {
      desiredMipLevels = (int)floor(log2(max(expectedW, expectedH))) + 1;
    }

    // REUSE LOGIC: Reuse existing texture if dimensions match to prevent
    // "flashing".
    if (mImage->GetTexture()) {
      if (mImage->GetWidth() != expectedW || mImage->GetHeight() != expectedH ||
          (int)mImage->GetTexture()->mipmapLevelCount() != desiredMipLevels) {
        Reset();
      }
    }

    mBufferPitch = pitch;

    if (!texbuffer.mBuffer) {
      return;
    }

    MTL::PixelFormat format =
        indexed ? MTL::PixelFormatR8Unorm : MTL::PixelFormatBGRA8Unorm;
    MTL::Texture *texture = nullptr;

    if (disableMips) {
      // STARTUP/UI PATH: Optimized for fast uploads
      // Use dynamic storage mode (Managed/Shared) to avoid blit overhead if possible
      auto storageMode = fb->mVersionManager.GetDynamicStorageMode();

      if (!mImage->GetTexture() || mImage->GetWidth() != expectedW ||
          mImage->GetHeight() != expectedH) {
        if (mImage->GetTexture()) {
          fb->RecycleTexture(mImage->GetTexture());
          mImage->SetTexture(nullptr);
        }

        auto desc = MTL::TextureDescriptor::alloc()->init();
        desc->setWidth(expectedW);
        desc->setHeight(expectedH);
        desc->setPixelFormat(format);
        desc->setMipmapLevelCount(1);
        desc->setStorageMode(storageMode);
        desc->setUsage(MTL::TextureUsageShaderRead);

        MTL::Texture *mtlTex = fb->device->device->newTexture(desc);
        mImage->SetTexture(mtlTex);
        mImage->SetWidth(expectedW);
        mImage->SetHeight(expectedH);
        mImage->SetFormat((int)format);
        mBufferPitch = pitch; // Use natural pitch for managed textures
        desc->release();
      }

      texture = mImage->GetTexture();

      if (texbuffer.mBuffer) {
        // Direct upload via replaceRegion (works for Managed and Shared)
        MTL::Region region = MTL::Region::Make2D(0, 0, expectedW, expectedH);
        texture->replaceRegion(region, 0, texbuffer.mBuffer, pitch);
      }
    } else {
      // WORLD TEXTURE PATH: Use PRIVATE + Blit.
      // This is the highest performance sampling path for world geometry.
      if (!mImage->GetTexture() ||
          mImage->GetTexture()->storageMode() != MTL::StorageModePrivate ||
          mImage->GetTexture()->mipmapLevelCount() != (NS::UInteger)desiredMipLevels) {
        
        if (mImage->GetTexture()) {
          fb->RecycleTexture(mImage->GetTexture());
          mImage->SetTexture(nullptr);
        }

        auto desc = MTL::TextureDescriptor::alloc()->init();
        desc->setWidth(expectedW);
        desc->setHeight(expectedH);
        desc->setPixelFormat(format);
        desc->setMipmapLevelCount(desiredMipLevels);
        desc->setStorageMode(MTL::StorageModePrivate);
        desc->setUsage(MTL::TextureUsageShaderRead);
        if (desiredMipLevels > 1) {
          desc->setUsage(desc->usage() | MTL::TextureUsageRenderTarget);
        }
        texture = fb->device->device->newTexture(desc);
        mImage->SetTexture(texture);
        mImage->SetWidth(expectedW);
        mImage->SetHeight(expectedH);
        mImage->SetFormat((int)format);
        desc->release();
      } else {
        texture = mImage->GetTexture();
      }

      if (texbuffer.mBuffer) {
        // Blit update for Private texture
        size_t alignedPitch = (pitch + 1023) & ~1023;
        size_t totalSize = alignedPitch * expectedH;
        MTL::Buffer *staging = fb->device->device->newBuffer(totalSize, MTL::StorageModeShared);
        
        if (staging) {
            uint8_t *dst = (uint8_t *)staging->contents();
            uint8_t *src = (uint8_t *)texbuffer.mBuffer;
            for (int y = 0; y < expectedH; y++) {
                memcpy(dst + y * alignedPitch, src + y * pitch, pitch);
            }

            auto cmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
            if (cmdBuf) {
                auto blit = cmdBuf->blitCommandEncoder();
                blit->copyFromBuffer(staging, 0, alignedPitch, 0,
                                     MTL::Size::Make(expectedW, expectedH, 1), texture, 0,
                                     0, MTL::Origin::Make(0, 0, 0));
                if (desiredMipLevels > 1) {
                    blit->generateMipmaps(texture);
                }
                blit->endEncoding();
                cmdBuf->commit();
                // We don't force wait here for world textures to allow parallelism
            }
            fb->RecycleBuffer(staging);
        }
      }
    }

    static_cast<MtRenderState *>(fb->RenderState())->MarkAsFilled(texture);
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
      Printf(PRINT_LOG, "Metal: Failed to create hardware canvas %dx%d\n", w,
             h);
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

void MtTextureManager::GenerateMipmaps(MTL::Texture *texture) {
  if (!texture || texture->mipmapLevelCount() <= 1)
    return;

  // Ensure any active render pass is ended before creating a blit encoder.
  // Metal does not allow multiple encoders to be active on the same command
  // buffer.
  auto renderState = static_cast<MtRenderState *>(fb->RenderState());
  if (renderState) {
    renderState->EndRenderPass();
  }

  // Use the active render command buffer to ensure correct ordering and zero
  // stalls.
  auto cmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
  if (!cmdBuf)
    return;

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

IHardwareTexture *MtTextureManager::GetPaletteTexture(int translation,
                                                      bool highlight) {
  uint32_t key = (translation << 1) | (highlight ? 1 : 0);
  auto it = mPaletteTextures.find(key);
  if (it != mPaletteTextures.end())
    return it->second.get();

  auto hwTex = std::make_unique<MtHardwareTexture>(fb, 4);

  // Translation table from palette
  FRemapTable *remap = GPalette.GetTranslation(
      GetTranslationType(translation), GetTranslationIndex(translation));
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

  hwTex->CreateTexture((unsigned char *)colors, 256, 1, 0, false,
                       "PaletteTexture");

  auto ptr = hwTex.get();
  mPaletteTextures[key] = std::move(hwTex);
  return ptr;
}

MtPPTexture::MtPPTexture(MetalRenderDevice *fb, PPTexture *texture) : fb(fb) {
  MTL::PixelFormat format =
      MTL::PixelFormatInvalid; // Initialize to an invalid but known value
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
    desc->setStorageMode(
        fb->mVersionManager
            .GetDynamicStorageMode()); // Shared for robustness on Intel/macOS
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
