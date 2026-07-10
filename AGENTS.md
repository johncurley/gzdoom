# Agent Notes

## Active Roadmap

- The durable engine plan is `docs/engine-modernization.md`.
- Keep this file focused on current implementation state, benchmark workflows,
  and short-lived handoff notes.
- Current priority order (agreed 2026-07-10):
  1. Reconcile `mt_ao.cpp`'s inline `SSAO_COMPUTE_SOURCE` against
     `shaders/native/mt_ao.metal` — the `.metal` file is the one actually
     compiled into `native_shaders.metallib` and loaded at runtime (see
     "Native vs inline Metal shader source" below); the inline string has
     drifted and is mostly dead code. Decide per-difference which behavior
     is correct, port deliberately, and validate visually before assuming
     either version's tuning was right.
  2. Continue the broader compute-shader postprocess conversion (the "compute
     shader map": AO and bloom are done: see which other postprocess passes
     are worth moving to compute next).
  3. After that, per-operating-system engine optimizations, including the
     ARM64/AArch64 JIT gap noted below.
- Opaque batching is recorded as a deferred investigation. Benchmark the
  existing `gl_sort_textures` option before changing draw-list ordering.

## Native vs inline Metal shader source (found 2026-07-10)

`src/common/rendering/metal/shaders/native/*.metal` files are **not** dead
reference copies, despite header comments in some of them (e.g. `mt_ao.metal`)
claiming the inline C++ string in the matching `renderer/mt_*.cpp` file is
authoritative. The build compiles the `.metal` files into
`native_shaders.metallib` (CMake target `metal_native_shaders`), which is
bundled into the app and loaded by `MtShaderManager::LoadNativeLibrary()`
*before* any inline string — the inline string in the `.cpp` file is only a
fallback used if the metallib can't be found. `mt_ao.cpp`'s inline
`SSAO_COMPUTE_SOURCE` and `mt_ao.metal`'s `ssao_compute` kernel had drifted
significantly (LDS threadgroup caching present only in the inline version;
different `numSteps`/`numDirections` clamps — the inline version's wider
clamp respects `mt_compute_ao_steps`, the metal version hardcodes
`numSteps = 4` and ignores that cvar; different jitter formula; `bias`
scaling; `aoMultiplier` scaling). Treat `shaders/native/*.metal` as the real
source of truth for any Metal shader edit going forward; keep the inline
string in sync as a fallback but don't assume it's what's rendering. Check
whether `mt_bloom.cpp`/`mt_bloom.metal` have the same split before editing
bloom.

## Metal binary pipeline cache (found 2026-07-10)

`MtBinaryArchive` (`src/common/rendering/metal/system/mt_binaryarchive.cpp`)
persists compiled Metal pipeline state objects to
`~/Library/Application Support/zdoom/cache/mt_pipelines.bin` and reloads it on
every launch, across rebuilds. If a Metal shader source change doesn't seem to
take effect in-game after a correct rebuild, delete that file before assuming
the fix is wrong.

## Metal Compute Postprocess

- Metal compute AO and compute bloom are implemented behind `mt_compute_ao` and `mt_compute_bloom`.
- Benchmark workflow:
  - `mt_metrics_reset`
  - wait in a stable view for several seconds
  - `mt_metrics`
- Current AO/bloom metrics measure CPU command-encoding time, not per-pass GPU
  execution time. Metal counter sample buffers are required for GPU timing.
- Bloom composite validation modes:
  - `mt_compute_bloom_composite 0`: automatic selection.
  - `mt_compute_bloom_composite 1`: force Tier 1 high-precision compute plus
    raster composite.
  - `mt_compute_bloom_composite 2`: require Tier 2 direct read/write or fall
    back to PP bloom.
- Current benchmark baseline on Intel MacBookAir7,2:
  - PP AO active average was around `0.715ms`.
  - Compute AO active average was around `0.278ms`.
  - Compute bloom active average was around `0.13ms`.
- `MtComputeManager` uses backend-neutral `HWComputeEffect` values from `hwrenderer/postprocessing/hw_compute.h`, so future Vulkan/OpenGL compute paths should reuse those effect names instead of adding backend-specific effect enums.
- Remaining visual polish focus:
  - AO intermediate orientation is intentionally local to the AO pipeline:
    compute scene sampling flips Y and the combine pass flips the AO texture
    back. Do not derive this from engine-facing `RenderTextureIsFlipped()`.
  - ~~Compute AO had minor white speckle/snow artifacts in very dark areas.~~ FIXED 2025-06-19.
  - Root cause: in `ssao_combine_fs`, when a pixel at a depth discontinuity has no valid neighbors (all four have `depth <= 1e-5`), `neighborWeight` stays at 1e-5, forcing `neighborAlpha = 0`. This bypasses both speckle-removal checks (`aoAlpha < neighborAlpha * 0.85` is always false, and `max(aoAlpha, neighborAlpha * 0.3)` has no effect). The isolated pixel's weak AO alpha passes through `smoothstep(0.001, 0.015, aoAlpha)` and blends bright `SceneFog` color over dark scene color, creating white specks.
  - Fix: Replaced the fixed `smoothstep(0.001, 0.015, aoAlpha)` with a neighbor-confidence-driven smoothstep. When `neighborWeight` is low (isolated pixel), thresholds shift to `smoothstep(0.005, 0.030, aoAlpha)` for stronger suppression. When neighbors are plenty, thresholds remain at `smoothstep(0.001, 0.015, aoAlpha)`. Applied to both `mt_ao.cpp` inline shader and `shaders/native/mt_ao.metal`.
  - Build verified: `cmake --build build --target zdoom -j 8` succeeds.
