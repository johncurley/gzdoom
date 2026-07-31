#include "mt_ao.h"
#include "c_cvars.h"
#include "mt_renderbuffers.h"
#include "../system/mt_renderdevice.h"
#include "../shaders/mt_shader.h"
#include "../renderer/mt_debug.h"
#include "flatvertices.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "metal/renderer/mt_compute.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_hwbuffer.h"
#include "metal/textures/mt_texture.h"
#include "matrix.h"
#include "printf.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

EXTERN_CVAR(Int, mt_compute_ao_scale)
EXTERN_CVAR(Bool, mt_compute_ao_normal_upsample)
EXTERN_CVAR(Bool, mt_compute_ao_normal_blur)
EXTERN_CVAR(Bool, mt_compute_ao_fullres_cleanup)
EXTERN_CVAR(Int, mt_compute_ao_blur_passes)
EXTERN_CVAR(Float, mt_compute_ao_combine_smooth)
EXTERN_CVAR(Bool, mt_compute_ao_skip_fullres)
EXTERN_CVAR(Int, mt_compute_ao_atrous_passes)
EXTERN_CVAR(Int, mt_compute_ao_steps)
EXTERN_CVAR(Int, mt_compute_ao_directions)
EXTERN_CVAR(Float, mt_compute_ao_fade_start)
EXTERN_CVAR(Float, mt_compute_ao_fade_end)
EXTERN_CVAR(Int, mt_compute_ao_algorithm)
EXTERN_CVAR(Int, mt_compute_ao_alchemy_samples)
EXTERN_CVAR(Int, mt_compute_ao_worldpos_debug)
EXTERN_CVAR(Float, mt_compute_ao_noise_cellsize)
EXTERN_CVAR(Float, mt_compute_ao_noise_pixels)
EXTERN_CVAR(Float, mt_compute_ao_noise_screenmix)
EXTERN_CVAR(Float, mt_compute_ao_thickness)

// NOTE: Kept in sync with the authoritative
// src/common/rendering/metal/shaders/native/mt_ao.metal — this string is only
// a fallback compiled at runtime if native_shaders.metallib can't be loaded.
static const char* SSAO_COMPUTE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct SSAOParams {
    // Affine view-space -> world-space transform for this frame's camera
    // (rotation-transpose in the upper-left 3x3, camera world position in
    // the translation column, (0,0,0,1) bottom row) -- built CPU-side in
    // MtAOModule::Render(). Used to world-lock the per-pixel jitter noise
    // (see AGENTS.md). Repurposes the field that used to hold an
    // inverse-projection matrix for the now-deleted ReconstructViewPos
    // helper (confirmed dead: zero live call sites, all sample kernels use
    // FetchViewPos instead).
    float4x4 viewToWorld;
    float radius;
    float bias;
    float intensity;
    float screenResX;
    float screenResY;
    float zNear;
    float zFar;
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    float uvToViewAX;
    float uvToViewAY;
    float uvToViewBX;
    float uvToViewBY;
    float negInvR2;
    float radiusToScreen;
    float aoMultiplier;
    float visibilityStrength;
    int numDirections;
    int numSteps;
    float maxThickness;
    float fadeStartDistance;
    float fadeEndDistance;
    // World-space grid cell size (map units) the noise hash quantizes to --
    // analogous to the old dither texture's implicit tiling frequency. See
    // mt_compute_ao_noise_cellsize's doc comment (mt_postprocess.cpp).
    float noiseCellSize;
    // 0 = normal AO. Nonzero renders a world-locked-noise diagnostic
    // straight to aoOutput (viewable via the existing gl_ssao_debug 2
    // raw-AO display) instead of running real AO math -- see
    // mt_compute_ao_worldpos_debug's doc comment (mt_postprocess.cpp).
    int debugMode;
    // World units per AO pixel at unit view depth, used to scale the
    // noise cell size with distance (see NoiseCellSize). <= 0 disables
    // the depth-adaptive path, falling back to the fixed noiseCellSize.
    float pixelWorldScale;
    // Weight of the screen-space decorrelation term mixed into the
    // world-cell jitter (see AoNoise). 0 = pure world-locked noise.
    float screenNoiseMix;
};

struct AOFlags {
    int flipY;
    float invBackingScale;
};

struct AOBlurParams {
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    float blurSharpness;
    float powExponent;
    int normalAware;
    int flipY;
    float maxThickness;
    int applyExponent; // Moved to end for alignment safety
};

struct AOFullresParams {
    float2 sceneScale;
    float2 sceneOffset;
    float2 fullRes;
    float blurSharpness;
    float zNear;
    float zFar;
    int normalAware;
    int atrousStep;
};

#define GOLDEN_ANGLE 2.39996323
#define NUM_SAMPLES 16

float3 FetchViewPos(float2 uv, float linearDepth, constant SSAOParams &params) {
    float2 uvToViewA = float2(params.uvToViewAX, params.uvToViewAY);
    float2 uvToViewB = float2(params.uvToViewBX, params.uvToViewBY);
    return float3((uvToViewA * uv + uvToViewB) * linearDepth, linearDepth);
}

// Reconstructs world-space position from a view-space one, undoing
// FetchViewPos's "+Z = distance in front of camera" convention (always
// positive) back to the real view matrix's standard OpenGL "-Z in front"
// convention -- this exact negation was already empirically verified during
// the temporal-AO investigation's "Z-sign" bug fix (see AGENTS.md), reused
// here rather than re-derived. params.viewToWorld is an ordinary affine
// transform (rotation + translation, no projection), so no perspective
// divide is needed.
float3 WorldPosFromViewPos(float3 centerViewPos, constant SSAOParams &params) {
    float3 realViewPos = float3(centerViewPos.xy, -centerViewPos.z);
    return (params.viewToWorld * float4(realViewPos, 1.0)).xyz;
}

