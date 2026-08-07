#include <metal_stdlib>
using namespace metal;

// NOTE: This file is the authoritative Metal SSAO shader source. It is
// compiled into native_shaders.metallib (CMake target metal_native_shaders)
// and loaded by MtShaderManager::LoadNativeLibrary() before any inline
// string. The matching SSAO_COMPUTE_SOURCE inline string in
// src/common/rendering/metal/renderer/mt_ao.cpp is only a fallback used if
// the metallib can't be found — keep it in sync with this file, not the
// other way around.

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
    // screen->stencilValue for this frame. Samples whose stencil differs
    // belong to a different portal layer and are rejected.
    uint stencilRef;
};

// Portal-layer coverage, read straight out of the scene stencil buffer.
//
// This replaces a full-screen render pass (RenderCoverageMask) that existed
// only because compute kernels cannot run a stencil test: it materialized
// "stencil == ref" into an R8 texture so the kernels could sample it. That pass
// cost 3.12ms/frame on the reference Intel machine -- MORE than the entire AO
// computation -- because it cleared and stored a screen-sized target and loaded
// both depth and stencil attachments every frame, all independent of AO
// resolution. See AGENTS.md 2026-08-07.
//
// A stencil texture VIEW (X32_Stencil8 over the Depth32Float_Stencil8 scene
// buffer) gives the kernel the same information for free. read() takes integer
// texel coordinates and has no sampler, so the clamp the old nearestClampSampler
// provided has to be done by hand -- sampleSceneUV routinely goes out of range
// at screen edges, and an unclamped read() is undefined rather than merely wrong.
inline bool StencilPasses(texture2d<uint, access::read> stencilTex, float2 uv, uint ref) {
    uint2 dim = uint2(stencilTex.get_width(), stencilTex.get_height());
    if (dim.x == 0 || dim.y == 0) return true;
    float2 c = clamp(uv, 0.0, 1.0) * float2(dim);
    uint2 p = min(uint2(c), dim - uint2(1));
    return stencilTex.read(p).r == ref;
}

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
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::write> aoOutput [[texture(2)]],
    texture2d<float, access::sample> normalTexture [[texture(3)]],
    texture2d<float, access::sample> sceneColorTexture [[texture(4)]],
    texture2d<uint, access::read> stencilTexture [[texture(5)]])
{
    float2 outSize = float2((float)aoOutput.get_width(), (float)aoOutput.get_height());
    if (gid.x >= outSize.x || gid.y >= outSize.y) return;

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

            // Portal-layer guard, read from the stencil buffer directly.
            if (!StencilPasses(stencilTexture, sampleSceneUV, params.stencilRef)) continue;

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

    // Distance fade: see mt_ao.cpp SSAO_COMPUTE_SOURCE (authoritative) for
    // the full rationale. Fades occlusion strength to 0 past fadeStartDistance
    // so distant real geometry (e.g. sky-camera rooms) doesn't self-occlude.
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
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::write> aoOutput [[texture(2)]],
    texture2d<float, access::sample> normalTexture [[texture(3)]],
    texture2d<float, access::sample> sceneColorTexture [[texture(4)]],
    texture2d<uint, access::read> stencilTexture [[texture(5)]])
{
    float2 outSize = float2((float)aoOutput.get_width(), (float)aoOutput.get_height());
    if (gid.x >= outSize.x || gid.y >= outSize.y) return;

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

        // Portal-layer guard, read from the stencil buffer directly.
        if (!StencilPasses(stencilTexture, sampleSceneUV, params.stencilRef)) continue;

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

    // Distance fade: see mt_ao.cpp SSAO_COMPUTE_SOURCE (authoritative) for
    // the full rationale. Fades occlusion strength to 0 past fadeStartDistance
    // so distant real geometry (e.g. sky-camera rooms) doesn't self-occlude.
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
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    texture2d<float, access::write> aoOutput [[texture(2)]],
    texture2d<float, access::sample> normalTexture [[texture(3)]],
    texture2d<float, access::sample> sceneColorTexture [[texture(4)]],
    texture2d<uint, access::read> stencilTexture [[texture(5)]],
    texture2d<float, access::sample> depthPyramid [[texture(6)]])
{
    float2 outSize = float2((float)aoOutput.get_width(), (float)aoOutput.get_height());
    if (gid.x >= outSize.x || gid.y >= outSize.y) return;

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

            // Portal-layer guard, read from the stencil buffer directly.
            if (!StencilPasses(stencilTexture, sampleSceneUV, params.stencilRef)) continue;

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

    // Distance fade: see mt_ao.cpp SSAO_COMPUTE_SOURCE (authoritative) for
    // the full rationale. Fades occlusion strength to 0 past fadeStartDistance
    // so distant real geometry (e.g. sky-camera rooms) doesn't self-occlude.
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
    float depthSignal = 1.0 - exp2(-sceneDepth * 0.01);
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

        float centerAlpha = (1.0 - attenuation) * saturate(1.0 - exp2(-ssao.y * 0.01));
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
            float tapAlpha = (1.0 - taps[i].x) * saturate(1.0 - exp2(-taps[i].y * 0.01));
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
            float nAlpha = (1.0 - neighborVals[n].x) * saturate(1.0 - exp2(-neighborVals[n].y * 0.01));
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