- If speckles remain after this fix, further investigation should focus on:
  - The SSAO compute kernel itself producing spurious low-occlusion values at isolated pixels near depth edges (rather than the combine pass).
  - The fullres upsample path which has a simpler `smoothstep(0.002, 0.020, aoAlpha)` without neighbor clamping.

## 2025-06-21 Session Changes

### Skybox AO seam fix (Trenchfoot FN-TrenchFoot.pk3, Intel HD 6000 / Metal 2.0)

Both compute and PP AO showed dark seams at sky dome face edges. Root cause:
Metal 2.0 on Intel HD 6000 (macOS 12.7.6) has undefined behavior for
`normalize(vec3(0))` — the GPU may return `(1,0,0)` or NaN instead of `(0,0,0)`.
The sky dome has no normal vertex attribute → `GetAttrNormal()` returns `(0,0,0)`
→ `normalize(bones.Normal)` produces non-zero output → fragment shader's
`viewNormal != vec3(0.0)` passes → SSAO computes occlusion on sky geometry.
At dome face edges, normals differ, creating visible dark bands.

OpenGL/Vulkan don't have this issue because GLSL spec §8.5 guarantees
`normalize(vec3(0)) = vec3(0)`.

**Root-cause fix** (`mt_shader.cpp:750-756`): Added `normalize(bones.Normal)` zero-guard
in `PatchVertexShader`. When `length(bones.Normal) < 0.0001`, returns `vec3(0.0)`
instead of calling `normalize()` with undefined-behavior input. This ensures sky
dome normals are `(0,0,0)` in the GBuffer SceneNormal texture, letting both PP
and compute AO paths correctly detect and skip sky pixels.

**Additional defensive fixes:**

| File | Change |
|------|--------|
| `mt_shader.cpp` (lineardepth.fp) | PP Reverse-Z: forced sky depth 1.0→0.0 (was zNear, now zFar) |
| `mt_shader.cpp` (ssao.fp) | PP sky-dome guard: `viewPosition.z < 50000.0` check before AO |
| `mt_ao.cpp` (ssao_compute) | Compute sky-dome guard: `linearDepth >= zFar*0.99` skip |
| `mt_ao.cpp` (ssao_compute) | 100x depth ratio guard + proportional front thickness |
| `mt_ao.cpp` (ssao_combine_fs) | AO-depth combine guard: `ssao.y <= 1e-5` |
| `mt_ao.cpp` | Stencil coverage mask: `R8Unorm` texture + `RenderCoverageMask()` |
| `mt_ao.h` | `mCoverageMask`, `coverageMaskPSO`, method declarations |
| `mt_ao.metal` | Synced all compute kernel changes |
| `ssao.fp` | Depth ratio guard in `ComputeSampleHorizon` |
| `ssaocombine.fp` | AO-depth guard: `ssao.y > 1e-5 ? ... : 0.0` |

**Dead code:** `PatchFragmentShader` (lines 760-827) defined but never called.

**Build note:** `build/tools/zipdir/zipdir -udf build/gzdoom.pk3 wadsrc/static`
to rebuild pk3 after wadsrc shader changes.

- **AO speckle fix**: Neighbor-confidence-driven adaptive smoothstep in `ssao_combine_fs` (both `mt_ao.cpp` and `mt_ao.metal`). Verified no more specks.
- **Texture filter menu simplified**: Main VideoOptions now shows 3-value `SimpleFilterModes` (None/Smooth/Linear) instead of 7-value `FilterModes`. Full enum still in Advanced → Texture Options.
- **Default backend**: `vid_preferbackend` defaults to Metal (3) on macOS, Vulkan (1) elsewhere, GLES (2) or OpenGL (0) as fallback.
- **Menu backend awareness**: Added `Vulkan`, `Metal`, `GLES` IfOption checks to menudef parser. `VKOptions` hidden on non-Vulkan builds. `VR3DMenu` hidden on non-Windows.
- **Compute defaults**: `mt_compute_ao` and `mt_compute_bloom` now default to `true`. AO is 2.6x faster than PP, bloom is 5x+ faster, both stable.
- **Bloom cleanup**: Removed dead `bloom_combine_contrib` kernel, `combinePSO`, and orphaned `combParams` code. Extract kernel now adds +0.001 bias matching PP path.

## 2026-07-10 Session Changes

### Procedural sky dome garbage-normal fix (Metal-only, all content)