// PCG3D integer hash (Jarzynski & Olano, "Hash Functions for GPU
// Rendering") -- pure bit-mixing, no sin()/cos() on large arguments. A
// classic frac(sin(dot(p,K))*C)-style hash loses precision and bands at
// large input magnitudes; Doom/UDMF world coordinates comfortably fit
// int32 but can still be in the tens of thousands, so an integer hash is
// used instead of a sine-based one.
uint3 Pcg3d(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

// World-locked replacement for the old screen-space dither-texture lookup
// (pixelCenter / 64.0). Quantizes worldPos to an integer grid cell --
// cellSize controls the dither's spatial frequency, analogous to the old
// texture's implicit tiling scale -- and hashes the cell coordinate. A
// static world point always lands in the same cell and gets the same
// jitter regardless of camera position/orientation, which is what actually
// fixes the "AO pattern slides near geometry while moving" bug (see
// AGENTS.md): the old pixelCenter-based noise changed every frame as the
// camera moved, even for a perfectly static surface point. Returns
// (rotation in radians, stepJitter, directionJitter) -- directly usable,
// no atan2 round-trip needed since there's no texture-storage requirement
// forcing the old cos/sin encoding.
float3 WorldNoise(float3 worldPos, float cellSize) {
    int3 cell = int3(floor(worldPos / max(cellSize, 1e-4)));
    uint3 h = Pcg3d(uint3(cell));
    float3 unorm = float3(h) * (1.0 / 4294967295.0);
    return float3(unorm.x * 6.28318530718, unorm.y, unorm.z);
}

// Depth-adaptive cell size for WorldNoise. A *fixed* world-space cell size
// (params.noiseCellSize) cannot work at both ends of the depth range: one
// cell is one noise sample, so at close range a 12-unit cell spans dozens
// of AO pixels and every pixel inside it gets the identical jitter --
// which is what produces the large correlated AO blotches, and, because
// the cell grid is nailed to the world while the camera is not, makes them
// crawl diagonally across surfaces as the player walks. (The same fixed
// size goes sub-pixel at distance and aliases instead.) Scaling the cell
// with view depth keeps each cell roughly one AO pixel wide at every
// distance, restoring the per-pixel decorrelation the blur pass expects
// while keeping the noise world-anchored rather than screen-anchored.
// Quantizing to a power of two means the size is constant across a whole
// depth range instead of changing continuously with every step, so a
// surface's noise stays put except at the (blur-hidden, ~1px scale) ring
// boundaries where it doubles. params.pixelWorldScale <= 0 disables this
// and restores the old fixed-size behaviour.
float NoiseCellSize(float viewZ, constant SSAOParams &params) {
    if (params.pixelWorldScale <= 0.0)
        return max(params.noiseCellSize, 1e-4);
    float target = params.pixelWorldScale * max(viewZ, 1e-4);
    return exp2(ceil(log2(max(target, 1e-4))));
}

// Screen-space interleaved gradient noise (Jimenez, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare"). A pure function of the
// pixel coordinate -- no frame counter -- so it is *static in screen
// space*: it cannot shimmer when the player stands still and cannot crawl
// when they move, because it does not move at all.
float InterleavedGradientNoise(float2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Final per-pixel jitter: the world cell hash decorrelated by a
// screen-space term.
//
// WorldNoise alone cannot decorrelate every pixel, and the failure is
// geometric rather than a tuning problem. NoiseCellSize picks a cell size
// from view depth, which assumes a pixel's world footprint is isotropic --
// true only on surfaces facing the camera. On a surface at a grazing angle
// (which is exactly what you get standing close to a wall, floor or ledge)
// the footprint is stretched enormously along the grazing direction, so a
// world-space cube projects to a long thin run of screen pixels that all
// fall in the same cell and therefore march the same directions with the
// same jitter. That coherent run is the dark streak reported 2026-07-26,
// and no isotropic cell size can remove it: shrinking cells to fit the
// long axis makes them correlate across the short axis instead, trading
// the streaks for equally long perpendicular ones.
//
// Breaking it requires decorrelation that is per *pixel* rather than per
// world point, which by definition can only come from screen space. This
// is also what production GTAO implementations do (Activision's original,
// Intel's XeGTAO): screen-space blue/gradient noise plus a denoiser, no
// world anchoring. The world term is kept as a per-cell offset so the
// low-frequency structure still has some world anchoring, but the
// screen-space term is what guarantees neighbouring pixels differ.
// params.screenNoiseMix = 0 restores the pure world-locked behaviour.
float3 AoNoise(float3 worldPos, float viewZ, float2 pixel, constant SSAOParams &params) {
    float3 h = WorldNoise(worldPos, NoiseCellSize(viewZ, params));
    float3 unorm = float3(h.x * (1.0 / 6.28318530718), h.y, h.z);
    float ign = InterleavedGradientNoise(pixel) * params.screenNoiseMix;
    float3 mixed = fract(unorm + ign * float3(1.0, 0.6180339887, 0.3819660113));
    return float3(mixed.x * 6.28318530718, mixed.y, mixed.z);
}

float LinearizeSceneDepth(float depth, constant SSAOParams &params) {
    float normalizedDepth = clamp(1.0 - depth, 0.0, 1.0);
    float linearizeA = 1.0 / params.zFar - 1.0 / params.zNear;
    float linearizeB = max(1.0 / params.zNear, 1e-8);
    return 1.0 / (normalizedDepth * linearizeA + linearizeB);
}

float LinearizeDepth(float depth, float zNear, float zFar) {
    float normalizedDepth = clamp(1.0 - depth, 0.0, 1.0);
    float linearizeA = 1.0 / zFar - 1.0 / zNear;
    float linearizeB = max(1.0 / zNear, 1e-8);
    return 1.0 / (normalizedDepth * linearizeA + linearizeB);
}

float3 DecodeSceneNormal(float3 encodedNormal) {
    float3 normal = encodedNormal * 2.0 - 1.0;
    if (length(normal) <= 0.1) {
        return float3(0.0);
    }

    // Match the Metal-patched postprocess SSAO shader: the regular GLSL
    // normal.z flip is intentionally suppressed for Metal's view-space setup.
    return normalize(normal);
}

float NormalWeight(float3 centerNormal, float3 sampleNormal) {
    if (all(centerNormal == float3(0.0)) || all(sampleNormal == float3(0.0))) {
        return 0.0;
    }
    // Much sharper normal weight to prevent bleeding across 90-degree edges
    return pow(saturate(dot(centerNormal, sampleNormal)), 8.0);
}

kernel void ssao_compute(
    uint2 gid [[thread_position_in_grid]],
    constant SSAOParams &params [[buffer(0)]],
    constant AOFlags &flags [[buffer(1)]],
    texture2d<float, access::sample> ditherTexture [[texture(0)]],
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::write> aoOutput [[texture(2)]],
    texture2d<float, access::sample> normalTexture [[texture(3)]],
    texture2d<float, access::sample> sceneColorTexture [[texture(4)]],
    texture2d<float, access::sample> coverageMask [[texture(5)]])
{
    float2 outSize = float2((float)aoOutput.get_width(), (float)aoOutput.get_height());
    if (gid.x >= outSize.x || gid.y >= outSize.y) return;

    // ditherTexture is intentionally unused now (world-locked noise
    // replaced it -- see WorldNoise) but its parameter/binding is left in
    // place; removing it means renumbering every subsequent texture index
    // in this kernel, mt_ao.cpp's Execute(), and the other two sample
    // kernels -- deferred to its own isolated cleanup once the new noise is
    // confirmed correct in-game (see AGENTS.md).
    sampler nearestClampSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);

    float2 pixelCenter = float2(gid) + 0.5;
    float2 halfTexel = 0.5 / outSize;
    float2 uv = pixelCenter / outSize;
    if (flags.flipY == 1) uv.y = 1.0 - uv.y;
    float2 sceneUV = float2(params.offsetX, params.offsetY) + uv * float2(params.scaleX, params.scaleY);
    float centerCoverage = sceneColorTexture.sample(nearestClampSampler, sceneUV).a;
    if (centerCoverage <= 0.0001) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float centerDepth = depthTexture.sample(nearestClampSampler, sceneUV).r;

    if (centerDepth <= 0.0001) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float centerLinearDepth = LinearizeSceneDepth(centerDepth, params);
    float3 centerViewPos = FetchViewPos(uv, centerLinearDepth, params);

    // Sky-dome guard: depth-clamped sky geometry sits at far plane
    if (centerLinearDepth >= params.zFar * 0.99) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float3 worldPos = WorldPosFromViewPos(centerViewPos, params);

    // Visual debug for the world-locked noise fix itself -- see
    // mt_compute_ao_worldpos_debug's doc comment (mt_postprocess.cpp).
    // Deliberately bypasses the rest of the AO computation entirely.
    if (params.debugMode != 0) {
        // Deliberately the *fixed* cvar cell size, not NoiseCellSize's
        // depth-adaptive one: this grid exists to be read by a human
        // eye, and the adaptive size is ~1 AO pixel by design, which
        // puts fract() right at Nyquist and turns the whole screen into
        // moire instead of a legible grid. A fixed size is also the
        // correct thing to test world-locking with -- it isolates the
        // worldPos reconstruction from the depth-dependent cell sizing.
        float cellSize = max(params.noiseCellSize, 1.0);
        float2 dbg = params.debugMode == 1 ? fract(worldPos.xy / cellSize) :
                     params.debugMode == 2 ? fract(worldPos.xz / cellSize) :
                                              fract(worldPos.yz / cellSize);
        aoOutput.write(float4(dbg.x, dbg.y, 0.0, 1.0), gid);
        return;
    }

    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sceneUV).xyz);
    if (all(centerNormal == float3(0.0))) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float3 worldNoise = AoNoise(worldPos, centerViewPos.z, pixelCenter, params);
    float rotation = worldNoise.x;
    float stepJitter = worldNoise.y;
    float directionJitter = worldNoise.z;

    float radiusPixels = params.radiusToScreen / max(centerViewPos.z, 1e-5);
    // Cap the screen-space sample radius. Uncapped, standing close to any
    // surface (centerViewPos.z small -- completely ordinary at typical
    // player-to-wall distances, not just extreme clipping) sends
    // radiusPixels past the AO texture's own dimensions: every sample's UV
    // then hard-clamps to the texture edge (see the sampleUV clamp below),
    // collapsing the sample disk into a handful of edge pixels unrelated to
    // local geometry -- and which edge pixel gets hit is hypersensitive to
    // tiny position/angle changes, producing a visibly rotating/stretching
    // AO pattern up close. Reported 2026-07-19. Bounding to a fraction of
    // the shorter texture dimension keeps the sample disk within a
    // meaningful local neighborhood at any distance.
    radiusPixels = min(radiusPixels, min(outSize.x, outSize.y) * 0.5);
    const int numSteps = clamp(params.numSteps, 2, 8); // Capped for Intel
    const int numDirections = clamp(params.numDirections, 2, 6); // Capped for Intel
    float stepSizePixels = radiusPixels / (float(numSteps) + 1.0);
    float minStepPixels = max(stepSizePixels, 1.0);

    float occlusion = 0.0;

    for(int directionIndex = 0; directionIndex < numDirections; directionIndex++) {
        float theta = (6.28318530718 / float(numDirections)) * (float(directionIndex) + directionJitter * 0.25) + rotation;
        float2 dir = float2(cos(theta), sin(theta));
        float horizon = 0.0;

        for (int stepIndex = 0; stepIndex < numSteps; stepIndex++) {
            float rayPixels = (float(stepIndex) + stepJitter + 1.0) * minStepPixels;
            float2 sampleUV = uv + dir * rayPixels / outSize;
            // Off-screen samples are REJECTED, not clamped. Clamping walked the
            // sample back to the nearest border texel, so every step of every
            // direction that left the screen landed on the *same* edge pixel and
            // voted as an occluder over and over. For a pixel within radiusPixels
            // of the viewport border that is most of its sample budget, and
            // radiusPixels is largest exactly when the player is close to a
            // surface -- which is why it showed up as a wide dark band hugging the
            // screen edge, spanning the full width, tracking no geometry, and
            // growing/shrinking with proximity rather than with the scene
            // (reported 2026-07-26). Skipping instead just lowers the effective
            // sample count near the border: less occlusion there, no fabricated
            // occluder. This is the standard SSAO border handling.
            if (any(sampleUV < halfTexel) || any(sampleUV > float2(1.0) - halfTexel)) continue;
            float2 sampleSceneUV = float2(params.offsetX, params.offsetY) + sampleUV * float2(params.scaleX, params.scaleY);

            float sampleCoverage = sceneColorTexture.sample(nearestClampSampler, sampleSceneUV).a;
            if (sampleCoverage <= 0.0001) {
                continue;
            }

            // Stencil coverage guard: skip samples from different portal layers.
            // Sampled with sampleSceneUV, not sampleUV: the mask is allocated at
            // the stencil attachment's resolution, so it shares the scene
            // textures' coordinate space, not the AO-local one.
            float sampleCov = coverageMask.sample(nearestClampSampler, sampleSceneUV).r;
            if (sampleCov < 0.5) continue;

            float sampleRawDepth = depthTexture.sample(nearestClampSampler, sampleSceneUV).r;
            if (sampleRawDepth <= 0.0001) {
                continue;
            }

            float3 sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sampleSceneUV).xyz);
            if (all(sampleNormal == float3(0.0))) {
                continue;
            }

            float sampleLinearDepth = LinearizeSceneDepth(sampleRawDepth, params);
            float3 sampleViewPos = FetchViewPos(sampleUV, sampleLinearDepth, params);
            float3 sampleVector = sampleViewPos - centerViewPos;

            float depthDiff = sampleViewPos.z - centerViewPos.z;
            float thicknessThreshold = params.maxThickness * (1.0 + centerViewPos.z * 0.05);
            float frontThickness = params.maxThickness * (0.5 + centerViewPos.z * 0.02);
            if (depthDiff > thicknessThreshold || depthDiff < -frontThickness) continue;
            // Skybox/portal guard: reject samples from incompatible camera views
            float depthRatio = max(centerViewPos.z, sampleViewPos.z) / max(min(centerViewPos.z, sampleViewPos.z), 1e-5f);
            if (depthRatio > 100.0) continue;

            float distanceSquare = max(dot(sampleVector, sampleVector), 1e-6);
            float invDistance = rsqrt(distanceSquare);
            float normalAngle = dot(centerNormal, sampleVector) * invDistance;
            float sampleHorizon = max(normalAngle - params.bias, 0.0);
            float falloff = saturate(distanceSquare * params.negInvR2 + 1.0);
            occlusion += max(sampleHorizon - horizon, 0.0) * falloff;
            horizon = max(horizon, sampleHorizon);
        }
    }

    occlusion *= params.aoMultiplier / float(numDirections * numSteps);
    float visibility = clamp(1.0 - occlusion * params.visibilityStrength, 0.0, 1.0);

    // Distance fade: see the comment at its use site in shaders/native/mt_ao.metal
    // (authoritative) for the full rationale. Fades occlusion strength to 0 past
    // fadeStartDistance so distant real geometry (e.g. sky-camera rooms) doesn't
    // self-occlude.
    float distanceFade = 1.0 - smoothstep(params.fadeStartDistance, params.fadeEndDistance, centerViewPos.z);
    float effectiveStrength = params.intensity * distanceFade;
    visibility = visibility * effectiveStrength + (1.0 - effectiveStrength);

    aoOutput.write(float4(saturate(visibility), centerLinearDepth, 0.0, 1.0), gid);
}

