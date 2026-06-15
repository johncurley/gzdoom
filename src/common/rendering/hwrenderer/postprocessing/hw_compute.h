#pragma once

#include <cstdint>

// Backend-neutral identities for postprocess effects that can be implemented
// by native compute paths. Backends map these to their own pipeline and metric
// systems without changing the higher-level effect vocabulary.
enum class HWComputeEffect : uint8_t {
  AmbientOcclusion,
  Bloom,
};

