#pragma once

#ifdef __APPLE__
#include <Metal/Metal.hpp>
#endif

#include <string>

enum class MtGPUArchitecture {
  Unknown,
  AppleSilicon, // TBDR (Tile-Based Deferred Renderer)
  Intel,        // IMR (Immediate Mode Renderer)
  NVIDIA,       // IMR
  AMD           // IMR
};

struct MtVersionManager {
  MtGPUArchitecture architecture = MtGPUArchitecture::Unknown;
  int metalVersion = 20; // Default 2.0
  int osMajor = 0;
  int osMinor = 0;
  int osPatch = 0;
  bool isTBDR = false;
  bool supportsMemoryless = false;
  bool supportsAppleGPU = false;
  bool supportsRGB10A2 = false;
  bool supportsReadWriteBGRA8 = false;
  bool supportsArgumentBuffers = false;
  int argumentBufferTier = 0;
  bool supportsSIMDGroup = false;
  bool supportsNonUniformThreadgroups = false;

  // Feature flags for workarounds
  bool useManagedStorage = false;
  bool needsVertexNormalization = false;
  bool presentsWithTransaction = false; // Synchronize with Cocoa UI
  bool supportsBinaryArchives = false;
  int maxDrawableCount = 2;
  // MTLCommandBuffer::GPUStartTime()/GPUEndTime() require macOS 10.15+.
  bool supportsGPUTimestamps = false;
  // Whether this GPU can place a counter-sampling point at a render/compute
  // encoder's stage boundary (MTLCounterSampleBuffer). Needed for real
  // per-pass GPU timing (opaque geo / sky / AO / bloom / composite);
  // whole-frame timing above doesn't need this. Queried via
  // supportsCounterSampling(), which is safe to call on any OS/GPU (uses a
  // respondsToSelector-guarded message send, not a raw one).
  bool supportsStageCounterSampling = false;

#ifdef __APPLE__
  void Initialize(MTL::Device *device) {
    if (!device)
      return;
    // OS Version detection using NSProcessInfo (via Foundation)
    auto processInfo = NS::ProcessInfo::processInfo();
    auto version = processInfo->operatingSystemVersion();
    osMajor = (int)version.majorVersion;
    osMinor = (int)version.minorVersion;
    osPatch = (int)version.patchVersion;

    // macOS 10.15 (Catalina) and later support presentsWithTransaction and
    // MTLCommandBuffer GPU timestamp queries.
    if (osMajor > 10 || (osMajor == 10 && osMinor >= 15)) {
      presentsWithTransaction = true;
      supportsGPUTimestamps = true;
    }

    // Binary archives require macOS 11.0 (Big Sur)
    if (osMajor >= 11) {
      supportsBinaryArchives = true;
      maxDrawableCount = 3; // Triple buffering at layer level
    }

    std::string name = device->name()->utf8String();

    // Architecture detection
    if (device->supportsFamily(MTL::GPUFamilyApple1)) {
      architecture = MtGPUArchitecture::AppleSilicon;
      isTBDR = true;
      supportsAppleGPU = true;
      useManagedStorage = false; // Apple uses Shared
    } else {
      useManagedStorage =
          true; // Intel/AMD/NVIDIA on macOS prefer Managed for CPU-GPU sync
      if (name.find("Intel") != std::string::npos) {
        architecture = MtGPUArchitecture::Intel;
      } else if (name.find("NVIDIA") != std::string::npos) {
        architecture = MtGPUArchitecture::NVIDIA;
      } else if (name.find("AMD") != std::string::npos ||
                 name.find("Radeon") != std::string::npos) {
        architecture = MtGPUArchitecture::AMD;
      }
    }

    // Feature detection
    // Enable memoryless for Apple Silicon if requested (will be used in
    // MtRenderBuffers)
    supportsMemoryless = (architecture == MtGPUArchitecture::AppleSilicon);

    if (device->supportsFamily(MTL::GPUFamilyApple1) ||
        device->supportsFamily(MTL::GPUFamilyMac2)) {
      // Most modern Metal GPUs support RGB10A2
      supportsRGB10A2 = true;
    }

    // Per-pass GPU timing capability probe -- safe to call unconditionally,
    // supportsCounterSampling() guards its own message send.
    supportsStageCounterSampling =
        device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary);

    // Metal Version (rough estimation based on GPU family)
    if (device->supportsFamily(MTL::GPUFamilyMac2))
      metalVersion = 21;
    if (device->supportsFamily(MTL::GPUFamilyMacCatalyst2))
      metalVersion = 22;
    if (device->supportsFamily(MTL::GPUFamilyApple7))
      metalVersion = 23; // A14/M1

    // Read-write texture support tier: Tier2 enables writing to common formats like BGRA8Unorm
    // MTL::ReadWriteTextureTier2 indicates broader read-write format support.
    auto rwTier = device->readWriteTextureSupport();
    supportsReadWriteBGRA8 = (rwTier == MTL::ReadWriteTextureTier2);

    // Argument Buffers detection
    auto argTier = device->argumentBuffersSupport();
    supportsArgumentBuffers = (argTier != MTL::ArgumentBuffersTier1); // Tier 1 is limited, Tier 2 is preferred
    argumentBufferTier = (int)argTier;

    // SIMDGroup and Non-uniform threadgroup support
    if (device->supportsFamily(MTL::GPUFamilyApple4) || device->supportsFamily(MTL::GPUFamilyMac2)) {
        supportsSIMDGroup = true;
        supportsNonUniformThreadgroups = true;
    }

    // Intel specific workarounds
    if (architecture == MtGPUArchitecture::Intel) {
      needsVertexNormalization = true;
    }
  }

  MTL::StorageMode GetDynamicStorageMode() const {
    return useManagedStorage ? MTL::StorageModeManaged : MTL::StorageModeShared;
  }
#endif // __APPLE__

  const char *GetArchName() const {
    switch (architecture) {
    case MtGPUArchitecture::AppleSilicon:
      return "Apple Silicon (TBDR)";
    case MtGPUArchitecture::Intel:
      return "Intel (IMR)";
    case MtGPUArchitecture::NVIDIA:
      return "NVIDIA (IMR)";
    case MtGPUArchitecture::AMD:
      return "AMD (IMR)";
    default:
      return "Unknown";
    }
  }
};
