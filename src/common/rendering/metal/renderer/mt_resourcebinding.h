#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace MTL {
class Texture;
class Buffer;
class SamplerState;
class RenderCommandEncoder;
} // namespace MTL

class MetalRenderDevice;

// Three-tier resource binding strategy (adapted from Vulkan)
// Tier 0: Fixed textures (shadowmap, lightmap, RTX)
// Tier 1: Per-frame buffers (viewpoint, matrices, stream data, lights, bones)
// Tier 2: Per-material textures (texture layers)

class MtResourceBindingManager {
public:
  MtResourceBindingManager(MetalRenderDevice *fb);
  ~MtResourceBindingManager();

  // Tier 0: Fixed textures (bind once at init)
  void BindFixedTexture(int slot, MTL::Texture *texture);

  // Tier 1: Per-frame buffers (update offsets per-frame)
  void BindPerFrameBuffer(int slot, MTL::Buffer *buffer, uint32_t offset);

  // Tier 2: Per-material textures (bind per-material)
  void BindMaterialTexture(int slot, MTL::Texture *texture,
                           MTL::SamplerState *sampler);

  // Apply bindings to encoder
  void ApplyBindings(MTL::RenderCommandEncoder *encoder, bool vertex,
                     bool fragment);

  // Reset for new frame
  void BeginFrame();

  // Reset tracking for a new encoder
  void ResetEncoderState();

  // Clear material texture bindings (called when switching materials)
  void ClearMaterialTextures();

private:
  MetalRenderDevice *fb = nullptr;

  // Tier 0: Fixed textures
  std::vector<MTL::Texture *> mFixedTextures;

  // Tier 1: Per-frame buffers
  struct BufferBinding {
    MTL::Buffer *buffer = nullptr;
    uint32_t offset = 0;
  };
  std::vector<BufferBinding> mPerFrameBuffers;

  // Tier 2: Per-material textures
  struct TextureBinding {
    MTL::Texture *texture = nullptr;
    MTL::SamplerState *sampler = nullptr;
  };
  std::vector<TextureBinding> mMaterialTextures;

  // Redundant state tracking (Shadow State)
  // We track vertex and fragment stages separately as Metal requires it
  struct State {
    MTL::Texture *textures[32] = {};
    MTL::SamplerState *samplers[32] = {};
    MTL::Buffer *buffers[32] = {};
    uint32_t offsets[32] = {};
  } mVertexState, mFragmentState;
};