// AlchemyAO/SAO: a fixed, flat, Vogel-disk-distributed sample set instead of
// ssao_compute's numDirections x numSteps horizon-marching loop -- one
// dependent texture-fetch chain per sample (coverage -> depth -> normal),
// no per-direction horizon-max tracking. Targets the "fewer total samples"
// cost axis, complementary to ssao_compute_mip's "cheaper per-sample at
// distance" axis below. numSteps is repurposed as the flat sample count
// (see mt_ao.cpp Render(): the existing Intel numSteps clamp then also caps
// this algorithm's sample count for free); numDirections is unused.
kernel void ssao_compute_alchemy(
    uint2 gid [[thread_position_in_grid]],
    constant SSAOParams &params [[buffer(0)]],
    constant AOFlags &flags [[buffer(1)]],
    texture2d<float, access::sample> ditherTexture [[texture(0)]],
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::write> aoOutput [[texture(2)]],
    texture2d<float, access::sample> normalTexture [[texture(3)]],
    texture2d<float, access::sample> sceneColorTexture [[texture(4)]],
    texture2d<float, access::sample> coverageMask [[texture(5)]])
{
    float2 outSize = float2((float)aoOutput.get_width(), (float)aoOutput.get_height());
    if (gid.x >= outSize.x || gid.y >= outSize.y) return;

    // ditherTexture is intentionally unused now (world-locked noise
    // replaced it -- see WorldNoise) but its parameter/binding is left in
    // place; removal is deferred to its own isolated cleanup once the new
    // noise is confirmed correct in-game (see AGENTS.md).
    sampler nearestClampSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);

    float2 pixelCenter = float2(gid) + 0.5;
    float2 halfTexel = 0.5 / outSize;
    float2 uv = pixelCenter / outSize;
    if (flags.flipY == 1) uv.y = 1.0 - uv.y;
    float2 sceneUV = float2(params.offsetX, params.offsetY) + uv * float2(params.scaleX, params.scaleY);
    float centerCoverage = sceneColorTexture.sample(nearestClampSampler, sceneUV).a;
    if (centerCoverage <= 0.0001) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float centerDepth = depthTexture.sample(nearestClampSampler, sceneUV).r;
    if (centerDepth <= 0.0001) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float centerLinearDepth = LinearizeSceneDepth(centerDepth, params);
    float3 centerViewPos = FetchViewPos(uv, centerLinearDepth, params);

    // Sky-dome guard: depth-clamped sky geometry sits at far plane
    if (centerLinearDepth >= params.zFar * 0.99) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float3 worldPos = WorldPosFromViewPos(centerViewPos, params);

    // Visual debug for the world-locked noise fix itself -- see
    // mt_compute_ao_worldpos_debug's doc comment (mt_postprocess.cpp).
    if (params.debugMode != 0) {
        // Deliberately the *fixed* cvar cell size, not NoiseCellSize's
        // depth-adaptive one: this grid exists to be read by a human
        // eye, and the adaptive size is ~1 AO pixel by design, which
        // puts fract() right at Nyquist and turns the whole screen into
        // moire instead of a legible grid. A fixed size is also the
        // correct thing to test world-locking with -- it isolates the
        // worldPos reconstruction from the depth-dependent cell sizing.
        float cellSize = max(params.noiseCellSize, 1.0);
        float2 dbg = params.debugMode == 1 ? fract(worldPos.xy / cellSize) :
                     params.debugMode == 2 ? fract(worldPos.xz / cellSize) :
                                              fract(worldPos.yz / cellSize);
        aoOutput.write(float4(dbg.x, dbg.y, 0.0, 1.0), gid);
        return;
    }

    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sceneUV).xyz);
    if (all(centerNormal == float3(0.0))) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float3 worldNoise = AoNoise(worldPos, centerViewPos.z, pixelCenter, params);
    float rotation = worldNoise.x;
    // Per-pixel radius jitter breaks the deterministic Vogel/Fibonacci
    // spiral coherence between neighboring pixels. Without it, every pixel
    // places its i-th sample at exactly the same fractional ring radius, so
    // the golden-angle spiral becomes visible as a rotating pinwheel once
    // radiusPixels (which grows as centerViewPos.z shrinks, i.e. up close)
    // spreads the rings far enough apart on-screen to resolve individually.
    // Reported 2026-07-17: "AO coordinates seem to rotate... when close to
    // something" -- this is the fix, same jitter role ssao_compute's own
    // stepJitter plays to avoid the equivalent artifact in its own grid.
    float radiusJitter = worldNoise.y;

    float radiusPixels = params.radiusToScreen / max(centerViewPos.z, 1e-5);
    // Cap the screen-space sample radius. Uncapped, standing close to any
    // surface (centerViewPos.z small -- completely ordinary at typical
    // player-to-wall distances, not just extreme clipping) sends
    // radiusPixels past the AO texture's own dimensions: every sample's UV
    // then hard-clamps to the texture edge (see the sampleUV clamp below),
    // collapsing the sample disk into a handful of edge pixels unrelated to
    // local geometry -- and which edge pixel gets hit is hypersensitive to
    // tiny position/angle changes, producing a visibly rotating/stretching
    // AO pattern up close. Reported 2026-07-19. Bounding to a fraction of
    // the shorter texture dimension keeps the sample disk within a
    // meaningful local neighborhood at any distance.
    radiusPixels = min(radiusPixels, min(outSize.x, outSize.y) * 0.5);
    int sampleCount = clamp(params.numSteps, 1, NUM_SAMPLES);
    float occlusion = 0.0;

    for (int i = 0; i < sampleCount; i++) {
        float ang = GOLDEN_ANGLE * float(i) + rotation;
        // Vogel/Fibonacci disk: sqrt spacing gives uniform sample density
        // across the disk area, not just around its circumference.
        float radiusFrac = sqrt((float(i) + 0.5 + radiusJitter) / float(sampleCount));
        float2 dir = float2(cos(ang), sin(ang));
        float rayPixels = radiusFrac * radiusPixels;
        float2 sampleUV = uv + dir * rayPixels / outSize;
        // Off-screen samples are REJECTED, not clamped. Clamping walked the
        // sample back to the nearest border texel, so every step of every
        // direction that left the screen landed on the *same* edge pixel and
        // voted as an occluder over and over. For a pixel within radiusPixels
        // of the viewport border that is most of its sample budget, and
        // radiusPixels is largest exactly when the player is close to a
        // surface -- which is why it showed up as a wide dark band hugging the
        // screen edge, spanning the full width, tracking no geometry, and
        // growing/shrinking with proximity rather than with the scene
        // (reported 2026-07-26). Skipping instead just lowers the effective
        // sample count near the border: less occlusion there, no fabricated
        // occluder. This is the standard SSAO border handling.
        if (any(sampleUV < halfTexel) || any(sampleUV > float2(1.0) - halfTexel)) continue;
        float2 sampleSceneUV = float2(params.offsetX, params.offsetY) + sampleUV * float2(params.scaleX, params.scaleY);

        float sampleCoverage = sceneColorTexture.sample(nearestClampSampler, sampleSceneUV).a;
        if (sampleCoverage <= 0.0001) {
            continue;
        }

        // Stencil coverage guard: skip samples from different portal layers.
        // Sampled with sampleSceneUV, not sampleUV: the mask is allocated at
        // the stencil attachment's resolution, so it shares the scene
        // textures' coordinate space, not the AO-local one.
        float sampleCov = coverageMask.sample(nearestClampSampler, sampleSceneUV).r;
        if (sampleCov < 0.5) continue;

        float sampleRawDepth = depthTexture.sample(nearestClampSampler, sampleSceneUV).r;
        if (sampleRawDepth <= 0.0001) {
            continue;
        }

        float3 sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sampleSceneUV).xyz);
        if (all(sampleNormal == float3(0.0))) {
            continue;
        }

        float sampleLinearDepth = LinearizeSceneDepth(sampleRawDepth, params);
        float3 sampleViewPos = FetchViewPos(sampleUV, sampleLinearDepth, params);
        float3 sampleVector = sampleViewPos - centerViewPos;

        float depthDiff = sampleViewPos.z - centerViewPos.z;
        float thicknessThreshold = params.maxThickness * (1.0 + centerViewPos.z * 0.05);
        float frontThickness = params.maxThickness * (0.5 + centerViewPos.z * 0.02);
        if (depthDiff > thicknessThreshold || depthDiff < -frontThickness) continue;
        // Skybox/portal guard: reject samples from incompatible camera views
        float depthRatio = max(centerViewPos.z, sampleViewPos.z) / max(min(centerViewPos.z, sampleViewPos.z), 1e-5f);
        if (depthRatio > 100.0) continue;

        // Same per-sample math as ssao_compute (proven correct there),
        // applied to a flat sample set instead of nested direction/step
        // loops with horizon-max tracking -- deliberately NOT adding any
        // extra distance-weighting term (a previous version multiplied by
        // 1/|v| to mimic the classic AlchemyAO paper's falloff shape; that
        // was never visually validated and is the prime suspect for a
        // reported "coordinate system off / not responding to normals"
        // artifact, so it's removed rather than debugged further -- keep
        // this kernel's only difference from ssao_compute being the sample
        // pattern, not the occlusion math itself).
        float distanceSquare = max(dot(sampleVector, sampleVector), 1e-6);
        float invDistance = rsqrt(distanceSquare);
        float normalAngle = dot(centerNormal, sampleVector) * invDistance;
        float falloff = saturate(distanceSquare * params.negInvR2 + 1.0);
        occlusion += max(normalAngle - params.bias, 0.0) * falloff;
    }

    occlusion *= params.aoMultiplier / float(sampleCount);
    float visibility = clamp(1.0 - occlusion * params.visibilityStrength, 0.0, 1.0);

    // Distance fade: see the comment at its use site in shaders/native/mt_ao.metal
    // (authoritative) for the full rationale. Fades occlusion strength to 0 past
    // fadeStartDistance so distant real geometry (e.g. sky-camera rooms) doesn't
    // self-occlude.
    float distanceFade = 1.0 - smoothstep(params.fadeStartDistance, params.fadeEndDistance, centerViewPos.z);
    float effectiveStrength = params.intensity * distanceFade;
    visibility = visibility * effectiveStrength + (1.0 - effectiveStrength);

    aoOutput.write(float4(saturate(visibility), centerLinearDepth, 0.0, 1.0), gid);
}

// Depth mip pyramid seed: writes linearized view-space depth from the raw
// SceneDepthStencil into mip 0 of a dedicated R16Float pyramid texture.
// Mips 1+ are generated afterward via MTLBlitCommandEncoder::generateMipmaps
// (mt_ao.cpp Execute()) -- box-filtered, which is fine here since a distant
// horizon-search tap only needs an approximately-right coarse depth, not a
// conservative Hi-Z-style bound.
kernel void linearize_depth_mip0(
    uint2 gid [[thread_position_in_grid]],
    constant SSAOParams &params [[buffer(0)]],
    texture2d<float, access::sample> depthTexture [[texture(0)]],
    texture2d<float, access::write> pyramidOut [[texture(1)]])
{
    if (gid.x >= pyramidOut.get_width() || gid.y >= pyramidOut.get_height()) return;

    constexpr sampler nearestClampSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / float2((float)pyramidOut.get_width(), (float)pyramidOut.get_height());
    float raw = depthTexture.sample(nearestClampSampler, uv).r;
    // Preserve the invalid-texel sentinel explicitly: box-filtering an
    // unconditionally-linearized value across a sky/geometry edge into
    // coarser mips would blend a real depth into this pixel and lose the
    // "invalid" signal ssao_compute_mip's sampleLinearDepth <= 0.0001 guard
    // relies on downstream. Known, accepted limitation: this can still blend
    // a stale 0.0 into a bogus mid-value at such an edge in coarser mips --
    // not fixed here, since the thickness/depth-ratio rejection in
    // ssao_compute_mip already discards samples too far from centerViewPos.z,
    // and only distant, already-falloff-discounted taps ever reach coarse
    // mips.
    float linear = (raw <= 0.0001) ? 0.0 : LinearizeSceneDepth(raw, params);
    pyramidOut.write(float4(linear, 0.0, 0.0, 1.0), gid);
}

