#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "metal/system/mt_renderdevice.h"
#include "mt_resourcebinding.h"
#include "c_cvars.h"
#include "printf.h"

EXTERN_CVAR(Bool, mt_debug)

MtResourceBindingManager::MtResourceBindingManager(MetalRenderDevice *fb)
    : fb(fb) {}
MtResourceBindingManager::~MtResourceBindingManager() {}

void MtResourceBindingManager::BindFixedTexture(int slot,
                                                MTL::Texture *texture) {
  if (slot >= (int)mFixedTextures.size())
    mFixedTextures.resize(slot + 1);
  mFixedTextures[slot] = texture;
}

void MtResourceBindingManager::BindPerFrameBuffer(int slot, MTL::Buffer *buffer,
                                                  uint32_t offset) {
  if (slot >= (int)mPerFrameBuffers.size())
    mPerFrameBuffers.resize(slot + 1);
  mPerFrameBuffers[slot].buffer = buffer;
  mPerFrameBuffers[slot].offset = offset;
}

void MtResourceBindingManager::BindMaterialTexture(int slot,
                                                   MTL::Texture *texture,
                                                   MTL::SamplerState *sampler) {
  if (slot >= (int)mMaterialTextures.size())
    mMaterialTextures.resize(slot + 1);
  mMaterialTextures[slot].texture = texture;
  mMaterialTextures[slot].sampler = sampler;
}

void MtResourceBindingManager::ApplyBindings(MTL::RenderCommandEncoder *encoder,
                                             bool vertex, bool fragment) {
  if (!encoder)
    return;

  // Tier 0: Bind fixed textures (shadowmaps, lightmaps, etc.)
  // These are typically bound once and rarely change
  for (size_t i = 0; i < mFixedTextures.size(); i++) {
    if (mFixedTextures[i]) {
      MTL::Texture *texture = mFixedTextures[i];
      if (vertex)
        encoder->setVertexTexture(texture, i);
      if (fragment)
        encoder->setFragmentTexture(texture, i);
    }
  }

  // Tier 1: Bind per-frame buffers (viewpoint, matrices, stream data, lights,
  // bones) These update every frame but use dynamic offsets for efficiency
  for (size_t i = 0; i < mPerFrameBuffers.size(); i++) {
    if (mPerFrameBuffers[i].buffer) {
      MTL::Buffer *buffer = mPerFrameBuffers[i].buffer;
      NS::UInteger offset = mPerFrameBuffers[i].offset;

      if (vertex)
        encoder->setVertexBuffer(buffer, offset, (NS::UInteger)i);
      if (fragment)
        encoder->setFragmentBuffer(buffer, offset, (NS::UInteger)i);
    }
  }

  // Tier 2: Bind per-material textures (texture layers)
  // These change per-material during rendering
  for (size_t i = 0; i < mMaterialTextures.size(); i++) {
    if (mMaterialTextures[i].texture) {
      MTL::Texture *texture = mMaterialTextures[i].texture;
      MTL::SamplerState *sampler = mMaterialTextures[i].sampler;

      if (vertex) {
        encoder->setVertexTexture(texture, i);
        if (sampler)
          encoder->setVertexSamplerState(sampler, i);
      }
      if (fragment) {
        encoder->setFragmentTexture(texture, i);
        if (sampler)
          encoder->setFragmentSamplerState(sampler, i);
      }
    }
  }
}

void MtResourceBindingManager::BeginFrame() {
  // Clear material textures (Tier 2) - these are rebound every draw
  // Keep fixed textures (Tier 0) and per-frame buffers (Tier 1) intact
  mMaterialTextures.clear();
}

void MtResourceBindingManager::ResetEncoderState() {
  // Current implementation doesn't track redundant state across encoders,
  // so this is a no-op for now.
}

void MtResourceBindingManager::ClearMaterialTextures() {
  // Clear all material texture bindings (called when switching materials)
  mMaterialTextures.clear();
}
