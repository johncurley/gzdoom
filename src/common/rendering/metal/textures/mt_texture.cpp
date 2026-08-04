#include "i_time.h"
#include <Metal/Metal.hpp>

#include "bitmap.h"
#include "c_cvars.h"
#include "filesystem.h" // For ns_graphics
#include "gamestate.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "image.h"
#include "metal/renderer/mt_debug.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_renderdevice.h"
#include "mt_texture.h"
#include "mt_textureloader.h"
#include "printf.h"

#include "textures.h"
#include <chrono>
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
      MTL::Buffer *staging = fb->GetStagingBuffer(totalSize);
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
          cmdBuf->release();
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

  // Must match the pipeline image format. BlitCurrentToImage takes a raw
  // copyFromTexture when the formats agree and a present-shader draw when they
  // do not -- and those two paths have opposite vertical orientation, because
  // the draw goes through PatchVertexShader's Y-flip. A mismatch here shows up
  // as an upside-down screen wipe while the world view looks fine.
  const MTL::PixelFormat format =
      fb->GetBuffers() ? (MTL::PixelFormat)fb->GetBuffers()->GetPipelineFormat()
                       : MTL::PixelFormatBGRA8Unorm;

  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(w);
  desc->setHeight(h);
  desc->setPixelFormat(format);
  desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
  desc->setStorageMode(MTL::StorageModePrivate);

  MTL::Texture *texture = fb->device->device->newTexture(desc);
  desc->release();

  mImage->SetTexture(texture);
  mImage->SetWidth(w);
  mImage->SetHeight(h);
  mImage->SetFormat((int)format);

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
    // If we already have a texture, don't recreate it unless the size changed
    if (mImage->GetTexture()) {
        if (mImage->GetWidth() == tex->GetWidth() && mImage->GetHeight() == tex->GetHeight()) {
            // Texture size matches, but we need to upload the new data.
            // Wait for GPU to finish with this texture before we overwrite it.
            fb->GetCommands()->WaitForCommands(true);

            FTextureBuffer texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
            if (texbuffer.mBuffer) {
                int uploadW = texbuffer.mWidth;
                int uploadH = texbuffer.mHeight;
                MTL::Region region = MTL::Region::Make2D(0, 0, uploadW, uploadH);
                mBufferPitch = uploadW * ((flags & CTF_Indexed) ? 1 : 4);
                mImage->GetTexture()->replaceRegion(region, 0, texbuffer.mBuffer, mBufferPitch);
                
                // Mark as filled so the renderer doesn't try to clear it if used as a target
                static_cast<MtRenderState*>(fb->RenderState())->MarkAsFilled(mImage->GetTexture());

                if (mImage->GetTexture()->mipmapLevelCount() > 1) {
                    fb->GetTextureManager()->GenerateMipmaps(mImage->GetTexture());
                }
            }
            mNeedsUpload = false;
            return;
        }
        Reset();
    }

    // Regular texture - get pixel data from game texture and upload to GPU
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
        // Blit update for Private texture. Timed separately from stalls --
        // this doesn't block (the command buffer commits async, no
        // waitUntilCompleted), but it's real render-thread CPU cost
        // (staging memcpy + blit encode + commit of its own separate
        // command buffer) worth tracking on its own. See
        // MtDebugManager::RecordTextureUpload.
        auto uploadStart = std::chrono::high_resolution_clock::now();

        size_t alignedPitch = (pitch + 1023) & ~1023;
        size_t totalSize = alignedPitch * expectedH;
        MTL::Buffer *staging = fb->GetStagingBuffer(totalSize);

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
                cmdBuf->release();
            }
            fb->RecycleBuffer(staging);
        }

        if (fb->GetDebugManager()) {
            auto uploadEnd = std::chrono::high_resolution_clock::now();
            float uploadMs = std::chrono::duration<float, std::milli>(uploadEnd - uploadStart).count();
            fb->GetDebugManager()->RecordTextureUpload(uploadMs);
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
MtTextureManager::MtTextureManager(MetalRenderDevice *fb) : fb(fb) {
  mTextureLoader = std::make_unique<MtTextureLoader>(fb);
}
MtTextureManager::~MtTextureManager() {
  mTextureLoader.reset();
  if (mLightmapStaging) {
    mLightmapStaging->release();
    mLightmapStaging = nullptr;
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

    MTL::Buffer *mtlStaging = fb->GetStagingBuffer(totalBytes);
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
        cmdBuf->release();
      }
      fb->RecycleBuffer(mtlStaging);
    }
  }
}

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

  // Use a dedicated blit command buffer and commit it immediately.
  // This avoids conflicts if we are currently inside a render pass.
  auto cmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
  if (!cmdBuf) return;

  auto blitEncoder = cmdBuf->blitCommandEncoder();
  if (blitEncoder) {
    blitEncoder->generateMipmaps(texture);
    blitEncoder->endEncoding();
  }
  
  cmdBuf->commit();
  cmdBuf->waitUntilCompleted();
  cmdBuf->release();
}