// Depth-mip-pyramid horizon sampling: identical GTAO horizon-marching
// structure/loop count to ssao_compute, but distant steps in the loop read a
// coarser mip of the pyramid seeded above instead of always sampling
// full-res depth. Targets the "cheaper per-sample cost at distance" axis --
// same sample count as ssao_compute, cheaper texture-cache behavior for far
// taps. Coverage and normal fetches stay full-res; there is no normal
// pyramid (not worth the extra texture/blit cost for uncertain benefit).
kernel void ssao_compute_mip(
    uint2 gid [[thread_position_in_grid]],
    constant SSAOParams &params [[buffer(0)]],
    constant AOFlags &flags [[buffer(1)]],
    texture2d<float, access::sample> ditherTexture [[texture(0)]],
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::write> aoOutput [[texture(2)]],
    texture2d<float, access::sample> normalTexture [[texture(3)]],
    texture2d<float, access::sample> sceneColorTexture [[texture(4)]],
    texture2d<float, access::sample> coverageMask [[texture(5)]],
    texture2d<float, access::sample> depthPyramid [[texture(6)]])
{
    float2 outSize = float2((float)aoOutput.get_width(), (float)aoOutput.get_height());
    if (gid.x >= outSize.x || gid.y >= outSize.y) return;

    // ditherTexture is intentionally unused now (world-locked noise
    // replaced it -- see WorldNoise) but its parameter/binding is left in
    // place; removal is deferred to its own isolated cleanup once the new
    // noise is confirmed correct in-game (see AGENTS.md).
    sampler nearestClampSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);

    float2 pixelCenter = float2(gid) + 0.5;
    float2 halfTexel = 0.5 / outSize;
    float2 uv = pixelCenter / outSize;
    if (flags.flipY == 1) uv.y = 1.0 - uv.y;
    float2 sceneUV = float2(params.offsetX, params.offsetY) + uv * float2(params.scaleX, params.scaleY);
    float centerCoverage = sceneColorTexture.sample(nearestClampSampler, sceneUV).a;
    if (centerCoverage <= 0.0001) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float centerDepth = depthTexture.sample(nearestClampSampler, sceneUV).r;
    if (centerDepth <= 0.0001) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float centerLinearDepth = LinearizeSceneDepth(centerDepth, params);
    float3 centerViewPos = FetchViewPos(uv, centerLinearDepth, params);

    // Sky-dome guard: depth-clamped sky geometry sits at far plane
    if (centerLinearDepth >= params.zFar * 0.99) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float3 worldPos = WorldPosFromViewPos(centerViewPos, params);

    // Visual debug for the world-locked noise fix itself -- see
    // mt_compute_ao_worldpos_debug's doc comment (mt_postprocess.cpp).
    if (params.debugMode != 0) {
        // Deliberately the *fixed* cvar cell size, not NoiseCellSize's
        // depth-adaptive one: this grid exists to be read by a human
        // eye, and the adaptive size is ~1 AO pixel by design, which
        // puts fract() right at Nyquist and turns the whole screen into
        // moire instead of a legible grid. A fixed size is also the
        // correct thing to test world-locking with -- it isolates the
        // worldPos reconstruction from the depth-dependent cell sizing.
        float cellSize = max(params.noiseCellSize, 1.0);
        float2 dbg = params.debugMode == 1 ? fract(worldPos.xy / cellSize) :
                     params.debugMode == 2 ? fract(worldPos.xz / cellSize) :
                                              fract(worldPos.yz / cellSize);
        aoOutput.write(float4(dbg.x, dbg.y, 0.0, 1.0), gid);
        return;
    }

    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sceneUV).xyz);
    if (all(centerNormal == float3(0.0))) {
        aoOutput.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float3 worldNoise = AoNoise(worldPos, centerViewPos.z, pixelCenter, params);
    float rotation = worldNoise.x;
    float stepJitter = worldNoise.y;
    float directionJitter = worldNoise.z;

    float radiusPixels = params.radiusToScreen / max(centerViewPos.z, 1e-5);
    // Cap the screen-space sample radius. Uncapped, standing close to any
    // surface (centerViewPos.z small -- completely ordinary at typical
    // player-to-wall distances, not just extreme clipping) sends
    // radiusPixels past the AO texture's own dimensions: every sample's UV
    // then hard-clamps to the texture edge (see the sampleUV clamp below),
    // collapsing the sample disk into a handful of edge pixels unrelated to
    // local geometry -- and which edge pixel gets hit is hypersensitive to
    // tiny position/angle changes, producing a visibly rotating/stretching
    // AO pattern up close. Reported 2026-07-19. Bounding to a fraction of
    // the shorter texture dimension keeps the sample disk within a
    // meaningful local neighborhood at any distance.
    radiusPixels = min(radiusPixels, min(outSize.x, outSize.y) * 0.5);
    const int numSteps = clamp(params.numSteps, 2, 8); // Capped for Intel
    const int numDirections = clamp(params.numDirections, 2, 6); // Capped for Intel
    float stepSizePixels = radiusPixels / (float(numSteps) + 1.0);
    float minStepPixels = max(stepSizePixels, 1.0);
    float maxLod = float(depthPyramid.get_num_mip_levels() - 1);

    float occlusion = 0.0;

    for(int directionIndex = 0; directionIndex < numDirections; directionIndex++) {
        float theta = (6.28318530718 / float(numDirections)) * (float(directionIndex) + directionJitter * 0.25) + rotation;
        float2 dir = float2(cos(theta), sin(theta));
        float horizon = 0.0;

        for (int stepIndex = 0; stepIndex < numSteps; stepIndex++) {
            float rayPixels = (float(stepIndex) + stepJitter + 1.0) * minStepPixels;
            float2 sampleUV = uv + dir * rayPixels / outSize;
            // Off-screen samples are REJECTED, not clamped. Clamping walked the
            // sample back to the nearest border texel, so every step of every
            // direction that left the screen landed on the *same* edge pixel and
            // voted as an occluder over and over. For a pixel within radiusPixels
            // of the viewport border that is most of its sample budget, and
            // radiusPixels is largest exactly when the player is close to a
            // surface -- which is why it showed up as a wide dark band hugging the
            // screen edge, spanning the full width, tracking no geometry, and
            // growing/shrinking with proximity rather than with the scene
            // (reported 2026-07-26). Skipping instead just lowers the effective
            // sample count near the border: less occlusion there, no fabricated
            // occluder. This is the standard SSAO border handling.
            if (any(sampleUV < halfTexel) || any(sampleUV > float2(1.0) - halfTexel)) continue;
            float2 sampleSceneUV = float2(params.offsetX, params.offsetY) + sampleUV * float2(params.scaleX, params.scaleY);

            float sampleCoverage = sceneColorTexture.sample(nearestClampSampler, sampleSceneUV).a;
            if (sampleCoverage <= 0.0001) {
                continue;
            }

            // Stencil coverage guard: skip samples from different portal layers.
            // Sampled with sampleSceneUV, not sampleUV: the mask is allocated at
            // the stencil attachment's resolution, so it shares the scene
            // textures' coordinate space, not the AO-local one.
            float sampleCov = coverageMask.sample(nearestClampSampler, sampleSceneUV).r;
            if (sampleCov < 0.5) continue;

            // Mip-selected depth fetch: distant steps read a coarser,
            // cache-friendlier level of the pre-linearized depth pyramid
            // instead of always sampling full-res depth (the actual cost
            // driver this algorithm targets). 8px near-field threshold keeps
            // typical near taps (minStepPixels ~1-4px) at lod 0.
            float lod = clamp(log2(max(rayPixels / 8.0, 1.0)), 0.0, maxLod);
            float sampleLinearDepth = depthPyramid.sample(nearestClampSampler, sampleSceneUV, level(lod)).r;
            if (sampleLinearDepth <= 0.0001) {
                continue;
            }

            float3 sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestClampSampler, sampleSceneUV).xyz);
            if (all(sampleNormal == float3(0.0))) {
                continue;
            }

            float3 sampleViewPos = FetchViewPos(sampleUV, sampleLinearDepth, params);
            float3 sampleVector = sampleViewPos - centerViewPos;

            float depthDiff = sampleViewPos.z - centerViewPos.z;
            float thicknessThreshold = params.maxThickness * (1.0 + centerViewPos.z * 0.05);
            float frontThickness = params.maxThickness * (0.5 + centerViewPos.z * 0.02);
            if (depthDiff > thicknessThreshold || depthDiff < -frontThickness) continue;
            // Skybox/portal guard: reject samples from incompatible camera views
            float depthRatio = max(centerViewPos.z, sampleViewPos.z) / max(min(centerViewPos.z, sampleViewPos.z), 1e-5f);
            if (depthRatio > 100.0) continue;

            float distanceSquare = max(dot(sampleVector, sampleVector), 1e-6);
            float invDistance = rsqrt(distanceSquare);
            float normalAngle = dot(centerNormal, sampleVector) * invDistance;
            float sampleHorizon = max(normalAngle - params.bias, 0.0);
            float falloff = saturate(distanceSquare * params.negInvR2 + 1.0);
            occlusion += max(sampleHorizon - horizon, 0.0) * falloff;
            horizon = max(horizon, sampleHorizon);
        }
    }

    occlusion *= params.aoMultiplier / float(numDirections * numSteps);
    float visibility = clamp(1.0 - occlusion * params.visibilityStrength, 0.0, 1.0);

    // Distance fade: see the comment at its use site in shaders/native/mt_ao.metal
    // (authoritative) for the full rationale. Fades occlusion strength to 0 past
    // fadeStartDistance so distant real geometry (e.g. sky-camera rooms) doesn't
    // self-occlude.
    float distanceFade = 1.0 - smoothstep(params.fadeStartDistance, params.fadeEndDistance, centerViewPos.z);
    float effectiveStrength = params.intensity * distanceFade;
    visibility = visibility * effectiveStrength + (1.0 - effectiveStrength);

    aoOutput.write(float4(saturate(visibility), centerLinearDepth, 0.0, 1.0), gid);
}

kernel void bilateral_blur(
    uint2 gid [[thread_position_in_grid]],
    constant AOBlurParams &params [[buffer(0)]],
    texture2d<float, access::read> sourceTexture [[texture(0)]],
    texture2d<float, access::write> destTexture [[texture(1)]],
    texture2d<float, access::sample> normalTexture [[texture(2)]])
{
    if (gid.x >= destTexture.get_width() || gid.y >= destTexture.get_height()) return;

    constexpr sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);
    float4 centerSample = sourceTexture.read(gid);
    float center = centerSample.r;
    float depth = centerSample.g;
    if (depth <= 1e-5) {
        destTexture.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float sum = center;
    float totalWeight = 1.0;
    bool useNormals = params.normalAware != 0;
    float2 texSize = float2((float)sourceTexture.get_width(), (float)sourceTexture.get_height());
    float2 centerUV = (float2(gid) + 0.5) / texSize;
    if (params.flipY != 0) {
        centerUV.y = 1.0 - centerUV.y;
    }
    float2 centerSceneUV = float2(params.offsetX, params.offsetY) + centerUV * float2(params.scaleX, params.scaleY);
    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, centerSceneUV).xyz);
    if (all(centerNormal == float3(0.0))) {
        useNormals = false;
    }

    int2 offsets[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };

    for(int i = 0; i < 8; i++) {
        uint2 sampleCoord = uint2(int2(gid) + offsets[i]);
        if (sampleCoord.x < sourceTexture.get_width() && sampleCoord.y < sourceTexture.get_height()) {
            float4 sampleValue = sourceTexture.read(sampleCoord);
            float val = sampleValue.r;
            float sampleDepth = sampleValue.g;
            if (sampleDepth <= 1e-5) {
                continue;
            }
            // Hard-reject background bleeding onto foreground: the smooth
            // exp2 falloff below is calibrated against typical in-scene
            // depth deltas (gl_ssao_blur), not maxThickness's much finer
            // scale (~1.25 units) -- at that scale the falloff alone barely
            // attenuates (weight stays ~0.996 right at this threshold), so
            // without an explicit cutoff a foreground pixel next to a
            // distant background (e.g. a doorway silhouette) blends in
            // background AO as a halo. Restores a guard that was present
            // before this kernel's weight formula was reworked to 8 taps.
            if (sampleDepth - depth > params.maxThickness) {
                continue;
            }
            float r = (i >= 4) ? 1.41421356 : 1.0;
            float deltaZ = (sampleDepth - depth) * params.blurSharpness;
            float weight = exp2(-r * r * 0.22222222 - deltaZ * deltaZ);
            if (i >= 4) {
                weight *= 0.7071;
            }
            if (useNormals) {
                float2 sampleUV = (float2(sampleCoord) + 0.5) / texSize;
                if (params.flipY != 0) {
                    sampleUV.y = 1.0 - sampleUV.y;
                }
                float2 sampleSceneUV = float2(params.offsetX, params.offsetY) + sampleUV * float2(params.scaleX, params.scaleY);
                float3 sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, sampleSceneUV).xyz);
                weight *= mix(0.35, 1.0, NormalWeight(centerNormal, sampleNormal));
            }
            weight = saturate(weight);
            sum += val * weight;
            totalWeight += weight;
        }
    }

    float blurred = sum / totalWeight;
    if (params.applyExponent != 0) {
        blurred = pow(saturate(blurred), params.powExponent);
    }
    destTexture.write(float4(blurred, depth, 0, 1.0), gid);
}

kernel void ao_upsample_fullres(
    uint2 gid [[thread_position_in_grid]],
    constant AOFullresParams &params [[buffer(0)]],
    texture2d<float, access::sample> lowresAO [[texture(0)]],
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::sample> normalTexture [[texture(2)]],
    texture2d<float, access::write> outTexture [[texture(3)]])
{
    if (gid.x >= outTexture.get_width() || gid.y >= outTexture.get_height()) return;

    constexpr sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);
    float2 localUV = (float2(gid) + 0.5) / params.fullRes;
    float2 sceneUV = params.sceneOffset + localUV * params.sceneScale;
    float rawDepth = depthTexture.sample(nearestSampler, sceneUV).r;
    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, sceneUV).xyz);
    if (rawDepth <= 0.0001 || all(centerNormal == float3(0.0))) {
        outTexture.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    float sceneDepth = LinearizeDepth(rawDepth, params.zNear, params.zFar);
    float2 aoSize = float2((float)lowresAO.get_width(), (float)lowresAO.get_height());
    float2 aoUV = localUV;
    float2 aoCoord = aoUV * aoSize - 0.5;
    float2 aoBase = floor(aoCoord);
    float2 aoFrac = fract(aoCoord);
    float combineSharpness = max(params.blurSharpness * 4.0, 0.02);

    float alpha = 0.0;
    float totalWeight = 0.0;
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            float2 tap = float2((float)x, (float)y);
            float2 tapCoord = clamp(aoBase + tap, float2(0.0), aoSize - 1.0);
            float2 tapUV = (tapCoord + 0.5) / aoSize;
            float2 tapSceneUV = params.sceneOffset + tapUV * params.sceneScale;
            float4 sampleAO = lowresAO.sample(nearestSampler, tapUV);
            if (sampleAO.y <= 1e-5) {
                continue;
            }

            float bilinearWeight = ((x == 0) ? (1.0 - aoFrac.x) : aoFrac.x) *
                                   ((y == 0) ? (1.0 - aoFrac.y) : aoFrac.y);
            float depthDelta = (sampleAO.y - sceneDepth) * combineSharpness;
            float weight = bilinearWeight * exp2(-depthDelta * depthDelta);
            if (params.normalAware != 0) {
                float3 tapNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, tapSceneUV).xyz);
                weight *= NormalWeight(centerNormal, tapNormal);
            }

            alpha += (1.0 - sampleAO.x) * weight;
            totalWeight += weight;
        }
    }

    alpha = (totalWeight > 1e-5) ? alpha / totalWeight : 0.0;
    outTexture.write(float4(1.0 - alpha, sceneDepth, 0.0, 1.0), gid);
}

kernel void ao_atrous_fullres(
    uint2 gid [[thread_position_in_grid]],
    constant AOFullresParams &params [[buffer(0)]],
    texture2d<float, access::read> sourceTexture [[texture(0)]],
    texture2d<float, access::sample> normalTexture [[texture(1)]],
    texture2d<float, access::write> destTexture [[texture(2)]])
{
    if (gid.x >= destTexture.get_width() || gid.y >= destTexture.get_height()) return;

    constexpr sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);
    float4 centerAO = sourceTexture.read(gid);
    float centerDepth = centerAO.y;
    float2 localUV = (float2(gid) + 0.5) / params.fullRes;
    float2 sceneUV = params.sceneOffset + localUV * params.sceneScale;
    float3 centerNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, sceneUV).xyz);
    if (centerDepth <= 1e-5 || all(centerNormal == float3(0.0))) {
        destTexture.write(float4(1.0, 0.0, 0.0, 1.0), gid);
        return;
    }

    int stepSize = max(params.atrousStep, 1);
    int2 offsets[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };
    float sum = centerAO.x;
    float totalWeight = 1.0;
    for (int i = 0; i < 8; i++) {
        int2 icoord = int2(gid) + offsets[i] * stepSize;
        if (icoord.x < 0 || icoord.y < 0 ||
            icoord.x >= (int)sourceTexture.get_width() ||
            icoord.y >= (int)sourceTexture.get_height()) {
            continue;
        }

        uint2 sampleCoord = uint2(icoord);
        float4 sampleAO = sourceTexture.read(sampleCoord);
        if (sampleAO.y <= 1e-5) {
            continue;
        }

        float2 sampleLocalUV = (float2(sampleCoord) + 0.5) / params.fullRes;
        float2 sampleSceneUV = params.sceneOffset + sampleLocalUV * params.sceneScale;
        float3 sampleNormal = DecodeSceneNormal(normalTexture.sample(nearestSampler, sampleSceneUV).xyz);
        float depthDelta = (sampleAO.y - centerDepth) * max(params.blurSharpness * 4.0, 0.02);
        float spatial = (i >= 4) ? 0.5 : 0.75;
        float weight = spatial * exp2(-depthDelta * depthDelta);
        if (params.normalAware != 0) {
            weight *= NormalWeight(centerNormal, sampleNormal);
        }

        sum += sampleAO.x * weight;
        totalWeight += weight;
    }

    destTexture.write(float4(sum / totalWeight, centerDepth, 0.0, 1.0), gid);
}

