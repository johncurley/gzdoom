#include "mt_bloom.h"
#include "../system/mt_renderdevice.h"
#include "../renderer/mt_debug.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/textures/mt_sampler.h"
#include "printf.h"
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <algorithm>
#include <chrono>

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

kernel void downsample_box(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> src [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / float2(out.get_width(), out.get_height());
    float4 c = src.sample(s, uv);
    out.write(c, gid);
}

// Compute kernel that writes the bloom contribution (no direct scene writes).
kernel void bloom_combine_contrib(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> bloomTex [[texture(0)]],
    texture2d<float, access::write> outTex [[texture(1)]])
{
    if (gid.x >= outTex.get_width() || gid.y >= outTex.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 bloomUv = (float2(gid) + 0.5) / params.bloomRes;
    float4 bloom = bloomTex.sample(s, bloomUv);
    float4 contrib = float4(bloom.rgb * params.strength, 1.0);
    outTex.write(contrib, gid);
}

// Compute kernel that writes directly into the scene render target (RW BGRA8) on Tier 2 devices.
kernel void bloom_combine_rw(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::read_write> sceneTex [[texture(0)]],
    texture2d<float, access::sample> bloomTex [[texture(1)]])
{
    if (gid.x >= sceneTex.get_width() || gid.y >= sceneTex.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 bloomUv = (float2(gid) + 0.5) / params.bloomRes;
    float4 bloom = bloomTex.sample(s, bloomUv);
    float4 scene = sceneTex.read(gid);
    scene.rgb += bloom.rgb * params.strength;
    sceneTex.write(scene, gid);
}

// Fullscreen triangle vertex and composite fragment shader
struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut bloom_vs(uint vid [[vertex_id]]) {
    VSOut out;
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    out.position = float4(pos[vid], 0.0, 1.0);
    // uv in 0..1
    out.uv = out.position.xy * 0.5 + float2(0.5);
    return out;
}

fragment float4 bloom_fs(VSOut in [[stage_in]],
                         constant BloomParams &params [[buffer(0)]],
                         texture2d<float, access::sample> bloomTex [[texture(0)]],
                         sampler samp [[sampler(0)]]) {
    // Map fullscreen uv to bloom texture coordinates (bloomTex is lower resolution)
    float2 frag = in.uv * params.srcRes;
    float2 bloomUv = (frag + 0.5) / params.bloomRes;
    float4 bloom = bloomTex.sample(samp, bloomUv);
    // Output bloom contribution; additive blending will be used when rendering.
    return float4(bloom.rgb * params.strength, 1.0);
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
    f = library->newFunction(NS::String::string("downsample_box", NS::UTF8StringEncoding));
    if (f) { downsamplePSO = deviceObj->newComputePipelineState(f, (NS::Error**)nullptr); f->release(); }

    // Create compute combine pipeline to write bloom contributions into a temporary texture
    f = library->newFunction(NS::String::string("bloom_combine_contrib", NS::UTF8StringEncoding));
    if (f) { combinePSO = deviceObj->newComputePipelineState(f, (NS::Error**)nullptr); f->release(); }

    // Create compute pipeline that writes directly into the scene color (read-write) for Tier 2 devices
    f = library->newFunction(NS::String::string("bloom_combine_rw", NS::UTF8StringEncoding));
    if (f) { combineRWPSO = deviceObj->newComputePipelineState(f, (NS::Error**)nullptr); f->release(); }

    // Create composite render pipeline (vertex + fragment) for additive bloom compose
    auto vert = library->newFunction(NS::String::string("bloom_vs", NS::UTF8StringEncoding));
    auto frag = library->newFunction(NS::String::string("bloom_fs", NS::UTF8StringEncoding));
    if (vert && frag) {
        NS::Error* rpErr = nullptr;
        auto rpDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        rpDesc->setVertexFunction(vert);
        rpDesc->setFragmentFunction(frag);
        auto ca = rpDesc->colorAttachments()->object(0);
        ca->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        ca->setBlendingEnabled(true);
        ca->setRgbBlendOperation(MTL::BlendOperationAdd);
        ca->setAlphaBlendOperation(MTL::BlendOperationAdd);
        ca->setSourceRGBBlendFactor(MTL::BlendFactorOne);
        ca->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
        ca->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
        compositePSO = deviceObj->newRenderPipelineState(rpDesc, &rpErr);
        if (rpErr) {
            if (rpErr->localizedDescription())
                Printf(PRINT_BOLD, "MtBloomModule: composite pipeline error: %s\n", rpErr->localizedDescription()->utf8String());
            rpErr->release();
        }
        rpDesc->release();
    }
    if (vert) vert->release();
    if (frag) frag->release();

    library->release();
}

MtBloomModule::~MtBloomModule() {
    ReleaseTextures();
    if (extractPSO) extractPSO->release();
    if (downsamplePSO) downsamplePSO->release();
    if (blurHPSO) blurHPSO->release();
    if (blurVPSO) blurVPSO->release();
    if (combinePSO) combinePSO->release();
    if (combineRWPSO) combineRWPSO->release();
    if (compositePSO) compositePSO->release();
    if (mCompositeTex) { mCompositeTex->release(); mCompositeTex = nullptr; }
}

void MtBloomModule::CreateTextures(int width, int height, MTL::PixelFormat format) {
    if (mBloomA && mBloomB && mCachedBloomW == width && mCachedBloomH == height) return;
    ReleaseTextures();
    auto desc = MTL::TextureDescriptor::alloc()->init();
    // Use a high-precision float format for compute-friendly bloom buffers
    desc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    desc->setStorageMode(MTL::StorageModePrivate);

    // Primary bloom ping-pong textures
    desc->setWidth(width);
    desc->setHeight(height);
    mBloomA = fb->device->device->newTexture(desc);
    mBloomB = fb->device->device->newTexture(desc);

    // Create a small mip chain for multi-scale bloom (half, quarter, eighth)
    const int mipLevels = 3;
    for (int i = 1; i <= mipLevels; ++i) {
        int w2 = std::max(1, width >> i);
        int h2 = std::max(1, height >> i);
        desc->setWidth(w2);
        desc->setHeight(h2);
        MTL::Texture* t = fb->device->device->newTexture(desc);
        mDownsampledTextures.push_back(t);
        // Allocate a temporary ping-pong texture for this mip level
        MTL::Texture* tmp = fb->device->device->newTexture(desc);
        mDownsampledTempTextures.push_back(tmp);
    }

    desc->release();
    mCachedBloomW = width;
    mCachedBloomH = height;
}

void MtBloomModule::ReleaseTextures() {
    if (mBloomA) { mBloomA->release(); mBloomA = nullptr; }
    if (mBloomB) { mBloomB->release(); mBloomB = nullptr; }
    for (auto *t : mDownsampledTextures) { if (t) t->release(); }
    mDownsampledTextures.clear();
    for (auto *t : mDownsampledTempTextures) { if (t) t->release(); }
    mDownsampledTempTextures.clear();
    if (mTempBlurTexture) { mTempBlurTexture->release(); mTempBlurTexture = nullptr; }
    if (mCompositeTex) { mCompositeTex->release(); mCompositeTex = nullptr; }
    mCachedBloomW = mCachedBloomH = 0;
    mCompositeW = mCompositeH = 0;
}

void MtBloomModule::Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* srcTex, float amount) {
    auto tStart = std::chrono::high_resolution_clock::now();
    if (!extractPSO || !blurHPSO || !blurVPSO || !cmdBuf || !srcTex) return;

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


    // 5) Downsample primary bloom into mip levels and combine them with reduced strength
    for (size_t i = 0; i < mDownsampledTextures.size(); ++i) {
        MTL::Texture* dst = mDownsampledTextures[i];
        if (!dst) continue;

        // Downsample: bloomA -> dst
        encoder->setComputePipelineState(downsamplePSO);
        BloomParams downParams = params;
        downParams.bloomRes[0] = (float)dst->width(); downParams.bloomRes[1] = (float)dst->height();
        encoder->setBytes(&downParams, sizeof(BloomParams), 0);
        encoder->setTexture(bloomA, 0);
        encoder->setTexture(dst, 1);
        MTL::Size dstGrid = { (NS::UInteger)dst->width(), (NS::UInteger)dst->height(), 1 };
        encoder->dispatchThreads(dstGrid, MTL::Size(16,16,1));
        encoder->memoryBarrier(MTL::BarrierScopeTextures);

        // Blur the downsampled texture in-place using the temp ping-pong texture:
        MTL::Texture* tmp = mDownsampledTempTextures[i];
        // Horizontal blur: dst -> tmp
        encoder->setComputePipelineState(blurHPSO);
        encoder->setBytes(&downParams, sizeof(BloomParams), 0);
        encoder->setTexture(dst, 0);
        encoder->setTexture(tmp, 1);
        encoder->dispatchThreads(dstGrid, MTL::Size(16,16,1));
        encoder->memoryBarrier(MTL::BarrierScopeTextures);
        // Vertical blur: tmp -> dst
        encoder->setComputePipelineState(blurVPSO);
        encoder->setBytes(&downParams, sizeof(BloomParams), 0);
        encoder->setTexture(tmp, 0);
        encoder->setTexture(dst, 1);
        encoder->dispatchThreads(dstGrid, MTL::Size(16,16,1));
        encoder->memoryBarrier(MTL::BarrierScopeTextures);

        // Combine downsampled into scene with reduced strength
        BloomParams combParams = downParams;
        combParams.strength = amount * (0.5f / (float)(i + 1));
        combParams.srcRes[0] = (float)srcW; combParams.srcRes[1] = (float)srcH;
    }

    // Finish initial compute encoder before creating another one
    encoder->endEncoding();

    // Combine bloomA and downsampled textures: either write directly into scene on Tier2 devices,
    // or compute into a temp composite texture and render-add it into the scene.
    MtSamplerKey sk; sk.MinFilter = 1; sk.MagFilter = 1; sk.MipFilter = 0; sk.AddressU = 3; sk.AddressV = 3; sk.AddressW = 3; sk.MaxAnisotropy = 1;
    auto sampler = fb->GetSamplerManager()->GetSamplerState(sk);

    // Full-resolution dispatch grid
    MTL::Size fullGrid = { (NS::UInteger)srcW, (NS::UInteger)srcH, 1 };

    if (fb->mVersionManager.supportsReadWriteBGRA8 && combineRWPSO) {
        // Fast-path: compute directly writes into the scene color (requires Tier 2)
        auto combEnc = cmdBuf->computeCommandEncoder();
        combEnc->setComputePipelineState(combineRWPSO);

        // Main bloom level
        BloomParams bp = params;
        bp.bloomRes[0] = (float)bloomW; bp.bloomRes[1] = (float)bloomH;
        bp.srcRes[0] = (float)srcW; bp.srcRes[1] = (float)srcH;
        bp.strength = amount;
        combEnc->setBytes(&bp, sizeof(BloomParams), 0);
        combEnc->setTexture(srcTex, 0);
        combEnc->setTexture(bloomA, 1);
        combEnc->dispatchThreads(fullGrid, MTL::Size(16,16,1));
        combEnc->memoryBarrier(MTL::BarrierScopeTextures);

        // Downsampled levels
        for (size_t i = 0; i < mDownsampledTextures.size(); ++i) {
            MTL::Texture* dst = mDownsampledTextures[i];
            if (!dst) continue;
            BloomParams downParams = params;
            downParams.bloomRes[0] = (float)dst->width(); downParams.bloomRes[1] = (float)dst->height();
            downParams.srcRes[0] = (float)srcW; downParams.srcRes[1] = (float)srcH;
            downParams.strength = amount * (0.5f / (float)(i + 1));
            combEnc->setBytes(&downParams, sizeof(BloomParams), 0);
            combEnc->setTexture(srcTex, 0);
            combEnc->setTexture(dst, 1);
            combEnc->dispatchThreads(fullGrid, MTL::Size(16,16,1));
            combEnc->memoryBarrier(MTL::BarrierScopeTextures);
        }

        combEnc->endEncoding();
    } else if (compositePSO && combinePSO) {
        // Create a temporary high-precision target for compute contributions if needed
        if (!mCompositeTex || mCompositeW != srcW || mCompositeH != srcH) {
            if (mCompositeTex) { mCompositeTex->release(); mCompositeTex = nullptr; }
            auto desc2 = MTL::TextureDescriptor::alloc()->init();
            desc2->setWidth(srcW);
            desc2->setHeight(srcH);
            desc2->setPixelFormat(MTL::PixelFormatRGBA16Float);
            desc2->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
            desc2->setStorageMode(MTL::StorageModePrivate);
            mCompositeTex = fb->device->device->newTexture(desc2);
            desc2->release();
            mCompositeW = srcW; mCompositeH = srcH;
        }

        // Helper lambda to run compute combine then render composite
        auto doCombineAndComposite = [&](MTL::Texture* srcBloom, const BloomParams &localParams) {
            // Compute: write bloom contribution into mCompositeTex
            auto combEnc = cmdBuf->computeCommandEncoder();
            combEnc->setComputePipelineState(combinePSO);
            combEnc->setBytes(&localParams, sizeof(BloomParams), 0);
            combEnc->setTexture(srcBloom, 0);
            combEnc->setTexture(mCompositeTex, 1);
            combEnc->dispatchThreads(fullGrid, MTL::Size(16,16,1));
            combEnc->memoryBarrier(MTL::BarrierScopeTextures);
            combEnc->endEncoding();

            // Render-pass: add mCompositeTex into the scene
            auto mtRenderState = static_cast<MtRenderState *>(fb->GetRenderState());
            mtRenderState->SetRenderTarget(srcTex, nullptr, srcW, srcH, (int)srcTex->pixelFormat(), 1);
            mtRenderState->EnableDrawBuffers(1, false);
            mtRenderState->SetViewport(0, 0, srcW, srcH);
            mtRenderState->SetScissor(0, 0, srcW, srcH);
            mtRenderState->BeginRenderPass();
            auto renc = mtRenderState->GetEncoder();
            if (renc) {
                renc->setRenderPipelineState(compositePSO);
                renc->setFragmentBytes(&localParams, sizeof(BloomParams), 0);
                renc->setFragmentTexture(mCompositeTex, 0);
                if (sampler) renc->setFragmentSamplerState(sampler, 0);
                renc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
            }
            mtRenderState->EndRenderPass();
        };

        // First: main bloomA at half resolution (will be sampled appropriately)
        BloomParams bp = params;
        bp.bloomRes[0] = (float)bloomW; bp.bloomRes[1] = (float)bloomH;
        bp.srcRes[0] = (float)srcW; bp.srcRes[1] = (float)srcH;
        bp.strength = amount;
        doCombineAndComposite(bloomA, bp);

        // Then: each downsampled level with reduced strength
        for (size_t i = 0; i < mDownsampledTextures.size(); ++i) {
            MTL::Texture* dst = mDownsampledTextures[i];
            if (!dst) continue;
            BloomParams downParams = params;
            downParams.bloomRes[0] = (float)dst->width(); downParams.bloomRes[1] = (float)dst->height();
            downParams.srcRes[0] = (float)srcW; downParams.srcRes[1] = (float)srcH;
            downParams.strength = amount * (0.5f / (float)(i + 1));
            doCombineAndComposite(dst, downParams);
        }
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(tEnd - tStart).count();
    Printf(PRINT_LOG, "MtBloomModule::Execute encode time: %.3f ms, bloom %dx%d, mips %d\n", elapsedMs, bloomW, bloomH, (int)mDownsampledTextures.size());


}