static MTL::PixelFormat GetMetalPPTextureFormat(PixelFormat format,
                                                int *bytesPerPixel = nullptr) {
  int bpp = 4;
  MTL::PixelFormat metalFormat = MTL::PixelFormatBGRA8Unorm;
  switch (format) {
  case PixelFormat::Rgba8:
    metalFormat = MTL::PixelFormatBGRA8Unorm;
    bpp = 4;
    break;
  case PixelFormat::Rgba16f:
    // Half-float, NOT BGRA8Unorm. This mapped to 8-bit unorm until 2026-08-04,
    // which silently ran every Rgba16f postprocess target at 8-bit LDR --
    // clamped at 1.0 and quantized to 256 levels -- where Vulkan uses
    // VK_FORMAT_R16G16B16A16_SFLOAT (vk_pptexture.cpp:36) and GL uses
    // GL_RGBA16F (gl_renderbuffers.cpp:806). The only shared-code consumer is
    // the bloom pyramid (hw_postprocess.cpp:64-65), so Metal's reference PP
    // bloom was running its whole pyramid in LDR while the compute path ran in
    // RGBA16Float -- which made the reference path an invalid baseline to
    // compare the compute path against.
    metalFormat = MTL::PixelFormatRGBA16Float;
    bpp = 8;
    break;
  case PixelFormat::R32f:
    metalFormat = MTL::PixelFormatR32Float;
    bpp = 4;
    break;
  case PixelFormat::Rg16f:
    metalFormat = MTL::PixelFormatRG16Float;
    bpp = 4;
    break;
  case PixelFormat::Rgba16_snorm:
    metalFormat = MTL::PixelFormatRGBA16Snorm;
    bpp = 8;
    break;
  default:
    metalFormat = MTL::PixelFormatBGRA8Unorm;
    bpp = 4;
    break;
  }
  if (bytesPerPixel)
    *bytesPerPixel = bpp;
  return metalFormat;
}