struct AOCombineParams {
    int debugMode;
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    float blurSharpness;
    float zNear;
    float zFar;
    float combineSmooth;
    int fullresAO;
};

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut ssao_combine_vs(uint vid [[vertex_id]]) {
    VSOut out;
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    out.position = float4(pos[vid], 0.0, 1.0);
    out.uv = out.position.xy * 0.5 + float2(0.5);
    return out;
}

fragment float4 ssao_combine_fs(VSOut in [[stage_in]],
                                constant AOCombineParams &params [[buffer(0)]],
                                texture2d<float, access::sample> aoTexture [[texture(0)]],
                                texture2d<float, access::sample> fogTexture [[texture(1)]],
                                texture2d<float, access::sample> normalTexture [[texture(2)]],
                                texture2d<float, access::sample> depthTexture [[texture(3)]],
                                texture2d<float, access::sample> sceneColorTexture [[texture(4)]])
{
    constexpr sampler linearSampler(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    constexpr sampler nearestSampler(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);

    // Metal's fullscreen-triangle UV is vertically opposite to the scene
    // render textures. Flip every combine input together so AO remains aligned
    // with fog, normals, and depth.
    float2 sceneUV = float2(in.uv.x, 1.0 - in.uv.y);
    float2 aoUV = sceneUV;
    float2 fogUV = float2(params.offsetX, params.offsetY) + sceneUV * float2(params.scaleX, params.scaleY);
    float4 ssao = aoTexture.sample(linearSampler, aoUV);
    float4 fogSample = fogTexture.sample(nearestSampler, fogUV);
    float3 sceneNormal = normalTexture.sample(linearSampler, fogUV).xyz;
    float attenuation = ssao.x;
    float rawSceneDepth = depthTexture.sample(nearestSampler, fogUV).r;
    float normalizedDepth = clamp(1.0 - rawSceneDepth, 0.0, 1.0);
    float sceneDepth = 1.0 / (normalizedDepth * (1.0 / params.zFar - 1.0 / params.zNear) + max(1.0 / params.zNear, 1e-8));
    float depthSignal = 1.0 - exp2(-sceneDepth * 0.005);
    float depthMask = saturate(depthSignal);
    float3 decodedNormal = sceneNormal * 2.0 - 1.0;

    // Scene color alpha: 0 for sky, 1 for scene geometry — used as smooth
    // weight to prevent hard AO transitions at sky dome boundaries.
    float sceneAlpha = sceneColorTexture.sample(nearestSampler, fogUV).a;
    // AO confidence from nearest-sampled depth — linear interpolation at
    // boundaries would blend sky (ssao.y≈0) with scene, making aoConfidence
    // incorrectly treat boundary pixels as valid scene. Nearest keeps the
    // binary distinction: 0 for sky, non-zero for scene.
    float aoConfidenceRaw = aoTexture.sample(nearestSampler, aoUV).y;
    float aoConfidence = smoothstep(0.0, 1e-5, aoConfidenceRaw);
    // Full-res nearest-sampled normal: sky dome pixels have cleared normals
    // (0,0,0). Linear sampling at boundaries bleeds scene normals into sky,
    // but nearest keeps the binary distinction.
    float3 nearestNormal = normalTexture.sample(nearestSampler, fogUV).xyz * 2.0 - 1.0;
    float normalConfidence = saturate(length(nearestNormal) * 10.0);
    bool isFarPlane = (sceneDepth >= params.zFar * 0.99);

    if (params.debugMode == 0) {
        if (isFarPlane || length(decodedNormal) <= 0.1) {
            return float4(fogSample.rgb, 0.0);
        }

        if (params.fullresAO != 0) {
            float aoAlpha = (1.0 - ssao.x) * depthMask;
            // Less aggressive smoothstep to preserve subtle AO while still killing speckles
            aoAlpha *= smoothstep(0.002, 0.020, aoAlpha);
            // AO confidence + normal confidence: smooth blend at sky boundaries
            aoAlpha *= sceneAlpha * aoConfidence * normalConfidence;
            return float4(fogSample.rgb, aoAlpha);
        }

        float centerAlpha = (1.0 - attenuation) * saturate(1.0 - exp2(-ssao.y * 0.005));
        float2 aoTexel = 1.0 / float2((float)aoTexture.get_width(), (float)aoTexture.get_height());
        float4 ssaoL = aoTexture.sample(nearestSampler, aoUV + float2(-aoTexel.x, 0.0));
        float4 ssaoR = aoTexture.sample(nearestSampler, aoUV + float2( aoTexel.x, 0.0));
        float4 ssaoU = aoTexture.sample(nearestSampler, aoUV + float2(0.0, -aoTexel.y));
        float4 ssaoD = aoTexture.sample(nearestSampler, aoUV + float2(0.0,  aoTexel.y));
        float centerDepth = max(ssao.y, 1e-5);
        float blurSharpness = max(params.blurSharpness * 4.0, 0.02);
        float4 taps[4] = { ssaoL, ssaoR, ssaoU, ssaoD };
        float alphaSum = centerAlpha;
        float weightSum = 1.0;
        for (int i = 0; i < 4; i++) {
            if (taps[i].y < 2.0) {
                continue;
            }
            float tapAlpha = (1.0 - taps[i].x) * saturate(1.0 - exp2(-taps[i].y * 0.005));
            float depthDelta = (taps[i].y - centerDepth) * blurSharpness;
            float weight = exp2(-0.35 - depthDelta * depthDelta);
            alphaSum += tapAlpha * weight;
            weightSum += weight;
        }
        float aoAlpha = mix(centerAlpha, alphaSum / weightSum, saturate(params.combineSmooth));
        // Depth-weighted neighbor average — prevents stair-edge bleeding
        float neighborAlpha = 0.0;
        float neighborWeight = 0.0;
        float4 neighborVals[4] = { ssaoL, ssaoR, ssaoU, ssaoD };
        for (int n = 0; n < 4; n++) {
            if (neighborVals[n].y < 2.0) continue;
            float nAlpha = (1.0 - neighborVals[n].x) * saturate(1.0 - exp2(-neighborVals[n].y * 0.005));
            float nDepthDelta = abs(neighborVals[n].y - centerDepth) * blurSharpness;
            float nWeight = exp2(-0.35 - nDepthDelta * nDepthDelta);
            neighborAlpha += nAlpha * nWeight;
            neighborWeight += nWeight;
        }
        neighborAlpha = (neighborWeight > 1e-5) ? neighborAlpha / neighborWeight : 0.0;

        // Speckle removal: pull weak isolated pixels toward neighbor average.
        // A single bright pixel surrounded by darker neighbors is noise.
        if (aoAlpha < neighborAlpha * 0.85 && neighborAlpha > 0.005) {
            aoAlpha = mix(aoAlpha, neighborAlpha, 0.8);
        }
        // Hard floor: no pixel can be drastically brighter than its neighbors
        aoAlpha = max(aoAlpha, neighborAlpha * 0.3);

        // Multi-bounce AO approximation (Jimenez 2016)
        // Helps darken corners while preventing the "glow" artifact around edges.
        float3 albedo = float3(0.5); // Assume neutral albedo
        float3 a = 2.0 * albedo - 0.33;
        float3 b = -4.8 * albedo + 0.64;
        float3 c = 2.8 * albedo + 0.69;
        float3 multiBounce = max(aoAlpha, ((a * aoAlpha + b) * aoAlpha + c) * aoAlpha);
        aoAlpha = multiBounce.x;

        // When a pixel is isolated (no valid depth neighbors), suppress AO
        // more aggressively to prevent white speckles from bright fog
        // blending over dark scene color at depth discontinuities.
        float neighborConfidence = saturate(neighborWeight * 2.0);
        float speckleThreshold = mix(0.005, 0.001, neighborConfidence);
        float speckleEdge      = mix(0.030, 0.015, neighborConfidence);
        aoAlpha *= smoothstep(speckleThreshold, speckleEdge, aoAlpha);
        // AO confidence + normal confidence: smooth blend at sky boundaries
        aoAlpha *= sceneAlpha * aoConfidence * normalConfidence;
        return float4(fogSample.rgb, aoAlpha);
    }
    else if (params.debugMode < 3)
        return float4(float3(attenuation), 1.0);
    else if (params.debugMode == 3)
        return float4(ssao.yyy / 1000.0, 1.0);
    else if (params.debugMode == 4)
        return float4(sceneNormal, 1.0);
    else if (params.debugMode == 5)
        return float4(float3(ssao.x), 1.0);
    else if (params.debugMode == 6)
        return float4(float3(depthSignal), 1.0);
    else if (params.debugMode == 7)
        return float4(float3(step(1e-5, ssao.y)), 1.0);
    else if (params.debugMode == 8)
        return float4(float3(depthMask), 1.0);
    else if (params.debugMode == 9)
        return float4(float3(sceneAlpha), 1.0);
    else if (params.debugMode == 10)
        return float4(float3(aoConfidence), 1.0);
    else
        return float4(ssao.xyz, 1.0);
}
)";

static const char* COVERAGE_MASK_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct CoverageVSOut {
    float4 position [[position]];
};

vertex CoverageVSOut coverage_mask_vs(uint vid [[vertex_id]]) {
    CoverageVSOut out;
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    out.position = float4(pos[vid], 0.0, 1.0);
    return out;
}

fragment float4 coverage_mask_fs(CoverageVSOut in [[stage_in]]) {
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

MtAOModule::MtAOModule(MetalRenderDevice* device) : fb(device) {
    auto shaderManager = fb->GetShaderManager();
    ssaoPSO = shaderManager->CreateComputePipeline("ssao_compute", SSAO_COMPUTE_SOURCE, "SSAO ssao_compute");
    ssaoAlchemyPSO = shaderManager->CreateComputePipeline("ssao_compute_alchemy", SSAO_COMPUTE_SOURCE, "SSAO ssao_compute_alchemy");
    depthLinearizePSO = shaderManager->CreateComputePipeline("linearize_depth_mip0", SSAO_COMPUTE_SOURCE, "SSAO linearize_depth_mip0");
    ssaoMipPSO = shaderManager->CreateComputePipeline("ssao_compute_mip", SSAO_COMPUTE_SOURCE, "SSAO ssao_compute_mip");
    blurPSO = shaderManager->CreateComputePipeline("bilateral_blur", SSAO_COMPUTE_SOURCE, "SSAO bilateral_blur");
    upsamplePSO = shaderManager->CreateComputePipeline("ao_upsample_fullres", SSAO_COMPUTE_SOURCE, "SSAO fullres upsample");
    atrousPSO = shaderManager->CreateComputePipeline("ao_atrous_fullres", SSAO_COMPUTE_SOURCE, "SSAO fullres atrous");

    auto createCombinePipeline = [&](MTL::Library* library) -> MTL::RenderPipelineState* {
        if (!library)
            return nullptr;
        auto vert = library->newFunction(NS::String::string("ssao_combine_vs", NS::UTF8StringEncoding));
        auto frag = library->newFunction(NS::String::string("ssao_combine_fs", NS::UTF8StringEncoding));
        if (!vert || !frag) {
            if (vert) vert->release();
            if (frag) frag->release();
            return nullptr;
        }

        auto desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vert);
        desc->setFragmentFunction(frag);
        desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        desc->colorAttachments()->object(0)->setBlendingEnabled(true);
        desc->colorAttachments()->object(0)->setRgbBlendOperation(MTL::BlendOperationAdd);
        desc->colorAttachments()->object(0)->setAlphaBlendOperation(MTL::BlendOperationAdd);
        desc->colorAttachments()->object(0)->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
        desc->colorAttachments()->object(0)->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        desc->colorAttachments()->object(0)->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        desc->colorAttachments()->object(0)->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        desc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
        desc->setStencilAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);

        NS::Error* error = nullptr;
        auto pso = fb->device->device->newRenderPipelineState(desc, &error);
        if (!pso && error) {
            Printf(PRINT_LOG, "Metal: Failed to create SSAO combine render pipeline: %s\n",
                   error->localizedDescription()->utf8String());
            error->release();
        }

        desc->release();
        vert->release();
        frag->release();
        return pso;
    };

    combineRenderPSO = createCombinePipeline(shaderManager->LoadNativeLibrary());
    if (!combineRenderPSO) {
        auto sourceString = NS::String::string(SSAO_COMPUTE_SOURCE, NS::UTF8StringEncoding);
        auto compileOptions = MTL::CompileOptions::alloc()->init();
        compileOptions->setLanguageVersion(MTL::LanguageVersion2_0);
        NS::Error* error = nullptr;
        auto library = fb->device->device->newLibrary(sourceString, compileOptions, &error);
        compileOptions->release();
        if (library) {
            combineRenderPSO = createCombinePipeline(library);
            library->release();
        } else if (error) {
            Printf(PRINT_LOG, "Metal: Failed to compile SSAO combine fallback library: %s\n",
                   error->localizedDescription()->utf8String());
            error->release();
        }
    }

    CreateDitherTexture();
    CreateCoverageMaskPipeline();
}

