#pragma once

#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

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
    
    // Feature flags for workarounds
    bool useManagedStorage = false;
    bool explicitFlushing = false;
    bool needsVertexNormalization = false;
    bool presentsWithTransaction = false; // Synchronize with Cocoa UI
    bool supportsBinaryArchives = false;
    int maxDrawableCount = 2;

    void Initialize(MTL::Device* device) {
        if (!device) return;

        // OS Version detection using NSProcessInfo (via Foundation)
        auto processInfo = NS::ProcessInfo::processInfo();
        auto version = processInfo->operatingSystemVersion();
        osMajor = (int)version.majorVersion;
        osMinor = (int)version.minorVersion;
        osPatch = (int)version.patchVersion;

        // macOS 10.15 (Catalina) and later support presentsWithTransaction
        if (osMajor > 10 || (osMajor == 10 && osMinor >= 15)) {
            presentsWithTransaction = true; 
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
            useManagedStorage = true; // Intel/AMD/NVIDIA on macOS prefer Managed for CPU-GPU sync
            if (name.find("Intel") != std::string::npos) {
                architecture = MtGPUArchitecture::Intel;
                explicitFlushing = true; // Intel Broadwell+ needs careful flushing
            } else if (name.find("NVIDIA") != std::string::npos) {
                architecture = MtGPUArchitecture::NVIDIA;
            } else if (name.find("AMD") != std::string::npos || name.find("Radeon") != std::string::npos) {
                architecture = MtGPUArchitecture::AMD;
            }
        }

        // Feature detection
        if (device->supportsFamily(MTL::GPUFamilyApple7) || device->supportsFamily(MTL::GPUFamilyMac2)) {
            supportsMemoryless = true;
        }

        // Metal Version (rough estimation based on GPU family)
        if (device->supportsFamily(MTL::GPUFamilyMac2)) metalVersion = 21;
        if (device->supportsFamily(MTL::GPUFamilyMacCatalyst2)) metalVersion = 22;
        if (device->supportsFamily(MTL::GPUFamilyApple7)) metalVersion = 23; // A14/M1
        
        // Intel specific workarounds
        if (architecture == MtGPUArchitecture::Intel) {
            needsVertexNormalization = true;
        }
    }

    MTL::StorageMode GetDynamicStorageMode() const {
        return useManagedStorage ? MTL::StorageModeManaged : MTL::StorageModeShared;
    }

    const char* GetArchName() const {
        switch (architecture) {
            case MtGPUArchitecture::AppleSilicon: return "Apple Silicon (TBDR)";
            case MtGPUArchitecture::Intel: return "Intel (IMR)";
            case MtGPUArchitecture::NVIDIA: return "NVIDIA (IMR)";
            case MtGPUArchitecture::AMD: return "AMD (IMR)";
            default: return "Unknown";
        }
    }
};
