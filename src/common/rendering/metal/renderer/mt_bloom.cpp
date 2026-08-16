#include "mt_bloom.h"
#include "mt_resources.h"
#include "../system/mt_renderdevice.h"
#include "../shaders/mt_shader.h"
#include "metal/renderer/mt_compute.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/textures/mt_sampler.h"
bool MtBloomDumpWantsExtract();

#include "c_cvars.h"
#include "printf.h"
#include "v_video.h"
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

EXTERN_CVAR(Int, mt_compute_bloom_composite)

static const char* BLOOM_COMPUTE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct BloomParams {
    float threshold;
    float strength;
    float2 srcRes;
    float2 bloomRes;
    float2 srcScale;
    float2 srcOffset;
    float2 viewportOrigin;
    // Nonzero when an exposure texture is bound to bloom_extract. When zero
    // the extract runs unexposed, which is only correct if the camera
    // exposure pass did not run this frame.
    float useExposure;
    float sampleWeights[8];
};

struct BloomCompositeParams {
    float strength[4];
    float2 srcRes;
    float2 bloomRes[4];
    float2 viewportOrigin;
};

kernel void bloom_extract(
    uint2 gid [[thread_position_in_grid]],
    constant BloomParams &params [[buffer(0)]],
    texture2d<float, access::sample> src [[texture(0)]],
    texture2d<float, access::write> out [[texture(1)]],
    texture2d<float, access::sample> exposureTex [[texture(2)]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = params.srcOffset + ((float2(gid) + 0.5) / params.bloomRes) * params.srcScale;
    float4 c = src.sample(s, uv);

    // Match PP bloom extract (shaders/pp/bloomextract.fp) exactly:
    //     max((color + 0.001) * exposureAdjustment - threshold, 0)
    // The bias is applied *before* the exposure multiply and the threshold
    // subtraction *after*, which is not the same as scaling the threshold:
    // the reference scales the bias too. Getting this order wrong changes
    // which pixels survive the extract at all.
    //
    // Omitting the exposure term made compute bloom measurably dimmer than
    // the reference path in dark scenes (verified 2026-07-27: with exposure
    // forced off the two paths produce byte-identical frames, and with it
    // live the compute path was darker across ~3.8% of the frame around
    // light sources, peak -7 luminance).
    float exposureAdjustment = params.useExposure != 0.0
        ? exposureTex.sample(s, float2(0.5)).x
        : 1.0;
    float3 res = max((c.rgb + float3(0.001)) * exposureAdjustment - float3(params.threshold),
                     float3(0.0));
    out.write(float4(res, 1.0), gid);
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
    sum += inTex.sample(s, uv) * params.sampleWeights[0];
    sum += inTex.sample(s, uv + float2(1.0*texelX, 0.0)) * params.sampleWeights[1];
    sum += inTex.sample(s, uv + float2(-1.0*texelX, 0.0)) * params.sampleWeights[2];
    sum += inTex.sample(s, uv + float2(2.0*texelX, 0.0)) * params.sampleWeights[3];
    sum += inTex.sample(s, uv + float2(-2.0*texelX, 0.0)) * params.sampleWeights[4];
    sum += inTex.sample(s, uv + float2(3.0*texelX, 0.0)) * params.sampleWeights[5];
    sum += inTex.sample(s, uv + float2(-3.0*texelX, 0.0)) * params.sampleWeights[6];
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
    sum += inTex.sample(s, uv) * params.sampleWeights[0];
    sum += inTex.sample(s, uv + float2(0.0, 1.0*texelY)) * params.sampleWeights[1];
    sum += inTex.sample(s, uv + float2(0.0, -1.0*texelY)) * params.sampleWeights[2];
    sum += inTex.sample(s, uv + float2(0.0, 2.0*texelY)) * params.sampleWeights[3];
    sum += inTex.sample(s, uv + float2(0.0, -2.0*texelY)) * params.sampleWeights[4];
    sum += inTex.sample(s, uv + float2(0.0, 3.0*texelY)) * params.sampleWeights[5];
    sum += inTex.sample(s, uv + float2(0.0, -3.0*texelY)) * params.sampleWeights[6];
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
    float2 uv = pixel / params.srcRes;
    float3 sum = float3(0.0);
    if (params.strength[0] > 0.0) sum += bloom0.sample(s, uv).rgb * params.strength[0];
    if (params.strength[1] > 0.0) sum += bloom1.sample(s, uv).rgb * params.strength[1];
    if (params.strength[2] > 0.0) sum += bloom2.sample(s, uv).rgb * params.strength[2];
    if (params.strength[3] > 0.0) sum += bloom3.sample(s, uv).rgb * params.strength[3];
    outTex.write(float4(sum, 1.0), gid);
}

// Tier 2: combine every bloom level and update the scene in one read-write pass.
kernel void bloom_combine_rw_all(
    uint2 gid [[thread_position_in_grid]],
    constant BloomCompositeParams &params [[buffer(0)]],
    texture2d<float, access::read_write> sceneTex [[texture(0)]],
    texture2d<float, access::sample> bloom0 [[texture(1)]],
    texture2d<float, access::sample> bloom1 [[texture(2)]],
    texture2d<float, access::sample> bloom2 [[texture(3)]],
    texture2d<float, access::sample> bloom3 [[texture(4)]])
{
    uint2 sceneGid = gid + uint2(params.viewportOrigin.x, params.viewportOrigin.y);
    if (gid.x >= uint(params.srcRes.x) || gid.y >= uint(params.srcRes.y) ||
        sceneGid.x >= sceneTex.get_width() || sceneGid.y >= sceneTex.get_height()) return;
    sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / params.srcRes;
    float3 bloom = float3(0.0);
    if (params.strength[0] > 0.0) bloom += bloom0.sample(s, uv).rgb * params.strength[0];
    if (params.strength[1] > 0.0) bloom += bloom1.sample(s, uv).rgb * params.strength[1];
    if (params.strength[2] > 0.0) bloom += bloom2.sample(s, uv).rgb * params.strength[2];
    if (params.strength[3] > 0.0) bloom += bloom3.sample(s, uv).rgb * params.strength[3];
    float4 scene = sceneTex.read(sceneGid);
    scene.rgb += bloom;
    sceneTex.write(scene, sceneGid);
}

struct BloomVSOut {
    float4 position [[position]];
    float2 uv;
};

vertex BloomVSOut bloom_vs(uint vid [[vertex_id]]) {
    BloomVSOut out;
    float2 pos[3] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)
    };
    out.position = float4(pos[vid], 0.0, 1.0);
    out.uv = out.position.xy * 0.5 + float2(0.5);
    return out;
}

