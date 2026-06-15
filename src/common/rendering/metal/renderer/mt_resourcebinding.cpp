#include "i_time.h"
#include <Metal/Metal.hpp>

#include "metal/system/mt_renderdevice.h"
#include "mt_resourcebinding.h"
#include "metal/textures/mt_sampler.h"
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
  for (size_t i = 0; i < mFixedTextures.size(); i++) {
    MTL::Texture *texture = mFixedTextures[i];
    if (!texture) continue;

    if (vertex && mVertexState.textures[i] != texture) {
      encoder->setVertexTexture(texture, i);
      mVertexState.textures[i] = texture;
    }

    if (fragment) {
      if (mFragmentState.textures[i] != texture) {
        encoder->setFragmentTexture(texture, i);
        mFragmentState.textures[i] = texture;
      }

      // Handle samplers for fixed slots
      MtSamplerKey sk;
      if ((int)i == 14) {
        sk.MinFilter = 0; sk.MagFilter = 0; sk.MipFilter = 0; sk.AddressU = 3; sk.AddressV = 3; sk.AddressW = 3; sk.MaxAnisotropy = 1.0f;
      } else if ((int)i == 15) {
        sk.MinFilter = 1; sk.MagFilter = 1; sk.MipFilter = 0; sk.AddressU = 3; sk.AddressV = 3; sk.AddressW = 3; sk.MaxAnisotropy = 1.0f;
      } else {
        sk.MinFilter = 1; sk.MagFilter = 1; sk.MipFilter = 0; sk.AddressU = 3; sk.AddressV = 3; sk.AddressW = 3; sk.MaxAnisotropy = 1.0f;
      }
      
      MTL::SamplerState *sampler = fb->GetSamplerManager()->GetSamplerState(sk);
      if (sampler) {
        if (mFragmentState.samplers[i] != sampler) {
          encoder->setFragmentSamplerState(sampler, i);
          mFragmentState.samplers[i] = sampler;
        }
        if (vertex && mVertexState.samplers[i] != sampler) {
          encoder->setVertexSamplerState(sampler, i);
          mVertexState.samplers[i] = sampler;
        }
      }
    }
  }

  // Tier 1: Bind per-frame buffers
  for (size_t i = 0; i < mPerFrameBuffers.size(); i++) {
    MTL::Buffer *buffer = mPerFrameBuffers[i].buffer;
    if (!buffer) continue;
    uint32_t offset = mPerFrameBuffers[i].offset;

    if (vertex && (mVertexState.buffers[i] != buffer || mVertexState.offsets[i] != offset)) {
      encoder->setVertexBuffer(buffer, (NS::UInteger)offset, (NS::UInteger)i);
      mVertexState.buffers[i] = buffer;
      mVertexState.offsets[i] = offset;
    }
    if (fragment && (mFragmentState.buffers[i] != buffer || mFragmentState.offsets[i] != offset)) {
      encoder->setFragmentBuffer(buffer, (NS::UInteger)offset, (NS::UInteger)i);
      mFragmentState.buffers[i] = buffer;
      mFragmentState.offsets[i] = offset;
    }
  }

  // Tier 2: Bind per-material textures
  for (size_t i = 0; i < mMaterialTextures.size(); i++) {
    MTL::Texture *texture = mMaterialTextures[i].texture;
    if (!texture) continue;
    MTL::SamplerState *sampler = mMaterialTextures[i].sampler;

    if (vertex) {
      if (mVertexState.textures[i] != texture) {
        encoder->setVertexTexture(texture, i);
        mVertexState.textures[i] = texture;
      }
      if (sampler && mVertexState.samplers[i] != sampler) {
        encoder->setVertexSamplerState(sampler, i);
        mVertexState.samplers[i] = sampler;
      }
    }
    if (fragment) {
      if (mFragmentState.textures[i] != texture) {
        encoder->setFragmentTexture(texture, i);
        mFragmentState.textures[i] = texture;
      }
      if (sampler && mFragmentState.samplers[i] != sampler) {
        encoder->setFragmentSamplerState(sampler, i);
        mFragmentState.samplers[i] = sampler;
      }
    }
  }
}

void MtResourceBindingManager::BeginFrame() {
  // Clear material textures (Tier 2) - these are rebound every draw
  mMaterialTextures.clear();
}

void MtResourceBindingManager::ResetEncoderState() {
  // Clear tracking caches when starting a new encoder
  memset(&mVertexState, 0, sizeof(mVertexState));
  memset(&mFragmentState, 0, sizeof(mFragmentState));
}

void MtResourceBindingManager::ClearMaterialTextures() {
  // Clear all material texture bindings (called when switching materials)
  mMaterialTextures.clear();
}
