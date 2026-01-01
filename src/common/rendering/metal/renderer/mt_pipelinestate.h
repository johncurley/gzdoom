#pragma once

#include "renderstyle.h"
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

// Pipeline key for caching (similar to Vulkan)
struct MtPipelineKey {
  int VertexFormat = 0;
  int SpecialEffect = -1;
  int EffectState = 0;
  int AlphaTest = 0;
  int BlendMode = 0;
  int DepthFunc = 0;
  int StencilOp = 0;
  int StencilFunc = 0; // New: Compare function for stencil
  int ColorMask = 0;
  int CullMode = 0;
  int DepthClampMode = 0;
  int DepthWrite = 0;
  int StencilTest = 0;
  int SampleCount = 1;
  int DrawBufferCount = 1;
  int PixelFormat = 0;
  int DepthStencilFormat = 0;
  int ClipDistanceMask = 0;

  bool operator==(const MtPipelineKey &other) const;
  bool operator!=(const MtPipelineKey &other) const {
    return !(*this == other);
  }
};

// Hash function for pipeline key caching
namespace std {
template <> struct hash<MtPipelineKey> {
  size_t operator()(const MtPipelineKey &key) const;
};
} // namespace std

// Metal pipeline state wrapper
class MtPipelineState {
public:
  MTL::RenderPipelineState *pipelineState = nullptr;
  MTL::DepthStencilState *depthStencilState = nullptr;

  MtPipelineKey Key;
};

// Pipeline state manager
class MtPipelineStateManager {
public:
  MtPipelineStateManager(class MetalRenderDevice *fb);
  ~MtPipelineStateManager();

  // Get or create pipeline state for given key
  MtPipelineState *
  GetPipelineState(const MtPipelineKey &key,
                   class MtVertexBuffer *vertexBuffer = nullptr);

  // Get or create pipeline for post-processing
  MTL::RenderPipelineState *GetPPPipelineState(class MtShaderProgram *program,
                                               MTL::PixelFormat colorFormat,
                                               FRenderStyle blendMode);

  MTL::DepthStencilState *GetDisabledDepthStencilState();

  // Clear cache
  void ClearCache();

private:
  // Helper methods for pipeline creation
  MTL::RenderPipelineState *
  CreateRenderPipelineState(const MtPipelineKey &key,
                            class MtVertexBuffer *vertexBuffer);
  MTL::DepthStencilState *CreateDepthStencilState(const MtPipelineKey &key);
  void ConfigureBlendMode(
      MTL::RenderPipelineColorAttachmentDescriptor *colorAttachment,
      int blendMode);

  class MetalRenderDevice *fb = nullptr;
  std::unordered_map<MtPipelineKey, std::unique_ptr<MtPipelineState>>
      mPipelineCache;

  struct PPKey {
    class MtShaderProgram *program;
    MTL::PixelFormat colorFormat;
    FRenderStyle blendMode;

    bool operator==(const PPKey &other) const {
      return program == other.program && colorFormat == other.colorFormat &&
             blendMode.BlendOp == other.blendMode.BlendOp &&
             blendMode.SrcAlpha == other.blendMode.SrcAlpha &&
             blendMode.DestAlpha == other.blendMode.DestAlpha;
    }
  };

  struct PPKeyHash {
    size_t operator()(const PPKey &key) const {
      size_t h = std::hash<void *>{}(key.program);
      h ^= std::hash<int>{}((int)key.colorFormat) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<uint32_t>{}(key.blendMode.AsDWORD) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  std::unordered_map<PPKey, MTL::RenderPipelineState *, PPKeyHash>
      mPPPipelineCache;
  MTL::DepthStencilState *mDisabledDepthStencilState = nullptr;
};