MTL::Texture *MtTextureManager::GetPPTexture(PPTexture *texture) {
  if (!texture)
    return nullptr;

  auto backend = static_cast<MtPPTexture *>(texture->Backend.get());
  MTL::PixelFormat expectedFormat = GetMetalPPTextureFormat(texture->Format);

  if (backend && backend->mTexture) {
    bool stale =
        backend->mTexture->width() != (NS::UInteger)texture->Width ||
        backend->mTexture->height() != (NS::UInteger)texture->Height ||
        backend->mTexture->pixelFormat() != expectedFormat;
    if (stale) {
      texture->Backend.reset();
      backend = nullptr;
      mPPTextures.erase(texture);
    }
  } else {
    mPPTextures.erase(texture);
  }

  if (!backend) {
    texture->Backend = std::make_unique<MtPPTexture>(fb, texture);
    backend = static_cast<MtPPTexture *>(texture->Backend.get());
  }

  mPPTextures[texture] = backend->mTexture;
  return backend->mTexture;
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
  int bytesPerPixel = 4;
  MTL::PixelFormat format =
      GetMetalPPTextureFormat(texture->Format, &bytesPerPixel);

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

void MtTextureManager::QueueHardwareTextureLoad(MtHardwareTexture *hwTex,
                                                FTexture *tex, int translation,
                                                int flags, bool wantMipmap) {
  if (!hwTex || !tex || !mTextureLoader) return;
  if (hwTex->GetPendingLoadId() != 0) return; // already queued

  uint32_t taskId = mTextureLoader->QueueTextureLoad(tex, translation, flags);
  if (taskId == 0) return;

  hwTex->SetPendingLoadId(taskId);
  mPendingUploads[taskId] = {hwTex, wantMipmap};
}

void MtTextureManager::ProcessAsyncTextureLoads() {
  // Process completed texture loads from GCD queue
  if (!mTextureLoader) return;

  auto completedTasks = mTextureLoader->GetCompletedTasks();
  if (completedTasks.empty()) return;

  for (const auto &task : completedTasks) {
    auto it = mPendingUploads.find(task.taskId);
    if (it == mPendingUploads.end()) continue;

    MtHardwareTexture *hwTex = it->second.hwTex;
    bool wantMipmap = it->second.wantMipmap;
    mPendingUploads.erase(it);

    if (hwTex && task.completed && !task.pixelData.empty()) {
      PerformAsyncGPUUpload(hwTex, task, wantMipmap);
    }
    if (hwTex) {
      hwTex->SetPendingLoadId(0);
    }
  }
}

void MtTextureManager::PerformAsyncGPUUpload(MtHardwareTexture *hwTex,
                                             const TextureLoadTask &task,
                                             bool wantMipmap) {
  if (!hwTex || task.pixelData.empty() || task.width <= 0 || task.height <= 0)
    return;

  MtTextureImage *image = hwTex->GetImage();
  if (!image) return;

  MTL::PixelFormat format =
      task.indexed ? MTL::PixelFormatR8Unorm : MTL::PixelFormatBGRA8Unorm;
  int pitch = task.width * task.bytesPerPixel;

  int desiredMipLevels = 1;
  if (wantMipmap && !task.indexed && task.width > 1 && task.height > 1) {
    desiredMipLevels =
        (int)floor(log2(std::max(task.width, task.height))) + 1;
  }

  // Reuse existing texture if dimensions and format match; otherwise recreate.
  MTL::Texture *texture = image->GetTexture();
  if (!texture ||
      texture->storageMode() != MTL::StorageModePrivate ||
      (int)texture->width() != task.width ||
      (int)texture->height() != task.height ||
      texture->mipmapLevelCount() != (NS::UInteger)desiredMipLevels) {
    if (texture) {
      fb->RecycleTexture(texture);
    }
    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(task.width);
    desc->setHeight(task.height);
    desc->setPixelFormat(format);
    desc->setMipmapLevelCount(desiredMipLevels);
    desc->setStorageMode(MTL::StorageModePrivate);
    desc->setUsage(MTL::TextureUsageShaderRead);
    if (desiredMipLevels > 1) {
      desc->setUsage(desc->usage() | MTL::TextureUsageRenderTarget);
    }
    texture = fb->device->device->newTexture(desc);
    image->SetTexture(texture);
    image->SetWidth(task.width);
    image->SetHeight(task.height);
    image->SetFormat((int)format);
    desc->release();
  }

  // Timed for the same reason, and over the same span, as the synchronous
  // path in CreateImage: staging memcpy + blit encode + commit of a separate
  // command buffer is real render-thread CPU cost. This path was previously
  // untimed, so TextureUploadCPU read zero through a precache cold load even
  // though the work happened -- the extraCommandBuffers counter covered it
  // (GetBlitCommandBuffer increments centrally) but the timer did not, which
  // made the two disagree exactly when the metric mattered most.
  auto uploadStart = std::chrono::high_resolution_clock::now();

  size_t alignedPitch = (pitch + 1023) & ~1023;
  size_t totalSize = alignedPitch * task.height;
  MTL::Buffer *staging = fb->GetStagingBuffer(totalSize);
  if (!staging) return;

  uint8_t *dst = (uint8_t *)staging->contents();
  const uint8_t *src = task.pixelData.data();
  for (int y = 0; y < task.height; y++) {
    memcpy(dst + y * alignedPitch, src + y * pitch, pitch);
  }

  auto cmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
  if (cmdBuf) {
    auto blit = cmdBuf->blitCommandEncoder();
    blit->copyFromBuffer(staging, 0, alignedPitch, 0,
                         MTL::Size::Make(task.width, task.height, 1),
                         texture, 0, 0, MTL::Origin::Make(0, 0, 0));
    if (desiredMipLevels > 1) {
      blit->generateMipmaps(texture);
    }
    blit->endEncoding();
    cmdBuf->commit();
    cmdBuf->release();
  }
  fb->RecycleBuffer(staging);

  if (fb->GetDebugManager()) {
    auto uploadEnd = std::chrono::high_resolution_clock::now();
    float uploadMs = std::chrono::duration<float, std::milli>(uploadEnd - uploadStart).count();
    fb->GetDebugManager()->RecordTextureUpload(uploadMs);
  }

  static_cast<MtRenderState *>(fb->RenderState())->MarkAsFilled(texture);
  hwTex->ResetStagingBuffer(); // clears mNeedsUpload
}