MtAOModule::~MtAOModule() {
    if (ssaoPSO) ssaoPSO->release();
    if (ssaoAlchemyPSO) ssaoAlchemyPSO->release();
    if (depthLinearizePSO) depthLinearizePSO->release();
    if (ssaoMipPSO) ssaoMipPSO->release();
    if (blurPSO) blurPSO->release();
    if (upsamplePSO) upsamplePSO->release();
    if (atrousPSO) atrousPSO->release();
    if (combinePSO) combinePSO->release();
    if (combineRenderPSO) combineRenderPSO->release();
    if (coverageMaskPSO) coverageMaskPSO->release();
    if (mAOTexture) mAOTexture->release();
    if (mBlurTexture) mBlurTexture->release();
    if (mFullresAOTexture) mFullresAOTexture->release();
    if (mFullresTempTexture) mFullresTempTexture->release();
    if (mDitherTexture) mDitherTexture->release();
    if (mCoverageMask) mCoverageMask->release();
    if (mDepthPyramidTexture) mDepthPyramidTexture->release();
}

void MtAOModule::CreateDitherTexture() {
    if (mDitherTexture)
        return;

    std::mt19937 generator(1337);
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
    std::vector<int16_t> randomValues(64 * 64 * 4);
    for (int i = 0; i < 64 * 64; i++) {
        float angle = 2.0f * 3.14159265359f * distribution(generator);
        randomValues[i * 4 + 0] = (int16_t)clamp(cosf(angle) * 32767.0f, -32768.0f, 32767.0f);
        randomValues[i * 4 + 1] = (int16_t)clamp(sinf(angle) * 32767.0f, -32768.0f, 32767.0f);
        randomValues[i * 4 + 2] = (int16_t)clamp(distribution(generator) * 32767.0f, -32768.0f, 32767.0f);
        randomValues[i * 4 + 3] = (int16_t)clamp(distribution(generator) * 32767.0f, -32768.0f, 32767.0f);
    }

    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(64);
    desc->setHeight(64);
    desc->setPixelFormat(MTL::PixelFormatRGBA16Snorm);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(fb->mVersionManager.GetDynamicStorageMode());
    mDitherTexture = fb->device->device->newTexture(desc);
    desc->release();

    if (mDitherTexture) {
        auto region = MTL::Region::Make2D(0, 0, 64, 64);
        mDitherTexture->replaceRegion(region, 0, randomValues.data(), 64 * 8);
    }
}

void MtAOModule::EnsureTextures(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);

    if (mAOTexture && mBlurTexture && mAOWidth == width && mAOHeight == height)
        return;

    if (mAOTexture) {
        mAOTexture->release();
        mAOTexture = nullptr;
    }
    if (mBlurTexture) {
        mBlurTexture->release();
        mBlurTexture = nullptr;
    }

    auto compute = fb->GetComputeManager();
    if (!compute)
        return;

    const auto usage = (MTL::TextureUsage)(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    mAOTexture = compute->CreateTexture(width, height, MTL::PixelFormatRG16Float,
                                        usage, MTL::StorageModePrivate);
    mBlurTexture = compute->CreateTexture(width, height, MTL::PixelFormatRG16Float,
                                          usage, MTL::StorageModePrivate);

    mAOWidth = width;
    mAOHeight = height;
}

// Stencil coverage mask: R8Unorm at *scene* resolution, render target for a
// stencil-tested fullscreen triangle.
//
// It used to be allocated at AO resolution alongside mAOTexture, which cannot
// work: a stencil test compares at the fragment's own framebuffer coordinate,
// and the stencil attachment is the full-res SceneDepthStencil. A quarter-res
// render target therefore rasterized quarter-res fragments that stencil-tested
// against full-res stencil pixels (x,y) -- so the mask encoded the top-left
// quarter of the screen's stencil state, addressed as if it were the whole
// screen. No viewport or scissor setting can fix that; the sizes have to
// match. Harmless whenever the stencil buffer is uniform (mask comes back
// all-1), so it only ever manifested across portal boundaries.
void MtAOModule::EnsureCoverageMask(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);

    if (mCoverageMask && mCoverageWidth == width && mCoverageHeight == height)
        return;

    if (mCoverageMask) { mCoverageMask->release(); mCoverageMask = nullptr; }

    auto covDesc = MTL::TextureDescriptor::alloc()->init();
    covDesc->setWidth(width);
    covDesc->setHeight(height);
    covDesc->setPixelFormat(MTL::PixelFormatR8Unorm);
    covDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    covDesc->setStorageMode(MTL::StorageModePrivate);
    mCoverageMask = fb->device->device->newTexture(covDesc);
    covDesc->release();

    mCoverageWidth = width;
    mCoverageHeight = height;
}

void MtAOModule::EnsureFullresTextures(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);

    if (mFullresAOTexture && mFullresTempTexture &&
        mFullresWidth == width && mFullresHeight == height)
        return;

    if (mFullresAOTexture) {
        mFullresAOTexture->release();
        mFullresAOTexture = nullptr;
    }
    if (mFullresTempTexture) {
        mFullresTempTexture->release();
        mFullresTempTexture = nullptr;
    }

    auto compute = fb->GetComputeManager();
    if (!compute)
        return;

    const auto usage = (MTL::TextureUsage)(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    mFullresAOTexture = compute->CreateTexture(width, height, MTL::PixelFormatRG16Float,
                                               usage, MTL::StorageModePrivate);
    mFullresTempTexture = compute->CreateTexture(width, height, MTL::PixelFormatRG16Float,
                                                 usage, MTL::StorageModePrivate);
    mFullresWidth = width;
    mFullresHeight = height;
}

void MtAOModule::EnsureDepthPyramid(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);

    if (mDepthPyramidTexture && mDepthPyramidWidth == width && mDepthPyramidHeight == height)
        return;

    if (mDepthPyramidTexture) {
        mDepthPyramidTexture->release();
        mDepthPyramidTexture = nullptr;
    }

    // Full scene resolution (not AO-scaled) -- mip 0 must match what the
    // horizon-search loop currently pays full-res texture cost against.
    // R16Float is filterable on all Metal GPUs (unlike R32Float, which
    // needs a device capability check), so MTLBlitCommandEncoder::
    // generateMipmaps can box-filter the chain below unconditionally.
    int mipCount = std::min(6, (int)std::floor(std::log2((double)std::max(width, height))) + 1);
    mipCount = std::max(mipCount, 1);

    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(width);
    desc->setHeight(height);
    desc->setPixelFormat(MTL::PixelFormatR16Float);
    desc->setMipmapLevelCount(mipCount);
    desc->setUsage((MTL::TextureUsage)(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget));
    desc->setStorageMode(MTL::StorageModePrivate);
    mDepthPyramidTexture = fb->device->device->newTexture(desc);
    desc->release();

    mDepthPyramidWidth = mDepthPyramidTexture ? width : 0;
    mDepthPyramidHeight = mDepthPyramidTexture ? height : 0;
}

