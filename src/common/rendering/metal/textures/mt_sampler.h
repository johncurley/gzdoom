#pragma once

#include <memory>
#include <unordered_map>

#define TimeScale TimeScale_GZDOOM
namespace MTL {
class SamplerState;
} // namespace MTL
#undef TimeScale

class MetalRenderDevice;

// Sampler state key for caching
struct MtSamplerKey {
  int MinFilter = 0;
  int MagFilter = 0;
  int MipFilter = 0;
  int AddressU = 0;
  int AddressV = 0;
  int AddressW = 0;
  float MaxAnisotropy = 1.0f;

  bool operator==(const MtSamplerKey &other) const;
  bool operator!=(const MtSamplerKey &other) const { return !(*this == other); }
};

// Hash function for sampler key
namespace std {
template <> struct hash<MtSamplerKey> {
  size_t operator()(const MtSamplerKey &key) const;
};
} // namespace std

// Sampler manager
class MtSamplerManager {
public:
  MtSamplerManager(MetalRenderDevice *fb);
  ~MtSamplerManager();

  // Get or create sampler state
  MTL::SamplerState *GetSamplerState(const MtSamplerKey &key);

  // Clear cache
  void ClearCache();

private:
  MetalRenderDevice *fb = nullptr;

  std::unordered_map<MtSamplerKey, MTL::SamplerState *> mSamplerCache;
};
