#pragma once

#include "hw_texcontainer.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "mt_textureloader.h"
#include <memory>
#include <unordered_map>
#include <vector>

#define TimeScale TimeScale_GZDOOM
namespace MTL {
class Texture;
} // namespace MTL
#undef TimeScale

class MetalRenderDevice;
class FGameTexture;
class MtTextureLoader;

// Metal texture image
class MtTextureImage {
public:
  MtTextureImage(MetalRenderDevice *fb);
  ~MtTextureImage();

  MTL::Texture *GetTexture() const { return mTexture; }
  void SetTexture(MTL::Texture *texture) { mTexture = texture; }

  int GetWidth() const { return mWidth; }
  int GetHeight() const { return mHeight; }
  int GetFormat() const { return mFormat; }

  void SetWidth(int width) { mWidth = width; }
  void SetHeight(int height) { mHeight = height; }
  void SetFormat(int format) { mFormat = format; }

private:
  MTL::Texture *mTexture = nullptr;
  int mWidth = 0;
  int mHeight = 0;
  int mFormat = 0;
  MetalRenderDevice *fb = nullptr;
};

// Metal hardware texture
class MtHardwareTexture : public IHardwareTexture {
public:
  MtHardwareTexture(MetalRenderDevice *fb, int numchannels);
  ~MtHardwareTexture();

  // IHardwareTexture interface
  void AllocateBuffer(int w, int h, int texelsize) override;
  uint8_t *MapBuffer() override;
  unsigned int CreateTexture(unsigned char *buffer, int w, int h, int texunit,
                             bool mipmap, const char *name) override;
  void CreateWipeTexture(int w, int h, const char *name);

  // Additional methods
  void Reset();
  void CreateImage(FTexture *tex, int translation, int flags);

  MtTextureImage *GetImage() { return mImage.get(); }
  MtTextureImage *GetDepthStencil(FCanvasTexture *tex);
  size_t GetStagingBufferSize() const { return mStagingBuffer.size(); }
  const uint8_t *GetStagingBuffer() const { return mStagingBuffer.data(); }
  void ResetStagingBuffer() { mNeedsUpload = false; }
  bool NeedsUpload() const { return mNeedsUpload; }
  void SetNeedsUpload(bool needs) { mNeedsUpload = true; }
  const std::string& GetDebugName() const { return mDebugName; }
  int GetBufferPitch() const { return mBufferPitch; }
  int GetNumChannels() const { return mNumChannels; }
  uint32_t GetPendingLoadId() const { return mPendingLoadId; }
  void SetPendingLoadId(uint32_t id) { mPendingLoadId = id; }

private:
  std::unique_ptr<MtTextureImage> mImage;
  MetalRenderDevice *fb = nullptr;
  int mNumChannels = 0;
  std::vector<uint8_t> mStagingBuffer;
  MTL::Buffer *mBackingBuffer = nullptr; // For zero-copy dynamic textures
  bool mNeedsUpload = false;
  std::string mDebugName;
  int mBufferPitch = 0;
  uint32_t mPendingLoadId = 0;       // GCD async load task ID
  bool mNeedsAsyncUpload = false;    // Flag: async load completed, needs GPU upload
};

// Texture manager
class MtTextureManager {
public:
  MtTextureManager(MetalRenderDevice *fb);
  ~MtTextureManager();

  // Create textures
  MTL::Texture *CreateTexture(int width, int height, int format, int mipmaps);
  MTL::Texture *CreateTextureFromData(const void *data, int width, int height,
                                      int format, int mipmaps);

  // Update texture data
  void UpdateTexture(MTL::Texture *texture, int level, const void *data,
                     size_t size);
  void GenerateMipmaps(MTL::Texture *texture);

  // Async texture loading
  MtTextureLoader *GetTextureLoader() { return mTextureLoader.get(); }

  // Queue a hardware texture for async CPU decompression + GPU upload.
  // Safe to call from the render thread. Upload is deferred to next BeginFrame().
  void QueueHardwareTextureLoad(MtHardwareTexture *hwTex, FTexture *tex,
                                int translation, int flags, bool wantMipmap);

  // Process completed async texture loads (call from render thread)
  void ProcessAsyncTextureLoads();

  // PP Texture support
  MTL::Texture *GetPPTexture(PPTexture *texture);

  // Palette support
  IHardwareTexture *GetPaletteTexture(int translation, bool highlight);

  // Lightmap support
  void SetLightmap(int LMTextureSize, int LMTextureCount, const TArray<uint16_t>& LMTextureData);
  MTL::Texture* GetLightmap() { return mLightmap ? mLightmap->GetTexture() : nullptr; }

  std::unordered_map<FCanvasTexture *, std::unique_ptr<MtTextureImage>>
      mCanvasDepthStencils;

private:
  struct PendingUpload {
    MtHardwareTexture *hwTex = nullptr;
    bool wantMipmap = false;
  };

  void PerformAsyncGPUUpload(MtHardwareTexture *hwTex,
                             const TextureLoadTask &task, bool wantMipmap);

  MetalRenderDevice *fb = nullptr;
  std::unordered_map<PPTexture *, MTL::Texture *> mPPTextures;
  std::unordered_map<uint32_t, std::unique_ptr<MtHardwareTexture>> mPaletteTextures;
  std::unique_ptr<MtTextureImage> mLightmap;
  MTL::Buffer *mLightmapStaging = nullptr;
  std::unique_ptr<MtTextureLoader> mTextureLoader;
  std::unordered_map<uint32_t, PendingUpload> mPendingUploads;
};

class MtPPTexture : public PPTextureBackend {
public:
  MtPPTexture(MetalRenderDevice *fb, PPTexture *texture);
  ~MtPPTexture();

  MetalRenderDevice *fb = nullptr;
  MTL::Texture *mTexture = nullptr;
};