bool MtAOModule::Render(float m5, int sceneWidth, int sceneHeight, const HWViewpointUniforms* currentViewpoint) {
    if (!ssaoPSO || !blurPSO || !combineRenderPSO || !mDitherTexture || !fb->GetBuffers())
        return false;

    auto buffers = fb->GetBuffers();
    if (!buffers->SceneDepthStencil || !buffers->SceneNormal || !buffers->SceneFog ||
        !buffers->SceneDepthStencil->GetTexture() || !buffers->SceneNormal->GetTexture() ||
        !buffers->SceneFog->GetTexture()) {
        return false;
    }

    int aoScale = (int)mt_compute_ao_scale;
    // Intel integrated GPUs default to quarter-res to stay within 16ms budget.
    // Apple Silicon and discrete GPUs can handle half-res comfortably.
    if (fb->mVersionManager.architecture == MtGPUArchitecture::Intel && aoScale < 4)
        aoScale = 4;
    aoScale = clamp(aoScale, 2, 4);
    EnsureTextures((sceneWidth + aoScale - 1) / aoScale, (sceneHeight + aoScale - 1) / aoScale);
    if (!mAOTexture || !mBlurTexture)
        return false;

    int algorithm = clamp((int)mt_compute_ao_algorithm, 0, 2);
    if (algorithm == 2)
        EnsureDepthPyramid(sceneWidth, sceneHeight);

    // Automatically enable fullres cleanup for High quality settings if not explicitly set
    bool useFullresCleanup = mt_compute_ao_fullres_cleanup && !mt_compute_ao_skip_fullres;
    if (gl_ssao >= 3 && mt_compute_ao_atrous_passes >= 0 && !mt_compute_ao_skip_fullres) {
        useFullresCleanup = true;
    }
    useFullresCleanup = useFullresCleanup && upsamplePSO && atrousPSO;

    if (useFullresCleanup) {
        EnsureFullresTextures(sceneWidth, sceneHeight);
        if (!mFullresAOTexture || !mFullresTempTexture)
            return false;
    }

    SSAOParams params = {};

    // Build this frame's view-space -> world-space transform, used to
    // world-lock the AO noise (see AGENTS.md). Reuses the caller's own
    // currentViewpoint directly -- NOT fb->mLastSceneViewpoint, which is
    // unreliable by this point in the frame (see the AmbientOccludeScene
    // doc comment in v_video.h: sky/skybox rendering's own SetViewpoint
    // call overwrites it earlier in the same DrawScene() call and never
    // restores it). A general 4x4 inverse is required here, NOT a rotation
    // transpose: confirmed via HWDrawInfo::SetViewMatrix (hw_drawinfo.cpp)
    // that mViewMatrix bakes in a non-uniform scale
    // (Level->info->pixelstretch -- 1.2 by default, classic Doom's
    // non-square-pixel aspect correction -- plus a possible -1 mirror
    // flip) on top of the rotation, so it is NOT an orthonormal matrix and
    // transpose() != inverse() for it. (An earlier version of this code
    // used transpose() as a cheaper stand-in, valid only for pure
    // rotations; it produced a view-angle-dependent reconstruction error,
    // visible as the debug pattern still sliding near walls under both
    // turning and walking -- exactly what a wrong, angle-dependent inverse
    // would produce, and reported as such 2026-07-24.) inverseMatrix() is
    // the same general 4x4 inverse already used, and already proven
    // reliable, for the old invProj field it replaced.
    if (currentViewpoint) {
        VSMatrix viewMatrixCopy = currentViewpoint->mViewMatrix; // inverseMatrix() isn't const
        VSMatrix invView;
        if (viewMatrixCopy.inverseMatrix(invView)) {
            const auto *m = invView.get();
            for (int i = 0; i < 16; i++)
                params.viewToWorld[i] = (float)m[i];
        }
    }

    float aoStrength = gl_ssao_strength;
    switch (gl_ssao) {
    default:
    case 1: aoStrength *= 0.85f; break;
    case 2: break;
    case 3: aoStrength *= 1.15f; break;
    }

    params.radius = gl_ssao_radius;
    params.bias = clamp((float)gl_ssao_bias, 0.0f, 1.0f);
    params.intensity = clamp(aoStrength, 0.0f, 1.0f);
    params.screenResX = (float)sceneWidth;
    params.screenResY = (float)sceneHeight;
    params.zNear = fb->GetZNear();
    params.zFar = fb->GetZFar();
    auto sceneScale = fb->SceneScale();
    auto sceneOffset = fb->SceneOffset();
    params.scaleX = sceneScale.X;
    params.scaleY = sceneScale.Y;
    params.offsetX = sceneOffset.X;
    params.offsetY = sceneOffset.Y;

    float tanHalfFovy = 1.0f / m5;
    // invFocalLenX/Y are read straight off this frame's actual projection
    // matrix rather than re-derived from the viewport dimensions.
    // Upstream's GLSL SSAO assumes invFocalLenX == tanHalfFovy *
    // (sceneWidth / sceneHeight), but the projection is actually built as
    // perspective(fovy, ratio, ...) in hw_entrypoint.cpp, where `ratio` is
    // the *display* aspect (r_visualAspect / vid_aspect / letterboxing) and
    // fovy is divided by fovratio -- neither is the raw scene pixel aspect,
    // so the two disagree whenever the player isn't on a plain widescreen
    // setup. For plain occlusion that only costs a slightly wrong sample
    // radius, which is why upstream never noticed, but the world-locked
    // noise round-trips these through viewToWorld: a wrong horizontal
    // focal length scales reconstructed view-space X by a constant k, and
    // R^-1 applied to (k*x, y, -z) puts the resulting world-space error
    // along a camera-relative axis. That error therefore rotates with the
    // camera and slides with the camera position -- exactly the reported
    // "noise pattern rotates when turning and drifts when walking", worst
    // near screen edges and close walls where |x| is largest.
    // mProjectionMatrix is column-major: [0] = f/aspect, [5] = f.
    float invFocalLenX = tanHalfFovy * (sceneWidth / (float)sceneHeight);
    float invFocalLenY = tanHalfFovy;
    if (currentViewpoint) {
        const auto *proj = currentViewpoint->mProjectionMatrix.get();
        if (proj[0] != 0.0f && proj[5] != 0.0f) {
            invFocalLenX = 1.0f / (float)proj[0];
            invFocalLenY = 1.0f / (float)proj[5];
        }
    }
    float r2 = std::max(gl_ssao_radius * gl_ssao_radius, 1.0f);
    params.uvToViewAX = 2.0f * invFocalLenX;
    params.uvToViewAY = 2.0f * invFocalLenY;
    params.uvToViewBX = -invFocalLenX;
    params.uvToViewBY = -invFocalLenY;
    params.negInvR2 = -1.0f / r2;
    params.radiusToScreen = gl_ssao_radius * 0.5f / tanHalfFovy * (float)mAOHeight;
    params.aoMultiplier = 1.0f / std::max(1.0f - params.bias, 1e-5f);
    
    // Quality-based defaults:
    // Low: 4x4, Med: 4x4 (stronger), High: 5x4
    switch (gl_ssao) {
    default:
    case 1: params.visibilityStrength = 2.35f; params.numDirections = 4; params.numSteps = 4; break;
    case 2: params.visibilityStrength = 2.75f; params.numDirections = 4; params.numSteps = 4; break;
    case 3: params.visibilityStrength = 3.15f; params.numDirections = 6; params.numSteps = 8; break;
    }

    // Override with CVARs if they are > 0
    if (mt_compute_ao_directions > 0) params.numDirections = mt_compute_ao_directions;
    if (mt_compute_ao_steps > 0) params.numSteps = mt_compute_ao_steps;

    // Algorithm 1 (AlchemyAO) repurposes numSteps as a flat sample count with
    // its own tier defaults (8/8/12), not GTAO's numDirections x numSteps
    // (4x4/4x4/6x8) -- mt_compute_ao_alchemy_samples overrides it the same
    // "0 = use tier default" way mt_compute_ao_steps does for GTAO.
    if (algorithm == 1) {
        switch (gl_ssao) {
        default:
        case 1: params.numSteps = 8; break;
        case 2: params.numSteps = 8; break;
        case 3: params.numSteps = 12; break;
        }
        if (mt_compute_ao_alchemy_samples > 0) params.numSteps = mt_compute_ao_alchemy_samples;
    }

    // Intel integrated GPUs: measured 2026-07-14 that the horizon-sample
    // loop's dependent texture fetches (coverage/depth/normal per
    // iteration), not resolution or dispatch count, are the GPU cost driver
    // here -- gl_ssao 3's default 8x6=48 iterations cost ~33ms of GPU frame
    // time on Intel HD 6000 vs ~6.7ms at 4x4=16, both over a ~17ms
    // PP-AO-equivalent baseline. Clamp down unconditionally (same hard-clamp
    // pattern as the aoScale override above) to keep compute AO viable there
    // instead of defaulting it off entirely.
    if (fb->mVersionManager.architecture == MtGPUArchitecture::Intel) {
        if (algorithm == 1) {
            // Algorithm 1's numSteps is a flat total sample count, not one
            // factor of GTAO's numSteps x numDirections product. Reusing
            // GTAO's per-factor clamp (4) here would leave it with only 4
            // total samples vs GTAO's clamped 4x4=16 effective total -- a
            // 4x tighter budget for no cost reason, since both algorithms
            // pay the same class of per-sample dependent coverage/depth/
            // normal fetch that the original Intel cost bisection found to
            // be the actual driver. Clamp to a comparable effective total
            // instead of GTAO's raw per-factor value.
            params.numSteps = std::min(params.numSteps, 16);
        } else {
            params.numSteps = std::min(params.numSteps, 4);
            params.numDirections = std::min(params.numDirections, 4);
        }
    }

    // Thickness heuristic: prevent background from occluding foreground.
    // 1.5-2.0 units is usually a good balance for GZDoom scale.
    // Exposed as a cvar purely so it can be swept in-game: the base value
    // is an untraced magic number, and the shader's use of it (a linear
    // depth ramp at mt_ao.metal:381-383, not a step-distance or N.V slant
    // term as in Jimenez et al. 2016) is a candidate cause of the
    // grazing-angle streaks reported 2026-07-26. Setting it very large
    // effectively disables the reject, which is the cheap way to test
    // whether it is implicated at all before rewriting the heuristic.
    params.maxThickness = std::max((float)mt_compute_ao_thickness, 0.01f);

    // Distance fade: see the comment at its use site in ssao_compute.
    // Defaults (100/500) sit just outside typical room scale (player
    // height is 56 units, gl_ssao_radius defaults to 80) — sky-camera
    // rooms are compact content, not distant, so the fade needs to kick
    // in close. Exposed as cvars since appropriate distance is inherently
    // map/content-scale dependent.
    params.fadeStartDistance = std::max((float)mt_compute_ao_fade_start, 0.0f);
    params.fadeEndDistance = std::max((float)mt_compute_ao_fade_end, params.fadeStartDistance + 1.0f);
    params.debugMode = clamp((int)mt_compute_ao_worldpos_debug, 0, 3);
    params.noiseCellSize = std::max((float)mt_compute_ao_noise_cellsize, 1.0f);
    // World units spanned by one AO pixel at unit view depth. The AO
    // texture is mAOHeight tall and covers the full vertical FOV, so one
    // pixel subtends 2*tanHalfFovy/mAOHeight radians' worth of world at
    // depth 1; NoiseCellSize multiplies this by the pixel's actual depth.
    // See mt_compute_ao_noise_pixels (mt_postprocess.cpp).
    params.pixelWorldScale = mt_compute_ao_noise_pixels > 0.0f && mAOHeight > 0
        ? (float)mt_compute_ao_noise_pixels * 2.0f * invFocalLenY / (float)mAOHeight
        : 0.0f;
    params.screenNoiseMix = clamp((float)mt_compute_ao_noise_screenmix, 0.0f, 1.0f);

    auto cmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
    if (!cmdBuf)
        return false;

    fb->GetRenderState()->EndRenderPass();
    const bool blurAO = gl_ssao_debug < 2;
    auto aoStart = std::chrono::high_resolution_clock::now();

    // Render stencil coverage mask: white where stencil == screen->stencilValue
    // Used by compute kernel to reject samples crossing portal boundaries.
    // Sized from the stencil attachment inside RenderCoverageMask -- see
    // EnsureCoverageMask.
    RenderCoverageMask(buffers->SceneDepthStencil->GetTexture(), screen->stencilValue);

    Execute(cmdBuf, buffers->SceneDepthStencil->GetTexture(),
            buffers->SceneNormal->GetTexture(), buffers->SceneColor->GetTexture(),
            mAOTexture, mDitherTexture,
            buffers->SceneFog->GetTexture(), nullptr, mCoverageMask, params, blurAO,
            useFullresCleanup, algorithm);
    MTL::Texture *combineAO = (useFullresCleanup && mFullresResultTexture) ? mFullresResultTexture :
        (mLowresResultTexture ? mLowresResultTexture : mAOTexture);
    Combine(combineAO, sceneWidth, sceneHeight, combineAO == mFullresResultTexture);
    auto aoEnd = std::chrono::high_resolution_clock::now();
    if (fb->GetComputeManager()) {
        float ms = std::chrono::duration<float, std::milli>(aoEnd - aoStart).count();
        fb->GetComputeManager()->RecordTiming(
            HWComputeEffect::AmbientOcclusion, ms);
    }
    return true;
}

void MtAOModule::Combine(MTL::Texture* aoTex, int sceneWidth, int sceneHeight, bool fullresAO) {
    if (!aoTex || !combineRenderPSO || !fb->GetBuffers())
        return;

    auto buffers = fb->GetBuffers();
    auto renderState = fb->GetRenderState();
    renderState->SetRenderTarget(buffers->SceneColor->GetTexture(),
                                 buffers->SceneDepthStencil->GetTexture(),
                                 buffers->SceneColor->GetWidth(),
                                 buffers->SceneColor->GetHeight(),
                                 (int)MTL::PixelFormatBGRA8Unorm, 1);
    renderState->EnableDrawBuffers(1, false);
    const auto& viewport = screen->mSceneViewport;
    renderState->SetViewport(viewport.left, viewport.top, viewport.width, viewport.height);
    renderState->SetScissor(viewport.left, viewport.top, viewport.width, viewport.height);
    renderState->BeginRenderPass();

    auto encoder = renderState->GetEncoder();
    if (!encoder)
        return;

    struct AOCombineParams {
        int debugMode;
        float scaleX;
        float scaleY;
        float offsetX;
        float offsetY;
        float blurSharpness;
        float zNear;
        float zFar;
        float combineSmooth;
        int fullresAO;
    } combineParams = {};
    combineParams.debugMode = gl_ssao_debug;
    auto sceneScale = fb->SceneScale();
    auto sceneOffset = fb->SceneOffset();
    combineParams.scaleX = sceneScale.X;
    combineParams.scaleY = sceneScale.Y;
    combineParams.offsetX = sceneOffset.X;
    combineParams.offsetY = sceneOffset.Y;
    combineParams.blurSharpness = 1.0f / std::max((float)gl_ssao_blur, 0.1f);
    combineParams.zNear = fb->GetZNear();
    combineParams.zFar = fb->GetZFar();
    combineParams.combineSmooth = clamp((float)mt_compute_ao_combine_smooth, 0.0f, 1.0f);
    combineParams.fullresAO = fullresAO ? 1 : 0;

    renderState->SetVertexBuffer(screen->mVertexData);
    auto vb = dynamic_cast<MtVertexBuffer *>(screen->mVertexData->GetBufferObjects().first);
    if (vb) {
        MtPipelineKey ppKey;
        ppKey.VertexFormat = vb->VertexFormat;
        renderState->SetPipelineKey(ppKey);
        encoder->setVertexBuffer(vb->GetBuffer(), 0, 0);
        encoder->useResource(vb->GetBuffer(), MTL::ResourceUsageRead, MTL::RenderStageVertex);
    }

    encoder->setRenderPipelineState(combineRenderPSO);
    encoder->setDepthStencilState(fb->GetPipelineStateManager()->GetPPStencilState());
    encoder->setStencilReferenceValue(screen->stencilValue);
    encoder->setCullMode(MTL::CullModeNone);
    encoder->setFragmentBytes(&combineParams, sizeof(combineParams), 0);
    encoder->setFragmentTexture(aoTex, 0);
    encoder->setFragmentTexture(buffers->SceneFog->GetTexture(), 1);
    encoder->setFragmentTexture(buffers->SceneNormal->GetTexture(), 2);
    encoder->setFragmentTexture(buffers->SceneDepthStencil->GetTexture(), 3);
    encoder->setFragmentTexture(buffers->SceneColor->GetTexture(), 4);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0,
                            (NS::UInteger)3);
}