fragment float4 bloom_fs(BloomVSOut in [[stage_in]],
                         texture2d<float, access::sample> bloomTex [[texture(0)]],
                         sampler samp [[sampler(0)]]) {
    // bloom_vs derives uv from clip space, where +Y is up, but the composite
    // texture was written by bloom_combine_contrib_all in texture order, where
    // v=0 is the top row. Native libraries bypass PatchVertexShader, so nothing
    // reconciles the two -- without this the bloom contribution lands mirrored
    // about the viewport's horizontal centreline. Same correction, and same
    // reason, as ssao_combine_fs in mt_ao.metal.
    float2 uv = float2(in.uv.x, 1.0 - in.uv.y);
    return float4(bloomTex.sample(samp, uv).rgb, 0.0);
}
)";

static float BloomBlurGaussian(float n, float theta)
{
    theta = std::max(theta, 0.1f);
    constexpr float Pi = 3.14159265358979323846f;
    return (1.0f / std::sqrt(2.0f * Pi * theta)) * std::exp(-(n * n) / (2.0f * theta * theta));
}

static void ComputeBloomBlurSamples(int sampleCount, float blurAmount, float *sampleWeights)
{
    sampleWeights[0] = BloomBlurGaussian(0.0f, blurAmount);
    float totalWeights = sampleWeights[0];

    for (int i = 0; i < sampleCount / 2; i++) {
        float weight = BloomBlurGaussian(i + 1.0f, blurAmount);
        sampleWeights[i * 2 + 1] = weight;
        sampleWeights[i * 2 + 2] = weight;
        totalWeights += weight * 2.0f;
    }

    for (int i = 0; i < sampleCount; i++)
        sampleWeights[i] /= totalWeights;
}

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

    combineAllPSO = createComputePipeline("bloom_combine_contrib_all", "Bloom multi-level contribution combine");

    // Tier 2: compute writes directly into the scene color (read-write)
    combineRWPSO = createComputePipeline("bloom_combine_rw_all", "Bloom read-write combine");

    // Tier 1 cannot reliably write BGRA8 from compute. Composite the
    // high-precision bloom contribution with fixed-function blending instead.
    // The PSO itself is built on first use, once the scene colour format is
    // known -- see GetCompositePSO.
    compositeVertexFn = library->newFunction(NS::String::string("bloom_vs", NS::UTF8StringEncoding));
    compositeFragmentFn = library->newFunction(NS::String::string("bloom_fs", NS::UTF8StringEncoding));

    if (releaseLibrary)
        library->release();
}

