#pragma once

#include <string>
#include <vector>

#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>

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
    void AddComputePipeline(const MTL::ComputePipelineDescriptor* descriptor);

private:
    std::string GetArchivePath();
    
    MetalRenderDevice* fb;
    MTL::BinaryArchive* mArchive = nullptr;
    bool mModified = false;
};
