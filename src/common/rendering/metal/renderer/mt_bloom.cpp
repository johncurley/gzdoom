#include "mt_bloom.h"
#include "../system/mt_renderdevice.h"
#include "../renderer/mt_debug.h"
#include "printf.h"
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

static const char* BLOOM_COMPUTE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct BloomParams {
    float threshold;
    float strength;
    float2 srcRes;
    float2 bloomRes;
};

kernel void bloom_extract(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> src [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / params.bloomRes;
    float4 c = src.sample(s, uv);
    float lum = dot(c.rgb, float3(0.2126,0.7152,0.0722));
    float4 res = (lum > params.threshold) ? c * (lum - params.threshold) : float4(0.0);
    out.write(res, gid);
}

kernel void blur_horizontal(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> inTex [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / params.bloomRes;
    float texelX = 1.0 / params.bloomRes.x;
    float4 sum = float4(0.0);
    sum += inTex.sample(s, uv + float2(-2.0*texelX, 0.0)) * 0.0625;
    sum += inTex.sample(s, uv + float2(-1.0*texelX, 0.0)) * 0.25;
    sum += inTex.sample(s, uv) * 0.375;
    sum += inTex.sample(s, uv + float2(1.0*texelX, 0.0)) * 0.25;
    sum += inTex.sample(s, uv + float2(2.0*texelX, 0.0)) * 0.0625;
    out.write(sum, gid);
}

kernel void blur_vertical(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> inTex [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / params.bloomRes;
    float texelY = 1.0 / params.bloomRes.y;
    float4 sum = float4(0.0);
    sum += inTex.sample(s, uv + float2(0.0, -2.0*texelY)) * 0.0625;
    sum += inTex.sample(s, uv + float2(0.0, -1.0*texelY)) * 0.25;
    sum += inTex.sample(s, uv) * 0.375;
    sum += inTex.sample(s, uv + float2(0.0, 1.0*texelY)) * 0.25;
    sum += inTex.sample(s, uv + float2(0.0, 2.0*texelY)) * 0.0625;
    out.write(sum, gid);
}

kernel void bloom_combine(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> bloomTex [[texture(0)]],
    texture2d<float, access::read_write> sceneTex [[texture(1)]])
{
    if (gid.x >= sceneTex.get_width() || gid.y >= sceneTex.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 sceneUv = (float2(gid) + 0.5) / params.srcRes;
    float2 bloomUv = (float2(gid) + 0.5) / params.bloomRes;
    float4 scene = sceneTex.read(gid);
    float4 bloom = bloomTex.sample(s, bloomUv);
    scene.rgb = scene.rgb + bloom.rgb * params.strength;
    sceneTex.write(scene, gid);
}
)";

MtBloomModule::MtBloomModule(MetalRenderDevice* device) : fb(device) {
    auto deviceObj = fb->device->device;

    auto sourceString = NS::String::string(BLOOM_COMPUTE_SOURCE, NS::UTF8StringEncoding);

    NS::Error* error = nullptr;
    auto library = deviceObj->newLibrary(sourceString, nullptr, &error);

    if (!library) {
        if (error) {
            Printf(PRINT_BOLD, "Error: Failed to compile Metal Bloom library: %s\n", error->localizedDescription()->utf8String());
        } else {
            Printf(PRINT_BOLD, "Error: Failed to compile Metal Bloom library (unknown error).\n");
        }
        return;
    }

    auto f = library->newFunction(NS::String::string("bloom_extract", NS::UTF8StringEncoding));
    if (f) { extractPSO = deviceObj->newComputePipelineState(f, (NS::Error**)nullptr); f->release(); }
    f = library->newFunction(NS::String::string("blur_horizontal", NS::UTF8StringEncoding));
    if (f) { blurHPSO = deviceObj->newComputePipelineState(f, (NS::Error**)nullptr); f->release(); }
    f = library->newFunction(NS::String::string("blur_vertical", NS::UTF8StringEncoding));
    if (f) { blurVPSO = deviceObj->newComputePipelineState(f, (NS::Error**)nullptr); f->release(); }
    f = library->newFunction(NS::String::string("bloom_combine", NS::UTF8StringEncoding));
    if (f) { combinePSO = deviceObj->newComputePipelineState(f, (NS::Error**)nullptr); f->release(); }

    library->release();
}

MtBloomModule::~MtBloomModule() {
    ReleaseTextures();
    if (extractPSO) extractPSO->release();
    if (blurHPSO) blurHPSO->release();
    if (blurVPSO) blurVPSO->release();
    if (combinePSO) combinePSO->release();
}

void MtBloomModule::CreateTextures(int width, int height, MTL::PixelFormat format) {
    if (mBloomA && mBloomB && mCachedBloomW == width && mCachedBloomH == height) return;
    ReleaseTextures();
    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(width);
    desc->setHeight(height);
    desc->setPixelFormat(format);
    desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    desc->setStorageMode(MTL::StorageModePrivate);
    mBloomA = fb->device->device->newTexture(desc);
    mBloomB = fb->device->device->newTexture(desc);
    desc->release();
    mCachedBloomW = width;
    mCachedBloomH = height;
}

void MtBloomModule::ReleaseTextures() {
    if (mBloomA) { mBloomA->release(); mBloomA = nullptr; }
    if (mBloomB) { mBloomB->release(); mBloomB = nullptr; }
    mCachedBloomW = mCachedBloomH = 0;
}

void MtBloomModule::Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* srcTex, float amount) {
    if (!extractPSO || !blurHPSO || !blurVPSO || !combinePSO || !cmdBuf || !srcTex) return;

    int srcW = (int)srcTex->width();
    int srcH = (int)srcTex->height();
    int bloomW = max(1, srcW / 2);
    int bloomH = max(1, srcH / 2);

    // Ensure cached bloom textures exist for this resolution
    auto format = srcTex->pixelFormat();
    CreateTextures(bloomW, bloomH, format);

    MTL::Texture* bloomA = mBloomA;
    MTL::Texture* bloomB = mBloomB;

    BloomParams params;
    params.threshold = 0.8f; // tunable
    params.strength = amount;
    params.srcRes[0] = (float)srcW; params.srcRes[1] = (float)srcH;
    params.bloomRes[0] = (float)bloomW; params.bloomRes[1] = (float)bloomH;

    auto encoder = cmdBuf->computeCommandEncoder();

    // 1) Extract bright areas -> bloomA
    encoder->setComputePipelineState(extractPSO);
    encoder->setBytes(&params, sizeof(BloomParams), 0);
    encoder->setTexture(srcTex, 0);
    encoder->setTexture(bloomA, 1);
    MTL::Size grid = { (NS::UInteger)bloomW, (NS::UInteger)bloomH, 1 };
    encoder->dispatchThreads(grid, MTL::Size(16,16,1));
    encoder->memoryBarrier(MTL::BarrierScopeTextures);

    // 2) Blur horizontal: bloomA -> bloomB
    encoder->setComputePipelineState(blurHPSO);
    encoder->setBytes(&params, sizeof(BloomParams), 0);
    encoder->setTexture(bloomA, 0);
    encoder->setTexture(bloomB, 1);
    encoder->dispatchThreads(grid, MTL::Size(16,16,1));
    encoder->memoryBarrier(MTL::BarrierScopeTextures);

    // 3) Blur vertical: bloomB -> bloomA
    encoder->setComputePipelineState(blurVPSO);
    encoder->setBytes(&params, sizeof(BloomParams), 0);
    encoder->setTexture(bloomB, 0);
    encoder->setTexture(bloomA, 1);
    encoder->dispatchThreads(grid, MTL::Size(16,16,1));
    encoder->memoryBarrier(MTL::BarrierScopeTextures);

    // 4) Combine bloomA back into srcTex
    encoder->setComputePipelineState(combinePSO);
    encoder->setBytes(&params, sizeof(BloomParams), 0);
    encoder->setTexture(bloomA, 0);
    encoder->setTexture(srcTex, 1);
    MTL::Size fullGrid = { (NS::UInteger)srcW, (NS::UInteger)srcH, 1 };
    encoder->dispatchThreads(fullGrid, MTL::Size(16,16,1));

    encoder->endEncoding();


}
