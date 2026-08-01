#include "i_time.h"
#include <Metal/Metal.hpp>

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
    return MTL::SamplerAddressModeClampToEdge;
  case 9: // CLAMP_CAMTEX
    // CAMTEX *repeats* on the reference backends -- vk_samplers.cpp builds it
    // with REPEAT on all three axes and says so in its comment ("CAMTEX is
    // repeating with texture filter and no mipmap"); the GL path agrees.
    // Metal previously clamped, so for u = 1 + d it returned the right-edge
    // texel where the reference returns the texel at d. Hardware canvas
    // textures select CLAMP_CAMTEX explicitly (gametexture.h), so this was
    // visible on any camera texture whose UVs cross the edge.
    return MTL::SamplerAddressModeRepeat;
  case 0: // CLAMP_NONE
  default:
    return MTL::SamplerAddressModeRepeat;
  }
}

// The CLAMP_* modes are not purely address modes on the reference backends:
// several of them also pin filtering and mipmapping (vk_samplers.cpp
// CreateHWSamplers). Metal derived filtering from gl_texture_filter alone, so
// a minified CLAMP_NOFILTER texture came out linear- or mip-filtered where the
// reference stays nearest and unmipped. Applied to the key before the cache
// lookup, so equivalent requests collapse onto one cached sampler.
static void ApplyClampModeFilterOverrides(MtSamplerKey &key) {
  switch (key.AddressU) {
  case 5: // CLAMP_NOFILTER
  case 6: // CLAMP_NOFILTER_X
  case 7: // CLAMP_NOFILTER_Y
  case 8: // CLAMP_NOFILTER_XY
    // Reference: VK_FILTER_NEAREST min+mag, mip NEAREST, MaxLod 0.25.
    key.MinFilter = 0;
    key.MagFilter = 0;
    key.MipFilter = 0;
    break;
  case 4: // CLAMP_XY_NOMIP
  case 9: // CLAMP_CAMTEX
    // Reference: keeps the configured texture filter but disables mipmapping
    // (mip NEAREST + MaxLod 0.25). It also uses magFilter for BOTH min and
    // mag on these two samplers; mirrored here deliberately -- parity with the
    // reference matters more than whether that asymmetry was intentional.
    key.MinFilter = key.MagFilter;
    key.MipFilter = 0;
    break;
  default:
    break;
  }
}

MTL::SamplerState *MtSamplerManager::GetSamplerState(const MtSamplerKey &rawKey) {
  MtSamplerKey key = rawKey;
  ApplyClampModeFilterOverrides(key);

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