MtBloomModule::~MtBloomModule() {
    ReleaseTextures();
    if (extractPSO) extractPSO->release();
    if (downsamplePSO) downsamplePSO->release();
    if (blurHPSO) blurHPSO->release();
    if (blurVPSO) blurVPSO->release();
    if (combineAllPSO) combineAllPSO->release();
    if (combineRWPSO) combineRWPSO->release();
    if (compositePSO) compositePSO->release();
    if (compositeVertexFn) compositeVertexFn->release();
    if (compositeFragmentFn) compositeFragmentFn->release();
    if (mCompositeTex) { mCompositeTex->release(); mCompositeTex = nullptr; }
    if (mExtractSnapshot) { mExtractSnapshot->release(); mExtractSnapshot = nullptr; mExtractSnapW = mExtractSnapH = 0; }
}

MTL::RenderPipelineState* MtBloomModule::GetCompositePSO(MTL::PixelFormat format) {
    if (compositePSO && compositePSOFormat == (int)format)
        return compositePSO;
    if (!compositeVertexFn || !compositeFragmentFn)
        return nullptr;

    if (compositePSO) {
        compositePSO->release();
        compositePSO = nullptr;
    }
    compositePSOFormat = (int)format;

    NS::Error* rpError = nullptr;
    auto rpDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    rpDesc->setVertexFunction(compositeVertexFn);
    rpDesc->setFragmentFunction(compositeFragmentFn);
    auto color = rpDesc->colorAttachments()->object(0);
    color->setPixelFormat(format);
    color->setBlendingEnabled(true);
    color->setRgbBlendOperation(MTL::BlendOperationAdd);
    color->setAlphaBlendOperation(MTL::BlendOperationAdd);
    color->setSourceRGBBlendFactor(MTL::BlendFactorOne);
    color->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
    color->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
    color->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
    compositePSO = fb->device->device->newRenderPipelineState(rpDesc, &rpError);
    if (!compositePSO && rpError) {
        Printf(PRINT_LOG, "Metal: Failed to create Tier 1 bloom composite pipeline: %s\n",
               rpError->localizedDescription()->utf8String());
    }
    if (rpError) rpError->release();
    rpDesc->release();
    return compositePSO;
}