Reported as "the sky sphere is visible" in Trenchfoot. Root cause distinct
from the 2025-06-21 seam fix above: GZDoom's procedural sky dome
(`FSkyVertexBuffer`) has no per-vertex normal attribute, same on every
backend. GL/Vulkan get a free `(0,0,0)` normal for it because an unbound
vertex attribute reads the API's built-in zero default. Metal has no such
default — when a vertex format lacks a normal it falls back to reading the
`uVertexNormal` uniform (`GetAttrNormal()`, gated by `HAS_UNIFORM_VERTEX_DATA`,
Metal-only), which is only ever written by explicit `state.SetNormal(...)`
calls from wall/flat/model draws. `HWSkyPortal::DrawContents`
(`src/rendering/hwrenderer/scene/hw_skyportal.cpp`) never called it, so the
sky read whatever normal the *previous* draw left behind and got shaded like
real geometry. Fixed with `state.SetNormal(0.f, 0.f, 0.f)` right after
`state.ResetColor()` in that function (same idiom, same reason: reset a
uniform that isn't naturally provided by this draw's own data). Shared code,
harmless no-op on GL/Vulkan.

Separately confirmed (also via this bug report) that Metal's `useVertexData`
bit 2 (`VATTR_NORMAL`) is never set for *any* Metal draw —
`MtRenderState::ApplyStreamData()` only checks `HasColor()`. Vulkan sets it
per vertex format (`vk_renderpass.cpp:127-128`). This means every Metal draw
currently takes the uniform-fallback branch in `GetAttrNormal()`, even ones
with real per-vertex normal buffers (masked for walls/flats because they call
`SetNormal()` correctly anyway). Not fixed this session — fixing it properly
means wiring up `mHasNormal` on `MtVertexBuffer` *and* verifying Metal's
vertex descriptor pipeline actually binds the normal attribute when present,
which is unverified, untested code path. Flagged as a real latent gap, not
touched due to inability to visually verify on this machine in a tight loop.

### AO distance fade for sky-camera-room content

Trenchfoot's sky is actually a `PORTALTYPE_SKYBOX` sky-camera room (real
box-shaped physical geometry portaled in, see `HWWall::SkyPlane()` in
`hw_sky.cpp` and `HWSkyboxPortal` in `hw_portal.cpp`/`.h`), not the
procedural dome above. That content has genuine normals and legitimately
gets real AO occlusion on its creases — correct AO behavior, wrong for
content meant to read as open sky. Diagnosed via `gl_ssao_debug 3` (raw AO
depth channel): a ridge was still visible there even after the normal fix,
proving it wasn't hitting the sky early-return at all, i.e. real geometry.

Fix: distance-based AO fade (strength rolls off to 0 between two cvar
thresholds, view-space Z), not masking — same "blend toward no-AO" idiom
already used for the AOStrength/intensity slider. Implemented in both paths:
- Compute: `mt_compute_ao_fade_start` / `mt_compute_ao_fade_end`
  (`mt_ao.h` `SSAOParams`, `mt_ao.cpp` `SSAO_COMPUTE_SOURCE` + `Render()`,
  mirrored in `mt_ao.metal`).
- Shared GLSL (GL/Vulkan/Metal-PP-fallback): `gl_ssao_fade_start` /
  `gl_ssao_fade_end` (`hw_postprocess.h` `SSAOUniforms`, repurposing its two
  unused `Padding0/1` floats as `FadeStart/FadeEnd` — no layout change;
  `hw_postprocess.cpp` populates them; `wadsrc/static/shaders/pp/ssao.fp`
  applies the fade in `main()`).

Defaults for both: `100` / `500` map units (confirmed correct in-game).
GZDoom map units aren't meters — rule of thumb ~2-3cm/unit, player height is
56 units — so this sky room is close (~2-9x player height), not distant. An
initial guess of `3000`/`6000` did nothing because it was two orders of
magnitude too far for this content to ever cross the fade threshold.

### Two caching gotchas that make Metal shader edits look like they don't work

See the "Native vs inline Metal shader source" and "Metal binary pipeline
cache" sections above — both discovered while debugging why fixes above
"weren't taking effect" across several rebuild/retest cycles. Check both
before doubting a Metal shader fix on this project.

### Next steps (agreed with user)

1. Reconcile `mt_ao.cpp` inline vs `mt_ao.metal` (see roadmap above).
2. Continue the compute-shader postprocess conversion beyond AO/bloom.
3. Per-OS engine optimization pass. One concrete item raised: GZDoom's
   ZScript JIT (`src/common/scripting/jit/`) is written entirely against
   asmjit's `X86Gp`/`X86Xmm` types (checked `jit.cpp`, `jit_call.cpp`,
   `jit_flow.cpp` — no ARM/AArch64 codepath or architecture guard found).
   Confirmed this is genuinely unaddressed, not just unwritten — worth an
   ARM64/AArch64 JIT backend as a real future track for Apple Silicon (and
   any other ARM64 target) performance, verify current ARM64 behavior
   (silent VM-interpreter fallback vs. broken build) before scoping the work.
