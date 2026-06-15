#include "mt_bloom.h"
#include "../system/mt_renderdevice.h"
#include "../shaders/mt_shader.h"
#include "../renderer/mt_debug.h"
#include "metal/renderer/mt_compute.h"
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

struct BloomCompositeParams {
    float strength[4];
    float2 srcRes;
    float2 bloomRes[4];
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

kernel void bloom_combine_contrib_all(
    uint2 gid [[thread_position_in_grid]],
    constant BloomCompositeParams &params [[buffer(0)]],
    texture2d<float, access::sample> bloom0 [[texture(0)]],
    texture2d<float, access::sample> bloom1 [[texture(1)]],
    texture2d<float, access::sample> bloom2 [[texture(2)]],
    texture2d<float, access::sample> bloom3 [[texture(3)]],
    texture2d<float, access::write> outTex [[texture(4)]])
{
    if (gid.x >= outTex.get_width() || gid.y >= outTex.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 pixel = float2(gid) + 0.5;
    float3 sum = float3(0.0);
    if (params.strength[0] > 0.0) sum += bloom0.sample(s, pixel / params.bloomRes[0]).rgb * params.strength[0];
    if (params.strength[1] > 0.0) sum += bloom1.sample(s, pixel / params.bloomRes[1]).rgb * params.strength[1];
    if (params.strength[2] > 0.0) sum += bloom2.sample(s, pixel / params.bloomRes[2]).rgb * params.strength[2];
    if (params.strength[3] > 0.0) sum += bloom3.sample(s, pixel / params.bloomRes[3]).rgb * params.strength[3];
    outTex.write(float4(sum, 1.0), gid);
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

    bool releaseLibrary = false;
    auto library = fb->GetShaderManager()->LoadNativeLibrary();
    if (!library) {
        auto sourceString = NS::String::string(BLOOM_COMPUTE_SOURCE, NS::UTF8StringEncoding);
        auto compileOptions = MTL::CompileOptions::alloc()->init();
        compileOptions->setLanguageVersion(MTL::LanguageVersion2_0);
        NS::Error* error = nullptr;
        library = deviceObj->newLibrary(sourceString, compileOptions, &error);
        compileOptions->release();
        releaseLibrary = library != nullptr;

        if (!library) {
            if (error) {
                Printf(PRINT_BOLD, "Error: Failed to compile Metal Bloom library: %s\n", error->localizedDescription()->utf8String());
                error->release();
            } else {
                Printf(PRINT_BOLD, "Error: Failed to compile Metal Bloom library (unknown error).\n");
            }
            return;
        }
    }

    auto createComputePipeline = [&](const char *functionName, const char *debugName) -> MTL::ComputePipelineState* {
        auto f = library->newFunction(NS::String::string(functionName, NS::UTF8StringEncoding));
        if (!f) {
            Printf(PRINT_LOG, "Metal: Missing bloom compute function %s\n", functionName);
            return nullptr;
        }

        NS::Error* error = nullptr;
        auto pso = deviceObj->newComputePipelineState(f, &error);
        f->release();
        if (!pso && error) {
            Printf(PRINT_LOG, "Metal: Failed to create compute pipeline %s: %s\n",
                   debugName, error->localizedDescription()->utf8String());
            error->release();
        }
        return pso;
    };

    extractPSO = createComputePipeline("bloom_extract", "Bloom extract");
    blurHPSO = createComputePipeline("blur_horizontal", "Bloom horizontal blur");
    blurVPSO = createComputePipeline("blur_vertical", "Bloom vertical blur");
    downsamplePSO = createComputePipeline("downsample_box", "Bloom downsample");

    // Create compute combine pipeline to write bloom contributions into a temporary texture
    combinePSO = createComputePipeline("bloom_combine_contrib", "Bloom contribution combine");
    combineAllPSO = createComputePipeline("bloom_combine_contrib_all", "Bloom multi-level contribution combine");

    // Create compute pipeline that writes directly into the scene color (read-write) for Tier 2 devices
    combineRWPSO = createComputePipeline("bloom_combine_rw", "Bloom read-write combine");

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

    if (releaseLibrary)
        library->release();
}

MtBloomModule::~MtBloomModule() {
    ReleaseTextures();
    if (extractPSO) extractPSO->release();
    if (downsamplePSO) downsamplePSO->release();
    if (blurHPSO) blurHPSO->release();
    if (blurVPSO) blurVPSO->release();
    if (combinePSO) combinePSO->release();
    if (combineAllPSO) combineAllPSO->release();
    if (combineRWPSO) combineRWPSO->release();
    if (compositePSO) compositePSO->release();
    if (mCompositeTex) { mCompositeTex->release(); mCompositeTex = nullptr; }
}

void MtBloomModule::CreateTextures(int width, int height, MTL::PixelFormat format) {
    if (mBloomA && mBloomB && mCachedBloomW == width && mCachedBloomH == height) return;
    ReleaseTextures();
    auto compute = fb->GetComputeManager();
    if (!compute)
        return;

    const auto usage = (MTL::TextureUsage)(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);

    // Primary bloom ping-pong textures
    mBloomA = compute->CreateTexture(width, height, MTL::PixelFormatRGBA16Float,
                                     usage, MTL::StorageModePrivate);
    mBloomB = compute->CreateTexture(width, height, MTL::PixelFormatRGBA16Float,
                                     usage, MTL::StorageModePrivate);

    // Create a small mip chain for multi-scale bloom (half, quarter, eighth)
    const int mipLevels = 3;
    for (int i = 1; i <= mipLevels; ++i) {
        int w2 = std::max(1, width >> i);
        int h2 = std::max(1, height >> i);
        MTL::Texture* t = compute->CreateTexture(w2, h2, MTL::PixelFormatRGBA16Float,
                                                 usage, MTL::StorageModePrivate);
        mDownsampledTextures.push_back(t);
        // Allocate a temporary ping-pong texture for this mip level
        MTL::Texture* tmp = compute->CreateTexture(w2, h2, MTL::PixelFormatRGBA16Float,
                                                   usage, MTL::StorageModePrivate);
        mDownsampledTempTextures.push_back(tmp);
    }

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

bool MtBloomModule::Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* srcTex, float amount) {
    auto tStart = std::chrono::high_resolution_clock::now();
    if (!extractPSO || !blurHPSO || !blurVPSO || !cmdBuf || !srcTex) return false;

    int srcW = (int)srcTex->width();
    int srcH = (int)srcTex->height();
    int bloomW = max(1, srcW / 2);
    int bloomH = max(1, srcH / 2);

    // Ensure cached bloom textures exist for this resolution
    auto format = srcTex->pixelFormat();
    CreateTextures(bloomW, bloomH, format);

    MTL::Texture* bloomA = mBloomA;
    MTL::Texture* bloomB = mBloomB;
    if (!bloomA || !bloomB)
        return false;

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

    const bool srcSupportsShaderWrite = (srcTex->usage() & MTL::TextureUsageShaderWrite) != 0;
    if (fb->mVersionManager.supportsReadWriteBGRA8 && combineRWPSO && srcSupportsShaderWrite) {
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
    } else if (compositePSO && combineAllPSO) {
        // Create a temporary high-precision target for compute contributions if needed
        if (auto compute = fb->GetComputeManager()) {
            const auto usage = (MTL::TextureUsage)(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
            compute->EnsureTexture(mCompositeTex, mCompositeW, mCompositeH, srcW, srcH,
                                   MTL::PixelFormatRGBA16Float, usage,
                                   MTL::StorageModePrivate);
        }
        if (!mCompositeTex)
            return false;

        struct BloomCompositeParamsCPU {
            float strength[4];
            float srcRes[2];
            float bloomRes[4][2];
        } compositeParams = {};

        compositeParams.srcRes[0] = (float)srcW;
        compositeParams.srcRes[1] = (float)srcH;
        compositeParams.strength[0] = amount;
        compositeParams.bloomRes[0][0] = (float)bloomW;
        compositeParams.bloomRes[0][1] = (float)bloomH;

        MTL::Texture* compositeInputs[4] = { bloomA, bloomA, bloomA, bloomA };
        const size_t compositeLevels = std::min<size_t>(mDownsampledTextures.size(), 3);
        for (size_t i = 0; i < compositeLevels; ++i) {
            auto dst = mDownsampledTextures[i];
            if (!dst)
                continue;
            const size_t slot = i + 1;
            compositeInputs[slot] = dst;
            compositeParams.strength[slot] = amount * (0.5f / (float)(i + 1));
            compositeParams.bloomRes[slot][0] = (float)dst->width();
            compositeParams.bloomRes[slot][1] = (float)dst->height();
        }

        auto combEnc = cmdBuf->computeCommandEncoder();
        combEnc->setComputePipelineState(combineAllPSO);
        combEnc->setBytes(&compositeParams, sizeof(compositeParams), 0);
        for (int i = 0; i < 4; ++i)
            combEnc->setTexture(compositeInputs[i], i);
        combEnc->setTexture(mCompositeTex, 4);
        combEnc->dispatchThreads(fullGrid, MTL::Size(16,16,1));
        combEnc->memoryBarrier(MTL::BarrierScopeTextures);
        combEnc->endEncoding();

        BloomParams renderParams = params;
        renderParams.strength = 1.0f;
        renderParams.srcRes[0] = (float)srcW;
        renderParams.srcRes[1] = (float)srcH;
        renderParams.bloomRes[0] = (float)srcW;
        renderParams.bloomRes[1] = (float)srcH;

        auto mtRenderState = static_cast<MtRenderState *>(fb->GetRenderState());
        mtRenderState->SetRenderTarget(srcTex, nullptr, srcW, srcH, (int)srcTex->pixelFormat(), 1);
        mtRenderState->EnableDrawBuffers(1, false);
        mtRenderState->SetViewport(0, 0, srcW, srcH);
        mtRenderState->SetScissor(0, 0, srcW, srcH);
        mtRenderState->BeginRenderPass();
        auto renc = mtRenderState->GetEncoder();
        if (renc) {
            renc->setRenderPipelineState(compositePSO);
            renc->setFragmentBytes(&renderParams, sizeof(BloomParams), 0);
            renc->setFragmentTexture(mCompositeTex, 0);
            if (sampler) renc->setFragmentSamplerState(sampler, 0);
            renc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
        }
        mtRenderState->EndRenderPass();
    } else {
        return false;
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    if (auto compute = fb->GetComputeManager()) {
        float elapsedMs = std::chrono::duration<float, std::milli>(tEnd - tStart).count();
        compute->RecordTiming(HWComputeEffect::Bloom, elapsedMs);
    }
    return true;
}