MTL::Texture* MtBloomModule::DebugLevel(int i) const {
    if (i == 0) return mBloomA;
    size_t mip = (size_t)(i - 1);
    if (i < 1 || mip >= mDownsampledTextures.size()) return nullptr;
    return mDownsampledTextures[mip];
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

    // Tier 3 of docs/frame-analysis.md: private to this module. Bloom works at a
    // fixed fraction of the scene, so the divisor is derived from the size it was
    // asked for rather than assumed.
    {
        const int sw = MtResources().SceneWidth();
        const int div = (sw > 0 && width > 0) ? std::max(1, (sw + width - 1) / width) : 1;
        MtResources().Declare({"Bloom.A", "MtBloomModule", width, height, 1,
                               (int)MTL::PixelFormatRGBA16Float,
                               MtSizeRule::Scaled(div), true}, mBloomA);
        MtResources().Declare({"Bloom.B", "MtBloomModule", width, height, 1,
                               (int)MTL::PixelFormatRGBA16Float,
                               MtSizeRule::Scaled(div), true}, mBloomB);
    }

    // Create a small mip chain for multi-scale bloom (half, quarter, eighth)
    // Sizes must be halved by *iterative ceiling*, not by shifting: the
    // reference carries each level's rounded-up size into the next
    // (hw_postprocess.cpp:54-68, `(prior + 1) / 2`), so an odd level rounds up
    // and every level below inherits that. `width >> i` rounds down instead
    // and the error compounds -- at a 1600x900 viewport the reference gets
    // 200x113 / 100x57 / 50x29 where shifting gets 200x112 / 100x56 / 50x28,
    // giving the coarsest vertical blur a 3.6% larger footprint once
    // upsampled. Level 0 itself already agrees: ceil(w/4) == ceil(ceil(w/2)/2).
    const int mipLevels = 3;
    int w2 = width, h2 = height;
    for (int i = 1; i <= mipLevels; ++i) {
        w2 = std::max(1, (w2 + 1) / 2);
        h2 = std::max(1, (h2 + 1) / 2);
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

bool MtBloomModule::Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* srcTex, float amount,
                            MTL::Texture* exposureTex) {
    // Marks the compute bloom path as having run this frame -- the same
    // structural proof-of-execution the AO textures give.
    MtResources().Touch("Bloom.A");
    MtResources().Touch("Bloom.B");
    auto tStart = std::chrono::high_resolution_clock::now();
    if (!extractPSO || !blurHPSO || !blurVPSO || !cmdBuf || !srcTex) return false;

    const auto &viewport = screen->mSceneViewport;
    int srcW = std::max(1, viewport.width);
    int srcH = std::max(1, viewport.height);
    int bloomW = max(1, (srcW + 3) / 4);
    int bloomH = max(1, (srcH + 3) / 4);

    // Ensure cached bloom textures exist for this resolution
    auto format = srcTex->pixelFormat();
    CreateTextures(bloomW, bloomH, format);

    MTL::Texture* bloomA = mBloomA;
    MTL::Texture* bloomB = mBloomB;
    if (!bloomA || !bloomB)
        return false;

    BloomParams params = {};
    params.threshold = 1.0f;
    params.strength = 1.0f;
    params.srcRes[0] = (float)srcW; params.srcRes[1] = (float)srcH;
    params.bloomRes[0] = (float)bloomW; params.bloomRes[1] = (float)bloomH;
    auto sceneScale = screen->SceneScale();
    auto sceneOffset = screen->SceneOffset();
    params.srcScale[0] = sceneScale.X; params.srcScale[1] = sceneScale.Y;
    params.srcOffset[0] = sceneOffset.X; params.srcOffset[1] = sceneOffset.Y;
    params.viewportOrigin[0] = (float)viewport.left; params.viewportOrigin[1] = (float)viewport.top;
    params.useExposure = exposureTex ? 1.0f : 0.0f;
    ComputeBloomBlurSamples(7, amount, params.sampleWeights);

    auto encoder = cmdBuf->computeCommandEncoder();

    // 1) Extract bright areas -> bloomA
    encoder->setComputePipelineState(extractPSO);
    encoder->setBytes(&params, sizeof(BloomParams), 0);
    encoder->setTexture(srcTex, 0);
    encoder->setTexture(bloomA, 1);
    // Always bind slot 2, even when exposure is unavailable: the kernel's
    // sample is branch-guarded by params.useExposure, but leaving a declared
    // texture argument unbound is invalid. srcTex is the harmless stand-in.
    encoder->setTexture(exposureTex ? exposureTex : srcTex, 2);
    MTL::Size grid = { (NS::UInteger)bloomW, (NS::UInteger)bloomH, 1 };
    MtDispatchThreads(encoder, fb, grid, MTL::Size(16,16,1));
    encoder->memoryBarrier(MTL::BarrierScopeTextures);

    // Snapshot the extract before the pyramid overwrites bloomA. Uses
    // downsample_box at matching resolution, which is an identity copy, so it
    // stays inside this compute encoder -- a blit would need the encoder ended
    // and reopened. Only on a dump frame; costs nothing otherwise.
    if (MtBloomDumpWantsExtract()) {
        if (auto compute = fb->GetComputeManager()) {
            const auto snapUsage = (MTL::TextureUsage)(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
            compute->EnsureTexture(mExtractSnapshot, mExtractSnapW, mExtractSnapH,
                                   bloomW, bloomH, MTL::PixelFormatRGBA16Float,
                                   snapUsage, MTL::StorageModePrivate);
        }
        if (mExtractSnapshot && downsamplePSO) {
            BloomParams sp = params;
            sp.bloomRes[0] = (float)bloomW;
            sp.bloomRes[1] = (float)bloomH;
            encoder->setComputePipelineState(downsamplePSO);
            encoder->setBytes(&sp, sizeof(BloomParams), 0);
            encoder->setTexture(bloomA, 0);
            encoder->setTexture(mExtractSnapshot, 1);
            MtDispatchThreads(encoder, fb, grid, MTL::Size(16,16,1));
            encoder->memoryBarrier(MTL::BarrierScopeTextures);
        }
    }

    // 2) Blur/downscale/upscale pyramid, mirroring PPBloom::RenderBloom.
    //
    // Level 0 is bloomA (bloomB is its ping-pong temp); levels 1..N are
    // mDownsampledTextures[0..N-1] with mDownsampledTempTextures as temps.
    // Both paths run 4 levels (NumBloomLevels == 4; bloomA plus 3 mips).
    //
    // The reference blurs each level, downscales into the next, then walks
    // back up *replacing* each lower level with the upscaled one above it,
    // and finally composites level 0 alone at unit gain. This path used to
    // instead sum all four levels at 1.0/0.18/0.09/0.06, which both overshot
    // the gain (1.33x nominal) and piled correlated energy onto the same
    // light -- measured 2026-07-30 at ~2.1x the reference's bloom. It also
    // downsampled bloomA into every mip rather than chaining level to level,
    // so the mips were four blurs of the same image, not a real pyramid.
    const size_t numMips = mDownsampledTextures.size();

    auto levelTex = [&](size_t level) -> MTL::Texture* {
        return level == 0 ? bloomA : mDownsampledTextures[level - 1];
    };
    auto levelTmp = [&](size_t level) -> MTL::Texture* {
        return level == 0 ? bloomB : mDownsampledTempTextures[level - 1];
    };

    // Separable blur in place, via the level's ping-pong temp.
    auto blurLevel = [&](size_t level) {
        MTL::Texture* tex = levelTex(level);
        MTL::Texture* tmp = levelTmp(level);
        if (!tex || !tmp) return;
        BloomParams p = params;
        p.bloomRes[0] = (float)tex->width();
        p.bloomRes[1] = (float)tex->height();
        MTL::Size g = { tex->width(), tex->height(), 1 };
        encoder->setComputePipelineState(blurHPSO);
        encoder->setBytes(&p, sizeof(BloomParams), 0);
        encoder->setTexture(tex, 0);
        encoder->setTexture(tmp, 1);
        MtDispatchThreads(encoder, fb, g, MTL::Size(16,16,1));
        encoder->memoryBarrier(MTL::BarrierScopeTextures);
        encoder->setComputePipelineState(blurVPSO);
        encoder->setBytes(&p, sizeof(BloomParams), 0);
        encoder->setTexture(tmp, 0);
        encoder->setTexture(tex, 1);
        MtDispatchThreads(encoder, fb, g, MTL::Size(16,16,1));
        encoder->memoryBarrier(MTL::BarrierScopeTextures);
    };

    // Bilinear resample. downsample_box derives its UVs from the destination
    // extent, so the same kernel serves the downscale and the upscale legs.
    auto resample = [&](size_t from, size_t to) {
        MTL::Texture* src = levelTex(from);
        MTL::Texture* dst = levelTex(to);
        if (!src || !dst) return;
        BloomParams p = params;
        p.bloomRes[0] = (float)dst->width();
        p.bloomRes[1] = (float)dst->height();
        encoder->setComputePipelineState(downsamplePSO);
        encoder->setBytes(&p, sizeof(BloomParams), 0);
        encoder->setTexture(src, 0);
        encoder->setTexture(dst, 1);
        MTL::Size g = { dst->width(), dst->height(), 1 };
        MtDispatchThreads(encoder, fb, g, MTL::Size(16,16,1));
        encoder->memoryBarrier(MTL::BarrierScopeTextures);
    };

    // Blur and downscale.
    for (size_t i = 0; i < numMips; ++i) {
        blurLevel(i);
        resample(i, i + 1);
    }

    // Blur and upscale, each level replacing the one below it.
    for (size_t i = numMips; i > 0; --i) {
        blurLevel(i);
        resample(i, i - 1);
    }

    blurLevel(0);

    // Finish initial compute encoder before creating another one
    encoder->endEncoding();

    // Describe the multi-level bloom contribution once for both composite paths.
    struct BloomCompositeParamsCPU {
        float strength[4];
        float srcRes[2];
        float bloomRes[4][2];
        float viewportOrigin[2];
    } compositeParams = {};
    static_assert(sizeof(BloomCompositeParamsCPU) == 64,
                  "Bloom composite parameters must match Metal layout");

    compositeParams.srcRes[0] = (float)srcW;
    compositeParams.srcRes[1] = (float)srcH;
    compositeParams.strength[0] = 1.0f;
    compositeParams.bloomRes[0][0] = (float)bloomW;
    compositeParams.bloomRes[0][1] = (float)bloomH;
    compositeParams.viewportOrigin[0] = (float)viewport.left;
    compositeParams.viewportOrigin[1] = (float)viewport.top;

    // Only level 0 is composited, at unit gain, matching the reference's single
    // additive BloomCombine draw of level0.VTexture (hw_postprocess.cpp:151-159;
    // bloomcombine.fp is a plain copy with no gain). The pyramid above has
    // already folded the coarser levels into bloomA by upscale-replace, so
    // summing the mips here would double-count them.
    //
    // Slots 1-3 keep bloomA bound with strength 0: the kernel skips zero-weight
    // levels, but every declared texture argument must still be bound.
    MTL::Texture* compositeInputs[4] = { bloomA, bloomA, bloomA, bloomA };
    for (int i = 1; i < 4; ++i) {
        compositeParams.strength[i] = 0.0f;
        compositeParams.bloomRes[i][0] = (float)bloomW;
        compositeParams.bloomRes[i][1] = (float)bloomH;
    }

    // Either write directly into the scene on Tier 2 devices, or compute into
    // same-format temporary targets and blit back on Tier 1.

    // Full-resolution dispatch grid
    MTL::Size fullGrid = { (NS::UInteger)srcW, (NS::UInteger)srcH, 1 };

    const bool srcSupportsShaderWrite = (srcTex->usage() & MTL::TextureUsageShaderWrite) != 0;
    const bool directCompositeSupported = fb->mVersionManager.supportsReadWriteBGRA8 &&
                                          combineRWPSO && srcSupportsShaderWrite;
    const bool useDirectComposite = directCompositeSupported && mt_compute_bloom_composite != 1;
    if (mt_compute_bloom_composite == 2 && !directCompositeSupported)
        return false;

    if (useDirectComposite) {
        // Fast-path: compute directly writes into the scene color (requires Tier 2)
        auto combEnc = cmdBuf->computeCommandEncoder();
        combEnc->setComputePipelineState(combineRWPSO);
        combEnc->setBytes(&compositeParams, sizeof(compositeParams), 0);
        combEnc->setTexture(srcTex, 0);
        for (int i = 0; i < 4; ++i)
            combEnc->setTexture(compositeInputs[i], i + 1);
        MtDispatchThreads(combEnc, fb, fullGrid, MTL::Size(16,16,1));
        combEnc->endEncoding();
    } else if (auto composite = GetCompositePSO(srcTex->pixelFormat());
               composite && combineAllPSO) {
        // Create a high-precision target for the combined bloom contribution.
        if (auto compute = fb->GetComputeManager()) {
            const auto usage = (MTL::TextureUsage)(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
            compute->EnsureTexture(mCompositeTex, mCompositeW, mCompositeH, srcW, srcH,
                                   MTL::PixelFormatRGBA16Float, usage,
                                   MTL::StorageModePrivate);
        }
        if (!mCompositeTex)
            return false;

        auto combEnc = cmdBuf->computeCommandEncoder();
        combEnc->setComputePipelineState(combineAllPSO);
        combEnc->setBytes(&compositeParams, sizeof(compositeParams), 0);
        for (int i = 0; i < 4; ++i)
            combEnc->setTexture(compositeInputs[i], i);
        combEnc->setTexture(mCompositeTex, 4);
        MtDispatchThreads(combEnc, fb, fullGrid, MTL::Size(16,16,1));
        combEnc->endEncoding();

        MtSamplerKey samplerKey;
        samplerKey.MinFilter = 1;
        samplerKey.MagFilter = 1;
        samplerKey.MipFilter = 0;
        samplerKey.AddressU = 3;
        samplerKey.AddressV = 3;
        samplerKey.AddressW = 3;
        samplerKey.MaxAnisotropy = 1;
        auto sampler = fb->GetSamplerManager()->GetSamplerState(samplerKey);

        auto renderState = static_cast<MtRenderState *>(fb->GetRenderState());
        renderState->SetRenderTarget(srcTex, nullptr, (int)srcTex->width(),
                                     (int)srcTex->height(),
                                     (int)srcTex->pixelFormat(), 1);
        renderState->EnableDrawBuffers(1, false);
        renderState->SetViewport(viewport.left, viewport.top, srcW, srcH);
        renderState->SetScissor(viewport.left, viewport.top, srcW, srcH);
        renderState->BeginRenderPass();
        if (auto renderEncoder = renderState->GetEncoder()) {
            renderEncoder->setRenderPipelineState(composite);
            renderEncoder->setFragmentTexture(mCompositeTex, 0);
            if (sampler) renderEncoder->setFragmentSamplerState(sampler, 0);
            renderEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                          (NS::UInteger)0,
                                          (NS::UInteger)3);
        }
        renderState->EndRenderPass();
    } else {
        return false;
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    if (auto compute = fb->GetComputeManager()) {
        // This measures CPU command-encoding cost. Per-pass GPU timings require
        // Metal counter sample buffers and are intentionally reported separately.
        float elapsedMs = std::chrono::duration<float, std::milli>(tEnd - tStart).count();
        compute->RecordTiming(HWComputeEffect::Bloom, elapsedMs);
    }
    return true;
}
