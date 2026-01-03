#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "metal/system/mt_renderdevice.h"
#include "mt_sampler.h"

bool MtSamplerKey::operator==(const MtSamplerKey &other) const {
  return MinFilter == other.MinFilter && MagFilter == other.MagFilter &&
         MipFilter == other.MipFilter && AddressU == other.AddressU &&
         AddressV == other.AddressV && AddressW == other.AddressW &&
         MaxAnisotropy == other.MaxAnisotropy;
}

size_t std::hash<MtSamplerKey>::operator()(const MtSamplerKey &key) const {
  size_t hash = 0;
  hash ^= std::hash<int>()(key.MinFilter);
  hash ^= std::hash<int>()(key.MagFilter) << 1;
  hash ^= std::hash<int>()(key.MipFilter) << 2;
  hash ^= std::hash<int>()(key.AddressU) << 3;
  hash ^= std::hash<int>()(key.AddressV) << 4;
  hash ^= std::hash<int>()(key.AddressW) << 5;
  hash ^= std::hash<float>()(key.MaxAnisotropy);
  return hash;
}

MtSamplerManager::MtSamplerManager(MetalRenderDevice *fb) : fb(fb) {}

MtSamplerManager::~MtSamplerManager() { ClearCache(); }

// Sampler mapping helpers
static MTL::SamplerMinMagFilter MapMinMagFilter(int filter) {
  return (filter == 0) ? MTL::SamplerMinMagFilterNearest
                       : MTL::SamplerMinMagFilterLinear;
}

static MTL::SamplerMipFilter MapMipFilter(int filter) {
  if (filter == 0) return MTL::SamplerMipFilterNotMipmapped;
  if (filter == 1) return MTL::SamplerMipFilterNearest;
  return MTL::SamplerMipFilterLinear;
}

static MTL::SamplerAddressMode MapAddressMode(int clampMode, bool isV) {
  switch (clampMode) {
  case 1: // CLAMP_X
  case 6: // CLAMP_NOFILTER_X
    return isV ? MTL::SamplerAddressModeRepeat
               : MTL::SamplerAddressModeClampToEdge;
  case 2: // CLAMP_Y
  case 7: // CLAMP_NOFILTER_Y
    return isV ? MTL::SamplerAddressModeClampToEdge
               : MTL::SamplerAddressModeRepeat;
  case 3: // CLAMP_XY
  case 4: // CLAMP_XY_NOMIP
  case 5: // CLAMP_NOFILTER
  case 8: // CLAMP_NOFILTER_XY
  case 0: // CLAMP_NONE
  case 9: // CLAMP_CAMTEX
  default:
    return MTL::SamplerAddressModeRepeat;
  }
}

MTL::SamplerState *MtSamplerManager::GetSamplerState(const MtSamplerKey &key) {
  // Check cache
  auto it = mSamplerCache.find(key);
  if (it != mSamplerCache.end()) {
    return it->second;
  }

  // Create new sampler state
  auto desc = MTL::SamplerDescriptor::alloc()->init();

  // Set filter modes
  desc->setMinFilter(MapMinMagFilter(key.MinFilter));
  desc->setMagFilter(MapMinMagFilter(key.MagFilter));
  desc->setMipFilter(MapMipFilter(key.MipFilter));

  // Set address modes
  // Use key.AddressU/V/W directly as they are usually set to the GZDoom clamp
  // mode
  desc->setSAddressMode(MapAddressMode(key.AddressU, false));
  desc->setTAddressMode(MapAddressMode(key.AddressV, true));
  desc->setRAddressMode(MapAddressMode(key.AddressW, false));

  // Set LOD clamps (Vulkan parity)
  if (key.MipFilter == 0) { // MTL::SamplerMipFilterNotMipmapped
    desc->setLodMaxClamp(0.25f);
  } else {
    desc->setLodMaxClamp(100.0f);
  }

  // Set anisotropy
  if (key.MaxAnisotropy > 1.0f) {
    desc->setMaxAnisotropy(static_cast<NS::UInteger>(key.MaxAnisotropy));
  }

  MTL::SamplerState *samplerState = fb->device->device->newSamplerState(desc);
  desc->release();

  if (samplerState) {
    mSamplerCache[key] = samplerState;
  }

  return samplerState;
}

void MtSamplerManager::ClearCache() {
  for (auto &pair : mSamplerCache) {
    if (pair.second)
      pair.second->release();
  }
  mSamplerCache.clear();
}
