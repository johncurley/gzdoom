#include "mt_binaryarchive.h"
#include "mt_renderdevice.h"
#include "i_specialpaths.h"
#include "printf.h"
#include "cmdlib.h"

MtBinaryArchive::MtBinaryArchive(MetalRenderDevice* fb) : fb(fb) {}

MtBinaryArchive::~MtBinaryArchive() {
    if (mModified) {
        Save();
    }
    if (mArchive) {
        mArchive->release();
    }
}

void MtBinaryArchive::Init() {
    if (!fb->mVersionManager.supportsBinaryArchives) return;

    std::string path = GetArchivePath();
    
    auto desc = MTL::BinaryArchiveDescriptor::alloc()->init();
    
    if (FileExists(path.c_str())) {
        NS::String* nsPath = NS::String::string(path.c_str(), NS::UTF8StringEncoding);
        NS::URL* url = NS::URL::fileURLWithPath(nsPath);
        desc->setUrl(url);
        nsPath->release();
    }

    NS::Error* error = nullptr;
    mArchive = fb->device->device->newBinaryArchive(desc, &error);
    
    if (!mArchive && error) {
        Printf(PRINT_LOG, "Metal: Failed to load binary archive: %s\n", error->localizedDescription()->utf8String());
    } else if (mArchive && FileExists(path.c_str())) {
        Printf(PRINT_LOG, "Metal: Loaded binary pipeline archive from %s\n", path.c_str());
    }

    desc->release();
}

void MtBinaryArchive::Save() {
    if (!mArchive || !mModified) return;

    std::string path = GetArchivePath();
    
    // Ensure directory exists
    FString dirPart = ExtractFilePath(path.c_str());
    CreatePath(dirPart.GetChars());

    NS::String* nsPath = NS::String::string(path.c_str(), NS::UTF8StringEncoding);
    NS::URL* url = NS::URL::fileURLWithPath(nsPath);
    
    NS::Error* error = nullptr;
    if (mArchive->serializeToURL(url, &error)) {
        Printf(PRINT_LOG, "Metal: Serialized binary pipeline archive to %s\n", path.c_str());
        mModified = false;
    } else if (error) {
        Printf(PRINT_LOG, "Metal: Failed to serialize binary archive: %s\n", error->localizedDescription()->utf8String());
    }

    nsPath->release();
}

void MtBinaryArchive::AddRenderPipeline(const MTL::RenderPipelineDescriptor* descriptor) {
    if (!mArchive) return;

    NS::Error* error = nullptr;
    if (mArchive->addRenderPipelineFunctions(descriptor, &error)) {
        mModified = true;
    } else if (error) {
        // This is expected if the pipeline is already in the archive or there's a minor mismatch
        // We don't necessarily need to log every "add" failure.
    }
}

std::string MtBinaryArchive::GetArchivePath() {
    FString path = M_GetCachePath(true);
    path += "/mt_pipelines.bin";
    return path.GetChars();
}