void MtAOModule::Execute(MTL::CommandBuffer* cmdBuf, MTL::Texture* depthTex, MTL::Texture* normalTex, MTL::Texture* sceneColorTex, MTL::Texture* aoTex, MTL::Texture* ditherTex, MTL::Texture* fogTex, MTL::Texture* combineTex, MTL::Texture* coverageTex, const SSAOParams& params, bool blurAO, bool useFullresCleanup, int algorithm) {
    if (!ssaoPSO || (blurAO && (!blurPSO || !mBlurTexture)) || !depthTex || !normalTex || !sceneColorTex || !ditherTex || !aoTex) return;
    mFullresResultTexture = nullptr;
    mLowresResultTexture = aoTex;

    // Algorithm 2 (depth-mip-pyramid): the horizon-search kernel needs the
    // pyramid fully built (mip 0 seeded + generateMipmaps) before it can
    // dispatch, and mip generation is a blit-encoder operation -- Metal
    // requires ending the compute encoder before opening a blit encoder on
    // the same command buffer, then reopening compute afterward. This costs
    // 2 extra encoder transitions vs algorithm 0/1's single continuous
    // encoder -- a real, unmeasured overhead (see AGENTS.md). Must stay on
    // this cmdBuf's own blitCommandEncoder(), not
    // fb->GetCommands()->GetBlitCommandBuffer() -- that creates a separate,
    // independently-committed command buffer with no ordering guarantee
    // relative to cmdBuf, which would make mip generation race the
    // dispatches that depend on it.
    bool useMipAlgorithm = (algorithm == 2) && ssaoMipPSO && depthLinearizePSO && mDepthPyramidTexture;
    if (useMipAlgorithm) {
        auto linearizeEncoder = cmdBuf->computeCommandEncoder();
        if (linearizeEncoder) {
            linearizeEncoder->setComputePipelineState(depthLinearizePSO);
            linearizeEncoder->setBytes(&params, sizeof(SSAOParams), 0);
            linearizeEncoder->setTexture(depthTex, 0);
            linearizeEncoder->setTexture(mDepthPyramidTexture, 1);
            MTL::Size pyramidGrid = { (NS::UInteger)mDepthPyramidTexture->width(), (NS::UInteger)mDepthPyramidTexture->height(), 1 };
            linearizeEncoder->dispatchThreads(pyramidGrid, MTL::Size(8, 8, 1));
            linearizeEncoder->endEncoding();

            auto blit = cmdBuf->blitCommandEncoder();
            if (blit) {
                blit->generateMipmaps(mDepthPyramidTexture);
                blit->endEncoding();
            }
        } else {
            useMipAlgorithm = false;
        }
    }

    auto encoder = cmdBuf->computeCommandEncoder();
    if (!encoder) return;

    // 1. SSAO Pass -- pick the algorithm's kernel, silently falling back to
    // GTAO (ssaoPSO) if the requested algorithm's PSO/resources aren't
    // available (metallib missing the function and fallback compile
    // failed), so behavior for existing users/configs never regresses.
    MTL::ComputePipelineState* ssaoPass = ssaoPSO;
    if (useMipAlgorithm) ssaoPass = ssaoMipPSO;
    else if (algorithm == 1 && ssaoAlchemyPSO) ssaoPass = ssaoAlchemyPSO;

    encoder->setComputePipelineState(ssaoPass);
    encoder->setBytes(&params, sizeof(SSAOParams), 0);
    encoder->setTexture(ditherTex, 0);
    encoder->setTexture(depthTex, 1);
    encoder->setTexture(aoTex, 2);
    encoder->setTexture(normalTex, 3);
    encoder->setTexture(sceneColorTex, 4);
    if (coverageTex) encoder->setTexture(coverageTex, 5);
    if (useMipAlgorithm) encoder->setTexture(mDepthPyramidTexture, 6);

    struct AOFlags { int flipY; float invBackingScale; } aoFlags;
    // Keep AO in the same texture coordinate space as scene depth, normals,
    // fog, and the fullscreen combine pass. An AO-only Y flip misaligns the
    // fog contribution and appears as a bright, vertically inverted layer.
    aoFlags.flipY = 0;
    aoFlags.invBackingScale = params.screenResY / (float)aoTex->height();
    encoder->setBytes(&aoFlags, sizeof(aoFlags), 1);

    MTL::Size gridSize = { (NS::UInteger)aoTex->width(), (NS::UInteger)aoTex->height(), 1 };
    encoder->dispatchThreads(gridSize, MTL::Size(8, 8, 1));
    
    if (blurAO) {
        encoder->memoryBarrier(MTL::BarrierScopeTextures);

        struct AOBlurParams {
            float scaleX, scaleY, offsetX, offsetY;
            float blurSharpness, powExponent;
            int normalAware, flipY;
            float maxThickness;
            int applyExponent; // Moved to end for alignment safety
        } blurParams = {};
        blurParams.scaleX = params.scaleX;
        blurParams.scaleY = params.scaleY;
        blurParams.offsetX = params.offsetX;
        blurParams.offsetY = params.offsetY;
        blurParams.blurSharpness = 1.0f / std::max((float)gl_ssao_blur, 0.1f);
        blurParams.powExponent = std::max((float)gl_ssao_exponent, 0.1f);
        blurParams.normalAware = mt_compute_ao_normal_blur ? 1 : 0;
        blurParams.flipY = aoFlags.flipY;
        blurParams.maxThickness = params.maxThickness;

        encoder->setComputePipelineState(blurPSO);
        // Algorithm 1 (AlchemyAO) sums independently-noisy per-sample
        // contributions with no horizon-max suppression -- unlike
        // ssao_compute's telescoped `occlusion += max(sampleHorizon -
        // horizon, 0)` accumulation, which structurally collapses each
        // direction's numSteps samples down to a single winning value (the
        // running max), AlchemyAO's flat Vogel-disk loop adds all
        // sampleCount contributions independently. Same visibilityStrength
        // tuning (shared with GTAO, see Render()) then amplifies that
        // higher per-pixel variance into visible salt-and-pepper grain that
        // survives the default 2-pass blur (screenshots, 2026-07-21).
        // Force the max blur passes for this algorithm specifically rather
        // than touching the proven occlusion math again -- blur is a cheap
        // 3x3 pass over the (usually quarter-res) AO texture, not the
        // horizon-search/sample-loop cost the Intel bisection identified as
        // the actual GPU driver, so this shouldn't meaningfully erase
        // algorithm 1's measured GPU-time win.
        int blurPasses = clamp((int)mt_compute_ao_blur_passes, 1, 4);
        if (algorithm == 1) blurPasses = 4;
        MTL::Texture *src = aoTex;
        MTL::Texture *dst = mBlurTexture;
        for (int pass = 0; pass < blurPasses; pass++) {
            if (pass > 0) encoder->memoryBarrier(MTL::BarrierScopeTextures);
            dst = (src == aoTex) ? mBlurTexture : aoTex;
            blurParams.applyExponent = (pass == blurPasses - 1) ? 1 : 0;
            encoder->setBytes(&blurParams, sizeof(blurParams), 0);
            encoder->setTexture(src, 0);
            encoder->setTexture(dst, 1);
            encoder->setTexture(normalTex, 2);
            encoder->dispatchThreads(gridSize, MTL::Size(8, 8, 1));
            src = dst;
        }
        mLowresResultTexture = src;
    }

    // Fullres cleanup decision was computed in Render() — just verify textures exist
    useFullresCleanup = useFullresCleanup && upsamplePSO && atrousPSO && mFullresAOTexture && mFullresTempTexture;

    if (useFullresCleanup) {
        encoder->memoryBarrier(MTL::BarrierScopeTextures);
        struct AOFullresParamsCPU {
            float sceneScale[2], sceneOffset[2], fullRes[2];
            float blurSharpness, zNear, zFar;
            int normalAware, atrousStep;
        } fullresParams = {};
        fullresParams.sceneScale[0] = params.scaleX;
        fullresParams.sceneScale[1] = params.scaleY;
        fullresParams.sceneOffset[0] = params.offsetX;
        fullresParams.sceneOffset[1] = params.offsetY;
        fullresParams.fullRes[0] = (float)mFullresWidth;
        fullresParams.fullRes[1] = (float)mFullresHeight;
        fullresParams.blurSharpness = 1.0f / std::max((float)gl_ssao_blur, 0.1f);
        fullresParams.zNear = params.zNear;
        fullresParams.zFar = params.zFar;
        fullresParams.normalAware = mt_compute_ao_normal_upsample ? 1 : 0;

        MTL::Size fullGrid = { (NS::UInteger)mFullresWidth, (NS::UInteger)mFullresHeight, 1 };
        encoder->setComputePipelineState(upsamplePSO);
        encoder->setBytes(&fullresParams, sizeof(fullresParams), 0);
        encoder->setTexture(mLowresResultTexture ? mLowresResultTexture : aoTex, 0);
        encoder->setTexture(depthTex, 1);
        encoder->setTexture(normalTex, 2);
        encoder->setTexture(mFullresAOTexture, 3);
        encoder->dispatchThreads(fullGrid, MTL::Size(8, 8, 1));
        mFullresResultTexture = mFullresAOTexture;

        int atrousPasses = clamp((int)mt_compute_ao_atrous_passes, 0, 3);
        if (gl_ssao >= 3 && atrousPasses == 0) atrousPasses = 1;

        MTL::Texture *srcPass = mFullresAOTexture;
        MTL::Texture *dstPass = mFullresTempTexture;
        for (int pass = 0; pass < atrousPasses; pass++) {
            fullresParams.atrousStep = 1 << pass;
            encoder->memoryBarrier(MTL::BarrierScopeTextures);
            encoder->setComputePipelineState(atrousPSO);
            encoder->setBytes(&fullresParams, sizeof(fullresParams), 0);
            encoder->setTexture(srcPass, 0);
            encoder->setTexture(normalTex, 1);
            encoder->setTexture(dstPass, 2);
            encoder->dispatchThreads(fullGrid, MTL::Size(8, 8, 1));
            mFullresResultTexture = dstPass;
            MTL::Texture *tmp = srcPass;
            srcPass = dstPass;
            dstPass = tmp;
        }
    }

    encoder->endEncoding();
}

void MtAOModule::CreateCoverageMaskPipeline() {
    if (coverageMaskPSO)
        return;

    auto sourceString = NS::String::string(COVERAGE_MASK_SOURCE, NS::UTF8StringEncoding);
    auto compileOptions = MTL::CompileOptions::alloc()->init();
    compileOptions->setLanguageVersion(MTL::LanguageVersion2_0);
    NS::Error* error = nullptr;
    auto library = fb->device->device->newLibrary(sourceString, compileOptions, &error);
    compileOptions->release();
    if (!library) {
        if (error) {
            Printf(PRINT_LOG, "Metal: Failed to compile coverage mask library: %s\n",
                   error->localizedDescription()->utf8String());
            error->release();
        }
        return;
    }

    auto vert = library->newFunction(NS::String::string("coverage_mask_vs", NS::UTF8StringEncoding));
    auto frag = library->newFunction(NS::String::string("coverage_mask_fs", NS::UTF8StringEncoding));
    library->release();
    if (!vert || !frag) {
        if (vert) vert->release();
        if (frag) frag->release();
        return;
    }

    auto desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setVertexFunction(vert);
    desc->setFragmentFunction(frag);
    desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatR8Unorm);
    desc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
    desc->setStencilAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);

    NS::Error* psoError = nullptr;
    coverageMaskPSO = fb->device->device->newRenderPipelineState(desc, &psoError);
    if (!coverageMaskPSO && psoError) {
        Printf(PRINT_LOG, "Metal: Failed to create coverage mask pipeline: %s\n",
               psoError->localizedDescription()->utf8String());
        psoError->release();
    }
    desc->release();
    vert->release();
    frag->release();
}

void MtAOModule::RenderCoverageMask(MTL::Texture* depthStencilTex, int stencilValue) {
    if (!coverageMaskPSO || !depthStencilTex)
        return;

    // Size the mask from the stencil attachment itself, so the two can never
    // drift out of sync: the mask's pixel grid *is* the stencil buffer's pixel
    // grid. Note this is the full attachment (screen-sized), not the scene
    // viewport sub-rect, which is why the kernels sample it with the same
    // scene-scaled UV they use for scene colour/depth/normal.
    EnsureCoverageMask((int)depthStencilTex->width(), (int)depthStencilTex->height());
    if (!mCoverageMask)
        return;

    auto cmdBuf = fb->GetCommands()->GetRenderCommandBuffer();
    if (!cmdBuf)
        return;

    auto desc = MTL::RenderPassDescriptor::alloc()->init();
    desc->colorAttachments()->object(0)->setTexture(mCoverageMask);
    desc->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionClear);
    desc->colorAttachments()->object(0)->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
    desc->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
    desc->depthAttachment()->setTexture(depthStencilTex);
    desc->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
    desc->depthAttachment()->setStoreAction(MTL::StoreActionDontCare);
    desc->stencilAttachment()->setTexture(depthStencilTex);
    desc->stencilAttachment()->setLoadAction(MTL::LoadActionLoad);
    desc->stencilAttachment()->setStoreAction(MTL::StoreActionDontCare);

    auto encoder = cmdBuf->renderCommandEncoder(desc);
    desc->release();
    if (!encoder)
        return;

    encoder->setRenderPipelineState(coverageMaskPSO);
    encoder->setDepthStencilState(fb->GetPipelineStateManager()->GetPPStencilState());
    encoder->setStencilReferenceValue((uint32_t)stencilValue);
    encoder->setCullMode(MTL::CullModeNone);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
    encoder->endEncoding();
}
