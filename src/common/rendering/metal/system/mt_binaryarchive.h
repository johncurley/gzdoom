#pragma once

#include <string>
#include <vector>

#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#undef TimeScale

class MetalRenderDevice;

class MtBinaryArchive {
public:
    MtBinaryArchive(MetalRenderDevice* fb);
    ~MtBinaryArchive();

    void Init();
    void Save();

    MTL::BinaryArchive* GetArchive() const { return mArchive; }
    
    // Add a pipeline to the archive for future serialization
    void AddRenderPipeline(const MTL::RenderPipelineDescriptor* descriptor);

private:
    std::string GetArchivePath();
    
    MetalRenderDevice* fb;
    MTL::BinaryArchive* mArchive = nullptr;
    bool mModified = false;
};
