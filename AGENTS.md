# Agent Notes

## Active Roadmap

- The durable engine plan is `docs/engine-modernization.md`.
- Keep this file focused on current implementation state, benchmark workflows,
  and short-lived handoff notes.
- Current priority order (agreed 2026-07-10, revised 2026-07-11, 2026-07-14, 2026-07-15, 2026-07-17):
  1. ~~Reconcile `mt_ao.cpp`'s inline `SSAO_COMPUTE_SOURCE` against
     `shaders/native/mt_ao.metal`~~ DONE 2026-07-10, see session notes below.
  2. ~~Compute AO's real GPU execution cost on Intel-class hardware is far
     worse than assumed~~ Bisected 2026-07-14 to the horizon-search sample
     loop specifically (not dispatch count, not resolution) — see "Compute
     AO's GPU cost is the actual bottleneck on Intel" below. Resolved by
     adding two alternative algorithms (AlchemyAO/SAO, depth-mip-pyramid
     sampling) behind `mt_compute_ao_algorithm`. **CLOSED 2026-07-17**:
     AlchemyAO (1) visually confirmed and re-measured as a real ~10-14%
     GPU-frame win (fixed a unit-mismatch bug found along the way);
     depth-mip-pyramid (2)'s controlled same-view A/B came back within
     run-to-run noise of baseline GTAO — deprioritized, left available but
     not pursued further. See "AlchemyAO/SAO and depth-mip-pyramid
     algorithms" below for full history.
  3. **NEW (2026-07-15):** investigating whether Intel's texture upload path
     is a CPU/GPU-race bottleneck — instrumentation added (`TextureUploadCPU`
     metric, `extraCommandBuffers` count), see "Texture upload
     instrumentation" below. **Not yet measured in-game** — the one check so
     far never triggered an actual upload; still need a genuine cold-load
     test (map transition or noclip into new territory).
  4. Once the texture-upload question is settled, continue the broader
     compute-shader postprocess conversion — re-validate any future compute
     pass with real GPU timing, not just CPU-encode timing, before calling
     it a win.
  5. After that, per-operating-system engine optimizations, including the
     ARM64/AArch64 JIT gap noted below.
- Opaque batching is recorded as a deferred investigation. Benchmark the
  existing `gl_sort_textures` option before changing draw-list ordering.
- **Sequencing decision (2026-07-22):** the independent Gemini audit (see
  "Independent Gemini audit" below) surfaced a real correctness bug (Finding
  6 — Metal silently dropped per-vertex normals on every draw), not a
  performance one. Decided to finish the correctness-audit thread — round 2
  with Gemini, then Finding 13 and the four Info notes — before resuming
  roadmap item 3 (texture-upload cold-load test) or item 4 (broader
  compute-postprocess conversion). Rationale: a first audit pass that found
  one live rendering bug is reason to let the audit run out rather than
  assume the rest of the backend is clean and shift focus back to
  performance work. `docs/engine-modernization.md`'s broader tracks stay
  out of scope until both the audit and the existing queued roadmap items
  (3-5) are done — not being pulled forward.

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
- ~~Current AO/bloom metrics measure CPU command-encoding time, not per-pass GPU
  execution time.~~ Whole-frame real GPU execution time added 2026-07-11, see
  "Real GPU frame timing" below. AO/bloom individual timers are still
  CPU-encode-only; per-pass GPU breakdown would need the full
  `MTLCounterSampleBuffer` API (sample points at pass boundaries), not yet
  done — see that section for why whole-frame timing came first.
- Bloom composite validation modes:
  - `mt_compute_bloom_composite 0`: automatic selection.
  - `mt_compute_bloom_composite 1`: force Tier 1 high-precision compute plus
    raster composite.
  - `mt_compute_bloom_composite 2`: require Tier 2 direct read/write or fall
    back to PP bloom.
- Current benchmark baseline on Intel MacBookAir7,2:
  - PP AO active average was around `0.715ms`.
  - Compute AO active average was around `0.278ms` (pre-2026-07-11 session).
  - Compute bloom active average was around `0.13ms`.
  - **2026-07-11 refresh** (after the mt_ao.cpp/.metal reconciliation, speckle-
    regression fix/revert, and IWAD picker work this session): Compute AO
    `active_avg` measured `~0.35ms` (samples 101-120, current 0.20-0.50ms
    range), frame time `20.5-24.3ms` avg (`41-49 fps`), `1900-2400` draws
    depending on view. The AO increase vs the `0.278ms` baseline is modest
    and not attributed to a specific regression — nothing landed this
    session should have made the compute kernel itself more expensive (the
    numSteps/numDirections clamp fix is a no-op at default cvar values; the
    blur thickness-clamp experiment was tried and reverted). More likely
    view/scene differences between benchmark runs or normal Intel HD 6000
    variance (thermal throttling). Treat `~0.35ms` as the new reference
    point; only chase it as a regression if it recurs after controlling for
    the same view.
### Real GPU frame timing (added 2026-07-11)

Motivation: `mt_metrics` showed Compute AO active-average around 0.35ms
inside a ~20-24ms frame — AO clearly isn't the bottleneck on the Intel HD
6000 target machine, but everything else (`Draws`, `Verts`, `State` counts)
was CPU-side only; no visibility into where actual GPU time goes. Chosen
deliberately over Xcode's GPU Frame Capture — the user wanted this wired
programmatically (`mt_metrics`) rather than reaching for a heavier
interactive tool on a weak/old laptop.

Added `MtMetric::FrameGPU` (`mt_metrics.h`/`.cpp`): whole-frame real GPU
execution time via `MTLCommandBuffer::GPUStartTime()`/`GPUEndTime()`.
Reported from a completion handler added in
`MtCommandBufferManager::EndFrame()` (`mt_commandbuffer.cpp`), alongside the
existing in-flight-semaphore handler — Metal invokes every handler added to
a command buffer, so this doesn't replace or race the existing one. Since
completion handlers run on a Metal-internal thread (not the render thread),
the handler only does a relaxed atomic store
(`MtDebugManager::RecordGPUFrameTimeAsync`); the render thread drains it
once per frame in `MtDebugManager::EndFrame()` before recording it into the
regular `MtMetricHistory` (same rolling-120-frame stats as AO/bloom). Value
is typically 1-2 frames stale due to pipelining (`maxDrawableCount`) —
acceptable for a rolling average, same tradeoff every async GPU timer makes.

Gated on a new `MtVersionManager::supportsGPUTimestamps` flag (macOS 10.15+,
same OS-version gate already used for `presentsWithTransaction`), following
the existing capability-flag pattern in `mt_version.h` rather than probing
per-frame. Confirmed via `libraries/metal-cpp` header inspection that
`GPUStartTime`/`GPUEndTime` (and the full `MTLCounterSampleBuffer`/
`MTLCounterSet` API, unused elsewhere in this codebase) are already fully
vendored — no metal-cpp update needed.

Scope decision: this measures **whole-frame** GPU time only, not a
per-pass breakdown (opaque geometry vs sky vs AO/bloom compute vs
composite) — the current architecture is one command buffer per frame with
strictly sequential encoders (`MtRenderState::BeginRenderPass` always calls
`EndRenderPass` first), so a single `GPUStartTime`/`GPUEndTime` pair covers
the whole frame cleanly with zero architectural change and zero added
risk. A true per-pass breakdown needs either splitting into multiple
command buffers (adds per-buffer submission overhead, a real cost on weak
GPUs — avoid unless whole-frame timing proves it's needed) or the full
`MTLCounterSampleBuffer` API with sample points at pass boundaries (Intel
HD 6000/Metal 2.0's actual support for `supportsCounterSampling` is
unverified and plausibly `false` given the GPU's age — untested, treat as
a real risk, not just a formality, if this is attempted later).

Output: `mt_metrics` now prints a `GPU  Frame` line (via `PrintMetricSummary`
reusing existing formatting) alongside the existing CPU `Frame` line, and
`mt_debug 1`'s per-frame printf gains a trailing `| GPU: X.XXms` when
available. Comparing the two numbers is the immediate next diagnostic step:
if GPU time tracks close to the CPU frame time, the bottleneck is genuinely
GPU-bound (raster/fill-rate on this iGPU); if GPU time is much lower, the
gap points at CPU-side stalling/driver overhead instead — very different
next steps depending on which. Not yet run/compared in-game this session.

Build verified: `cmake --build build --target zdoom -j 8` succeeds (touched
`mt_metrics.h/.cpp`, `mt_version.h`, `mt_debug.h/.cpp`,
`mt_commandbuffer.cpp`).

**First real result (2026-07-11, in-game on Intel MacBookAir7,2):** CPU
`Frame` avg ~18.6-19.5ms (fps=51-54 reported) vs GPU `Frame` active_avg
~44.3-50.6ms, samples ~106-119. These are not measuring the same thing —
this machine gets `maxDrawableCount=3` (past macOS 11), so the CPU races
ahead filling the pipeline; the GPU number is what actually gates what
reaches the screen. **Real deliverable frame rate is ~1000/47 ≈ 21fps, not
the ~51-54fps the CPU-only timer was previously reporting.** This is a
significantly worse picture than assumed before this instrumentation
existed — the old FPS reporting was accidentally optimistic by ~2.5x on
this hardware. Also added `supportsStageCounterSampling` capability probe
(`mt_version.h`, printed in `mt_metrics` output) to check whether Intel HD
6000 supports `MTLCounterSampleBuffer` stage-boundary sampling before
committing to building full per-pass GPU timing on top of it.

**Confirmed: no stage-boundary counter sampling support on Intel HD 6000**
(`mt_metrics` prints "NOT supported on this GPU/driver"). The fine-grained
`MTLCounterSampleBuffer` per-pass timing path is a dead end on this
hardware — any future per-pass GPU breakdown here would have to go through
the costlier multi-command-buffer split instead, and only if genuinely
needed (see below, it wasn't needed this time).

### Compute AO's GPU cost is the actual bottleneck on Intel (found 2026-07-11)

With real GPU frame timing in hand, isolated each compute postprocess pass
by toggling its existing cvar and comparing `GPU Frame` `active_avg` in
`mt_metrics` (no code changes needed — the existing `mt_compute_ao`/
`mt_compute_bloom` cvars were sufficient):

| `mt_compute_ao` | `mt_compute_bloom` | GPU Frame active_avg |
|---|---|---|
| 1 | 1 | ~50-51ms |
| 1 | 0 | ~50ms (no change) |
| **0** | 1 | **~17ms** |
| 0 | 0 | (not tested, redundant given above) |

Bloom's compute path made **no measurable difference** (~50-51ms regardless
of `mt_compute_bloom`, within run-to-run noise). Disabling compute AO alone
(falling back to PP AO) dropped GPU frame time by **~34ms — roughly
two-thirds of total GPU frame time** — even though PP AO's own CPU-encode
time is *higher* than compute AO's (0.67ms vs 0.35ms). This directly
inverts the assumption behind the existing "Compute defaults" note further
down this file ("AO is 2.6x faster than PP... both stable") — that 2.6x
number only ever measured CPU command-encoding time, never real GPU
execution cost, and on this hardware it's backwards: **compute AO is much
more GPU-expensive than PP AO**, despite encoding faster on the CPU.

Notable: this is happening even though compute AO is *already* forced to
quarter-resolution on Intel (`mt_ao.cpp`'s existing `aoScale` clamp to 4 for
`MtGPUArchitecture::Intel`). So the cost isn't primarily about resolution —
more likely the up-to-9-serial-compute-dispatches-per-frame structure
(SSAO pass, up to 4 `bilateral_blur` passes, optional `ao_upsample_fullres`,
up to 3 `ao_atrous_fullres` passes) — each dispatch has its own fixed
submission/sync overhead, which lands badly on a first-generation-Metal-
compute GPU (Intel Broadwell Gen8, 2015) that likely has much weaker
compute throughput relative to its raster/fragment pipeline than any GPU
this compute path was originally tuned against.

**Not yet resolved — two candidate directions, undecided as of this
session:**
1. Default Intel-class hardware back to PP AO (`mt_compute_ao` off by
   default when `architecture == MtGPUArchitecture::Intel`) — simple,
   immediate, backed by real data, but doesn't fix the compute kernel
   itself; a user who explicitly opts into compute AO on Intel still eats
   the ~34ms.
2. Bisect *which* of the ~9 dispatches actually dominates using the same
   free cvar-toggle + `mt_metrics` technique (`mt_compute_ao_blur_passes`,
   `mt_compute_ao_atrous_passes`, `mt_compute_ao_skip_fullres`,
   `mt_compute_ao_fullres_cleanup` are all already exposed, no code
   changes needed) before deciding whether the kernel can be made cheap
   enough on Intel to keep compute as the default there too, or whether
   direction 1 is unavoidable regardless.

**2026-07-14 bisection, round 1** (in-game, Intel MacBookAir7,2, `gl_ssao 3`,
default `mt_compute_ao_blur_passes 2`, `mt_compute_ao_atrous_passes 0`
— note the latter is forced to `1` internally at `gl_ssao>=3`, see
`mt_ao.cpp:1200):

| `mt_compute_ao_skip_fullres` | GPU Frame active_avg |
|---|---|
| 0 (full pipeline: SSAO + 2 blur + upsample + 1 atrous) | ~44.6ms |
| 1 (SSAO + 2 blur only, quarter-res, no fullres stage) | ~37.3ms |

Fullres upsample+atrous (2 dispatches, run at full screen resolution) costs
only **~7.4ms**. The remaining **~37ms** (vs. the ~17ms whole-frame baseline
with compute AO off entirely from the prior session) is the SSAO + 2 blur
dispatches at quarter-res — i.e. ~20ms of compute AO's own cost lives in 3
low-pixel-count dispatches, more than the 2 full-res dispatches cost. This
is the opposite of what pixel-count-driven cost would predict.

**Round 2** (same session, `skip_fullres 1` throughout): isolated blur and
the bare SSAO dispatch.

| config | GPU Frame active_avg |
|---|---|
| `blur_passes 1`, `gl_ssao_debug 2` (blur off, SSAO only) | ~33.4ms |
| `blur_passes 1`, blur on | ~33.8-36.0ms (run-to-run noise, ~same) |
| `blur_passes 2` (round 1 above) | ~37.3ms |

So a single blur pass costs ~0ms (noise-level), a second blur pass costs
~2-4ms, and **bare SSAO alone is ~33.4ms — almost the entire cost**,
overturning the round-1 fixed-per-dispatch-overhead theory. The
`ssao_compute` kernel body itself is the bottleneck, not dispatch count or
the fullres stage.

**Round 3**: to test whether it's the horizon-sample loop specifically,
dropped `mt_compute_ao_steps 4` / `mt_compute_ao_directions 4` (16
iterations/thread, vs. `gl_ssao 3`'s default 8x6=48) with `blur_passes 1`,
`skip_fullres 1`: GPU Frame active_avg **23.718ms**. Against the ~17ms
whole-frame baseline with compute AO off (prior session) and ~33.4-36ms at
48 iterations: cutting iterations 3x (48→16) cut AO's own cost roughly
2.5-2.8x (~16.4-19ms → ~6.7ms over baseline) — not perfectly linear (a
fixed per-thread/texture-setup cost remains independent of loop count), but
confirms the horizon-sample loop (`numSteps × numDirections` dependent
texture fetches: coverage mask, depth, normal per iteration, with
data-dependent `continue`s causing likely warp divergence) is the real
scaling driver on Intel HD 6000's older texture units — not dispatch count,
not resolution, not blur/fullres.

**Conclusion so far:** even at reduced 16-iteration quality, compute AO
still costs ~6.7ms/frame over the ~17ms PP-AO-comparable baseline — a real
but much smaller tax than the ~33ms at full `gl_ssao 3` defaults. Two
non-exclusive levers now backed by data:
1. Default `mt_compute_ao` off for `MtGPUArchitecture::Intel` (mirrors the
   existing `aoScale` Intel special-case in `mt_ao.cpp:913` — `mt_compute_ao`
   is currently a plain `CVAR(Bool, ..., true, ...)` in `mt_postprocess.cpp:34`
   with no architecture awareness at all).
2. Clamp `numSteps`/`numDirections` lower specifically for Intel (same
   pattern as the existing `aoScale` clamp), keeping compute AO available
   there but cheaper — doesn't fully close the gap to PP AO but meaningfully
   narrows it.
**Decided (2026-07-14): clamp steps/directions on Intel, keep compute AO on
by default there.** Rejected defaulting `mt_compute_ao` off on Intel — user
wants compute AO to remain the default everywhere and preferred narrowing
the cost instead of falling back. Implemented in `mt_ao.cpp` `Render()`,
right after the existing "Override with CVARs" block: unconditionally clamp
`params.numSteps`/`params.numDirections` to a max of 4 each when
`fb->mVersionManager.architecture == MtGPUArchitecture::Intel`, overriding
both the `gl_ssao` quality tier and any `mt_compute_ao_steps`/
`mt_compute_ao_directions` cvar override — same hard-clamp idiom already
used for `aoScale` a few lines above in the same function. This caps Intel
at the measured ~6.7ms-over-baseline tier instead of `gl_ssao 3`'s ~33ms
tier; does not fully close the gap to PP AO's ~17ms baseline, but is a
meaningful reduction while keeping compute AO the default. Build verified
(`cmake --build build --target zdoom -j 8` succeeds). **Not yet visually
re-verified in-game or re-measured with `mt_metrics`** — do that next
(confirm AO still looks acceptable at 4x4 on Intel and that GPU Frame
active_avg lands near the ~24ms measured for the equivalent manual
steps=4/directions=4 test above, at `gl_ssao 3` with no other cvars
overridden).

### AlchemyAO/SAO and depth-mip-pyramid algorithms (added 2026-07-14)

Follow-up to the Intel bisection above: instead of continuing to retune
GTAO's constants, added two structurally different compute AO algorithms
behind a new `mt_compute_ao_algorithm` cvar (`0` = GTAO, current default,
unchanged behavior; `1` = AlchemyAO/SAO; `2` = GTAO + depth-mip sampling),
each targeting one of the two cost axes the bisection separated:

- **Sample count** (`ssao_compute_alchemy` kernel, `mt_ao.metal`): GTAO's
  `numDirections x numSteps` horizon-marching loop replaced with a fixed,
  flat Vogel/Fibonacci-disk sample set (one dependent fetch chain per
  sample, no per-direction horizon-max tracking). Reuses `SSAOParams`
  as-is — `numSteps` repurposed as the flat sample count (own tier defaults
  8/8/12, overridable via new `mt_compute_ao_alchemy_samples` cvar, same
  "0 = tier default" convention as `mt_compute_ao_steps`), `numDirections`
  unused. The existing Intel `numSteps` hard-clamp (previous section) then
  automatically caps this algorithm's sample count too, for free. Uses the
  classic AlchemyAO occlusion term (`max(0, dot(v,n) - bias*centerViewPos.z)
  * falloff / dot(v,v)`), not GTAO's horizon-angle formula.
- **Per-sample cost at distance** (`linearize_depth_mip0` +
  `ssao_compute_mip` kernels): same GTAO horizon-marching structure/loop
  count, but a new dedicated `mDepthPyramidTexture` (R16Float, full scene
  resolution, up to 6 mips, seeded by `linearize_depth_mip0` then chained
  via `MTLBlitCommandEncoder::generateMipmaps` — box-filter is fine here,
  unlike Hi-Z culling, since a distant AO tap only needs an approximately-
  right coarse depth) lets far horizon-search steps read a coarser,
  cache-friendlier mip instead of always full-res depth. LOD selected
  per-sample via `clamp(log2(max(rayPixels / 8.0, 1.0)), 0, maxLod)`,
  queried in-shader via `depthPyramid.get_num_mip_levels()` (no new CPU
  param). Coverage and normal fetches stay full-res — no normal pyramid.
  Known, accepted limitation (not fixed): `linearize_depth_mip0` must
  preserve the invalid-texel sentinel (`raw <= 0.0001 -> 0.0`, not an
  unconditional linearize) so `ssao_compute_mip`'s downstream guard still
  works, but box-filtering that `0.0` across a sky/geometry edge into
  coarser mips can still blend it into a bogus mid-value there — accepted
  because only distant, already-falloff-discounted taps ever reach coarse
  mips and the existing thickness/depth-ratio rejection already discards
  samples too far from `centerViewPos.z`.

**Encoder-sequencing note (algorithm 2 specifically):** `MtAOModule::Execute`
normally holds one continuous `computeCommandEncoder()` for the whole AO
pass. Algorithm 2 needs, within the same `cmdBuf`: compute encoder
(linearize) → `endEncoding()` → blit encoder (`generateMipmaps`) →
`endEncoding()` → a *new* compute encoder (main horizon-search kernel,
falls through unchanged into the existing blur/fullres code). Confirmed by
reading `mt_commandbuffer.cpp:46-47` that `GetBlitCommandBuffer()` must NOT
be used here — it unconditionally creates a separate, independently-
committed command buffer with no ordering guarantee relative to `cmdBuf`,
which would let mip generation race the dispatches depending on it. Used
`cmdBuf->blitCommandEncoder()` directly instead. This costs 2 extra
encoder transitions per frame for algorithm 2 only (0/1 keep the original
single encoder) — **flagged as a real, unmeasured cost**, per this
session's own lesson that an "obvious" perf assumption (dispatch-count-
driven cost, from the original bisection) turned out wrong. First thing to
measure: algorithm 2 vs algorithm 0 at *equal* iteration counts, to
separate the mip-fetch win from the added encoder overhead.

Silent fallback to algorithm 0 (GTAO) if a requested algorithm's PSO or
resources aren't available (metallib missing the function and fallback
compile failed, or `mDepthPyramidTexture` allocation failed) — existing
users/configs see zero behavior change since the cvar defaults to `0`.
`Combine()` needed zero changes — it already operates generically on
whatever texture `Render()`/`Execute()` produced.

Also added `tools/check_shader_parity.py` (stdlib-only Python, manually run
or via optional non-default CMake target `check_shader_parity`): extracts
every named `kernel`/`vertex`/`fragment` function from a canonical `.metal`
file and its paired inline C++ fallback string, diffs bodies with comments
stripped (raw bodies false-positive on intentionally-different header-
comment wording between the two files), reports MATCH/FAIL per function.
Run against the current tree: **all 10 AO kernels (7 existing + 3 new) and
all 8 bloom kernels MATCH** — `mt_bloom.metal`/`mt_bloom.cpp` was flagged
as an open, never-audited risk in the prior session, but turned out not to
be actually content-drifted, only its header comment had the authority
direction backwards (same wording bug `mt_ao.metal` had before its
2026-07-10 fix) — fixed as a one-line companion change.

Build verified (`cmake --build build --target zdoom -j 8` succeeds,
including the native `.metal` compile step for the 3 new kernels) and
`tools/check_shader_parity.py` passes.

**First in-game measurement (2026-07-15, same view/room unless noted):**

| `mt_compute_ao_algorithm` | GPU Frame active_avg | ComputeCPU AO active_avg |
|---|---|---|
| 0 (GTAO, baseline) | ~20.56ms | ~0.56ms |
| 1 (AlchemyAO) | ~15.05ms, ~15.25ms (two consecutive samples, same view) | ~0.46-0.62ms |
| 2 (depth-mip-pyramid) | ~20.70ms (same view as baseline), ~22.70ms (different room) | ~0.68ms |

**AlchemyAO (1) is a clean GPU-time win but shipped visually broken** — user
report 2026-07-15: "only 0 and 2 working correctly... normals/geometry
stretching or coordinate issue." Root cause found on review, not by
guessing: the occlusion term used the classic AlchemyAO paper's
`max(0, dot(v,n) - bias*cszC.z) / dot(v,v)`, where `bias` is scaled by
camera-space Z directly. That trick only works if view-space Z is a small,
bounded quantity comparable in magnitude to the sample radius, which is the
reference implementation's convention — **not** this codebase's, where
`centerViewPos.z` is a raw linear-depth in world units (hundreds to
thousands). `bias * centerViewPos.z` therefore swamps `dot(v,n)` (which
scales with the tens-of-units sample radius) for anything beyond
point-blank range, collapsing `max(vn - bias*z, 0)` to 0 almost everywhere
except right next to the camera — AO stops tracking real geometry at any
distance, matching the reported symptom exactly.

**Fixed** in both `mt_ao.metal` and `mt_ao.cpp`'s fallback: replaced the
unnormalized `dot(v,n)`/`bias*z` pair with a normalized cosine term against
the same dimensionless `params.bias` convention `ssao_compute` (GTAO)
already uses — `normalAngle = dot(centerNormal, sampleVector) * invDistance`,
`occlusion += max(normalAngle - params.bias, 0.0) * falloff * invDistance`.
Keeps the "closer samples matter more" distance weighting (1/|v|, gentler
than the paper's 1/dot(v,v)) without the units bug. Build verified
(`cmake --build build --target zdoom -j 8` succeeds, including the native
`.metal` compile step) and `tools/check_shader_parity.py` still reports
MATCH for all 10 AO kernels. **Not yet re-verified in-game** — re-run the
Part A visual check (`mt_compute_ao_algorithm 1`, `gl_ssao_debug 0/1/2`)
and, if it looks correct, the perf A/B from the previous session again
(the fix changes the math, so the ~15ms result needs reconfirming, not just
assumed to still hold).

Lesson for next time a formula is ported from a paper/reference
implementation into this codebase: check whether the reference's view-space
convention matches GZDoom's (large raw world-unit linear depth, not a
small bounded camera-space quantity) before reusing a constant scaled by
that value directly — this is the same class of "assumption doesn't
transfer across unit systems" mistake as the `theorem_licensed_typestate`
digression earlier this session, just caught the ordinary way (user visual
report) rather than a formal method.

**User re-tested after the bias fix: "same issue... looks 90 degrees
orthogonal to the viewport."** Before assuming a second math bug, diffed
`ssao_compute_alchemy` against the known-working `ssao_compute` (both
extracted and unified-diffed programmatically) — confirmed the entire
shared prologue (UV/sceneUV computation, coverage/depth/sky/normal guards,
dither/rotation jitter) is byte-identical between the two; the only
differences are the intentional ones (flat sample loop vs nested
direction/step loop, the occlusion term itself). A second coordinate bug
in shared, already-proven code was very unlikely.

Instead, checked the other already-documented gotcha in this file first:
**Metal binary pipeline cache** (`~/Library/Application Support/zdoom/cache/mt_pipelines.bin`).
Confirmed it existed and had a same-day mtime, meaning it was actively
serving compiled pipelines across the exact test runs that reported "same
issue" after the source fix — the leading hypothesis is that the stale
cached `ssao_compute_alchemy` pipeline (from before the bias fix) was being
reused instead of recompiling from the corrected `.metal`/metallib, so the
fix never actually reached the running binary. Deleted the cache file
(disposable, regenerates automatically on next launch). **User needs to
fully relaunch the game (not just rebuild) and retest** — this is the
standard remedy already documented above ("Metal binary pipeline cache"
section) for exactly this "shader edit doesn't seem to take effect"
symptom; not yet confirmed this was the actual cause here, only that it's
the most concrete lead given the shared-code diff ruled out a second
coordinate bug.

**Cache theory ruled out** — user relaunched and retested, still broken:
"Coordinate system is still off... not responding to normals? ...not
normalized or else set for the reverse Z?" Before hunting a third
hypothesis, checked which binary/metallib was actually running: confirmed
via `ps`/`mdfind`/timestamp checks that only one `gzdoom.app` exists on the
machine (no stray older copy being launched by habit), and its
`native_shaders.metallib` (both the `Contents/MacOS` and `Contents/Resources`
copies) had the same-second timestamp as the freshly-built executable — so
infrastructure (stale binary, stale cache) is not the explanation; this is
a real bug in the kernel logic.

Given the shared prologue was already diff-confirmed identical to the
proven-working `ssao_compute`, the remaining, untested surface is the one
piece of math this kernel added beyond a straight port of GTAO's per-sample
term: the extra `* invDistance` (1/|v|) distance-weighting multiplier meant
to mimic the classic AlchemyAO paper's `1/dot(v,v)` falloff shape. That
term was never visually validated on its own. Rather than keep debugging an
unvalidated, invented formula, **removed it** — `ssao_compute_alchemy`'s
occlusion term is now `max(normalAngle - bias, 0.0) * falloff`, i.e.
byte-for-byte the same per-sample math `ssao_compute` already uses
(`normalAngle = dot(centerNormal, sampleVector) * invDistance`, same
`falloff`), with the *only* remaining difference between the two kernels
being the sample pattern itself (flat Vogel disk vs. nested
direction/step loop with horizon-max tracking) — not the occlusion math.
This should behave predictably (correctly geometry/normal-aware, since it's
proven code) modulo overall brightness, which any remaining mismatch
against GTAO can absorb via the existing `visibilityStrength`/`aoMultiplier`
tuning rather than needing new math. Build verified, parity check passes,
binary pipeline cache cleared again pre-emptively (same risk as before,
now that the kernel content changed again). **Confirmed working 2026-07-16**
after relaunch — visual check passed. Root cause was the invented, never-
independently-validated `1/|v|` distance-weighting term; removing it and
falling back to `ssao_compute`'s own proven per-sample math (applied to a
flat Vogel-disk sample set) fixed it.

**Re-measured 2026-07-16/17, same-view, post-fix**: algorithm 0 GPU Frame
active_avg `18.237ms`; algorithm 1 `15.764ms` and `16.528ms` across two
consecutive same-view samples — a real, reproducible **~10-14% reduction**.
Smaller than the pre-fix (buggy-formula) session's ~26% figure, but not a
regression concern: the bug was in the final arithmetic (clipping most
per-sample contributions to zero), not in which textures got fetched, so
GPU cost shouldn't have differed much between the buggy and fixed formula
either way — the ~26% vs ~11% gap is most likely ordinary view/scene
variance between test sessions, same as the run-to-run noise already
documented elsewhere in this file. AlchemyAO's GPU-time win is confirmed
real and holds after the correctness fix.

**Depth-mip-pyramid (2) is inconclusive, not a demonstrated win**: its two
samples (~20.70ms, ~22.70ms) sit in the same range as algorithm 0's
baseline (~20.56ms) rather than below it, but the two algorithm-2 samples
weren't both taken in the same view as each other or as the baseline (one
matched the baseline room, the other was after moving to a new "Empties"
room), so this isn't a fully controlled comparison yet. Plausible
explanation if it holds up: the flagged encoder-transition overhead (2
extra compute/blit encoder open-closes per frame) is offsetting the
mip-fetch bandwidth savings — exactly the risk called out when this was
designed. **Next step: a controlled same-view A/B** (algorithm 0 vs 2,
identical camera position, `mt_metrics_reset` before each) before drawing
any conclusion about algorithm 2's value.

**Controlled same-view A/B run (2026-07-17), same view throughout,
`mt_metrics_reset` before each sample:**

| `mt_compute_ao_algorithm` | GPU Frame active_avg |
|---|---|
| 2 | 21.476ms |
| 0 | 21.092ms |
| 0 (repeat) | 22.854ms |
| 2 (repeat) | 21.612ms |

**Conclusion: depth-mip-pyramid (2) is not a demonstrated win.** Both
algorithms land in the same ~21-23ms band on this same view — the spread
between repeats of the *same* algorithm (21.092 vs 22.854 for algorithm 0)
is as large as the spread between algorithms, i.e. within already-documented
run-to-run noise, not a real effect. The mip-fetch bandwidth savings this
algorithm targets are most likely being offset by its own added
encoder-transition overhead (2 extra compute/blit encoder open-closes per
frame vs. algorithms 0/1's single continuous encoder) — exactly the risk
flagged when it was designed. **Decided: deprioritize algorithm 2.** Not
deleted (silent-fallback-to-0 safety net already in place, and it's
correct/harmless, just not worth its complexity) — leave it available
behind the cvar for anyone curious, but stop spending further tuning effort
on it. AlchemyAO (algorithm 1) remains the one validated, real win from
this investigation (~10-14% GPU-frame reduction, confirmed working
in-game); no reason found to make it default over GTAO yet since that's a
separate decision (visual-quality parity across more content, not just this
one test view) — worth a broader visual pass before flipping the default,
not required before moving on.

**Regression found 2026-07-17: AlchemyAO "blotchy patches" on Intel.** User
report after the above was thought closed: "terrible looking AO... blotchy
patches all over the darkened geometry" at `mt_compute_ao_algorithm 1`.
Root cause found by reading `Render()`'s parameter setup in order, not by
guessing: algorithm 1 sets its own tier default at line ~1353-1360
(`numSteps` repurposed as a **flat total sample count** — 8/8/12 by tier),
but the existing Intel clamp block right after it (`numSteps =
std::min(numSteps, 4)`) was written for GTAO's `numSteps x numDirections`
**per-factor** budget and applied unconditionally to both algorithms. For
GTAO that clamp leaves an effective `4x4=16` total dependent-fetch
iterations; for algorithm 1 it collapsed the flat count straight to **4
total samples** — a 4x tighter budget than GTAO gets on the same hardware,
for no cost reason, since both algorithms pay the same class of per-sample
coverage/depth/normal dependent fetch that the original bisection found to
be the actual driver. 4 samples over a disk pattern with no horizon-search
structure to hide the noise is exactly what "blotchy patches" describes.
Likely present (masked) during the 2026-07-16 "confirmed working" visual
check too — that check was validating the coordinate/normal bug
specifically, which was real and is fixed; this separate undersampling
issue just wasn't the salient symptom at the time.

**Fixed**: Intel clamp block now special-cases `algorithm == 1`, clamping
its flat `numSteps` to `16` (matching GTAO's clamped *effective* total)
instead of reusing GTAO's raw per-factor clamp of `4`. Pure CPU-side
constant change in `mt_ao.cpp`'s `Render()` — no shader recompilation
involved, so the binary pipeline cache gotcha doesn't apply this time (no
need to clear `mt_pipelines.bin`). Build verified (`cmake --build build
--target zdoom -j 8` succeeds). **Not yet visually re-verified in-game** —
next step: relaunch, `mt_compute_ao_algorithm 1`, check the blotchy-patch
symptom is gone; if it looks acceptable, the GPU-time re-measurement from
2026-07-16/17 needs re-confirming too since the sample count just changed
(4→16, likely costs somewhat more than the buggy 4-sample version, though
still well under GTAO's un-clamped `gl_ssao 3` cost).

**Third regression found 2026-07-19: "AO rotates or moves at diagonal
angles/stretches" on all three compute algorithms, near geometry.**
Distinct from the algorithm-1-only spiral bug above (that fix stands;
this is separate and shared by algorithms 0/1/2 alike, so it lives in code
all three kernels share, not per-algorithm sample-pattern code). Diagnosed
with `gl_ssao_debug 7` first (visualizes `step(1e-5, ssao.y)` — white for
any pixel that passed the four early-return guards) to rule out the
guard-sentinel theory from the "unset AO" report that prompted this
investigation: user confirmed not black, so none of the four early-return
guards (coverage/depth/sky/normal) were misfiring — this was a real
sampling-math bug, not an invalid-pixel bug, despite the initial "unset"
description.

> **NOTE added 2026-08-06.** This `gl_ssao_debug 7` reasoning is sound *for the
> compute path it was applied to*, and the conclusion stands. Do NOT generalise
> it to the reference PP path. `hw_postprocess.cpp:910` gates BOTH blur passes
> behind `gl_ssao_debug < 2`, so on the reference path debug 7 reads `ssao.fp`'s
> raw output and cannot see what the vertical blur does to `.y`. That blind spot
> hid a total AO composite failure for months — see "SOLVED 2026-08-06" at the
> end of this file.

Root cause, confirmed by computing actual numbers rather than guessing:
`radiusPixels = params.radiusToScreen / max(centerViewPos.z, 1e-5)` has no
upper bound, in all three kernels identically. At `centerViewPos.z ≈ 20`
world units — a completely ordinary player-to-wall distance, well within
normal collision-radius range, not an extreme/clipping edge case —
`radiusPixels` already reaches ~658px against a quarter-res Intel AO
texture only ~270px tall (2.4x the texture's own height; verified with a
throwaway numeric check, not assumed). Every sample's UV then hits the
existing `clamp(sampleUV, halfTexel, 1-halfTexel)` texture-edge clamp,
collapsing the entire sample disk into a handful of edge pixels with no
relation to local geometry. Which edge pixel gets hit is hypersensitive to
tiny position/angle changes, which reads visually as the AO pattern
rotating/stretching/moving as the player shifts slightly — matches the
report exactly, and being present in all three kernels' identical
`radiusPixels` formula matches "all compute paths."

**Fixed** in both `mt_ao.metal` and the `mt_ao.cpp` fallback, all three
kernels (`ssao_compute`, `ssao_compute_alchemy`, `ssao_compute_mip`):
`radiusPixels = min(radiusPixels, min(outSize.x, outSize.y) * 0.5)` right
after the existing formula — bounds the sample disk to a sane fraction of
the texture regardless of proximity. At normal viewing distances
`radiusPixels` is already well under this bound (e.g. ~235px at
player-height distance against a 270px texture, per the same numeric
check), so this should be a no-op for already-correct AO and only kick in
for the previously-broken close-range case. Real shader-source change —
binary pipeline cache cleared pre-emptively. Build verified and
`tools/check_shader_parity.py` reports MATCH for all three kernels.
**Not yet visually re-verified in-game** — the PP (non-compute) GLSL path
(`wadsrc/static/shaders/pp/ssao.fp:110`) has the structurally identical
unbounded `radiusPixels = RadiusToScreen / viewPosition.z` formula and is
plausibly affected too, but that wasn't part of this report (user only
described compute paths) and hasn't been checked — flagged as a candidate
follow-up, not fixed preemptively without an observed symptom there.

**Second regression found 2026-07-17, same session: "AO coordinates seem to
rotate on algorithm 1 when close to something."** Distinct from the
blotchy-patches undersampling bug above (that fix already landed). Root
cause found by diffing `ssao_compute_alchemy` against the proven
`ssao_compute` for jitter technique, not by guessing: GTAO jitters each
sample's position along its ray (`stepJitter`, from `InterleavedGradientNoise`
+ an R2 low-discrepancy constant); the Vogel/Fibonacci-disk kernel had no
per-sample jitter at all, only a single global per-pixel `rotation`. Every
pixel therefore placed its i-th sample at exactly the same fractional ring
radius (`radiusFrac = sqrt((i+0.5)/sampleCount)`) — a textbook Vogel-disk
"pinwheel" artifact, normally hidden when the on-screen sample radius
(`radiusPixels`, which grows as `centerViewPos.z` shrinks — i.e. up close)
is small enough for the rings to overlap, but resolved into a visibly
rotating spiral once close enough to spread the rings apart. Matches the
report exactly. **Fixed** in both `mt_ao.metal` and the `mt_ao.cpp`
fallback: added a per-pixel `radiusJitter` term (`fract(noise.z + ign *
0.754877666)`, the same R2 constant/idiom `ssao_compute` already uses,
reused rather than inventing new math) folded into the radius calculation
(`sqrt((i + 0.5 + radiusJitter) / sampleCount)`), breaking the deterministic
ring coherence between neighboring pixels. This is a real shader-source
change (unlike the blotchy-patches fix, which was CPU-only) — binary
pipeline cache cleared pre-emptively. Build verified and
`tools/check_shader_parity.py` reports MATCH for all kernels including
`ssao_compute_alchemy`. **Not yet visually re-verified in-game.**

~~AO algorithm investigation is now closed out~~ **Reopened same session**
by the blotchy-patches regression above — see that section for the fix,
still pending visual re-verification. Once that's confirmed, next per
roadmap ordering: the texture-upload cold-load test (still unresolved —
see below), then the broader compute-shader postprocess conversion. Also
reprioritized mid-session (2026-07-17) to a renderer-wide Intel-optimization
audit pass — see "Dead `explicitFlushing` flag removed" below and the
pending `gl_sort_textures` opaque-batching benchmark, both ahead of the
texture-upload item in actual session order even though the roadmap lists
texture-upload first; not a re-ranking of the durable priority, just what
got worked on as it came up.

Frame rates in this session's test view (~90-150 fps CPU-side) are much
higher than the ~40-55 fps range seen in the prior Intel HD 6000 bisection
session — likely a lighter-geometry room, not different hardware; GPU
Frame time (the real bottleneck per the earlier finding) is the number to
trust for cross-algorithm comparison, not fps/CPU frame time.

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

### Fourth regression: AlchemyAO (algorithm 1) fine-grain noise survives blur (found 2026-07-21)

User-supplied screenshots (`gl_ssao_debug 1`, i.e. the *blurred* AO buffer, not
raw) showed dense salt-and-pepper grain across otherwise flat surfaces at
`mt_compute_ao_algorithm 1` — distinct from the three earlier algorithm-1
regressions above (blotchy patches = undersampling, now fixed at 16 samples;
pinwheel spiral = missing per-sample jitter, now fixed; edge-clamp
rotation/stretch near geometry = unbounded `radiusPixels`, now fixed). All
three of those fixes are present in the current tree, yet the noise persists,
so this is a fourth, separate issue.

Root cause, found by reading the two kernels' accumulation structure side by
side rather than guessing: `ssao_compute` (GTAO) accumulates per direction as
`occlusion += max(sampleHorizon - horizon, 0.0); horizon = max(horizon,
sampleHorizon)` — a telescoping sum that collapses each direction's
`numSteps` samples down to a single contribution (that direction's final max
horizon). A single noisy sample only matters if it happens to be the new
per-direction maximum, and even then its contribution is bounded by the
delta from the previous max. `ssao_compute_alchemy` has no equivalent
structure — by design (see the comment above it, added when the invented
`1/|v|` distance-weighting term was removed: "keep this kernel's only
difference from `ssao_compute` being the sample pattern, not the occlusion
math") — so all `sampleCount` (8-16) per-sample `max(normalAngle - bias,
0.0) * falloff` contributions sum independently. Same visibility strength
tuning is shared between both algorithms (`Render()`'s `gl_ssao` tier
switch), which was tuned against GTAO's low-variance telescoped estimator;
applied to AlchemyAO's higher-variance flat-sum estimator, the same
multiplier amplifies per-pixel noise into visible grain that the default
2-pass 3x3 bilateral blur doesn't fully remove. This is a real accuracy
trade-off inherent to classic AlchemyAO/SAO-style flat sampling vs.
horizon-search — not something fixable by tweaking the occlusion formula
further without either inventing new math (the exact mistake already made
and reverted twice in this section) or restructuring the sample pattern into
per-direction buckets with max-suppression (a bigger, riskier change, not
attempted here).

**Fixed**, conservatively: `Execute()` now forces `blurPasses` to the
existing max (4) specifically when `algorithm == 1`, instead of respecting
`mt_compute_ao_blur_passes`' default of 2 for it. Chosen over increasing
`sampleCount` or touching the occlusion math because blur is a cheap 3x3
pass over the (usually quarter-res) AO texture — the original Intel
bisection identified the horizon-search/sample-loop dependent fetches, not
blur or dispatch count, as the actual GPU cost driver — so this shouldn't
meaningfully erode algorithm 1's measured ~10-14% GPU-frame win. Pure
CPU-side change in `mt_ao.cpp`'s `Execute()`, no shader source touched, so
`tools/check_shader_parity.py` is unaffected (still passes) and no binary
pipeline cache concern. Build verified (`cmake --build build --target zdoom
-j 8` succeeds).

**Visually confirmed 2026-07-22**: user supplied same-content `gl_ssao_debug
1` screenshots (`algorithm0.png`/`algorithm1.png`/`algorithm2.png`,
"Night School - ASHES: HARD RESET") for all three algorithms. Rather than
eyeballing them, measured high-frequency grain (patch mean minus its own 5x5
local-mean, std of the residual) across a 5x5 grid of 25 sample patches per
image, avoiding the HUD and doorway silhouettes:

| Algorithm | median patch noise | mean patch noise |
|---|---|---|
| 0 (GTAO baseline) | 0.796 | 1.775 |
| 1 (AlchemyAO, post-fix) | 0.407 | 0.41 |
| 2 (depth-mip) | 0.758 | 1.915 |

Algorithm 1 now has *less* residual grain than the GTAO baseline post-fix,
and consistently so (median and mean are close, unlike 0/2 where a handful
of much-noisier patches — likely near geometry edges — drag the mean well
above the median). Caveat: the three captures weren't from a perfectly
locked camera position (window sizes and visible geometry differ slightly
between shots), so this is a directional read, not a rigorously controlled
same-pixel A/B — but a 2-4x gap is large enough to call the grain regression
resolved without needing a tighter re-test. Forced 4 blur passes is the
final fix; no further denoising work planned for algorithm 1 unless a new
symptom surfaces. The "next lever" ideas below (wider blur kernel,
re-measuring the GPU-time cost of 4 passes) are now optional follow-ups, not
required — flagging the GPU-time re-measurement as the more relevant one
if algorithm 1 is ever promoted to default, since 4 blur passes was chosen
for correctness, not benchmarked for cost.

### Pre-commit audit: bilateral_blur's background-bleed guard was silently dropped (found 2026-07-22)

Before committing this session's accumulated uncommitted changes, ran a
self-review pass over the full diff (not prompted by a user-visible report —
proactive, catching a regression before it shipped). `bilateral_blur`'s
8-tap rework (the diagonal-neighbor expansion earlier in this file) dropped
its explicit `if (sampleDepth - depth > params.maxThickness) continue;`
hard-reject without an equivalent replacement — the new `exp2(-r*r*0.222 -
deltaZ*deltaZ)` falloff is calibrated against `gl_ssao_blur`-scale depth
deltas, not `maxThickness`'s much finer ~1.25-unit scale, so at the old
threshold the new weight is ≈0.996 — effectively unattenuated. Confirmed
`params.maxThickness` had become fully dead inside the kernel (grep found
zero reads in either `mt_ao.metal` or the `mt_ao.cpp` fallback) — a strong
signal this was an incomplete refactor, not intentional retuning. Risk:
background AO bleeding into foreground pixels as a halo at strong depth
discontinuities (doorway/pillar silhouettes against distant rooms), with no
compensating guard since `mt_compute_ao_normal_blur` defaults off.

**Fixed**: restored the exact original hard-reject condition, immediately
after the existing `sampleDepth <= 1e-5` invalid-texel check, in both
`mt_ao.metal` and the `mt_ao.cpp` fallback — makes `maxThickness` live again
without touching the rest of the reworked 8-tap weighting. Build verified
(`cmake --build build --target zdoom -j 8` succeeds) and
`tools/check_shader_parity.py` still reports MATCH for all kernels.
**Not yet visually re-verified in-game** — check at a strong depth edge
(doorway/pillar against a distant background) for any remaining halo before
considering this closed.

Also flagged in the same pass, not yet fixed (low severity, metric-accuracy
only): `mt_texture.cpp`'s `RecordTextureUpload` fires even when
`GetStagingBuffer()` returns null, mislabeling a failed allocation as a
fast successful upload sample in the `TextureUploadCPU` metric — relevant to
the still-open "Texture upload instrumentation" cold-load investigation
above if that metric is revisited.

## Independent Gemini audit (2026-07-22) — verified, mixed results

Handed the Metal backend (`src/common/rendering/metal/`, ~14k lines,
cross-referenced against `src/common/rendering/vulkan/`) to Gemini for a
blind second opinion, using a self-contained brief
(`GEMINI_AUDIT_HANDOFF.md`) that deliberately withheld this file's own
findings so its conclusions would be independent. Output: `FINDINGS.md`,
18 findings (2 critical, 2 high, 5 medium, 5 low, 4 info).

**Before acting on any of it, verified the highest-severity findings against
the actual code** — turned up a high false-positive rate concentrated in
exactly the findings that would have mattered most if trusted blind:

- **Finding 1 (CRITICAL, `RenderPassDescriptor` leak in `BeginRenderPass`) —
  FALSE, and the suggested fix would have introduced a real bug.**
  `renderPassDescriptor()` is an Objective-C convenience factory method
  (no alloc/new/copy in the name) — by Cocoa convention it returns an
  **autoreleased** object the caller does not own. Confirmed this codebase
  already knows and follows that rule: `CreateNewCommandBuffer()` explicitly
  `retain()`s its `commandBuffer()` result with a comment explaining why
  (`mt_commandbuffer.cpp:33`), and `MetalRenderDevice::Update()` wraps each
  frame in `NS::AutoreleasePool` (create at line 310, `release()` at 382).
  `pRPD` in `BeginRenderPass()` is never retained and never escapes its own
  scope, so it's already correctly cleaned up once per frame. Adding
  `pRPD->release()` as suggested would be a genuine over-release/
  use-after-free.
- **Finding 2 (CRITICAL, `WaitForCommands(true)` is a sync no-op) — FALSE.**
  "Commit an empty dummy buffer after the real work, wait on the dummy" is a
  textbook-correct Metal idiom: command buffers on the same queue are
  guaranteed to execute and complete in commit order, so waiting on a later
  buffer transitively waits for everything committed before it. Not a no-op.
- **Finding 3 (HIGH, `PatchFragmentShader` dead code) — FALSE.**
  `MtShaderManager::LoadFragShader` calls it directly (`mt_shader.cpp:931`,
  added by commit `728c775de`). Gemini's own cross-check against this file
  cited a real but stale line (see the strikethrough a few sections up) as
  corroboration — worth noting since the audit brief specifically told it to
  form conclusions before reading this file, and it still ended up leaning
  on a stale note here rather than trusting its own (apparently incomplete)
  search.
- **Finding 8 (MEDIUM, `ClearScreen` missing scissor/viewport reset,
  claimed Vulkan-only) — FALSE.** Vulkan's `VkRenderState::ClearScreen`
  (`vk_renderstate.cpp:50`) has the identical structure — no explicit
  scissor/viewport reset either. No Metal-specific asymmetry exists.
- **Finding 9 (LOW, `AOBlurParams.applyExponent` dead field) — FALSE.**
  Written in `mt_ao.cpp:1676`, read in both `mt_ao.metal:692` and the
  `mt_ao.cpp` fallback at 729. Not dead.
- **Finding 10 (LOW, inflight semaphore never waited on) — FALSE.**
  `dispatch_semaphore_wait(mInflightFramesSemaphore, ...)` runs every frame
  in `MetalRenderDevice::BeginFrame()` (`mt_renderdevice.cpp:490` and 498,
  the latter an unbounded fallback wait after a 1s timeout) — real,
  functioning GPU-backpressure throttling.
- **Finding 5** (texture re-upload races) is built entirely on Finding 2
  being true; invalidated along with it.

**Confirmed real and fixed in this pass:**

- **Finding 6 (MEDIUM, missing normal-attribute detection) — confirmed,
  and probably underrated.** Vulkan's `VkRenderPassManager::GetVertexFormat`
  (`vk_renderpass.cpp:122-128`) sets a 2-bit `UseVertexData` flag (bit 0 =
  `VATTR_COLOR`, bit 1 = `VATTR_NORMAL`) that the shared `main.vp` vertex
  shader branches on independently. `MtVertexBuffer` had no `HasNormal()` at
  all, so Metal's `ApplyStreamData()` only ever set bit 0 — every Metal draw
  took the fallback-normal shader path regardless of whether the vertex
  buffer had real per-vertex normals, which is a plausible source of visibly
  wrong lighting on any such geometry, not just a structural nitpick.
  **Fixed**: added `HasNormal()`/`mHasNormal` to `MtVertexBuffer`
  (`mt_hwbuffer.h`/`.cpp`, mirrors the existing `HasColor()` pattern,
  detected in `SetFormat()`), and `ApplyStreamData()` now sets bit 1 from it
  (`mt_renderstate.cpp`). Build verified. **Not yet visually re-verified in
  game** — check lighting on models/geometry with per-vertex normals for a
  visible change.
- **Finding 7 (LOW, dead `mPassDescriptor` member) — confirmed.** Declared,
  destructor guarded a release on it, but never assigned anywhere (the real
  descriptor is `BeginRenderPass()`'s local `pRPD`, see Finding 1 above) —
  harmless (guard never fired) but dead. **Fixed**: removed the member and
  the now-empty destructor body's guard, `mt_renderstate.h`/`.cpp`.
- **Finding 4 (HIGH, `ShadowMap` null deref) — real inconsistency, not a
  live crash today.** `mt_renderstate.cpp:1434`'s color-clear-color check
  dereferenced `fb->GetBuffers()->ShadowMap->GetTexture()` without the null
  guard the depth-attachment check a few lines down already uses
  (`ShadowMap.get()`, `mt_renderstate.cpp:1474`). Traced `ShadowMap`'s
  lifecycle (`mt_renderbuffers.cpp`'s `CreateScene()`): allocated
  synchronously and unconditionally before any `BeginRenderPass()` could
  run, never reset afterward — so this can't actually crash in the current
  code paths, downgrading it from Gemini's HIGH. **Fixed anyway** (cheap,
  matches the existing guarded pattern) rather than left as a landmine for
  whenever `ShadowMap`'s lifecycle changes.
- **Findings 11, 12** — spot-checked, confirmed accurate as stated.
  Finding 11 (`EnableMultisampling`/`EnableLineSmooth` are empty no-op
  bodies, `mt_renderstate.cpp:628-630`) is a real, if low-severity,
  parity gap — not fixed here, since implementing real MSAA/line-smoothing
  state is a feature-sized task, not a quick correctness fix; left as a
  documented gap. Finding 12 (`ApplyDepthBias`'s `128.0f` Reverse-Z scale
  constant is undocumented) is an honestly-hedged observation, not a
  demonstrated bug — left alone.
- **Finding 13** (`Clear()`/`EndRenderPass()` interaction can theoretically
  drop a pending clear) and the four Info notes were not independently
  verified — treat as unconfirmed until checked the same way as the above,
  not as accepted findings.

**Takeaway**: 6 of the 13 distinct numbered findings were wrong, concentrated
in the highest-severity claims (both CRITICALs, one of two HIGHs) — high
enough to mean *any* finding from an independent-AI audit needs the same
direct-code verification before acting on it, not just the ones that sound
surprising. The audit still produced one genuinely valuable, previously-
undiscovered bug (Finding 6) plus one small confirmed cleanup (Finding 7),
so the exercise was worth doing — just not worth trusting unverified.
`FINDINGS.md` updated in place with a verification-status line per finding
rather than left as originally written.

**Round 2 (2026-07-22, same day) — audit thread closed.** Sent
`GEMINI_AUDIT_HANDOFF_ROUND2.md`, asking Gemini to peer-review the six
FALSE verdicts above rather than defer to them, finish Finding 13 and the
four Info notes, sanity-check the Findings 4/6/7 diffs, and expand into
unaudited territory if it had room. Result: **agreed with all six FALSE
verdicts** (explicitly re-derived each independently — the missing
`PatchFragmentShader` call site, the missing semaphore wait, the Cocoa
autorelease-ownership argument, the same-queue command-buffer ordering
guarantee, `applyExponent` not being dead, and the Vulkan `ClearScreen`
parity claim). **Finding 13** verified as real but near-zero-impact: stale
`mClearTargets` can persist across a frame boundary, but the `!filled`
safety net plus accidental correctness at the next frame's clear mean it
doesn't manifest as a visible bug — no fix needed. **All four Info notes**
confirmed with fresh citations. **Fixes for 4/6/7 reviewed and confirmed
correct** (proper null guard, proper `HasNormal()` detection, clean
`mPassDescriptor` removal). **New territory** (`mt_compute.cpp`,
`mt_pipelinestate.cpp`, `mt_streambuffer.cpp`): no additional bugs found.
Per the 2026-07-22 sequencing decision above, this closes the correctness-
audit thread — next up is resuming roadmap item 3 (texture-upload
cold-load test), unless a new correctness report surfaces first — see
below, one did.

## Fifth AO regression: pattern "slides" near geometry while moving, all algorithms (found 2026-07-22)

User report, same session as the Gemini audit: "AO is still shifting when
close to objects/walls... even visible on `gl_ssao_debug 1` for
`mt_compute_ao_algorithm 1`." Distinct from all four prior AO regressions in
this file (undersampling, pinwheel spiral, edge-clamp radius collapse, and
the corner-rounding trade-off found earlier this same session) — narrowed
down by asking two targeted questions before investigating code, same
approach that worked for the corner-rounding report:

1. **Only while moving, not while stationary** — rules out a purely
   non-deterministic/time-seeded cause; points to something tied to camera
   position specifically.
2. **Confirmed present on `mt_compute_ao_algorithm 0` (GTAO) too, "just less
   obviously"** — rules out this being specific to algorithm 1's sample
   pattern (unlike the corner-rounding issue, which GTAO doesn't show at
   all). This is shared-code behavior, just more visible on algorithm 1.

**Root cause (diagnosed, not yet fixed):** all three kernels seed their
per-pixel rotation/jitter noise via `sampler nearestSampler(mag_filter::nearest,
min_filter::nearest, address::repeat)` sampling a 64x64 dither texture at
`noiseUV = pixelCenter / 64.0`. This is fully deterministic and stable for a
*fixed* screen pixel, but a fixed *world*-space point's screen pixel shifts
continuously as the camera moves — so the dither texel (and thus the entire
noise seed) it samples changes non-continuously, popping to an uncorrelated
new value rather than drifting smoothly with the camera. `ssao_compute`
(GTAO)'s per-direction `occlusion += max(sampleHorizon - horizon, 0)`
telescoping absorbs a single popped sample without much visible effect (same
structural robustness already documented for the AlchemyAO grain
investigation above); `ssao_compute_alchemy`'s flat independent-sample sum
has no equivalent damping, so the same underlying pop is far more visible
there. This is a real, known limitation of any non-temporal screen-space AO
technique (full elimination needs temporal reprojection using motion
vectors — a much bigger feature, no motion-vector infrastructure exists in
this renderer today, not scoped into this investigation).

**Mitigation applied 2026-07-22**: switched the dither-texture sampler from
`nearest` to `linear` filtering, in all three kernels (`ssao_compute`,
`ssao_compute_alchemy`, `ssao_compute_mip`), both `mt_ao.metal` and the
`mt_ao.cpp` fallback — renamed the local `nearestSampler` variable to
`ditherSampler` at each of the three declarations since it's no longer
nearest-filtered (the *other*, differently-configured `nearestSampler`
declarations in `bilateral_blur`/`ao_upsample_fullres`/`ao_atrous_fullres`/
`ssao_combine_fs`, which use `address::clamp_to_edge` for depth/normal/AO
reads, are untouched — confirmed via grep that the repeat-mode
`nearestSampler` was used *only* for the dither/noise lookup in each of the
three sample kernels before changing it). This doesn't eliminate the
sliding (the noise is still screen-locked, not temporally reprojected) but
turns the abrupt per-texel pop into a smooth interpolated drift as the
camera moves. Build verified, `tools/check_shader_parity.py` still reports
MATCH for all kernels, binary pipeline cache cleared pre-emptively (real
shader-source change). **Re-tested in-game 2026-07-22: no improvement.**
User confirmed the sliding is unchanged with the linear-filtered dither
sampler. Consistent with how this mitigation was scoped from the start —
it only smooths the per-texel noise-seed pop into a gradual drift, it
never addressed the actual mechanism (no temporal memory at all, so any
noise-independent component of the per-pixel estimate still recomputes
from scratch every frame with no continuity). Cheap mitigation exhausted;
proceeding to the full camera-only temporal-reprojection design, which
already exists and was reviewed
(`/Users/johncurley/.claude/plans/floating-cooking-ripple.md`, referenced
here since it's outside the repo) — phased (baseline → plumbing → gating →
reprojection math → disocclusion/teleport rejection → tuning), scoped to
camera motion only (confirmed via tracing `hw_drawinfo.cpp::DrawScene()`
that actors never participate in the AO pass at all — `RenderTranslucent()`
runs after `AmbientOccludeScene()` — so the only residual limitation is
moving opaque sector geometry like doors/lifts, which the plan's
depth-based disocclusion test substantially self-mitigates). Not something
to build unless the cheap mitigation here turns out insufficient.

**Plan cross-checked via an independent "Ultraplan" refinement pass, then
reconciled back in** (same discipline as the earlier Gemini audit — verify
a second AI's specific technical claims against the actual code before
trusting them). One claim was flatly wrong: it asserted
`tools/check_shader_parity.py` doesn't exist and recommended manual
diffing instead — confirmed false by just running it (the script predates
this session, it's untracked/uncommitted in git, likely why a remote
session reading from a git clone missed it). Its genuinely useful,
independently-verified additions were merged into the plan: the shared
`AmbientOccludeScene(float m5)` virtual (`v_video.h:266`) spans all three
backend overrides (GL/Vulkan/Metal), so threading a new `isMainView`
parameter through touches `gl_framebuffer.h`/`.cpp` and
`vk_renderdevice.h`/`.cpp` too, not just Metal; and the combine shader's
debug-mode chain currently ends in a catch-all `else` (`mt_ao.metal:1011`,
line numbers shift as this feature lands more code) that any value above
10 falls into, needing conversion to an explicit branch before adding new
debug modes in Phase 5. Full detail and the reconciled plan:
`/Users/johncurley/.claude/plans/floating-cooking-ripple.md`.

**Phase 1 landed 2026-07-22 (plumbing only, no-op blend).** Added to both
`mt_ao.metal` and the `mt_ao.cpp` fallback: `AOTemporalParams` struct, new
`ssao_temporal_accumulate` kernel (currently just samples `currentAO` at
its own UV and writes it straight through — no reprojection, no blending,
no main-view gating yet). Added to `MtAOModule`: `mHistoryTexture[2]`
ping-pong pair (`RG16Float`, same `(visibility, linearDepth)` layout every
sample kernel already writes), `mHistoryIndex`/`mHistoryWidth`/
`mHistoryHeight`/`mHistoryValid`, `EnsureHistoryTextures()` (mirrors
`EnsureTextures`'s guard-and-reallocate idiom exactly, forces
`mHistoryValid = false` on reallocation), `temporalAccumulatePSO`. Wired
into `Execute()` immediately after the blur ping-pong loop and before the
fullres-cleanup branch (per the plan's insertion-point reasoning — one
barrier, reassigns `mLowresResultTexture` to the freshly-written history
slot so both the fullres-upsample path and the direct-to-`Combine` path
pick it up transparently); `mHistoryIndex` flips in `Render()` right after
the `Execute()` call returns. Build verified (full rebuild, native Metal
shader library recompiled), `tools/check_shader_parity.py` reports MATCH
for `ssao_temporal_accumulate` alongside all existing kernels, binary
pipeline cache cleared pre-emptively. **Not yet visually re-verified in
game** — per the plan's Phase 1 check, output should be pixel-identical to
before this change (the kernel is a straight pass-through); confirming
that is the next step before Phase 2 (main-view gating).

**Phase 1 visually confirmed 2026-07-22**: user reports output looks the
same as before — matches the expected pass-through check. Confirmed the
fix applies to all AO algorithms uniformly (0, 1, 2), not just algorithm
1: the temporal-accumulate kernel is dispatched unconditionally in
`Execute()` on whatever `mLowresResultTexture` the active algorithm
produced, with no algorithm-specific gating — intentional, since the
sliding root cause is shared code (the dither-texture sampling), confirmed
present on both algorithms 0 and 1 by the user, not something specific to
one algorithm's sample pattern.

**Phase 2 landed 2026-07-22 (main-view gating).** Threading `isMainView`
turned out to touch more than the plan's merged citation implied — reading
the actual call chain directly (rather than trusting either prior plan's
citations) found `AmbientOccludeScene` isn't just one virtual with three
overrides, each of those three *also* forwards to a second inner class
(`OpenGLFrameBuffer` → `FGLRenderer`, `VulkanRenderDevice` → `VkPostprocess`,
`MetalRenderDevice` → `MtPostprocess`) — confirmed via grep that the real
single call site is `hw_drawinfo.cpp:1073`
(`screen->AmbientOccludeScene(...)`). Only the Metal path needs
`isMainView` to actually do anything, so: added the parameter (no default
— there's exactly one call site, and virtual-function default arguments
are a known footgun since resolution uses the *static* type at the call
site, not the dynamic one) to `v_video.h`'s base virtual and all three
first-level overrides (`gl_framebuffer.h`/`.cpp`, `vk_renderdevice.h`/
`.cpp`, `mt_renderdevice.h`/`.cpp`); GL and Vulkan's bodies just ignore it
(don't cascade to their inner `FGLRenderer`/`VkPostprocess` calls, which
keep their original single-param signatures unchanged); only
`MetalRenderDevice::AmbientOccludeScene` threads it further into
`MtPostprocess::AmbientOccludeScene` → `MtAOModule::Render()`. Updated the
one real call site to pass `drawmode == DM_MAINVIEW`.

Inside `MtAOModule`: `Render()` and `Execute()` both gained an
`isMainView`/`useHistory` parameter. `EnsureHistoryTextures()`, the
post-`Execute()` history-index flip, and the new `mPrevMainViewpoint`
snapshot (added `HWViewpointUniforms mPrevMainViewpoint` +
`mHasPrevMainViewpoint` members, `#include "hwrenderer/data/hw_viewpointuniforms.h"`
added to `mt_ao.h`) are all now gated on `isMainView` in `Render()`;
`Execute()`'s temporal-accumulate dispatch block is gated on the
`useHistory` parameter it receives — portal/skybox `AmbientOccludeScene`
calls now bypass history allocation, dispatch, and the snapshot entirely,
exactly as the plan requires (a portal's AO must never overwrite next
frame's main-view history). The snapshot itself captures
`fb->mLastSceneViewpoint` at the end of a main-view `Render()` call, which
is provably correct at that instant (main view's `SetViewpoint` runs
immediately before this call, nothing else has run yet this frame) — it's
not consumed by any reprojection math yet, that's Phase 3.

Build verified: full clean rebuild (this change touches `v_video.h`, a
widely-included header, so it cascaded broadly across the engine, GL and
Vulkan backends included) succeeds with no errors. No `.metal` files
touched this phase, so no pipeline-cache clear needed;
`tools/check_shader_parity.py` still reports all-MATCH regardless. **Not
yet visually re-verified in-game** — per the plan's Phase 2 check: a
portal-containing map with `gl_ssao_portals 1` should show no history
bleed into/from portal views, and `gl_ssao_portals 0` should show no
regression. Since the temporal-accumulate kernel is still the Phase 1
no-op pass-through, this phase's own behavior should also still be
visually identical to before — the portal test is really confirming the
gating *doesn't crash or misbehave*, not a visual change to look for yet.

**Phase 2 confirmed 2026-07-22**: user tested main-view AO on both
algorithms (stable, matches Phase 1) — sufficient signal to proceed without
a portal-specific test (no portal-heavy map readily available); the gating
logic itself is simple enough that the main-view path exercising it every
frame without incident is good evidence it's not silently broken.

**Phase 3 landed 2026-07-22 (real reprojection math).** Added two new
cvars (`mt_postprocess.cpp`): `mt_compute_ao_temporal` (bool, default
true, master on/off — false makes `Execute()` skip the temporal dispatch
entirely rather than run it with a forced-zero weight, so cost and
behavior exactly match pre-temporal AO) and `mt_compute_ao_temporal_blend`
(float, default `0.6`, clamped `[0, 0.98]` — capped below 1.0 so history
can never fully lock out new-frame contribution).

`AOTemporalParams` moved from a function-local struct inside `Execute()`
to a proper member of `MtAOModule` (`mt_ao.h`, alongside `SSAOParams`) so
`Render()` can build it and pass it through — mirrors how `SSAOParams`
itself is already built in `Render()` and threaded into `Execute()`.
`Render()` now computes the reprojection matrix when
`useHistory && mHistoryValid && mHasPrevMainViewpoint`:
`invCurrentView = inverse(fb->mLastSceneViewpoint.mViewMatrix)`, then
`reprojMatrix = prevProj; reprojMatrix *= prevView; reprojMatrix *=
invCurrentView` using `VSMatrix::multMatrix`'s in-place
post-multiply (confirmed by reading `matrix.cpp`'s actual implementation
rather than assuming — classic `glMultMatrix` semantics, `A.multMatrix(B)`
→ `A = A*B`) — this single matrix takes a current-frame view-space
position straight to previous-frame clip space in one multiply.

Kernel-side (`ssao_temporal_accumulate`, both `mt_ao.metal` and the
`mt_ao.cpp` fallback): now takes a second buffer, `constant SSAOParams
&sceneParams [[buffer(1)]]`, purely so it can reuse the existing
`FetchViewPos` helper unchanged rather than duplicating the
view-reconstruction math — `Execute()` binds the same `params` it already
has for this. Reprojects `currentViewPos` through `reprojMatrix`, rejects
if `prevClip.w <= 0` (behind previous camera) or the resulting UV falls
outside `[0,1]` (panned off-history), and otherwise blends
`historyIn`-at-the-reprojected-UV against the current sample with
`historyWeight = blendFactor`. One easy-to-get-wrong detail caught by
checking the file's own existing (dead) `ReconstructViewPos` first rather
than guessing: converting clip-space NDC back to UV needs the same Y flip
this file's live kernels already use going the other direction (`ndc.y =
(1.0 - uv.y) * 2.0 - 1.0`) — used `prevUV.y = 1.0 - (prevNdc.y * 0.5 +
0.5)`, not a naive `prevNdc * 0.5 + 0.5`, or reprojected samples would land
at the mirrored row. `AOTemporalParams.reprojMatrix` is `float4x4` in both
Metal-language copies (native `.metal` and the embedded MSL string in
`mt_ao.cpp` — that string is Metal shader source, not C++, easy to forget)
but stays `float reprojMatrix[16]` in the real C++ struct in `mt_ao.h`,
mirroring the existing `SSAOParams.invProj` precedent exactly (same 64-byte
layout, `setBytes` doesn't care about the type name on either side, only
this codebase's now-two-kernel history of getting struct layout wrong on
purpose to avoid a third).

No disocclusion (depth-mismatch) or teleport rejection yet — ghosting on
moving opaque sector geometry (doors/lifts/platforms; confirmed earlier
this session that actors never participate in the AO pass, so they aren't
a concern here) is expected and accepted at this checkpoint, lands in
Phase 4.

Build verified (native Metal shader library recompiled),
`tools/check_shader_parity.py` reports MATCH for `ssao_temporal_accumulate`
including its new second buffer parameter, binary pipeline cache cleared
pre-emptively (real shader-source change). **Not yet visually re-verified
in-game** — per the plan's Phase 3 check: standing still should look
unchanged from the Phase 1/2 baseline; walking toward a wall should show
the AO pattern tracking the same world-space texels instead of popping,
though not yet perfectly (no denoising of reprojection error yet, and
moving opaque geometry will ghost until Phase 4).

**Regression found and fixed 2026-07-22: `fb->mLastSceneViewpoint` is not
reliably the main view's `mViewMatrix` by the time AO's temporal code reads
it.** User reported the sliding was completely unchanged after Phase 3,
even at `mt_compute_ao_temporal_blend 0.95` (which should be impossible if
reprojection were engaging at all — at that weight, any real reprojection,
even imperfect, would show obvious ghosting). Root-caused via a chain of
targeted `mt_debug`-gated instrumentation (added and removed over several
iterations — cheapest diagnostic first: confirmed all CPU-side gating flags
were `1`/correct, ruling out the gating logic added in Phase 2; then a
direct sanity check — transforming the camera-space origin through
`invCurrentView` should recover the camera's world position, comparable
against the already-known `mCameraPos` with no GPU readback needed — showed
it always recovered exactly `(0,0,0)`; then printing the raw matrix showed
real, camera-orientation-matching rotation but an exactly-zero translation
column; then bisecting the call chain from the `hw_drawinfo.cpp` call site
down through `MetalRenderDevice`/`MtPostprocess`/`MtAOModule::Render`
narrowed the corruption to somewhere before `AmbientOccludeScene` even
begins; finally, instrumenting the single write site,
`HWViewpointBuffer::SetViewpoint` (confirmed via grep to be the *only*
place `fb->mLastSceneViewpoint` is ever assigned), logging every call's
translation in sequence revealed three `SetViewpoint` calls between the
main view's own `SetupView()` and the AO call: the main view (correct,
matches the call-site value), a wildly different position (almost
certainly the sky/skybox camera — `portalState.RenderFirstSkyPortal()` runs
*before* `RenderScene()`/`AmbientOccludeScene()` in `DrawScene()`), and a
final call left at exactly zero translation with the sky camera's rotation
— and nothing restores the main viewpoint afterward before AO reads it.
This was invisible until Phase 3 specifically because every AO code path
before it only ever consumed `mProjectionMatrix` (FOV/aspect-based,
unaffected by which camera's translation is active) and the analytic
`uvToViewA/B` constants — never `mViewMatrix`.

**Fixed** by not trusting the shared, mutable `fb->mLastSceneViewpoint` for
this purpose at all: threaded a new `const HWViewpointUniforms*
currentViewpoint` parameter onto the shared `AmbientOccludeScene(float m5,
bool isMainView)` virtual (`v_video.h`, forward-declares
`HWViewpointUniforms` — the struct itself lives in
`hwrenderer/data/hw_viewpointuniforms.h`, not pulled into this
foundational header) and all three backend overrides
(`gl_framebuffer.h`/`.cpp`, `vk_renderdevice.h`/`.cpp` ignore it exactly
like `isMainView`; only Metal's chain — `mt_renderdevice.h`/`.cpp` →
`mt_postprocess.h`/`.cpp` → `MtAOModule::Render()` — threads it through and
uses it). The single real call site (`hw_drawinfo.cpp`'s `DrawScene()`) now
passes `&VPUniforms` directly — the same local object already proven
correct throughout this investigation, since it's the caller's own,
never-touched-by-sky-rendering copy. `MtAOModule::Render()`'s reprojection-
matrix build and the `mPrevMainViewpoint` snapshot both now read from
`currentViewpoint` instead of `fb->mLastSceneViewpoint`; the existing
`invProj`/`zNear`/`zFar` code paths are untouched, since those were never
actually broken (proven by AO working correctly through four prior
sessions before this). All temporary `mt_debug`-gated diagnostic prints
added during this investigation were removed once the fix landed — this
note is the permanent record instead. Build verified (full rebuild,
`v_video.h` is a widely-included header so this cascaded broadly across
GL/Vulkan/Metal/hwrenderer).

**Second, independent bug found immediately after: `fb->mLastSceneViewpoint`
fix alone did NOT change the visible sliding at all** ("exactly the same,
no perceptible difference" — a strong signal, since even an imperfect
reprojection at `mt_compute_ao_temporal_blend 0.95` should show *some*
visible effect, e.g. ghosting, if it were engaging). Re-verified the fix
itself first rather than assuming it was insufficient: re-added the
camera-space-origin recovery check (transform `(0,0,0)` through the now-
corrected `invCurrentView`, compare to `currentViewpoint->mCameraPos`) —
matched exactly, every frame, confirming the translation fix was genuinely
correct. But that check is structurally blind to a Z-sign error: rotation
applied to the origin is always the origin regardless of whether rotation
(or axis convention) is right, so it only ever validated the translation
component. Extended the check to a non-origin point — `(0, 0, 500)` in
`FetchViewPos`'s convention ("+Z = distance in front of camera", since it's
built directly from a linear depth value, always positive) — pushed through
the *full* `reprojMatrix` (not just `invCurrentView`, so it also exercises
`mPrevMainViewpoint`'s projection/view matrices and rotation). Result:
`prevClip.w` came back negative, magnitude almost exactly matching the
input Z (e.g. `w=-496.62` for `z=+500`) — meaning the engine's actual
`mProjectionMatrix`/`mViewMatrix` use the standard "-Z = in front of
camera" convention, the opposite of what `FetchViewPos` produces. Verified
the fix before applying it for real: re-ran the same CPU-side check with Z
negated going in, got `prevClip.w ≈ +506` (positive, correct sign) and
`prevUV ≈ (0.51, 0.50)` — dead center on screen with small, sensible
frame-to-frame drift matching normal walking motion, exactly as expected
for a point straight ahead of a slowly-moving camera.

**Fixed** in both `mt_ao.metal` and the `mt_ao.cpp` fallback's
`ssao_temporal_accumulate` kernel: `float4(currentViewPos.xy,
-currentViewPos.z, 1.0)` instead of `float4(currentViewPos, 1.0)` when
building `prevClip` — negates Z to convert from `FetchViewPos`'s convention
into the one `reprojMatrix` actually expects. All CPU-side diagnostic
instrumentation from this second investigation removed now that the fix is
applied and pre-verified numerically. Build verified (native Metal shader
library recompiled), `tools/check_shader_parity.py` reports MATCH,
binary pipeline cache cleared (real shader-source change this time).
**Not yet visually re-verified in-game** — this is the second of two
independent bugs found stacked on top of each other in this same feature;
between the two, this is the one that should actually make Phase 3's
"AO tracks world-space texels while moving" behavior visible for the first
time.

**Confirmed 2026-07-22/23: the Z-sign fix worked (real progress — reprojection
now visibly does something), but revealed the expected Phase 3 limitation
as a concrete artifact.** User report: "concentric rings pulsing," only
while moving (ruled out a standing-still feedback-loop hypothesis first via
a clarifying question, then the user self-corrected). Screenshot (a
corridor viewed head-on) showed the "rings" were actually repeated, offset
echoes of the corridor's corners/edges — a textbook ghosting artifact from
blending history at a flat weight with no disocclusion test, exactly what
the plan's Phase 3 checkpoint already flagged as expected ("ghosting on
moving geometry is expected and accepted at this checkpoint, lands in
Phase 4") — except it turned out to apply more broadly than the plan's
original framing ("moving opaque sector geometry like doors/lifts") since
*any* geometry, including entirely static walls, produces a continuously
changing screen-space mapping as the camera moves through a perspective
projection — the reprojected UV can be correct (right formula, right
matrix) while still no longer corresponding to the same surface point
frame-to-frame, which only a per-pixel depth check (not just an
in-bounds/behind-camera check) can catch.

**Fixed (Phase 4 — disocclusion + teleport rejection, landed early since
the artifact was live and reproducible).** Added depth-mismatch rejection
to `ssao_temporal_accumulate` (both `mt_ao.metal` and the `.cpp` fallback):
`prevClip.w` is already, for free, the distance from the *previous* camera
to the reprojected point (confirmed during the Z-sign investigation, no
separate view-only matrix needed) — compared against `historyIn`'s stored
`.g` (the depth actually recorded there last frame); a relative mismatch
beyond `depthRejectThreshold` rejects the sample, falling back to the
current frame's own estimate. New cvar
`mt_compute_ao_temporal_depth_reject` (default `0.1`, i.e. 10%) makes this
tunable without a rebuild. Also added a CPU-side camera-discontinuity
(teleport) guard in `Render()`, gating the whole frame's `historyValid` on
camera-position delta since the last main-view frame vs new cvar
`mt_compute_ao_temporal_teleport_dist` (default `64` map units,
~one player radius) — position-only for now; the rotation-delta half of
the original design (`mt_compute_ao_temporal_teleport_angle`, still defined
as a cvar but not yet wired up) needs extracting a forward vector from the
view matrix's rotation block, deliberately deferred rather than risk a
*third* unverified sign/convention bug in this same code after the two
already found and fixed this session — the per-pixel depth-mismatch test
already provides partial protection against fast turns in the meantime
(a violent swing sends most reprojected UVs out of bounds or onto
mismatched depths anyway). Build verified, `check_shader_parity.py`
MATCH, pipeline cache cleared.

**Confirmed 2026-07-23: disocclusion rejection helped (the general
concentric-echo pattern is gone) but revealed a third, narrower bug.**
User report: "visible but only vertically with the trails" — clarified via
a follow-up question that this means the smearing itself is oriented
vertically on screen (off door frames/corners), not that it's only
triggered by vertical camera movement. Rather than guess a third time,
derived the likely cause analytically first: the kernel's `prevUV.y`
included a flip (`1.0 - (ndc.y*0.5+0.5)`) borrowed from the dead,
never-executed `ReconstructViewPos` helper when the Z-sign bug was fixed —
but `FetchViewPos` (the function actually used, proven live and correct)
has `viewY = invFocalLenY * depth * (2*uv.y - 1)`: increasing `uv.y` means
increasing `viewY`, no flip. Confirmed via a CPU-side test (same technique
as the Z-sign fix — a point offset only in Y, pushed through the full
`reprojMatrix`, printed alongside both the flipped and non-flipped
candidate `prevUV.y`) that the flip was inconsistent with this live
relationship. **Fixed**: removed the Y flip in both `mt_ao.metal` and the
`mt_ao.cpp` fallback — `prevUV = prevNdc * 0.5 + 0.5`, matching X's
treatment exactly (previously `float2(prevNdc.x*0.5+0.5, 1.0-(prevNdc.y*0.5+0.5))`).
The wrong flip mirrored the reprojected Y coordinate through the screen's
vertical center — small near the center, growing with vertical distance
from it, which matches "trails oriented vertically" exactly (worse toward
the top/bottom of the frame). This was very likely present since the
temporal feature's Z-sign fix landed, but only became visible as a
*distinct, isolated* symptom once disocclusion rejection cleared away the
larger concentric-echo pattern it had been tangled up with. Build
verified, `check_shader_parity.py` MATCH, pipeline cache cleared. **Not
yet visually re-verified in-game** — re-run the corridor repro; if
resolved, this closes out the temporal-reprojection feature for now
(Phase 5 — debug visualization, tuning, benchmarking — is tuning/polish,
not correctness, lower urgency); if echoes persist, check whether
`depthRejectThreshold` needs tightening before assuming a fourth bug.

**Regression 2026-07-23: Y-flip fix retested, both symptoms returned.**
After the Y-flip fix above, user re-tested and reported both the vertical
trails AND the original diagonal-angle distortion still present — i.e. the
Y-flip fix did not close this out as hoped. Three bugs found and "fixed" in
a row (`mLastSceneViewpoint` corruption, Z-sign, Y-flip), each verified only
via single-point CPU-side `Printf` spot-checks (a fixed test point pushed
through the reprojection matrix, comparing before/after numbers) — that
method has now twice given false confidence: it passed the specific point
tested but missed something that clearly varies across the screen, since
the visible symptoms are spatially structured (vertical/diagonal patterns)
in a way a single point can't reveal. Decided against a fourth blind fix.
Instead, per explicit user direction ("keep debugging, but properly this
time"): (1) flipped `mt_compute_ao_temporal`'s default to `false` — the
feature no longer ships on by default until this is actually resolved,
`historyValid` is forced 0 every frame same as before the feature existed,
zero behavior change for anyone not manually re-enabling it; (2) built a
real full-screen visual debug mode instead of more spot-checks.

**New: `mt_compute_ao_temporal_debug` cvar (Int, 0-3, default 0).** Viewed
via the existing `gl_ssao_debug 1` grayscale-attenuation display, this
makes `ssao_temporal_accumulate` write a diagnostic value into the AO
channel instead of the normal blended result, for the *entire frame* at
once rather than one CPU-printed point:
- `1` = `prevUV.x` as grayscale. For a stationary or slowly panning camera
  this should look close to a plain left-to-right ramp (mirroring the
  screen's own horizontal gradient) — any banding, mirroring, or sudden
  discontinuity is a reprojection-math bug, visible immediately across the
  whole image instead of at one sampled point.
- `2` = `prevUV.y`, same idea, top-to-bottom. This is the value the Y-flip
  bug directly touched — the most likely place to still show something
  wrong given the regression report.
- `3` = `historyWeight` normalized against `blendFactor` (so a fully
  accepted sample reads white, fully rejected reads black) — makes the
  disocclusion/depth-mismatch rejection pattern visible: expect black
  bands right at depth discontinuities (corners, door edges) and white
  across flat continuous surfaces. Uniform black or uniform white
  everywhere would indicate the rejection test itself is broken (always
  rejecting or never rejecting), independent of the UV math.

Implementation: `prevUV` is now tracked in a variable declared *outside*
the nested `historyValid`/in-bounds conditional blocks (previously scoped
only inside the innermost `if`), defaulting to a `(-1,-1)` sentinel so
debug output can tell "never reached a candidate UV" (rendered as black)
apart from "reached UV `(0,0)`" (also black, but for a different reason —
this distinction matters when reading mode 1/2 output near screen edges).
Added `int debugMode` as the last field of `AOTemporalParams` in both
`mt_ao.h` (real C++ struct) and the two Metal-language copies
(`mt_ao.metal`, `mt_ao.cpp`'s `SSAO_COMPUTE_SOURCE` fallback — parity
re-verified via `tools/check_shader_parity.py`, all MATCH). `Render()`
clamps and forwards `mt_compute_ao_temporal_debug` into
`temporalParams.debugMode` every frame. The debug branch in the kernel
early-returns before the normal blend-and-write, deliberately overwriting
that frame's history with debug data — debug modes aren't meant to run
concurrently with normal operation, and re-enabling `debugMode 0` on a
later frame re-establishes normal history from the current frame's
(non-debug) AO estimate, same as a resize/invalidation would. Also removed
the now-superseded single-point Y=100 `Printf` diagnostic block from
`Render()` (added to verify the Y-flip fix, superseded by this tool). Build
verified, `check_shader_parity.py` MATCH, pipeline cache cleared.

**Abandoned 2026-07-23: feature fully reverted, not just disabled.**
Testing the debug-visualization build surfaced a fourth, more fundamental
symptom before the diagnostic images were even needed: the trail/ghosting
artifacts were visible **even with the camera standing still**, which a
correct camera-only reprojection cannot produce (a stationary camera makes
`reprojMatrix` collapse to identity, so blending against history should be
visually inert). That ruled out "one more sign/convention bug" as the
explanation and pointed at something structurally wrong in the ping-pong/
feedback mechanics instead — not worth chasing further for a feature meant
to fix a minor cosmetic AO pop while moving near geometry. Decision (user
call): full clean revert rather than leave the feature dormant behind its
now-`false` cvar default.

Removed entirely: the `AOTemporalParams` struct and `ssao_temporal_accumulate`
kernel (both `mt_ao.metal` and the `mt_ao.cpp` fallback), the history
ping-pong textures/`EnsureHistoryTextures`/`temporalAccumulatePSO`/
`mPrevMainViewpoint` state in `MtAOModule`, all five `mt_compute_ao_temporal*`
cvars, and the `isMainView`/`currentViewpoint` plumbing through
`AmbientOccludeScene` — restored to its original single-parameter signature
in `v_video.h` and all three backend overrides (GL/Vulkan/Metal), since
nothing else used the extra parameters. `hw_drawinfo.cpp`'s call site is
back to `screen->AmbientOccludeScene(VPUniforms.mProjectionMatrix.get()[5])`.
The unrelated multi-algorithm AO work from earlier in this investigation
(GTAO/AlchemyAO/depth-mip selection via `mt_compute_ao_algorithm`, the
dither-sampler linear-filter mitigation, the AlchemyAO blur-pass fix) was
kept — none of it depends on or was entangled with the temporal feature.
Build verified, `check_shader_parity.py` all MATCH (no `ssao_temporal_accumulate`
entry), pipeline cache cleared.

The original bug this feature set out to fix — AO pattern sliding near
geometry while moving, root-caused to nearest-filtered dither-texture
sampling with no per-frame continuity — remains unfixed. The dither
linear-filter mitigation (still in place, tested no-regression) softens the
pop into a drift but doesn't add real temporal stability. Any future
attempt at true temporal reprojection should start from scratch rather than
resurrecting this code, and should get a working full-screen visual debug
mode (the `mt_compute_ao_temporal_debug` approach was sound, just built too
late in the investigation) before the first bug is even suspected, not
after three rounds of single-point CPU spot-checks.

## Sixth AO attempt: world-locked noise (2026-07-24) — implemented, awaiting in-game verification

After the temporal-reprojection revert above, re-scoped the fix around a
stateless, single-frame approach instead: derive the per-pixel jitter/
rotation noise from the sample's **world-space position** instead of its
screen pixel coordinate. A static world point then always draws the same
jitter regardless of camera position/orientation, which directly kills the
original "AO slides near geometry while moving" bug with **zero cross-frame
state** — no history buffers, no ping-pong, no disocclusion/teleport
rejection, none of the bug classes that sank the temporal attempt. Full
design discussion (including an independent Plan-agent review of the
approach against the live code) is in the approved plan; this section
records what actually landed.

**Plumbing (re-added, scoped down from the reverted temporal version).**
`AmbientOccludeScene` grew back a `const HWViewpointUniforms*
currentViewpoint` parameter — `v_video.h` base virtual, GL/Vulkan overrides
(both ignore it), Metal's `MetalRenderDevice`/`MtPostprocess` chain down to
`MtAOModule::Render()`. Unlike the temporal version, there is no
`isMainView`, no previous-frame snapshot, no history-scoping logic at all —
every `AmbientOccludeScene` call (main view or portal) just passes its own
current `&VPUniforms` straight through from `hw_drawinfo.cpp`'s
`DrawScene()`, unconditionally, since there's nothing to protect across
frames or views. This plumbing was never the buggy part of the reverted
attempt (confirmed by review), so it's considered low-risk.

**CPU-side `viewToWorld` (repurposes dead code instead of growing the
struct).** Grepped and confirmed `SSAOParams.invProj` had exactly one
consumer, `ReconstructViewPos`, which itself had **zero live call sites**
(every sample kernel uses `FetchViewPos`) — it was pure dead weight, paying
for a full CPU-side general 4x4 matrix inverse every frame for a value
nothing read. Renamed the field to `viewToWorld` (same `float[16]`/
`float4x4` layout convention, both `mt_ao.h` and the two Metal-language
copies) and deleted `ReconstructViewPos` outright. `MtAOModule::Render()`
now builds `viewToWorld` as an affine view-space→world-space transform:
copy `currentViewpoint->mViewMatrix`, call its existing `.transpose()`
(column-major, confirmed via `transpose()`'s own index shuffle in
`matrix.cpp`) to get the rotation block's transpose — the inverse of a pure
rotation, cheaper and more numerically robust than a general 4x4 inverse,
which can fail on degenerate input — then hand-assemble a 16-float affine
matrix: transposed-rotation in the upper-left 3x3, `currentViewpoint->
mCameraPos.X/Y/Z` (already true world-space position, engine's axis swap
already baked in at its source) in the translation column, `(0,0,0,1)`
bottom row.

**GPU-side reconstruction, reusing a bug already found and fixed once.**
New `WorldPosFromViewPos` helper: `FetchViewPos` returns "+Z = distance in
front of camera" (always positive), but the real view matrix (and thus
`viewToWorld`) uses the standard OpenGL "-Z in front" convention — negating
Z (`float3(centerViewPos.xy, -centerViewPos.z)`) converts between them.
This exact negation was already empirically verified during the temporal
attempt's "Z-sign" bug fix (see above) — reused directly rather than
re-derived, so it carries none of that investigation's original risk.
`worldPos = (params.viewToWorld * float4(realViewPos, 1.0)).xyz` — an
ordinary affine transform, no perspective divide.

**Hash-based noise instead of the dither texture.** New `Pcg3d` (Jarzynski
& Olano integer bit-mixing hash) and `WorldNoise` helpers, added once to
`mt_ao.metal` and mirrored in `mt_ao.cpp`'s `SSAO_COMPUTE_SOURCE`.
`WorldNoise` quantizes `worldPos` to an integer grid cell (size controlled
by new cvar `mt_compute_ao_noise_cellsize`, default `12.0`, map units —
untuned starting point, needs empirical adjustment against the old look)
and hashes the cell coordinate, producing `(rotation radians, stepJitter,
directionJitter)` directly — no `atan2`/cos-sin round-trip needed since
there's no texture-storage format forcing the old encoding. Deliberately
**not** a naive `frac(sin(dot(p,K))*C)`-style hash: those lose precision
and band at large coordinate magnitudes, and Doom/UDMF world coordinates
can be in the tens of thousands — an integer hash sidesteps this. All
three sample kernels (`ssao_compute`, `ssao_compute_alchemy`,
`ssao_compute_mip`) were updated identically; `InterleavedGradientNoise`
(also screen-pixel-based, would have reintroduced a residual screen-locked
component into stepJitter/directionJitter if left in) was deleted as dead
code once its three call sites were removed.

**Dither texture and `ditherTexture` kernel parameter deliberately NOT
removed yet.** Sampling was removed but the texture/binding is still
physically present in all three kernels and `Execute()` — removing it means
renumbering every subsequent texture index across 3 kernels × 2 files ×
`Execute()`'s `setTexture` calls, exactly the kind of easy-to-get-subtly-
wrong mechanical change that caused pain in the temporal attempt. Deferred
to its own isolated, easily-revertible cleanup once the new noise is
confirmed correct in-game.

**Visual debug, built alongside the fix (not bolted on after, unlike the
temporal attempt).** New `int debugMode` field on `SSAOParams` (appended at
the end, following `AOBlurParams.applyExponent`'s "moved to end for
alignment safety" convention) plus cvar `mt_compute_ao_worldpos_debug`
(0-3). When nonzero, each sample kernel early-returns
`fract(worldPos.xy|xz|yz / cellSize)` straight to `aoOutput` instead of
running real AO math. **Must be viewed via `gl_ssao_debug 2`, not `1`** —
`gl_ssao_debug < 2` still runs the bilateral-blur pass on top of whatever
the kernel wrote, which would smear the diagnostic; `2` skips blur (`const
bool blurAO = gl_ssao_debug < 2` in `Render()`) and both values map to the
same `attenuation = ssao.x` display branch in the combine shader's existing
debug chain (confirmed by reading `ssao_combine_fs`), so `2` shows the raw
per-pixel kernel output with no interference. A correct world-locked
pattern should look like a stable grid painted onto geometry: it must not
slide when panning past a wall, and must not rotate/shift when turning in
place near it.

**Status: build verified, `check_shader_parity.py` all MATCH, pipeline
cache cleared. Not yet visually verified in-game** — this environment can't
drive the GUI. Next step is for the user to run the full Phase 4 checklist
from the plan: standing still (baseline, should be unchanged), walking
toward/away from a wall (the original repro), turning in place, a
long-corridor distance test (watching for coarsening/moiré near
`fadeEndDistance`, default 500), a ramp/staircase test (watching for
axis-aligned banding — a new artifact class this technique can introduce
that the old screen-space scheme never had, flagged during design review as
worth an explicit look), all three algorithms
(`mt_compute_ao_algorithm` 0/1/2), and a portal-heavy map if available. Use
the debug mode first (`gl_ssao_debug 2` + `mt_compute_ao_worldpos_debug
1/2/3`) to confirm the
world-locked pattern is genuinely stable before judging normal shaded
output, per the lesson from the temporal attempt's repeated false
confidence.

**Bug found via that debug mode, 2026-07-24: `transpose()` is not a valid
inverse for `mViewMatrix`.** User tested `gl_ssao_debug 2` +
`mt_compute_ao_worldpos_debug 1` (screenshot: large flat quad blocks with
sharp edges aligned to wall panels — internally coherent, so the per-frame
math wasn't garbled) and reported the pattern still slides near walls, under
**both turning and walking in place**. Root cause: `viewToWorld` was being
built by `transpose()`-ing `mViewMatrix`'s rotation block, on the assumption
that the block is orthonormal (transpose = inverse for pure rotations). That
assumption was checked against the code, not just asserted, and was wrong —
`HWDrawInfo::SetViewMatrix` (`hw_drawinfo.cpp`) ends with
`mViewMatrix.scale(-mult, planemult, 1)`, where `planemult` is
`Level->info->pixelstretch` (**1.2 by default** — classic Doom's
non-square-pixel aspect-ratio correction, confirmed via
`g_mapinfo.cpp`), plus a possible `-1` mirror flip. A non-uniform scale on
top of the rotation makes the matrix non-orthonormal, so `transpose() !=
inverse()` for it — using transpose anyway produces a reconstruction error
that depends on the current view angle (since the erroneous "inverse"
doesn't cleanly cancel the true forward transform), which is exactly what
"still slides under both turning and walking" looks like: a fixed world
point's *reconstructed* position drifts as the camera rotates, even though
the real world position hasn't moved.

**Fixed**: `MtAOModule::Render()` now builds `viewToWorld` via
`VSMatrix::inverseMatrix()` — the same general 4x4 inverse (via
adjoint/determinant, confirmed in `matrix.cpp`, works for any invertible
matrix, not just orthonormal ones) already used, and already proven
reliable, for the old `invProj` field this one replaced. This also
simplified the CPU-side code: no more manual 3x3-block-plus-translation
assembly, just a straight 16-float copy of the inverse matrix, mirroring the
original `invProj` population code almost exactly. Build verified,
`check_shader_parity.py` all MATCH (GPU-side kernels unchanged — this was a
CPU-only fix), pipeline cache cleared. **Not yet re-verified in-game** —
same Phase 4 checklist as above, starting again with the debug-mode check
before trusting normal shaded output.

## Dead `explicitFlushing` flag removed (2026-07-17)

Renderer-wide Intel-optimization audit (prompted by the AO/bloom work above
maturing enough to widen scope). `MtVersionManager::explicitFlushing`
(`mt_version.h`) was set `true` for all Intel GPUs with the comment "Intel
Broadwell+ needs careful flushing", but `git log -S` showed it was dead from
the exact commit that introduced it (`c6727085a`, the original
"Architecture Tuning" commit) — no later commit ever added a reader.
Checked whether the CPU→GPU sync it gestures at was actually needed and
just implemented elsewhere instead of gated by this flag: yes —
`didModifyRange` calls in `mt_hwbuffer.cpp`/`mt_streambuffer.cpp` already
correctly and unconditionally check `buffer->storageMode() == Managed`
directly, independent of this flag. Also checked `AGENTS.md`'s own
`RecordStall` history (`pso_compile`/`streambuffer`/`semaphore`/
`semaphore_timeout`/`displaylink_timeout`) for any recorded symptom this
might have been meant to address — none found. With no consumer and no
concrete bug on record, implementing a guessed-at "careful flushing"
workaround would have been inventing a fix for an unobserved problem;
removed the flag and its assignment instead. Build verified (`cmake --build
build --target zdoom -j 8` succeeds). If a real Intel command-buffer
backpressure/stall symptom ever surfaces, re-derive the fix from that
symptom (e.g. via `mt_debug`'s stall counters) rather than resurrecting
this flag blind.

## Texture upload instrumentation (added 2026-07-15)

Motivation: with real GPU frame timing now standard practice for this
renderer (see "Real GPU frame timing" above), the next suspected CPU/GPU
race point raised was texture uploads on Intel — same underlying question
as the AO work: is the CPU racing ahead and creating overhead the GPU has
to absorb, this time in the texture streaming path rather than AO. Checked
first rather than assumed: `MtDebugManager::RecordStall`'s doc comment
already listed `"texture_upload"` as a stall type, but grepping the whole
`metal/` tree found it was **never actually called** — only `pso_compile`,
`streambuffer`, `semaphore`, `semaphore_timeout`, and `displaylink_timeout`
are wired up. So there was no real data here at all, same situation AO was
in before `mt_metrics` existed.

Read the actual upload code (`mt_texture.cpp`) before instrumenting:
world-game-texture content upload (as opposed to the UI/startup path, which
does a direct `replaceRegion` into `Managed`/`Shared` storage with no
command buffer) stages into a buffer, blits into a `Private`-storage
texture, generates mips, then commits its **own separate command buffer**
via `MtCommandBufferManager::GetBlitCommandBuffer()` — confirmed (again, by
reading `mt_commandbuffer.cpp`) that this call creates a brand-new command
buffer every time and never calls `waitUntilCompleted()`. So there is **no
literal blocking stall** in this path today — forcing this into
`RecordStall` would have been dishonest. The real, measurable things are:
(1) the CPU cost of the staging/memcpy/blit-encode/commit sequence itself,
and (2) how many of these separate command buffers get submitted per frame
(a burst — e.g. during a level transition — is the same class of
per-submission-overhead cost this session already found dominates AO on
Intel, just in a different subsystem; not yet proven here, only plausible).

Added two independent, complementary signals:

- **`MtMetric::TextureUploadCPU`** (`mt_metrics.h`/`.cpp`): CPU-side timing
  of the world-texture upload block specifically (`mt_texture.cpp`,
  `std::chrono` around the staging-buffer/memcpy/blit/commit sequence),
  recorded via new `MtDebugManager::RecordTextureUpload(float durationMs)`.
  Same rolling-120-frame stats infra as AO/bloom CPU timing; shown in
  `mt_metrics` via `PrintMetricSummary` and in `mt_debug 1`'s live line.
- **`extraCommandBuffers` frame counter** (`MtDebugManager::FrameStats`):
  incremented centrally inside `MtCommandBufferManager::GetBlitCommandBuffer()`
  itself (`mt_commandbuffer.cpp`), not at each call site — this catches
  world-texture uploads, lightmap uploads (`mt_texture.cpp`, a second,
  separate `GetBlitCommandBuffer()` user), and mipmap regeneration
  (`MtTextureManager::GenerateMipmaps`) uniformly, and automatically covers
  any future caller without further changes. Printed in `mt_metrics` (last
  frame's count) and `mt_debug 1`'s live line alongside the stall counter.
  Deliberately a raw per-frame count, not a rolling-average duration metric
  — it's a burst indicator, and averaging it over 120 frames would wash out
  the level-transition spikes it exists to catch.

Scope decision: only the world-texture path is CPU-timed (the highest-
frequency case — one call per regular game texture load); the lightmap
upload and mipmap-regeneration paths are covered by the `extraCommandBuffers`
count but not individually timed yet. Revisit if the count metric shows
they're a meaningful fraction of a level-transition spike.

Build verified (`cmake --build build --target zdoom -j 8` succeeds). CSV
logging (`mt_debug_startlog`) extended with `TextureUploads`,
`TextureUploadCPUms`, `ExtraCommandBuffers` columns, same pattern as the
`FrameGPUms` column added for GPU frame timing.

**First in-game check (2026-07-16/17): negative/inconclusive.** Ran
`mt_debug 1` continuously across ~150 live frames while walking around
(draw count climbed 1943→2320, so real new geometry became visible) —
`extraCommandBuffers` was 0 and `TexUpload` never printed on any single
frame. Not a failure of the instrumentation (it's proven wired correctly —
same code path, same call sites); most likely this playthrough segment
didn't trigger any actual texture content uploads, because the area's
textures were already fully resident from earlier testing sessions in this
same map. **Still need a genuine cold-load test**: a full `map <mapname>`
transition, or noclipping into territory not yet rendered this session, to
actually exercise the upload path and get a real burst reading. If it does,
the next question is the same fork as AO: batch/coalesce uploads into fewer
command buffers, or just confirm it's not big enough to matter and move on
— don't tune before
that data exists.

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

~~**Dead code:** `PatchFragmentShader` (lines 760-827) defined but never called.~~
**Stale as of 2026-07-22**: `MtShaderManager::LoadFragShader` calls
`PatchFragmentShader(fragSource, shadername)` directly (`mt_shader.cpp:931`),
added by commit `728c775de` ("Metal: Stabilized renderer with Reverse-Z,
functional portals/mirrors, and refined shadow mapping") — this note simply
predates that fix and was never removed. Caught when an independent Gemini
audit (see `FINDINGS.md`) cited this exact line as corroborating evidence
for its own "never called" claim, which was otherwise false (Gemini's
search missed the real call site). Left the strikethrough in place rather
than deleting the line outright, as a record of how a stale note can end up
cited as false corroboration later.

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

## 2026-07-10 Session: mt_ao.cpp/mt_ao.metal reconciliation

`shaders/native/mt_ao.metal`'s header comment had it backwards — it claimed
the `.cpp` inline `SSAO_COMPUTE_SOURCE` string was authoritative. Fixed: the
`.metal` file is what's actually compiled into `native_shaders.metallib` and
loaded first; the inline string is the fallback. Reconciled the two so the
inline fallback is now a verbatim copy of the canonical `.metal` kernels
(previously they'd drifted enough that the fallback likely didn't even
compile — see bugs below), rather than maintaining two different tunings.

Per-difference resolution:

- **numSteps/numDirections clamp** (real bug, ported into canonical
  `.metal`): `.metal` hardcoded `numSteps = 4` and clamped
  `numDirections` to `[4,5]`, silently ignoring `mt_compute_ao_steps` /
  `mt_compute_ao_directions`. `.cpp`'s wider, cvar-respecting
  `clamp(params.numSteps, 2, 8)` / `clamp(params.numDirections, 2, 6)` was
  correct and is now what `.metal` uses too.
- **LDS threadgroup caching** (`.cpp`-only, dropped, not ported): looked
  like an optimization but was actually broken — each thread only wrote its
  *own* center pixel into a shared 16x16 array at `tid+4`; the halo cells a
  sample lookup could land on (outside the 8x8 self-written block) were
  never written by anyone in that dispatch, so sample fetches there read
  uninitialized threadgroup memory. Dropped rather than fixed properly
  (would need a full cooperative tile load with halo); compute AO is
  already fast enough (2.6x PP) that this isn't worth the risk right now.
- **bilateral_blur `maxThickness`**: `AOBlurParams.maxThickness` is threaded
  all the way from CPU through the struct but still unread in the `.metal`
  kernel body — left as dead/unused for now. Initially ported `.cpp`'s hard
  thickness-clamp skip (`sampleDepth - depth > maxThickness -> continue`)
  into canonical `.metal`, reasoning it looked like a real improvement. It
  wasn't: reported and confirmed as a regression — fog speckle artifacts
  (previously fixed 2025-06-19) came back. Root cause: `maxThickness`
  defaults to `1.25` world units, tiny relative to typical per-pixel depth
  variation across the 8-tap blur radius, so the hard cutoff excluded most
  neighbor taps in ordinary geometry, collapsing blur toward the unblurred
  center sample in large parts of the image and letting raw per-pixel AO
  noise pass through into combine, which isn't tuned to suppress that much
  raw noise. **Reverted** in both `.metal` and the `.cpp` fallback back to
  the original always-blend continuous depth-weighted falloff (no hard
  exclusion). Lesson: the `.cpp` inline source this came from was dead,
  never-compiled code (see the `applyExponent` field bug below) — never
  visually validated, so "looks like an improvement" isn't sufficient
  evidence on its own; treat anything sourced from the fallback as unproven
  until checked in-game. Also fixed regardless: `.cpp`'s GPU-side
  `AOBlurParams` struct was missing the `applyExponent` field the kernel
  body referenced (compile error) and didn't match the CPU-side struct
  layout in `Execute()`.
- **`ssao_combine_fs` duplicate `sceneAlpha` declaration** (real bug, fixed):
  `.cpp`'s inline fallback declared `float sceneAlpha` twice in the same
  scope — would not have compiled if the fallback path were ever exercised.
  Removed the dead first declaration.
- **Jitter/rotation formula, bias scaling (`bias*0.5` vs `bias`), occlusion
  multiplier (`*1.15` boost vs none), blur tap count (4 vs 8) and falloff
  steepness, `ao_upsample_fullres`/`ao_atrous_fullres` sharpness constants**:
  pure tuning differences, no correctness argument either way — kept
  `.metal`'s existing (already-shipping, already speckle-free) values rather
  than guessing. One data point that came out of checking anyway: `.metal`'s
  rotation decode (`atan2(noise.y, noise.x)`) is actually the mathematically
  correct way to recover an angle from the dither texture's
  Snorm-encoded `(cos,sin)` pair, whereas `.cpp`'s `(noise.x*2-1)*pi` formula
  double-applied a Unorm decode to data that's already Snorm — so `.metal`
  winning by default here also happened to be the objectively correct choice.
  `.metal`'s `stepJitter`/`directionJitter` constants (`0.754877666`,
  `0.569840296`) are the R2 low-discrepancy sequence conjugates (Martin
  Roberts), a more principled 2D dithering base than `.cpp`'s mix of the 1D
  golden ratio and the raw plastic number.
- Build verified: `cmake --build build --target zdoom -j 8` succeeds, both
  the native `.metal` compile and the `.cpp` fallback compile clean.
- User caught the blur thickness-clamp regression (fog speckles back) by
  visual check in-game; reverted per above. Re-verify speckles are gone
  again before further GTAO tuning passes — not yet re-confirmed in-game
  after the revert.

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

## Seventh AO pass: four distinct bugs behind the "moving/distorted AO" reports (2026-07-26) — fixed, verified in-game

Follow-on from the sixth attempt (world-locked noise, above), which had
shipped WIP and was still reported as "not right, moving around when the
player walks." Four *separate* mechanisms turned out to be in play. They
had been treated as one artifact across several prior sessions, which is
why single fixes kept only partly helping.

### 1. Wrong horizontal focal length in the view->world reconstruction

`invFocalLenX/Y` were re-derived as `tanHalfFovy * (sceneWidth /
sceneHeight)`, copying upstream's GLSL SSAO. But the projection is built as
`perspective(fovy, ratio, ...)` in `hw_entrypoint.cpp:169`, where `ratio` is
the *display* aspect (`r_visualAspect`/`vid_aspect`/letterboxing) and fovy
is divided by `fovratio`. Neither equals the raw scene pixel aspect, so the
two disagree off a plain widescreen setup.

For plain occlusion this only skews the sample radius, which is why
upstream never noticed. But the world-locked noise round-trips through
`viewToWorld`: a wrong horizontal focal length scales reconstructed
view-space X by a constant k, and `R^-1` applied to `(k*x, y, -z)` puts the
error along a *camera-relative* axis — so it rotates when turning and
slides when walking, worst at screen edges and near walls where |x| is
largest. Now read straight off `mProjectionMatrix` ([0] and [5],
column-major) in `MtAOModule::Render()`. No assumptions, and it inherits
any future projection change for free.

### 2. Fixed world-space noise cell size (the crawling blotches)

One `WorldNoise` cell is one noise sample. At the fixed 12-unit default a
cell spans dozens of AO pixels up close, so every pixel in it got identical
jitter — correlated error the bilateral blur cannot average away, appearing
as large AO blotches. Because the cell grid is world-anchored and the
camera is not, they crawled diagonally across surfaces while walking. (The
same fixed size goes sub-pixel at distance and aliases instead — the
artifact cut both ways.)

Added `NoiseCellSize()`: scales the cell with view depth so it stays ~1 AO
pixel wide at any distance, power-of-two quantized so the size is constant
across a depth band rather than drifting with every step. New cvar
`mt_compute_ao_noise_pixels` (default 1.0; 0 restores the fixed size).
Confirmed in-game — the floor checkerboard is gone and the raw AO buffer
shows clean per-pixel grain.

### 3. World-cell noise cannot decorrelate grazing surfaces (the mottling)

`NoiseCellSize` picks a cell size from depth, which assumes a pixel's world
footprint is isotropic — true only on surfaces facing the camera. On a
grazing surface (standing close to any wall/floor/ledge) the footprint is
stretched enormously along the grazing direction, so a world cube projects
to a long thin run of screen pixels that all march identically. **No
isotropic cell size fixes this**: sizing to the long axis just correlates
across the short axis, trading the streaks for perpendicular ones of equal
length.

Per-pixel decorrelation can only come from screen space. `AoNoise()` keeps
the world-cell hash as a per-cell offset and adds Jimenez interleaved
gradient noise. The IGN term is a pure function of pixel coordinate with no
frame counter, so it is static in screen space — it cannot shimmer when
stationary. Cvar `mt_compute_ao_noise_screenmix` (default 1.0; 0 = pure
world-locked). A/B'd in-game from a fixed camera: the mottling clearly
responds, the dark bands did not — which is what split bug 3 from bug 4.

Note this partly walks back the pure world-locking premise of the sixth
attempt, and matches what production GTAO does (Activision's original,
Intel's XeGTAO): screen-space noise plus a denoiser, no world anchoring.

### 4. Off-screen samples were CLAMPED, not rejected (the dark bands) — the big one

```
sampleUV = clamp(sampleUV, halfTexel, float2(1.0) - halfTexel);   // was
```

Every march step leaving the screen was walked back onto the nearest border
texel, so step after step and direction after direction landed on the *same*
edge pixel and voted as an occluder repeatedly — fabricating an occluder out
of nothing. For any pixel within `radiusPixels` of the viewport border that
is most of its sample budget, and `radiusPixels` is largest exactly when the
player is close to a surface.

Signature, all confirmed on screenshots: a wide dark band hugging the
viewport edge, spanning the full width, tracking no geometry, growing and
shrinking with *proximity* rather than with the scene. It survived every
earlier experiment (noise, thickness, step count) because it has nothing to
do with any of them. Now rejected with a bounds test + `continue` in all
three sample kernels; skipping merely lowers the effective sample count near
the border. Standard SSAO border handling.

### Also in this pass

- `mt_compute_ao_thickness` cvar (default 1.25) — was a hard-coded `1.25f`
  in `Render()` annotated with a comment claiming "1.5-2.0", untraceable and
  unsweepable. Exposed purely so it can be swept.
- `mt_compute_ao_worldpos_debug` now prints a yellow warning when nonzero.
  It is `CVAR_ARCHIVE`, and *three* rounds of screenshots this session were
  spent analysing its `fract()` grid before anyone noticed it was still
  enabled from a previous session. The debug viz also now uses the fixed
  cvar cell size, not the adaptive one — the adaptive size is ~1 pixel by
  design, which puts `fract()` at Nyquist and renders the grid as moire.
- Repaired `AOCombineParams` in the `SSAO_COMPUTE_SOURCE` fallback string:
  a scripted edit had spliced `pixelWorldScale`/`screenNoiseMix` into it
  (both `SSAOParams` and `AOCombineParams` begin with `int debugMode;`),
  shifting every field after it. Latent — the fallback only runs if the
  metallib is missing. `check_shader_parity.py` MATCHes all kernels.

### Perf

`ComputeCPU AO active_avg` 0.430-0.462ms vs the 2026-07-15 GTAO baseline of
~0.56ms. No regression; marginally cheaper, consistent with the off-screen
reject skipping texture fetches the clamp used to perform.

### Corrections worth keeping (these each cost a round trip)

- **The "Y sign" theory was wrong.** A negation was added to
  `WorldPosFromViewPos` on the theory that `FetchViewPos`'s Y disagreed with
  `mViewMatrix`. Tracing the Metal Y-flip properly: the patched vertex
  shader emits `clip.y' = -clip.y` and Metal maps NDC y=+1 to row 0, so
  scene textures are stored bottom-up and `uv.y=0` is the image *bottom* —
  the original formula was already correct. Corroborated by the fact that
  the kernels compare `FetchViewPos` positions against G-buffer view-space
  normals, and a global Y mirror between those two would invert occlusion on
  every floor and ceiling. Reverted.
- **The thickness hypothesis was empirically killed, by sign.** Gemini
  proposed that a too-tight thickness threshold causes over-darkening at
  grazing angles. Both branches at the reject `continue`, i.e. *discard* the
  sample, so a tighter threshold must *brighten*. Confirmed in-game:
  `mt_compute_ao_thickness 1000` (reject effectively disabled) made the
  scene dramatically **darker**. The mechanism as stated predicts the wrong
  sign.
- **Gemini's "disk collapse" instinct was right, mis-sited.** It was
  dismissed here because the radius clamp it pointed at
  (`min(outSize.x, outSize.y) * 0.5`) was already present and demonstrably
  did not fix the crawl. But hard-clamping UVs collapsing the sample disk
  onto repeated edge texels was exactly right — it just happens at the
  *viewport border* (bug 4), not from an unbounded radius. Dismiss the site,
  not the mechanism.
- Screenshots proved decisive repeatedly and code reading did not. Several
  confident mechanisms derived from reading needed correcting against what
  was actually on screen. Two shots from a *fixed* camera with one variable
  changed beat any amount of reasoning; when the camera moved between
  shots, the comparison was worthless.

### Open / next

- Residual soft darkening low in the frame after the bug-4 fix is
  unclassified: strafe to see whether it moves with the scene (real AO) or
  stays glued to the viewport (border residue).
- A `mt_compute_ao_temporal*` cvar family still exists (`temporal`,
  `temporal_blend`, `temporal_depth_reject`, `temporal_teleport_angle`,
  `temporal_teleport_dist`). Flagged during this session as a possible
  denoiser that would reopen bug 3's design; **user confirmed 2026-07-26
  that this whole section is superseded by the current refactor**. Treat it
  as dead scaffolding, not as an input to the noise design. Removal not
  attempted here — it is its own isolated cleanup.

## Development machine capability profile (Intel Mac, recorded 2026-07-26)

Added `mt_caps` (CCMD, `mt_debug.cpp`) to dump `MtVersionManager`'s probed
capabilities plus the raw `supportsFamily()` answers. None of this was
reachable in-game before, and its absence caused a whole planning round to
be built on the assumption that this was an Apple Silicon machine. **It is
not.** Verified output:

```
Architecture:            Intel (IMR)
Metal version (approx):  2.0          macOS: 12.7.6
TBDR / memoryless:       no / no
ReadWrite BGRA8 (Tier2): NO           <- bloom Tier 2 gate
Argument buffers:        no (tier 0)
RGB10A2:                 no
SIMD-group / non-uniform threadgroups: no / no
Binary archives:         yes          GPU timestamps: yes
Stage counter sampling:  no           <- per-pass GPU timing gate
Managed storage:         yes          Max drawables: 3
Raw supportsFamily(): Common1 yes, Common2 yes, Common3 no,
                      Mac2 no, MacCatalyst2 no, Apple1/4/7 no
```

`supportsRGB10A2`, `metalVersion`, `supportsSIMDGroup` and
`supportsNonUniformThreadgroups` are all derived from the single
`supportsFamily(GPUFamilyMac2)` probe, so all four go false together. That
looked like a broken probe; the raw dump disproves it — Common1/Common2 yes
with Common3/Mac2 no is a coherent profile for an older Intel iGPU, not the
incoherent pattern an enum/ABI mismatch would produce. **The capability
gating is correct. Do not "fix" it.**

### Consequences

- **Bloom Tier 2 is unreachable here.** `composite 0` and `1` both take the
  Tier 1 path (identical behaviour); `composite 2` hits
  `if (composite == 2 && !directCompositeSupported) return false` and falls
  back to upstream `hw_postprocess` bloom. Tier 2 validation is blocked on
  acquiring Apple Silicon or discrete-AMD hardware. Usefully, this makes
  `composite 2` a **ground-truth oracle**: Tier 1 compute bloom can be
  A/B'd against the reference postprocess implementation, which is a better
  test than comparing two compute paths that might share a bug.
- **Per-pass GPU timings are hardware-blocked**, not unimplemented
  (`docs/engine-modernization.md:96-98`). Stage-boundary counter sampling is
  unsupported. Whole-frame GPU timing works (GPU timestamps yes).
- **All AO work of 2026-07-26 was validated at quarter-res only** —
  `mt_ao.cpp:1553` forces `aoScale >= 4` on Intel. The projection fix, the
  off-screen sample reject, and the `AoNoise` screen mix are
  resolution-independent by construction. `NoiseCellSize` is **not**: it
  scales by `mAOHeight`, so at half-res cells are half the world size and
  the power-of-two ring boundaries fall at different depths. Re-verify that
  one specifically on Apple Silicon before calling it universally validated.
- Binary archives are enabled here, so the PSO cache is live — the
  stale-kernel trap applies to every shader change on this machine.
- No SIMD-group ops and no non-uniform threadgroups: kernels must keep
  bounds-checking their grid (they do) and cannot use wave intrinsics. Any
  frame-graph or compute work should treat Tier 2 RW textures, argument
  buffers tier 2, and SIMD-group ops as unavailable on the primary dev
  machine.

## Bloom Tier 1 vs reference: first Intel measurement (2026-07-26)

Roadmap item #2 ("record stable Intel baseline measurements"), taken on the
Intel dev machine profiled above. Ashes: Afterglow, dark corridor with a
bright lamp in frame, fixed camera and window size, `0 -> 2 -> 0`.

On this hardware Tier 2 is unreachable, so `composite 0` and `1` are the
same Tier 1 path and `composite 2` falls back to upstream
`hw_postprocess` bloom -- which makes `2` a ground-truth reference rather
than merely a third variant.

| Run | Config | GPU Frame active_avg | Bloom encode active_avg |
|---|---|---|---|
| 1 | `composite 0` Tier 1 compute | 21.206ms | 0.266ms (ComputeBloom) |
| 2 | `composite 2` reference       | 20.589ms | 1.101ms (PPBloom)      |
| 3 | `composite 0` Tier 1 compute | 20.523ms | 0.278ms (ComputeBloom) |

**Conclusions:**

- **CPU encoding: Tier 1 compute is ~4x cheaper** (0.27ms vs 1.08ms,
  ~0.8ms/frame saved). Repeatable across runs. This is the first hard
  justification for the compute postprocess path on low-end hardware.
- **GPU cost: no detectable difference.** Runs 1 and 3 are the *same*
  config and differ by 0.68ms -- as large as the apparent between-config
  gap -- and run 3 (compute) landed *below* run 2 (reference), so the
  ordering isn't monotonic. An earlier pair of runs suggested a ~2ms GPU
  regression for compute; that was a confound (the reference run had been
  taken at a smaller window, and fewer pixels alone lowers GPU frame time)
  plus this noise. **The third run existed specifically to catch that, and
  did.** Always take a same-config control run before believing a
  between-config delta on this machine.
- The reference path shows occasional large spikes (`PPBloom max 9.051ms`
  vs compute's `0.459ms`), likely first-use pipeline compilation.

**Measurement caveat:** the metrics window is 120 frames and straddles each
switch, so runs 2 and 3 report both counters (105/15 and 66/54 samples).
Run 1 is the only pure sample. Since the control already shows variance
exceeds the effect, tighter isolation would not change the conclusion.

### Still open

- **Visual A/B not yet done.** No fixed-camera screenshot pair exists for
  Tier 1 vs reference. Needed because the two extracts are **not**
  equivalent by construction:

  ```
  reference (bloomextract.fp):  max((color.rgb + 0.001) * exposureAdjustment - 1, 0)
  compute   (mt_bloom.metal):   max( color.rgb - threshold + 0.001,             0)
  ```

  The compute extract has **no exposure term**, and `mt_bloom.cpp` hard-codes
  `params.threshold = 1.0f` while `Execute(cmdBuf, srcTex, gl_bloom_amount)`
  never receives the exposure texture. They agree only when
  `exposureAdjustment == 1`; exposure is live by default
  (`gl_exposure_base` / `gl_exposure_min` 0.35, `gl_exposure_scale` 1.3).
  Predicted symptom: compute bloom dimmer than reference in dark scenes,
  and not adapting to scene luminance. Fix is to plumb the exposure texture
  into `MtBloomModule::Execute` and multiply before the threshold -- but
  confirm the visible difference first.
- Remaining matrix on `composite 0` only: resize, fullscreen toggle,
  portals, camera textures.

## Independent GPT audit (2026-07-27) — 3 findings, all verified real

Second outside audit, deliberately run differently from the Gemini round
(see "Independent Gemini audit" above, which returned 6 false positives out
of 18 with both CRITICALs wrong). The brief was `GPT_AUDIT_CONTRACT.md`
(gitignored, root): scope narrowed to `mt_ao.*` first, bloom second, compute
glue third; the Intel `mt_caps` profile stated as a hard constraint; and a
standard-of-evidence section built from the three specific ways the previous
round went wrong — the autorelease false-CRITICAL (must cite an ownership
rule before reporting a leak), the thickness hypothesis (must state error
*direction*), and disk-collapse (mechanism and site argued separately).
Every finding had to arrive with a falsification test, expressible in
`mt_metrics` whole-frame terms for perf claims since per-pass GPU timing is
hardware-blocked here. Findings also self-label as correctness /
visual-quality / performance / maintainability, and performance findings
must name a bottleneck axis.

**Result: 3 findings, 0 false positives.** Lower volume, and the count is
the point — the brief explicitly said an empty report was acceptable and
three well-evidenced findings beat eighteen. The narrow scope plus required
falsification test is the shape to reuse. Keep §5 strict; only loosen if a
future pass comes back too timid.

The auditor disclosed up front that it had seen `AGENTS.md` content before
starting, so finding 1 is a careful re-derivation, not independent
confirmation. Findings 2 and 3 are genuinely new.

### Finding 1 (bloom exposure) — confirmed, already documented

Restates the divergence recorded under "Bloom Tier 1 vs reference" above,
but added something that section missed: the reference's `+0.001` bias is
**also** multiplied by exposure, so folding exposure into a scalar threshold
cannot reach parity — it must multiply before thresholding. It also notes
the reference samples an exposure *texture*, not a scalar, so any
cvar-shaped fix is wrong by construction. Fix direction updated accordingly.

### Finding 2 (`SSAOParams` struct drift) — FIXED this session

`mt_ao.cpp`'s inline `SSAO_COMPUTE_SOURCE` had lost `float noiseCellSize`
while every kernel body in it still referenced `params.noiseCellSize`. So
the inline fallback would not have miscomputed — it would have **failed to
compile**, leaving the compute AO PSOs null and silently dropping to the PP
AO path. The shipped native path was never affected (`mt_ao.metal` and
`mt_ao.h` both had the field, in the same order).

The valuable half was the auditor noticing **why the existing parity script
missed it**: `tools/check_shader_parity.py` compared kernel *bodies* only,
and reported MATCH on all ten while the shared struct declaration had
diverged.

Fixed: added the field at `mt_ao.cpp:84`, plus a stale `gl_ssao_debug 1` ->
`2` in the adjacent comment that had drifted invisibly (comments are
stripped before comparison).

**`check_shader_parity.py` extended** to compare shared struct declarations
in two directions. The second matters more than the first:

1. native `.metal` <-> inline fallback string — catches this drift, but only
   ever breaks the rarely-exercised fallback, and breaks it loudly.
2. native `.metal` <-> the C++ header (`mt_ao.h`, `mt_bloom.h`) — a
   CPU/GPU field-order or size mismatch **silently misreads every parameter
   after the divergence, on the path that actually ships**, and would
   present as a shader math bug.

Field *order* is compared, not just the name set: order determines layout,
and a reordering with an unchanged name set is exactly the "someone appended
a field for alignment on one side only" case. Two implementation notes for
whoever touches this next:

- First run false-positived on `float4x4 viewToWorld` vs
  `float viewToWorld[16]` and `float2 srcRes` vs `float srcRes[2]` — same
  bytes, different spelling. It now compares (scalar base, component count)
  footprints, so those pass while real drift (`float2` vs `float4`, `int`
  vs `float`) still fails.
- Collapsing those spellings opens a gap: MSL `float2` aligns to 8 and
  `float3`/`float4` to 16, but a C++ scalar array aligns to 4 — equal size,
  different layout. A `check_alignment` pass walks byte offsets and WARNs
  when a vector field sits where MSL would pad and a scalar-array
  counterpart would not. Silent on current structs because they are
  well-packed; verified to fire on a synthetic `float` + `float2` struct.
- A clean run is **not** proof of identical layout. It cannot see `alignas`,
  `packed_*`, or implicit tail padding. It rules out the drift class that
  has actually bitten this codebase, nothing more.

Both the fix and the script were validated: parity check exits 0, `zdoom`
target builds clean.

### Finding 3 (coverage mask coordinate space) — real, and the site is worse than reported

Auditor's instinct was right and its site was wrong, the same pattern as
Gemini's disk-collapse finding. It correctly spotted that `coverageMask` is
sampled at raw `sampleUV` (`mt_ao.metal:375,561,782`) while every scene
texture on the adjacent lines uses `offset + sampleUV * scale`, but blamed
`SceneScale`/`SceneOffset` and rated it low severity, contingent on a
letterboxed viewport.

The sharper mechanism needs no letterboxing at all — it is a **render-target
size mismatch**:

- `EnsureTextures` is called with `(sceneWidth/aoScale, sceneHeight/aoScale)`
  (`mt_ao.cpp:1556`) and `mCoverageMask` is allocated at that AO resolution
  (`mt_ao.cpp:1462-1468`).
- `RenderCoverageMask` (`mt_ao.cpp:2087-2097`) attaches that quarter-res
  color target alongside the **full-res** `SceneDepthStencil`, then draws a
  fullscreen triangle with **no viewport or scissor set**.

Rasterization covers the AO-res render area, so mask pixel (x,y)
stencil-tests against full-res stencil pixel (x,y). With `aoScale` forced to
4 on Intel, the mask encodes the stencil state of the **top-left quarter of
the screen, addressed as if it were the whole screen** — not a downsample of
it. Predicted direction: portal-boundary AO wrong across the other three
quadrants, anchored to the viewport rather than to scene geometry,
brightening where samples are falsely rejected.

Candidate cause for the unclassified "residual soft darkening low in the
frame" left open by the seventh AO pass. Not yet confirmed — needs the
strafe test, which now discriminates three ways rather than two: tracks the
world (real AO) / glued to viewport border (bug-4 residue) / glued to a
quadrant boundary (this).

Open question a read cannot settle: whether Metal clamps the render area to
the smallest attachment or whether validation objects to the mismatched
attachment sizes.

### Finding 3 fix (2026-07-27) — implemented, NOT yet verified in-game

`mCoverageMask` is now allocated at the **stencil attachment's** resolution
rather than AO resolution, and sized from `depthStencilTex->width()/height()`
inside `RenderCoverageMask` itself so the two grids cannot drift apart.
Allocation moved out of `EnsureTextures` (which only ever sees AO dims) into
a new `EnsureCoverageMask`. All three AO kernels now sample the mask with
`sampleSceneUV` instead of `sampleUV`, matching the coordinate space they
already use for scene colour/depth/normal.

Note this is the full attachment (screen-sized), *not* the scene viewport
sub-rect — sizing to `sceneWidth/sceneHeight` was the first attempt and was
also wrong, because the scene viewport can be a sub-rect of the screen-sized
buffer and the stencil test compares in attachment coordinates.

### VERIFIED IN-GAME 2026-07-31 — three-capture A/B, prediction stated in advance

Scene: Ashes Afterglow, "Hard Reset", map **Night School**, standing in front
of a **mirror** with the player's reflection visible. Fixed camera from a
savegame across all three captures. `gl_ssao 1`, `gl_ssao_debug 2` (raw
unblurred AO), `mt_compute_ao 1`, `mt_compute_ao_scale 2`.

**Mirrors are portals** for this purpose -- `HWMirrorPortal` derives from
`HWScenePortalBase : HWPortal`, and portals bump `screen->stencilValue` and
draw a stencil mask (`hw_portal.cpp:309`). This is the cheapest way to get a
non-uniform stencil buffer on screen. Do **not** reach for vanilla Doom 2 to
test this: no mirrors, no line portals, no stacked sectors.

Three captures, cheapest-information-first (the ordering matters -- if A==B
the mask is inert in that scene and C is a wasted rebuild):

| Capture | Build | Result |
|---|---|---|
| A | current (fixed) | baseline |
| B | `sampleCov < 0.5` guard forced false (mask all-white) | 3,501 px differ (0.39%), **all darker**, in two narrow vertical bands: x 460-520 and x 966-1014 |
| C | `mt_ao.cpp` + `mt_ao.metal` checked out at `fc2000ecb^` | 19,593 px differ (2.16%), **all brighter** (26,236 brighter / 0 darker), spanning x 471-1011, peak block delta +11.06 at x=**927** |

**A vs B proves the mask is live and correctly placed.** The two bands are the
mirror's left and right edges. Mask on: samples crossing the boundary are
rejected, no halo. Mask off: they are admitted and manufacture spurious
occlusion along the edge. The effect is confined to ~1 AO radius either side
of the boundary, as theory predicts.

Note this **corrects an in-session hypothesis**: the flat white mirror
interior is *not* the mask rejecting the whole portal region (which would
have meant reflections get no AO at all). The interior is untouched; it is
genuinely unoccluded geometry. Only the boundary is affected.

**A vs C confirms the fix, via a prediction made before the capture.**
Pre-fix, screen position *x* read `stencil(x/2)` -- the top-left quadrant
stretched 2x. So the left mirror edge at x=460 was predicted to surface at
**x~920**, and the right edge at 966 to map to ~1932, off a 1552px frame.
Measured peak: **x=927** (7px error), and the right-hand edge band present in
the fixed build is absent as an edge feature in C. Every differing pixel is
brighter, i.e. the pre-fix mask over-rejects -- exactly the signature of a
doubled, top-left-anchored mask. No other mechanism in the pipeline produces
that combination.

**Two process traps this run, both cost real time elsewhere:**
1. The **PSO binary archive** (`~/Library/Application Support/zdoom/cache/mt_pipelines.bin`)
   was written *during* capture A's session, before the diagnostic metallib
   existed. Left in place it would very likely have replayed pre-diagnostic
   pipelines and produced B==A for entirely the wrong reason. Delete it after
   every shader-level rebuild in an A/B.
2. CMake printing `Built target metal_native_shaders` rather than `Building`
   is not proof the metallib changed. Check the `.metallib` mtime in all
   three locations (`build/src/`, `Contents/MacOS/`, `Contents/Resources/`).

**Severity correction.** The fix is right, but the impact is what the auditor
originally said (low, portal-only) and not what the AGENTS entry above
implied. `GetPPStencilState` uses `CompareFunctionAlways` with depth writes
off, so the mask is purely `stencil(x,y) == stencilValue`. In a non-portal
scene the stencil buffer is uniformly 0 and `stencilValue` is 0, so the mask
came back all-1 and the misalignment was invisible. **It is therefore not a
candidate for the residual soft darkening in a plain scene** — that remains
unexplained and still needs its own strafe test.

Verification needed, and it must be in a portal-heavy map with a fixed
camera; a plain corridor cannot distinguish this fix from a no-op by
construction. Nothing to clear beforehand: `~/Library/Caches/gzdoom` does not
currently exist, so neither the `.msl` cache nor `mt_pipelines.bin` can serve
a stale kernel. Build is clean and parity exits 0.

### Residual soft darkening: RESOLVED as real AO (2026-07-27)

The "residual soft darkening low in the frame" left unclassified by the
seventh AO pass was strafe-tested on DOOM2 MAP01 (compute AO on, algorithm 1
AlchemyAO, `gl_ssao_debug 1` raw-AO view, fixed camera, strafe only — no
mouse movement between observations). **It sticks to geometry**, so it is
genuine ambient occlusion, not viewport-anchored border residue.

Consequence: the bug-4 off-screen-sample-reject fix was complete. Nothing
further is owed on it, and the "Open / next" item above is closed.

#### Finding 3 fix: in-game result 2026-07-27 — NO REGRESSION, not confirmed

Trenchfoot, compute AO on, algorithm 1, sky in frame. Observed: sky is
masked correctly, no AO on it.

**Read this carefully before treating the fix as verified.** Absence of AO on
sky was *already* true pre-fix (see "Skybox AO seam fix" and "AO distance
fade for sky-camera-room content" above), and the pre-fix bug predicted
misplaced AO on **geometry** — occlusion sampled from the wrong screen
location — not AO bleeding onto sky. So this observation confirms the mask
changes did not break existing sky masking, and confirms nothing about
whether the coordinate fix does what it is supposed to do.

A true A/B needs the fix stashed and rebuilt, same fixed camera, in a portal
inner-scene pass. Not done: impact is low and portal-only, and the fix is
correct by construction (the mask's pixel grid is now derived from the
stencil attachment itself, so mask/stencil grid mismatch cannot recur).
Deprioritized in favour of the bloom exposure defect, which is visible in
every dark scene. Left here as an explicit "unverified but structurally
sound", not as a pass.

## Bloom exposure divergence: CONFIRMED and FIXED (2026-07-27)

Finding 1 of the GPT audit, previously recorded as a code-reading claim, is
now **experimentally confirmed** and the fix is implemented. The measurement
route matters as much as the result -- three earlier attempts produced
invalid comparisons before the confounds were understood.

### How it was finally measured

Visual A/B of `mt_compute_bloom_composite 0` vs `2` **does not work by
itself** on this machine, for two reasons discovered the hard way:

1. **Wrong scene.** Ashes: Afterglow's dark corridor has max viewport
   luminance ~231/255. Both extracts subtract 1.0 and clamp, so *nothing*
   exceeded threshold: seven screenshots across `gl_bloom 0/1` and every
   composite mode came back **pixel-identical**. Toggling bloom entirely off
   changed zero pixels. Always run a `gl_bloom 0` vs `1` control first --
   if that pair is identical, the location cannot test anything.
2. **Exposure adaptation drift.** `gl_exposure_speed` defaults to 0.05, so
   the scene keeps brightening for tens of seconds. At Ashes 2063 MAP51 the
   same-config control differed by max 52 luminance while the between-config
   difference was max 2 -- **the control drifted 26x more than the effect**.
   Set `gl_exposure_speed 1` before any bloom A/B -- **and reset it to 0.05
   afterwards.** Left at 1, adaptation snaps every frame and manufactures a
   motion-only bloom artifact on the compute path; that cost a session (see
   the ghosting entry below).

The test that worked is a **within-config toggle**, immune to both: hold the
config fixed and toggle `gl_exposure_base` between 0.35 and 10 (which forces
`exposureAdjustment` from ~2.9 down to ~0.1), then repeat under the other
config. Four shots, Ashes 2063 Enriched MAP51 "Dead Man Walking" (an
`Ashes2063Enriched2_23.pk3` map -- DOOM2 has no MAP51; the pk3 defines
slots 50+ via its own mapinfo, so the test needs `-file
Ashes2063Enriched2_23.pk3`, *not* Afterglow):

| Shot | Config | Result |
|---|---|---|
| 12.53.15 | reference, base 0.35 | **unique** |
| 12.53.36 | reference, base 10   | identical to below |
| 12.54.13 | compute,   base 0.35 | identical |
| 12.54.26 | compute,   base 10   | identical |

- **Reference responds to exposure; compute does not.** The compute pair is
  byte-identical under a 29x change in exposure.
- **Reference-with-exposure-disabled is byte-identical to compute.** Killing
  the exposure term in the reference path reproduces the compute path
  exactly, which proves exposure is the *only* difference between the two
  implementations -- blur chain, downsample and composite all agree bit for
  bit.
- Defect magnitude with exposure live: compute darker across 3.8% of the
  frame around light sources, peak block -7.0 luminance, max -10, and
  **zero** pixels brighter (3 of 900k). Direction as predicted.

Also confirmed en route, via `mt_metrics`, that the paths really do switch:
`composite 0` reports `ComputeCPU Bloom ~0.27ms` and no PP counter,
`composite 2` reports `PPCPU Bloom ~1.01ms` and no compute counter. They are
mutually exclusive, and the ~4x CPU encode advantage reproduces.

### The fix

`bloom_extract` now matches `shaders/pp/bloomextract.fp` exactly:

```
max((color + 0.001) * exposureAdjustment - threshold, 0)
```

Note the operand order -- bias applied **before** the exposure multiply,
threshold subtracted **after**. This is not the same as scaling the
threshold, because the reference scales the bias too; the GPT audit caught
that specific point and it would have been easy to get wrong.

- `MtBloomModule::Execute` takes an `exposureTex` parameter;
  `mt_postprocess.cpp` passes
  `GetPPTexture(&hw_postprocess.exposure.CameraTexture)`. Ordering is safe:
  `hw_postprocess.Pass1` runs `exposure.Render` before the compute bloom.
- `BloomParams` gains `float useExposure` (now 9 fields in all three copies).
  Slot 2 is always bound -- `srcTex` stands in when exposure is unavailable
  -- because a declared-but-unbound texture argument is invalid even when
  the sample is branch-guarded.

Build clean, parity exits 0.

### VERIFIED 2026-07-30, and it exposed a second, larger bug

The convergence check (Ashes 2063 MAP51, fixed camera, `gl_exposure_speed 1`)
did **not** converge on the first run -- it overshot and flipped direction.
Contribution over the `gl_bloom 0` baseline (19.2354): reference **+0.1387**,
compute **+0.2913**, i.e. compute applied ~2.1x the reference's bloom, over
4.63% of the frame, one-directional brighter (97,473 px brighter vs 103
darker). The pre-fix error had been compute *darker* by peak -7; the
overcorrection was larger than the original error.

The exposure fix was not at fault. `bloom_extract` matches `bloomextract.fp`
exactly, bias-before-multiply and threshold-after. What the exposure fix did
was let enough bloom survive the extract to make a **pre-existing combine bug**
measurable. See the pyramid fix below.

**Lesson: an A/B that overshoots is not a failed fix, it is a second bug.**
The instinct to revert the first fix would have been wrong here -- the
extract was already correct and reverting would have re-hidden the real
defect behind a compensating error.

### Compute bloom summed mip levels the reference never sums (fixed 2026-07-30)

`PPBloom::RenderBloom` adds bloom to the scene **exactly once**: `level0.VTexture`,
additive, unit gain (`hw_postprocess.cpp:151-159`), and `bloomcombine.fp` is a
plain copy with no gain. Its multi-scale look comes from a down-then-up chain
where each upscale **replaces** the level below it (`SetNoBlend()`), so the
final level0 is one wide soft unit-gain glow.

The compute path instead summed four levels at 1.0/0.18/0.09/0.06 = 1.33x
nominal gain. Worse, its down loop downsampled `bloomA` into *every* mip
rather than chaining level to level, so the mips were four blurs of the same
image -- correlated energy piled onto the same light, which is why the
measured overshoot (2.1x) exceeded the nominal 1.33x.

Fix (`mt_bloom.cpp`): run the reference's structure -- blur/downscale down the
chain, blur/upscale back up replacing each lower level, then composite
`bloomA` alone at strength 1.0. Both paths are 4 levels (`NumBloomLevels == 4`;
`bloomA` + 3 mips), so they map one-to-one. `downsample_box` derives its UVs
from the destination extent, so the same kernel serves both legs -- no new
kernel. Slots 1-3 stay bound to `bloomA` at strength 0, since every declared
texture argument must be bound.

**Result: 81 px differing (0.0089%), mean signed +0.0002, bidirectional (40
brighter / 41 darker), mean_lum 19.4665 vs 19.4666.** Down from 41,927 px
(4.63%) -- a 518x reduction. The residual being *bidirectional* is the
signal that gain is now correct and only bilinear-resample noise at one glow
edge remains; a one-directional residual would have meant gain was still off.

Caveat on method: the verification pair was captured at a slightly different
camera/time than the baseline batch (baseline mean_lum shifted 19.37 -> 19.47),
so contribution-over-baseline was **not** recomputed for it. The direct
`composite 2` vs `0` comparison is the valid test and does not depend on the
old baseline.

Still unrun (the stronger of the two original exposure checks): does the
compute path *respond* to `gl_exposure_base` 0.35 vs 10, where before the fix
it was byte-identical? That checks the exposure texture actually reaches the
kernel, rather than merely that two images agree.

### RESOLVED (not a renderer bug): compute-bloom "ghosting" above objects, motion-only (reported 2026-07-28, closed 2026-07-30)

**Confirmed 2026-07-30: the ghost disappears at the default `gl_exposure_speed
0.05` and does not reproduce. No code change was needed or made.**

**User-identified: the ghost is an artifact of `gl_exposure_speed 1`,
the diagnostic setting from the bloom-exposure A/B — not of the shipping
config.** At speed 1 the adaptation snaps fully every frame instead of easing
at the 0.05 default, so `CameraTexture` whipsaws whenever the view moves. The
compute extract multiplies by that texture (added by the exposure fix the same
session), so bloom brightness pulses with motion and reads as a ghost.

This accounts for all three symptoms: motion-only (a static scene has stable
adaptation), compute-specific (if compute and reference sample `CameraTexture`
at different points in the frame, the resulting one-frame lag is invisible at
0.05 and glaring at 1), and absent from stills (each screenshot is internally
consistent; the artifact lives in the frame-to-frame delta).

**Generalized trap: diagnostic cvars must be reverted before judging a
visual artifact.** An instrumentation setting chosen to make one effect
measurable can manufacture a different artifact entirely, and it will look
like a renderer bug because it only reproduces on the path under test.

The historical analysis below is kept for the ruled-out list; its "not caused
by the exposure fix" claim is superseded — the mechanism does run through
exposure, via the test config rather than the code change.

Additionally ruled out by reading (2026-07-30), do not redo:

- **The in-place composite asymmetry, previously the best suspect.** Compute
  reads `PipelineImage[mCurrentPipelineImage]` and blits back into it, leaving
  the index unchanged; reference writes N -> N+1 and advances. Both leave
  `mCurrentPipelineImage` pointing at the image carrying bloom, which is what
  `Pass2` consumes. Self-consistent, cannot displace anything.
- **A compute->render hazard on `mCompositeTex`.** `MtComputeManager::CreateTexture`
  (`mt_compute.cpp:11-27`) never sets `hazardTrackingMode`, so textures are
  Tracked and Metal inserts the barrier across the `endEncoding()` handoff.

### Original analysis (2026-07-28)

User reports a ghost image roughly an inch above objects (Ashes 2063
Enriched MAP51, campfire/crate scene) with `mt_compute_bloom_composite 0`,
**not** present under `composite 2` (reference). Compute-path-specific.

**Does not appear in screenshots.** Three stills at the reported camera
position were measured; the only compute-vs-reference difference is a broad
brightness change (35% of pixels, max 9 luminance, spread over ground and
sky) consistent with overall bloom strength, plus a rectangular darker patch
near the crate. No displaced copy of anything.

Since GZDoom's screenshot reads back the composed frame -- and bloom *is* in
that readback, every bloom measurement this session came from these files --
the artifact is probably **not in the frame's pixel data**. It is temporal or
presentational, visible across a sequence of frames rather than within one.

Not caused by the exposure fix: that change only multiplies extracted
values and cannot displace anything spatially. It may however have made a
pre-existing misalignment *visible* by letting more bloom survive the
extract.

Candidate mechanisms, none yet discriminated:

- One-frame-stale bloom (glow lagging geometry during motion; invisible when
  static because consecutive frames are identical).
- **In-place composite.** The Tier 1 path additively blits `mCompositeTex`
  back into `srcTex` -- the *same* pipeline image it read for the extract
  (`mt_bloom.cpp:553-568`). The reference path instead outputs to
  `NextPipelineTexture` and advances `mCurrentPipelineImage`
  (`mt_postprocess.cpp:377-381`). This asymmetry is compute-only, which
  matches the report. Best current suspect.
- Drawable recycling (max drawables 3 on this machine).

Ruled out by reading: mip-chain partial writes (every dispatch grid matches
its destination texture's dimensions exactly, so no stale texels survive);
and the Y/viewport coordinate chain, which round-trips correctly --
`mCompositeTex` is allocated at exactly `srcW x srcH`, the blit sets viewport
to the scene rect, and extract row 0 -> scene `uv.y=0` -> image bottom ->
`bloom_fs` `uv.y=0` -> viewport bottom.

Next diagnostics, cheapest first: (1) does it depend on motion type --
strafe vs rotate-in-place vs walk forward, since stale bloom tracks
screen-space motion; (2) screen recording, which captures presentation where
stills do not; (3) Xcode GPU frame capture to inspect pipeline images
mid-frame, which would settle the in-place-composite theory outright.

**Still frames cannot diagnose this.** Do not spend more screenshots on it.

## Intel now defaults to the reference AO path (2026-08-01, 2cf256d13)

`mt_compute_ao_intel` (Bool, default **false**) routes Intel integrated
GPUs to `hw_postprocess.ssao.Render`. Rationale is the ~2x measurement
below; on an HD 6000 that is ~45fps vs ~87fps, GPU-bound either way.

**Kept separate from `mt_compute_ao_intel_clamp` on purpose.** Folding them
into one flag would make the clamps unreachable: anyone clearing the clamp
flag to raise quality would simultaneously switch the compute path on, so
the clamped configuration could never execute.

**Non-Intel behaviour is unchanged, and that is the status quo rather than
a measured decision.** Nothing on the compute path has ever run on Apple
Silicon -- not the performance, not the Tier 2 read-write paths. Do not
promote "compute on advanced hardware" to a stated policy without
measuring it. The 15-minute test if hardware ever appears: `mt_caps`
(confirm Tier 2 and arch detection), then `mt_compute_ao` 1 and 0 with
`mt_metrics` either side, then `mt_compute_bloom 1` -- which would be the
first execution of the Tier 2 direct-composite path anywhere.

## Masking: the PP AO path is already stencil-masked on Metal (2026-08-01)

Two *different* mechanisms were conflated under "the compute path has
masking":

1. **Output-side stencil test** -- `mt_postprocess.cpp:251`, in
   `MtPPRenderState::Draw`. Fires for any hw_postprocess pass targeting
   `PPTextureType::SceneColor`. `PPAmbientOcclusion::Render`'s combine ends
   with `SetOutputSceneColor()`, so **the reference PP AO path already gets
   this on Metal**. It was never compute-specific.
2. **Per-sample coverage rejection** -- compute only. A mask texture
   sampled *inside* the AO loop (`sampleCov`, `mt_ao.cpp:418/608/833`) to
   reject individual samples crossing a portal boundary. Strictly stronger
   than masking the composite.

**Tested (Ashes: Hard Reset corridor, compute vs PP, all gates passing --
title bar max delta 1, HUD max 2):** no bleedthrough signature. The
column profile of `pp - compute` is a smooth gradient, ~0 at x=100, peaking
+4.27 at x~=493-504, decaying to 0 by x~=537 -- which is the wall/pillar
corner in frame. Sign is **PP brighter, i.e. less AO**, strongest *at the
corner*.

That is an algorithm difference in corner response (Alchemy darkens
concave corners more than the reference horizon path), not a masking
failure. The predicted signature of missing per-sample rejection -- PP
showing *extra* darkening in a band inside a portal edge -- did not appear.
**Prediction not supported.**

Practical: switching to `mt_compute_ao 0` does not break masking; it gives
**subtler corner occlusion**. That is the actual trade, not correctness.

**Caveats, deliberately not glossed:** no mirror was identifiable in the
test frame (the reporter states they were in front of one; the capture
shows a corridor, wall, pillar and doorway), so the hard portal case may
not have been exercised. The scene is also very dark, which compresses
differences -- max delta was 8/255. Treat this as "no evidence of
bleedthrough here", not "bleedthrough is impossible in the PP path".

## MEASURED 2026-08-01: compute AO costs ~2x the reference PP path

Settled the long-standing open question. `mt_compute_ao 0` falls through to
`hw_postprocess.ssao.Render` (`mt_postprocess.cpp:437-446`), the path whose
own comment says it "matches GL/Vulkan", so toggling it is a true
compute-vs-reference A/B on identical hardware, scene and view.

| path | FrameGPU avg |
|---|---|
| compute AO, with full-res cleanup | 22.05 / 22.41ms |
| **reference PP AO** | **11.42ms** |

**+10.8ms, a 1.95x ratio.** The noise floor is 0.36ms (established by two
readings of an identical configuration), so this is ~30x noise.

Backing out the full-res cleanup enabled the same day (+2.91ms), compute
without it would be ~19.35ms vs 11.42ms -- still +7.9ms, 1.70x. The gap is
not an artifact of that change.

This independently reproduces the 2026-07-14 Intel bisection recorded at
`mt_ao.cpp:1713` (~33ms compute vs a ~17ms PP-AO-equivalent baseline, a
1.9x ratio). Two measurements, very different code, same answer. **Treat
the Intel clamps as load-bearing** -- they are not conservative guesses.

**Do not describe the Metal compute AO as superior in performance.** The
defensible claims are masking (the coverage-mask fix was verified in-game
with a numeric prediction stated before capture, `a9f22ab5a`) and features
(multi-algorithm selection, world-locked noise, full-res cleanup -- none
of which the reference has). Speed is not one of them; it is roughly half.

**Validity check for anyone repeating this:** the `mt_compute_ao 1` leg
must print a `ComputeCPU AO` line and the `mt_compute_ao 0` leg a
`PPCPU AO` line. A missing AO line means AO did not run at all (
`PrintMetricSummary` returns early at `samples == 0`), and the wrong label
means the toggle did not take. Both failure modes occurred while taking
this measurement and both were caught by the label rather than by the
numbers looking wrong.

## AO artifacts 2026-08-01: it was the config, not the renderer

Reported: AO crawling, salt/pepper squares, and an artificial look that
popped in and out near walls. All three came from `gzdoom.ini`, not code.

The ini had three quality features disabled relative to their defaults:

```
mt_compute_ao_skip_fullres=true      # default false
mt_compute_ao_normal_upsample=false  # default true
mt_compute_ao_combine_smooth=0       # default 0.25
gl_ssao_strength=1                   # default 0.7 -- and 1 is the maximum,
                                     # intensity is clamped to [0,1]
```

**`skip_fullres=true` was the big one.** At `gl_ssao 3` the full-res
upsample + a-trous cleanup auto-enables (`mt_ao.cpp:1589`), and
`skip_fullres` vetoes it. AO was therefore being combined straight from
the quarter-res texture with no cleanup: a hard quarter-res block grid,
which is the "salt/pepper squares".

**Measured, one variable at a time, same post and view:**

| change | FrameGPU avg | delta |
|---|---|---|
| baseline | 19.06ms | -- |
| `gl_ssao_strength 0.7` | 19.35ms | +0.29ms (noise) |
| `mt_compute_ao_skip_fullres 0` | 22.26ms | **+2.91ms** |

~52fps -> ~45fps GPU-limited. The strength drop is free and fixed the
artificial look; the cleanup costs ~15% of frame time and fixed the
squares. Note `FrameGPU` is the only usable cost metric here -- the
`ComputeCPU AO` line is encode time, and stage-boundary counter sampling
is unsupported on HD 6000, so per-pass GPU timing is unavailable.

**The reported crawl also cleared, which was not predicted.** Most likely
it was *block-grid swim* rather than the historical bug: with AO pinned to
a quarter-res screen-space grid, world surfaces slide across fixed block
boundaries as the camera moves and the pattern reads as crawl. The
joint-bilateral upsample removes the grid, so there is nothing left to
swim. **This is probably not the same defect** as the six-attempt "AO
pattern slides near geometry while moving" bug, which was root-caused to
dither-texture sampling and would not be touched by an upsampling pass.
Do not record that one as fixed.

**Check the ini before diagnosing AO artifacts.** This looked exactly like
a renderer bug for a full session. A config that disables three quality
passes produces artifacts indistinguishable from broken code.

**Stale cvars: RESOLVED 2026-08-02.** `mt_compute_ao_temporal{,_blend,
_debug,_depth_reject,_teleport_angle,_teleport_dist}` were left in the ini
after the temporal feature was fully reverted. **The claim that they still
tab-complete was wrong** -- no such symbol exists anywhere in the non-doc
source tree, so a clean build cannot register them. They were parked in the
ini's `[GlobalSettings.Unknown]` section, which is exactly where GZDoom puts
values with no matching CVAR declaration; that section membership is the
proof they were unbound. Deleted from the ini 2026-08-02. If those names ever
autocomplete again, suspect a stale executable, not this tree.

**Correction worth keeping.** The mechanism (quarter-res block grid) was
identified correctly and the fix proposed was wrong: the first instinct
was to spend GPU computing *more* AO pixels via
`mt_compute_ao_intel_clamp 0`, when the actual fix was re-enabling the
pass that *resolves* quarter-res to full res. AO still runs at quarter res
after the fix. Trying free config changes before paid ones is what caught
it -- on a GPU already at 19ms/frame, the wrong lever would have spent a
budget that did not exist.

## TRAP: BlitCurrentToImage silently changes vertical orientation

`MtPostprocess::BlitCurrentToImage` (`mt_postprocess.cpp`) picks its path
from whether the source and destination pixel formats happen to match:

| formats | path | orientation |
|---|---|---|
| equal | `blitEncoder->copyFromTexture` | preserved (raw memory copy) |
| differ | fullscreen present-shader draw | **flipped** (`PatchVertexShader` Y-flip) |

So the function's output orientation is a function of a pixel format the
caller may not think it controls. This is not hypothetical: it shipped as
an upside-down screen wipe the moment `mt_hdr_pipeline` made the pipeline
images RGBA16Float while `CreateWipeTexture` still hardcoded BGRA8Unorm
(fixed in `dce604c31` by making the wipe texture follow the pipeline
format, restoring the copy path).

The world view is unaffected because it never goes through this function,
which is exactly what makes the symptom confusing -- "wipe inverted,
worldview fine" reads like a wipe bug and is not one.

**Current callers:** `CreateWipeTexture` (`mt_texture.cpp`) and the
screenshot path. Both are in step today, by hand.

**Fixed 2026-08-01 (2cf256d13), but UNVERIFIED and currently unverifiable.**
The conversion path now flips V (`present.fp` samples at
`TexCoord * UVScale + UVOffset`) so both paths share one orientation.

The branch has **no reachable caller**: `CreateWipeTexture` is the only
caller of `BlitCurrentToImage` and since `dce604c31` always matches the
pipeline format, so the copy path is always taken. The flip direction was
derived from the reported symptom, not observed. The path also logs when
taken, so a future mismatched caller is visible rather than silently
upside down.

To verify: temporarily make `CreateWipeTexture` request `BGRA8Unorm`
unconditionally, run a level-exit wipe under `mt_hdr_pipeline 1`, confirm
the wipe is upright, revert.

**Any new caller must still match the pipeline format** until that is done,
and any format change to the pipeline images must audit this function's
callers.

## ROADMAP-LEVEL: Metal has no HDR pipeline buffer (found 2026-07-31)

Probably the largest single fidelity gap between the backends, and it
explains several things that were previously confusing.

| buffer | OpenGL | Metal |
|---|---|---|
| Pipeline/postprocess textures | `GL_RGBA16F` (`gl_renderbuffers.cpp:246`) | `BGRA8Unorm` (`mt_renderbuffers.cpp:71`) |
| Scene colour | `GL_RGBA16F` (198, 207) | `BGRA8Unorm` (124) |
| Scene normal | `GL_RGB10_A2` (201, 219) | `RGB10A2Unorm` when supported |

**Metal clamps scene colour to [0,1]. OpenGL carries half-float HDR.**

Consequences:

1. **The user's observation that Metal "looks richer and deeper" than GL is
   probably Metal being *less* correct.** 8-bit clamping crushes highlights,
   which raises apparent contrast and saturation. GL preserves headroom and
   rolls it off. Punchier, not better.

2. **The bloom extract's threshold of 1.0 assumes HDR.** It computes
   `max((c + 0.001) * adj - 1.0, 0)`. On Metal `c <= 1.0` always, so the
   term is at most `adj - 1` -- **compute bloom can only fire when
   `exposureAdjustment > 1`.** On GL a bright light can be `c = 4.0` and
   bloom regardless of exposure. This retroactively explains:
   - why Metal bloom is so violently exposure-sensitive, and why
     `gl_exposure_base` dominated every bloom measurement;
   - why the Ashes Afterglow lantern was a **dead scene** for bloom --
     nothing can cross a threshold of 1.0 when the buffer clamps at 1.0;
   - why `gl_exposure_speed 1` had a large enough effect to manufacture a
     visible motion artifact.

**Do not treat this as a straightforward bug to fix.** Two caveats:

- `BGRA8Unorm` may be a deliberate constraint. The Tier 2 direct-composite
  bloom path is gated on `supportsReadWriteBGRA8` and exists *because* the
  format is BGRA8. Moving to RGBA16F changes that design. (Note that path
  has never executed on this hardware -- see the Apple Silicon gap.)
- **Unverified:** whether GZDoom's scene rendering actually emits values
  above 1.0 in practice. If the content is effectively LDR the headroom
  matters less than the argument implies, though the precision/banding
  difference would remain. Establish this before any format change.

### Both caveats resolved 2026-08-01 (beab06c94)

**Caveat 1 -- not a deliberate constraint.** Both reference backends use
half-float for scene colour *and* the pipeline images
(`gl_renderbuffers.cpp:198,246`, `vk_renderbuffers.cpp:134,174`). Metal is
the sole outlier.

The Tier 2 concern was misread. `supportsReadWriteBGRA8` is just
`readWriteTextureSupport() == Tier2` (`mt_version.h:121`) -- the tier gates
`read_write` *access*, not any particular format, and it gates RGBA16Float
exactly as it gates BGRA8. So the format change neither unlocks nor breaks
the direct-composite path; only the flag's **name** goes stale. Write-only
access needs no tier at all, which is why `mt_bloom.cpp` has been creating
RGBA16Float ping-pong textures with `ShaderWrite` on this Tier 1 machine
all along.

**Caveat 2 -- the scene does emit above 1.0, and the bound is 1.4.**
`ProcessMaterialLight` in `material_normal.fp:38`, default blend mode:

```glsl
frag = material.Base.rgb * clamp(color + desaturate(dynlight).rgb, 0.0, 1.4);
```

1.4 is the designed headroom. Blend mode 1 normalizes *to* 1.4; mode 2
(`UNCLAMPED`) is unbounded. `FragColor` is written unclamped
(`main.fp:903`) -- the `min(color, 1.0)` at `main.fp:733` sits *upstream*
of dynamic lights, so it does not bound the output.

This makes the bloom argument numeric rather than inferred. Extract is
`max((c + 0.001) * adj - 1.0, 0)`. GL at c=1.4, adj=1 yields 0.4; Metal
yields identically 0, always. The Afterglow dead scene is fully explained.

### VERIFIED 2026-08-01: HDR recovers highlights; bloom still never observed

Clean 2x2 at one spot in Ashes 2063 "Dead Man Walking" (bright room, pale
wood, strong lamp; probe peak 1.2). All controls passed -- **title bar
identical (max delta 0)**, HUD identical, no movement.

```
A = LDR bloom on    B = LDR bloom off    C = HDR bloom on    D = HDR bloom off
A == B byte-identical      C == D byte-identical      A/B != C/D
```

**Result 1 -- HDR recovers highlight detail, modestly.** Binning by source
luminance isolates it cleanly:

| A luminance | pixels | mean signed (C-A) | max |
|---|---|---|---|
| 0-159 | 780k | -0.10 .. -0.29 | 2-4 |
| **160-191** | 32.6k | **+0.641** | 6 |
| 224-255 (lamp) | 17k | -0.060 | 1 |

Bright saturated surfaces gain; everything else darkens slightly as
auto-exposure answers the higher scene luminance. The lamp is unchanged
because brightmaps clamp to 1.0 in the reference either way. Real,
systematic, and **small** -- 4.5% of pixels, max per-channel 29.

**Result 2 -- bloom did nothing, in either format.** `gl_bloom` on/off is
byte-identical under both. At peak 1.2 the extract yields at most 0.2 on a
few pixels, which is then downsampled 4x and blurred wide; the per-pixel
contribution falls below 1/255 and quantizes away.

**So the tranche's headline claim is still unproven.** "HDR lets compute
bloom fire" has never been observed to change a frame, across two maps,
four capture sessions and two probe designs. The clipping argument is
sound and now measured; the *bloom* consequence is not demonstrated. Do
not write it up as established.

Note also that bloom fires more readily in **dark** scenes, not bright
ones: the extract is `max((c+0.001)*adj - 1.0, 0)` and auto-exposure drives
`adj` far above 1 in the dark, which clears the threshold even with `c`
clamped at 1.0. An early prediction that bloom could not fire under BGRA8
was wrong for exactly this reason, despite the constraint being written
down in the original finding.

### Probe result 2026-08-01: the headroom is real and in use (peak 1.6055)

`mt_hdr_probe 120` in Ashes Afterglow, firing a weapon at close range:

```
peak channel:       1.6055 (frame 112 of 120)
peak pixels > 1.00: 6701 (0.6020%)
peak pixels > 1.20: 194  (0.0174%)
```

**BGRA8 was clipping all of it.** Compute bloom's threshold of 1.0 has
6701 pixels to extract at the peak, where before it had exactly zero.

**Correction to the 1.4 figure recorded above.** 1.4 is the bound for
`uLightBlendMode == 0` only. `lightblendmode` is a MAPINFO map option
(`g_mapinfo.cpp:1628`): DEFAULT, CLAMP_COLOR, and **NOCLAMP**, the last of
which is `frag = Base.rgb * (color + dynlight)` with no clamp at all. The
measured 1.6055 exceeds 1.4, so this content is on the unbounded path.
(Not directly confirmed in Ashes' MAPINFO -- but NOCLAMP is the only path
in the shader that can exceed 1.4.) The practical consequence is that the
HDR case is *stronger* than the 1.4 argument suggested: there is no
ceiling on what an 8-bit buffer discards.

**Why three earlier attempts found nothing.** The route above 1.0 needs a
strong dynamic light on a pale surface at close range, which is a
*transient* -- a muzzle flash or blast. Two still-frame A/Bs and two
single-frame probes in dark corridors could not catch one. The instrument
was wrong for the question, not just the scene. A peak of exactly 1.0000
in those runs was a brightmap: `main.fp:745` clamps brightmaps and
fullbright sprites to exactly 1.0 in the reference, so "1.0000" means a
lamp is in frame, not that headroom is in use. **Do not read 1.0000 as a
clamp bug.**

Superseded, kept for the reasoning trail:

**A/B result 2026-08-01: no visible difference, in two valid scenes.**
The Afterglow lantern pair (a clean pair -- max per-channel delta 2, zero
pixels differing above threshold, so no movement and no HUD change)
showed **max delta 2 across the whole frame, including at the lantern**.
Prediction 1 -- that bloom would visibly fire once the 1.0 threshold
became reachable -- is **not supported**. Mean luminance moved 14.507 ->
14.244, which is a real but sub-perceptual shift.

Do not read this as the format change being wrong; read it as the
*bloom* argument being unconfirmed. The likely explanation is that the
lantern sprite is drawn at 1.0 rather than above it, so nothing in these
scenes actually uses the headroom that ProcessMaterialLight makes
available. Parity with the reference and the precision/banding argument
stand on their own. **Still untested:** a scene with strong dynamic
lights on nearby geometry, which is where the 1.4 clamp actually binds.

**Status:** `mt_hdr_pipeline` CVAR added, defaulting **off**. The hardcoded
formats are gone (`MtRenderBuffers::GetSceneColorFormat` and friends own it
now), so the flip is one CVAR and takes effect on the next frame. Visual
A/B pending.

Three traps found while centralizing, worth remembering:

- The bloom Tier 1 composite PSO and the AO combine PSO both hardcoded a
  BGRA8 colour attachment while rendering into SceneColor, whose format
  came from the render target. Both are now keyed on the scene colour
  format and rebuilt on change. A stale attachment format is a Metal
  validation error, not a silent wrong result.
- **SceneFog must stay 8-bit** -- the reference keeps it RGBA8 too
  (`vk_renderpass.cpp:180` `drawBufferFormats`). Only SceneColor and the
  pipeline images go HDR.
- `mt_pipelinestate.cpp:504` already took attachment 0's format from the
  render target key, so the scene MRT pass needed no change.

**Unverified in beab06c94:** the RGBA16Float branch of
`CopyScreenToBuffer`. The A/B is being run with macOS Cmd+Shift+4 (see the
screenshot note below), so the engine screenshot path never executes. The
LDR branch is byte-identical to before and the split is on an exact format
check, so the default path cannot regress -- but the half-float unpack is
untested code.

**Pre-existing R/B swap in engine screenshots -- FIXED 2026-08-01
(3a2b21ebc).** `CopyScreenToBuffer` read BGRA8Unorm source bytes in memory
order (B,G,R) into a buffer the caller tags `SS_RGB`, so every engine
screenshot had red and blue exchanged. Unrelated to the HDR work; found
while trying to verify it.

Worth recording *how* it was settled, because the swap was reported as
looking fine. Two channel-invariant checks decide it instantly, without a
reference image:

- **The health counter.** GZDoom renders low HP red. The shot was at 10 HP
  and the file showed green.
- **Round-trip the image.** Writing an R/B-swapped copy and looking at
  both is decisive in one glance -- Ashes' brown dirt, the blood on the
  crowbar, and the amber HUD lettering all read as blue-teal in the
  original. `tools/pngdiff.py` has the stdlib PNG reader; the writer is
  ~15 lines of zlib + struct.

Note that magenta/purple regions are **invariant** under an R/B swap, so a
sky can look plausible in both. Judge on a warm/cool axis, never a
magenta one.

The RGBA16Float branch already read channels in the correct order, so
before the fix the two branches disagreed and toggling `mt_hdr_pipeline`
silently changed screenshot channel order.

**Still unverified:** the RGBA16Float branch of `CopyScreenToBuffer` has
never executed. Both screenshot attempts so far were taken with
`mt_hdr_pipeline` off.

## TOOLING GAP: the parity script cannot catch reference divergence

`tools/check_shader_parity.py` compares Metal-native against Metal-inline
against the CPU headers. It is blind to divergence from the **reference**
implementation in `wadsrc/static/shaders/pp/*.fp`.

Demonstrated twice on 2026-07-31: the `depthMask` ramp (0.005 vs the
reference's 0.01) drifted in the inline fallback, then a "parity sync"
propagated it into the shipping `.metal`. After that sync both Metal copies
agreed, the script reported MATCH, and the divergence from the reference was
invisible. The bloom mip-summation bug was the same shape -- self-consistent
Metal code that no longer matched what the reference actually does.

**Highest-leverage tooling addition available:** extend the script to
compare shared numeric constants and structure against the `.fp` reference
shaders. Both of this session's real defects would have been caught
automatically, and both cost hours to find by hand.

## Capture methodology: what a valid A/B scene requires (2026-07-31)

Hard-won during the exposure-response test, which produced two entirely
invalid data sets before the method was right. **Read this before planning
any screenshot A/B.**

**1. `pause` freezes the playsim but NOT the renderer.** Verified: with the
game paused, toggling `gl_ssao_debug 0 -> 2` changed the frame from
mean_lum 12.64 to 248.55. Postprocess re-runs on a paused frame, so `pause`
is the correct tool for holding an animated scene still while still A/B-ing
render settings. (`CHT_FREEZE` exists but is only reachable from bot code in
this build -- there is no console cheat for it.)

**2. Animated scene lighting invalidates everything.** Ashes 2063 MAP51's
campfire scene dims and brightens as part of the mod. A same-config control
pair three minutes apart differed by **12.4% of frame, mean delta 0.678** --
larger than the effect under test. Every conclusion drawn from that set was
noise. Always shoot a same-config control pair *in the same session* and
treat its delta as the noise floor; any result smaller than it is not a
result.

**3. Scene must actually exercise the effect.** The Ashes Afterglow lantern
in the dark area is a **dead scene for bloom** -- nothing crosses the
threshold, so `mt_compute_bloom_composite` 0 vs 2 and `gl_exposure_base`
0.35 vs 10 all produced *byte-identical* frames. Known-good bloom location
is the **Ashes 2063 Enriched MAP51 solar lantern** (`gl_bloom` 0 vs 1
differs across 9.4% of frame there).

**4. Diagnostic signature of each failure mode:**
- Shots that should differ are byte-identical -> dead scene (or a cvar that
  did not apply).
- Shots that should be identical differ -> scene animation or adaptation
  drift.
- Both at once -> you have the labels wrong.

**5. Exposure direction, for sanity-checking results.**
`exposureAdjustment = 1.0 / max(ExposureBase + light*ExposureScale, ExposureMin)`
with defaults `scale 1.3, min 0.35, base 0.35`. So base 0.35 -> adj ~2.4,
base 10 -> adj ~0.099. **Base 0.35 must be the brighter frame.** If a data
set says otherwise, the data set is wrong. `CameraTexture` feeds *only* the
bloom extract -- no tonemap path -- and `PPCameraExposure::Render` reads the
pre-bloom scene, so there is no bloom->exposure feedback loop to blame.

## Compute-bloom exposure response: inferred, not directly measured

The direct test (does compute bloom respond to `gl_exposure_base` 0.35 vs
10?) was attempted twice and abandoned -- both data sets were invalidated by
the traps above. It is **not** recorded as verified.

It is however strongly implied by the convergence result. Pre-fix, with
compute ignoring exposure entirely, the two paths differed across 3.8% of
frame at peak -7 luminance. Post-fix they agree to 81 px (0.0089%, mean
signed +0.0002). The reference definitely applies exposure (stock
`bloomextract.fp`, adj ~2.4 at defaults), so agreement that tight is only
possible if compute applies essentially the same factor. The residual
loophole -- compute applying a *constant* near 2.4 rather than sampling the
texture -- requires a bug nobody wrote, since the extract is byte-matched to
the reference and visibly samples `exposureTex` under `params.useExposure`.

## AO combine: two findings, BOTH NOW CLOSED (2026-07-31, closed 2026-08-02)

Both were still recorded as open here until 2026-08-02, when a source audit
found they had already been fixed and this section had gone stale. Verified
against HEAD before rewriting. **If you are reading a handoff that lists
either as open, the handoff is stale, not this section.**

**1. `ssao_combine` (dead compute kernel) -- DELETED in `f3f62c912`.**
It was never compiled into a PSO (only `ssao_combine_fs` was), and it lacked
the reference's validity guard and `depthMask` ramp, so it read as a bug in
the shipping path while being unreachable. No `ssao_combine` symbol remains
in `mt_ao.metal`; every surviving AO entry point has a PSO construction path
at `mt_ao.cpp:1339`.

**2. `depthMask` ramp rate -- RESTORED to the reference's `0.01` in
`aba5fba35`.** It had been `0.005` (half rate, so AO faded in too slowly
with distance). The live Metal combine path now matches
`ssaocombine.fp:33`'s `1.0 - exp2(-x * 0.01)` at `mt_ao.metal:1097`, and the
low-resolution branch uses `0.01` at `mt_ao.metal:1131`. The
"deliberate tuning or drift?" question is resolved as drift.

`tools/check_shader_parity.py` passes for all AO and bloom functions and
shared structs as of 2026-08-02.

## Not a renderer bug: pixel bleedthrough in Ashes Enriched (2026-07-31)

User-reported bleedthrough on some Ashes Enriched levels. **Reproduces
identically on both the Metal and OpenGL backends, and is present with AO
both on and off.** Therefore not a Metal issue and not AO -- it is mod
assets or shared engine code. Recorded so it is not re-investigated as a
backend bug. Backend switching for this kind of test:
`vid_preferbackend` 0=OpenGL, 1=Vulkan, 3=Metal (`v_video.cpp:122-140`),
restart required.

Related observation, **impression not measurement**: the Metal backend looks
visibly "richer/deeper" than OpenGL on the same scene. It is *not* pixel
format -- scene colour is `BGRA8Unorm` on Metal and 8-bit on GL, and both
use RGB10A2 for scene normals (`gl_renderbuffers.cpp:201,219`), with Metal
only ever *falling back* below that. The likely cause is the compute AO
being a more capable implementation than the stock PP SSAO. To settle it:
three captures at one spot -- Metal `mt_compute_ao 1`, Metal
`mt_compute_ao 0`, and OpenGL.

## Screenshot A/B tooling (added 2026-07-30)

`tools/cluster.py`, `tools/localize.py`, `tools/pngdiff.py` -- stdlib-only
(no PIL/ImageMagick on this machine). Use them instead of eyeballing:

    python3 tools/cluster.py shot*.png      # are any of these actually different?
    python3 tools/localize.py a.png b.png   # where, how much, and which direction?

Standing rules learned from the bloom work, in the scripts' docstrings too:

1. Run a control that is *supposed* to differ (e.g. `gl_bloom 0` vs `1`)
   before comparing anything subtler. Seven shots once came back as one
   distinct image because the scene never exceeded the bloom threshold.
2. Run a *same-config* control pair. Drift has exceeded the measured effect
   by 26x on this machine.
3. `gl_exposure_speed 1` before any bloom A/B (default 0.05 adapts for tens
   of seconds). Reset to 0.05 when done -- at 1 it fabricates a motion-only
   ghosting artifact under compute bloom.
4. Difference shape identifies cause: compact one-directional blob at a light
   = real bloom change; broad wash brightening over time = exposure drift;
   scattered and bidirectional = animation between captures, retake.

## STYLEOP_Shadow blend was wrong on Metal — found by audit, fixed and MEASURED (2026-08-03)

**Status: closed. Fix verified on screen against the Vulkan reference.**

`STYLEOP_Shadow` is enum value 8 (`renderstyle.h:96`), and `vk_renderpass.cpp`'s
`renderops[]` holds `-1` for every index above `STYLEOP_RevSub`, so the
reference falls into the "this was a fuzz style" branch:

    reference   Cout = Cs*Cd + Cd*(1 - As)
    Metal was   Cout = Cd*As

With `hw_sprites.cpp:160`'s `SetColor(0.2, 0.2, 0.2, fuzzalpha=0.44)` for
shadow sprites that is `0.76*Cd` against `0.44*Cd` — **Metal 42% too dark.**
Fixed in `fde91b90b` by routing `STYLEOP_Shadow` through the fuzz mapping
(`DestinationColor` / `OneMinusSourceAlpha`, Add) on both RGB and alpha —
alpha included because ZVulkan's `ColorBlendAttachmentBuilder::BlendMode`
assigns src/dst/op to the alpha slots too.

**The error was alpha-dependent, not a constant 42%.** Reference is
`(1.2 - As)*Cd`, old Metal was `As*Cd`; they cross at `As = 0.6`. The fog
branch (`hw_sprites.cpp:147-157`) scales `fuzzalpha` DOWN, so fogged shadow
sprites diverged *further*, never less. Nothing in normal play reaches the
crossover.

**How to reproduce a `STYLEOP_Shadow` sprite at all:** `r_drawfuzz 2`
converts every fuzz style into `STYLEOP_Shadow` (`renderstyle.cpp:151-154`).
That is the practical repro — `summon Spectre` with `r_drawfuzz 2`. At the
default `r_drawfuzz 1` you get `STYLEOP_Fuzz` and never touch this code. No
stock actor uses `STYLE_Shadow`; only `udmf.cpp:733`'s thing flag and mod
content do. **So this bug was real but not something every player hit.**

### The measurement, and the technique that made it possible

DOOM 2 MAP01, `r_drawfuzz 2`, `summon Spectre`, `pause`, Metal vs Vulkan
(`vid_preferbackend` 3 vs 1, restart between).

**A whole-frame diff was useless and will be again.** `pngdiff` reported 92.7%
of pixels differing, mean delta 24.4, bidirectional. Two confounds: the
restart forces relocation (the Spectre landed 20px higher in the Vulkan shot),
and the backends differ *globally* — the same "richer/deeper" difference
already noted in this file. A cross-backend whole-frame comparison cannot
isolate one sprite's blend mode. **Do not attempt it.**

**What worked: an intra-image ratio.** Measure the shadowed region against
unshadowed floor *on the same scanlines* within each image separately, then
compare the two ratios. Scale-invariant, so camera offset and global backend
differences both cancel.

    Metal    luminance ratio 0.679   per-channel 0.737 / 0.649 / 0.664
    Vulkan   luminance ratio 0.676   per-channel 0.724 / 0.651 / 0.666

Agreement to **0.003 (0.4%)**. Had the fix not taken, Metal would read
`0.44/0.76 = 0.58` of Vulkan's ratio — about 0.39 against 0.676, a gap ~100x
larger than observed. The discrimination was not marginal.

**Caveats, stated:** the absolute ratio is 0.68, not the modelled 0.76 —
silhouette edge pixels with partial sprite coverage pull the box mean, and the
floor reference box is not a perfect texture match for what sits under the
sprite. It applies equally to both backends, so it does not weaken the
agreement, but the absolute value does NOT independently validate the
arithmetic. This establishes that **Metal now matches the reference**, not
that both are correct.

`r_drawfuzz` is `CVAR_ARCHIVE` (`renderstyle.cpp:40`) and read back as `2`
from the ini after the restart, so the changed branch provably ran in both
captures. Check this class of thing before trusting any A/B — it is the same
"did the setting apply" trap that has invalidated data sets here before.

**Reuse the ratio technique** for any cross-backend comparison of a localized
effect. Whole-frame statistics are dominated by global backend differences and
will hide the thing you are looking for.

## COMPUTE BLOOM: the four-session null was CONFIG; a real difference remains, cause UNKNOWN (2026-08-03)

**Two findings. The first explains every previous null result; the second is
the answer to this branch's longest-standing open question.**

### 1. `gl_exposure_base=10` in the ini made bloom mathematically impossible

Left over from the abandoned exposure-response test. Work it through:

    exposureAdjustment = 1 / max(base + light*scale, min)   -> ~0.088-0.099 at base 10
    bloom extract       = max((c + 0.001) * adj - threshold, 0), threshold = 1.0
                                                    (hardcoded, mt_bloom.cpp:402)
    => a pixel must exceed c ~ 10.1 to contribute anything
    measured scene peak (mt_hdr_probe) = 1.6055

**Bloom extract output was identically zero, on both paths, under both pixel
formats.** That is precisely the recorded symptom — "compute bloom has never
been observed to change a frame, across two maps, four capture sessions and
two probe designs", and `gl_bloom` 0 vs 1 byte-identical under LDR and HDR.

Restoring `gl_exposure_base 0.35` (with `gl_bloom 1`) made bloom fire
immediately: `gl_bloom` 0 vs 1 now differs across **10.94% of frame**,
unidirectional brightening, max delta 16. **First time bloom has been observed
to change a frame on this branch.**

This cannot be *proven* retroactively for the earlier sessions — the ini is not
version controlled, so there is no history to check. But it is a leftover from
a test those same sessions ran and it produces exactly the observed signature.
CLAUDE.md's "check gzdoom.ini before diagnosing a renderer bug" has now
resolved a second multi-session investigation.

### 2. Compute bloom and the reference use DIFFERENT ALGORITHMS

Measured at the Ashes 2063 Enriched MAP51 solar lantern, each path against a
`gl_bloom 0` baseline. Noise floor was exactly **zero** — two same-config
captures a minute apart were byte-identical (`gl_bloom1.png` and
`mt_compute_bloom1.png` share an MD5), so the scene is perfectly static and
every delta below is signal.

                        bbox        aspect  centroid        peak   energy   px>2
    reference PP     469 x 154     3.05:1  (798.6,424.6)   29.9   424242   47328
    Metal compute    316 x 296     1.07:1  (794.3,478.4)   16.2   347624   54291

Compute spreads wider and flatter-peaked over more pixels with ~18% less total
energy. Root cause, confirmed in source:

**THE CAUSE IS NOT ESTABLISHED. A first attempt at it was WRONG — retracted
here so nobody rebuilds on it.**

The retracted claim was that Metal sums all four bloom levels while the
reference composites only level 0. It came from reading
`bloom_combine_contrib_all` (`mt_bloom.metal:124-143`), which does sum four
weighted levels — **without reading its caller.** `mt_bloom.cpp:536-541` sets
`strength[1..3] = 0.0f`, so only level 0 contributes, at unit gain, and the
comment there says so explicitly. The all-levels summation was a real bug but
it was **already fixed on 2026-07-30** (`mt_bloom.cpp:430-442` records the old
1.0/0.18/0.09/0.06 weights measuring ~2.1x the reference). Reading a kernel
without its binding site is exactly the error `audit-brief-02.md` warns against.

**Ruled out by inspection — do not re-check these:**

- Level count: both 4 (`NumBloomLevels == 4`; Metal is bloomA + 3 mips).
- Level 0 resolution: both quarter-res of `mSceneViewport`. Reference
  `((w+1)/2+1)/2` (`hw_postprocess.cpp:53-58`), Metal `(srcW+3)/4`
  (`mt_bloom.cpp:387-390`).
- Composite weights: level 0 only, unit gain, both.
- Resample: `downsample_box` (`mt_bloom.metal:111-122`) is a single bilinear
  sample despite the name — functionally the same as `bloomcombine.fp`, which
  is a plain bilinear copy. Used for both the down and up legs on both sides.
- Blur pyramid structure: blur-then-downscale walking down, blur-then-upscale
  replacing on the way back up, then a final blur of level 0. Matches.

So the measured difference is real and reproducible, and the structures agree
on every axis checked so far. **Open question, and now the highest-value one:
what actually produces it?** Remaining unexamined candidates include the blur
weight computation and whether Metal reads `gl_bloom_amount` at all, the
per-level texture dimension rounding as the pyramid descends, filter/edge
behaviour at level boundaries, and the composite's alpha handling
(`bloomcombine.fp` writes alpha 0, the Metal kernel writes 1.0).

Hand the measured signature above to the shader-parity audit — a table of
numbers to explain is far more use to it than the brief alone.

### Method notes

- **Check MD5s first, again.** The identical hash between `gl_bloom1.png` and
  `mt_compute_bloom1.png` was not an error -- `mt_compute_bloom` was already 1.
  It handed us a zero noise floor for free.
- **A whole-frame diff was fine here** because the camera did not move and the
  difference is localized; the flat surround is what proves the mid-session map
  reload did not relocate the view, and that the `gl_exposure_base 0.05`
  excursion in the console log is not in these captures.
- **A wrong hypothesis, recorded so it is not re-derived:** the 3.05 vs 1.07
  aspect difference looked like a normalized-UV vs texel-offset porting error.
  `blur.fp` disproves it -- the reference uses `textureOffset(..., ivec2(+-1..3, 0))`,
  texel offsets, same as the compute port. The spread comes from the mip chain,
  not the kernel.

## Audit round two: three confirmed source divergences, none yet fixed (2026-08-03)

From `audit-brief-02.md`'s results section (appended at `:269`). **All three
independently verified against the tree before recording here.** None is fixed
yet; none has been seen on screen.

### 1. Tier 1 bloom composite omits the Y flip the AO path documents

`bloom_vs` computes `out.uv = out.position.xy * 0.5 + 0.5` with no inversion
and `bloom_fs` samples `in.uv` directly (`mt_bloom.metal:175-190`). But
`mt_ao.metal:1084-1089` carries an explicit comment — "Metal's fullscreen-
triangle UV is vertically opposite to the scene render textures" — and applies
`float2(in.uv.x, 1.0 - in.uv.y)`. Native shaders bypass `PatchVertexShader`,
while the PP `screenquad.vp` path is processed by it, so the correction has to
be manual and bloom does not do it.

**This is a real inconsistency between two Metal paths.** It was found by
comparing Metal against Metal rather than against the reference — a technique
worth reusing.

**Leading candidate for the 54px Y-centroid displacement** in the bloom
signature, and it cannot explain the rest: a pure vertical reflection
preserves bbox extent, peak, energy and pixel count, so `469x154 -> 316x296`,
the lower peak and the 18% energy loss all remain unexplained.

**CAREFUL — one claim in the audit is overstated.** It reports the predicted
reflected centroid as "exactly" the measured value via `2 * 451.5 - 424.6 =
478.4`. That axis of 451.5 is the *midpoint of the two measured centroids* —
it was fitted to the data, not derived independently from the scene viewport
geometry. So the numbers are **consistent with** a vertical flip; they do not
confirm one. The code evidence carries this finding, not the arithmetic.

Settle it: capture Tier 1's `mCompositeTex` before the raster draw, then make
only `bloom_fs` sample `float2(in.uv.x, 1.0 - in.uv.y)` and re-run the
five-number capture. Y centroid should reflect back to ~424.6 with the other
four metrics unchanged. **Also check the prior 81-pixel agreement result** —
the audit notes it may be inconsistent with this source path if both captures
used Tier 1, and that deserves checking rather than explaining away.

### 2. Bloom mip chain rounds the wrong way

Reference halves iteratively with `(prior + 1) / 2` — ceiling
(`hw_postprocess.cpp:54-68`). Metal uses `width >> i` / `height >> i` — floor
(`mt_bloom.cpp:350-361`). Odd lower mip heights therefore differ by one texel,
giving the coarsest vertical blur a slightly larger footprint after upsampling.

Direction matches the observed vertical broadening, magnitude does not: this
cannot credibly produce a 92% height increase or the energy/peak change. Real
divergence, small contributor at most.

### 3. Metal writes depth when depth testing is disabled — CORRECTNESS BUG

**The most valuable finding of the round, and independent of bloom.**

    Vulkan   pipelineKey.DepthWrite = mDepthTest && mDepthWrite   (vk_renderstate.cpp:217-226)
    Metal    pipelineKey.DepthFunc  = mDepthTest ? mDepthFunc : DF_Always
             pipelineKey.DepthWrite = mDepthWrite ? 1 : 0          (mt_renderstate.cpp:750-752)

So a draw with `EnableDepthTest(false)` and the depth mask still true writes
every covered fragment's depth on Metal and none on Vulkan. In a reverse-Z
scene each such write can change whether a later `DF_Less` draw is occluded.

**Reachable:** `DrawEndScene2D` disables depth testing for player/HUD sprites
without disabling the mask (`hw_drawinfo.cpp:964-985`). Other paths are safe
because they explicitly clear the mask — `Draw2D` (`hw_draw2d.cpp:70-75`) and
the projected-plane stencil fill (`hw_flats.cpp:281-299`).

**Fix is to key the write on `mDepthTest && mDepthWrite`, matching Vulkan —
NOT to touch the reverse-Z compare mapping**, which is deliberate and
documented. Stencil function, reference, operation and masks otherwise match.

### Properly eliminated by this round — do not re-open

`gl_bloom_amount` **is** read and passed unchanged on both sides (PP via
`ComputeBlurSamples` at `hw_postprocess.cpp:104-106`; Metal from
`mt_postprocess.cpp:531` into `ComputeBloomBlurSamples` at
`mt_bloom.cpp:381-412`). The compute-only alpha `1.0` is consumed by a
fragment shader returning alpha `0.0` under additive RGB blend, so it cannot
affect the measured RGB delta. The reference blur's implicit sampler is
nearest where the native blur hardcodes linear, but all seven coordinates are
texel centres, so it can only alter edge/rounding samples.

**No single known source difference explains all five signature numbers.** The
next measurement should isolate the intermediate `bloomA` / `mCompositeTex`
outputs, which separates an upstream pyramid discrepancy from the final Y flip.

## Audit round two: all three divergences FIXED in source, none yet on screen (2026-08-04)

Commits `9d19739e4` and `3cb088b89`. Build green, `check_shader_parity.py`
passes. **Every claim below is a source claim.** Nothing in this section has
been seen on screen; the captures are the next session's job and the
predictions are stated here so they cannot be adjusted afterwards.

### The three round-two findings, now fixed

**1. Tier 1 bloom composite sampled the contribution upside down.**
`bloom_fs` now takes `float2(in.uv.x, 1.0 - in.uv.y)`. Confirmed in source
before changing: `bloom_combine_contrib_all` writes `mCompositeTex` at `gid`,
i.e. texture order with v=0 the top row, while `bloom_vs` sets
`uv = position.xy * 0.5 + 0.5` from clip space where +Y is up. Native
libraries bypass `PatchVertexShader`, so nothing reconciled them. Same
correction and same reason as `ssao_combine_fs`.

`bloom_vs`/`bloom_fs` are used by nothing but this composite (checked), so the
fix is contained. The Tier 2 read-write kernel writes in texture order and
never needed it.

**The prior 81-pixel agreement is NOT inconsistent with this, and that worry
is now closed.** The audit flagged it as something to check rather than
explain away — correct instinct, but the extract-stage comparison never runs
through the composite, so a broken composite could not have shown up in it.
No contradiction to chase.

**2. Bloom mip chain rounded the wrong way.** Now iterative ceiling-halving,
matching `hw_postprocess.cpp:54-68`. Level 0 already agreed —
`ceil(w/4) == ceil(ceil(w/2)/2)` — and every consumer (`blurLevel`,
`resample`, the composite) reads the textures' real `width()`/`height()`, so
the allocation in `CreateTextures` was the entire divergence.

**3. Depth writes while depth testing disabled.** `pipelineKey.DepthWrite` is
now `(mDepthTest && mDepthWrite)`, matching `vk_renderstate.cpp:217`. The
reverse-Z compare mapping was NOT touched.

### Predictions, stated before the captures

The branch rule is a numeric prediction before looking. These are it.

**Bloom, Ashes Enriched MAP51 solar lantern, each path against a `gl_bloom 0`
baseline.** The two-sided test — both halves must hold:

- Y centroid moves from the measured 478.4 to **424.6**, the reference value.
- *Independently*, `424.6 == 2 * (mSceneViewport.top + height/2) - 478.4`.
  **Read the real viewport at capture time and check this.** AGENTS.md's own
  note on the round-two finding is right that the 451.5 axis was fitted to the
  midpoint of the two measured centroids, so quoting it back as confirmation
  is circular. Deriving it from scene geometry is what makes this a test.
- X centroid unchanged (~794-798; it already agreed to 4px).
- bbox, peak, energy, px>2 near their measured compute values
  (316x296, 16.2, 347624, 54291) apart from a **small vertical contraction**
  from fix 2. A pure reflection changes none of them.

**If bbox instead jumps to 469x154 and peak to 29.9 — the reference values —
that is a good outcome but the model was wrong**, and the reason needs
finding rather than banking. Neither fixed mechanism predicts it: a flip
preserves all four, and one texel of mip rounding cannot produce a 92% height
change.

**Depth writes:** the expected result is *no visible change in normal play*.
The test is the two-primitive one — disable the test with the mask still true,
write a known depth, re-enable `DF_Less`, draw an overlapping primitive.
Metal should now retain the original depth as Vulkan does. A visible change
in HUD/player sprites would mean something else depended on the old behaviour.

### Also fixed: the deferred cleanups (`9d19739e4`)

- `MtAOModule::Execute` took `fogTex` and `combineTex` and read neither. The
  combine pass binds `SceneFog` itself; `combineTex` was `nullptr` at the sole
  call site. Both params gone.
- `combinePSO` was declared and released but never assigned — the combine pass
  is the `ssao_combine_vs`/`_fs` raster pair, not a kernel. Gone.
- The dither texture, superseded by the world-locked noise, was still created,
  still bound at `texture(0)`, and still **gated compute AO in two places**
  (`Render()` and `Execute()` both bailed without it) for a texture no kernel
  read. All of it gone, including the `[[texture(0)]]` params in all three
  kernels.

  **The deferral comment was wrong about the cost**: it said removal meant
  renumbering every subsequent texture index in the kernel, `Execute()`, and
  the other two kernels. The indices are explicit `[[texture(n)]]` attributes,
  so nothing else moved. That is why this was cheap after months of deferral —
  worth remembering the next time a comment prices its own cleanup.

- Sampler W address mode: `vk_samplers.cpp` passes `REPEAT` as the third
  `AddressMode()` argument at **all four** of its call sites. Metal derived W
  from `mClampMode`. No visual effect — everything sampled there is 2D — but
  it split the sampler cache into keys the reference never has. The
  postprocess path still sets W from its wrap mode, which is what its own
  reference (`vk_samplers.cpp:162`) does.

### Tooling

`check_shader_parity.py` gained **`native raster bloom combine flips V into
texture order`**. The V-flip is exactly the class the script exists to hold —
a one-line convention whose absence is invisible in review and expensive to
find by measurement.

### Still not verified on screen

The three fixes above, plus `CLAMP_CAMTEX` and the `NOFILTER` sampler changes
carried over from 2026-08-03, which still rest on reading alone. CAMTEX needs
a map with a camera texture whose UVs cross the edge.

**Before capturing, delete `~/Library/Application Support/zdoom/cache/
mt_pipelines.bin`.** A stale PSO binary archive can mask a native-shader
change — this is a recorded trap and the bloom fix is exactly the kind it
hides. It was moved aside on 2026-08-04 and regenerates on launch.

## Audit round three (2026-08-04): render state and bloom clean, one real instrumentation defect

Run against `643ab33a2`. Two of three tasks came back clean, which is itself
the result — they were the two most likely places for a remaining bloom or
constant-binding defect, and they are now eliminated by reading.

**Task 1, MtRenderState constants — CLEAN.** Vulkan's set-1 bindings map
deliberately onto Metal indices (light 16, viewpoint 17, bones 18, matrices
19, stream data 20), the inline buffer at index 21 is the correct equivalent
of Vulkan push constants with every consumed field and offset preserved, and
inline constants are explicitly invalidated for every new render encoder
(`mt_renderstate.cpp:1632`), which rules out the encoder-transition
stale-constant path. No field, offset, binding or staleness divergence.

**Task 2, the bloom remainder — CLEAN, no new source candidate.** All three
areas named unexamined by round two came back matching: the blur's linear
samples at constructed texel centres land on the same centres as the
reference's nearest `textureOffset` at every level, clamp-to-edge agrees for
out-of-range taps, and `(gid + 0.5) / destExtent` is the same
destination-pixel-centre coordinate the reference screen quad generates, on
both the downscale and the upscale-replace leg.

**This is important for the pending capture.** There is now no known
source-only mechanism left that predicts a visible bloom mismatch beyond the
two already fixed. If the re-measurement still shows a large gap, the cause is
not in the shader constants or the resampling coordinates, and the next step
is isolating the intermediate `bloomA`/`mCompositeTex` outputs rather than
another source read. Three source reads have now been spent on this.

### The one real finding: TextureUploadCPU misses the async upload path

**VERIFIED IN SOURCE, and it contradicted this document — the doc was wrong.**

`RecordTextureUpload` was called from exactly one place, the synchronous
`CreateImage` blit path (`mt_texture.cpp:495`). But normal precaching queues
async loads (`mt_renderdevice.cpp:807`) whose GPU upload runs in
`PerformAsyncGPUUpload`, which does the same staging memcpy + blit encode +
separate command-buffer commit and **never recorded a timing**.

The `extraCommandBuffers` counter *does* cover both, because it increments
centrally in `GetBlitCommandBuffer`. So the two metrics disagree exactly when
the metric matters most: a genuine precache cold load would show extra command
buffers while `TextureUploadCPU` sat at zero.

This also resolves the long-standing "the upload timer has never fired"
item — **and the prior all-zero readings are still valid as evidence that no
upload occurred**, since the sync path was instrumented correctly. The defect
is that a *future* cold load would have been missed too, and silently.

Fixed 2026-08-04: `PerformAsyncGPUUpload` now times the same span as the sync
path. **Not yet observed firing** — that needs a real precache cold load.

### Verdicts on the other two long-open items

- **`BlitCurrentToImage`'s conversion branch is genuinely unreachable.**
  `CreateWipeTexture` is the only caller and creates the destination in the
  pipeline format immediately before calling it (`mt_texture.cpp:214`). Keep
  it as a guarded future conversion path rather than delete it, but note its
  V-flip is still unverified — and given the composite V-flip found in round
  two, that neighbourhood has now produced one real orientation bug. The
  forced-format wipe test is the right way to settle it.
- **Round-1 Finding 11 is CLOSED as not a divergence.** `EnableMultisampling`
  and `EnableLineSmooth` are empty no-ops in *Vulkan too*
  (`vk_renderstate.cpp:173-179`) — only OpenGL toggles those legacy raster
  states. Metal matches the reference. This was carried as an open item for
  weeks; it was never a gap.
- **Round-1 Finding 12: the `128.0f` depth-bias factor is a FITTED constant.**
  Not derivable from `Depth32Float` or the reverse-Z mapping; history shows
  `1/2^24` was introduced as a "reasonable" 24-bit emulation and the `* 128`
  was added later with no derivation and no measurement. At the common
  `units = -128` it produces a `+0.0009765625` reverse-Z offset, 128x the
  preceding formula. **Left in place** — changing it needs a controlled
  decal/coplanar-plane sweep against Vulkan, not a code review — but the
  comment at `mt_renderstate.cpp:829` now says plainly that it is fitted, so
  nobody derives anything else from it.

## MEASURED 2026-08-04: the bloom V-flip fix is CONFIRMED; a broader-blob difference remains

Captures at Ashes Enriched MAP51, Metal Tier 1, scene viewport `0,0 1440x773`
(from `mt_caps`), `gl_exposure_speed 1`. Five Cmd+Shift+4 window captures at
1552x913, measured with the new `tools/blobstats.py`.

**Both controls passed**, which is what makes the rest trustworthy:
bloom-off before and after the whole sequence were **byte-identical**
(`b878ce7b`), and the compute config captured twice was **byte-identical**
(`80f4c7a9`). Zero noise floor. Negative-delta pixels were ~0.1% of the
positive count on both paths, so both are cleanly additive.

                     bbox        aspect  centroid        peak   energy    px>2
    reference PP   539 x 334    1.61:1  (834.5,412.5)   43.0    823856   86911
    Metal compute  471 x 436    1.08:1  (829.1,410.8)   37.0   1092532  116478

**The Y centroids agree to 1.7px, X to 5.4px.** Before the fix they differed
by 53.8px. The composite V-flip (`3cb088b89`) is confirmed on screen.

This is much better evidence than the arithmetic that motivated the fix. The
round-two audit offered `2 * 451.5 - 424.6 = 478.4` as confirmation, with the
axis fitted from the midpoint of the two measurements it was explaining. The
within-session test needs no axis at all: the two paths now land in the same
place. **Prefer a test that compares two live measurements over one that
compares a measurement to a number derived from it.**

### The prediction was right in substance and wrong in coordinates

`AGENTS.md` predicted a Y centroid of **424.6**. That number was never
reachable: it came from a session at a different viewport, and these are
window captures including chrome. Absolute positions do not survive across
capture geometries. Stating the prediction as "Metal's Y centroid will equal
the reference's Y centroid, both measured this session" would have been both
falsifiable and reachable. **Predict a relationship between two things you
will measure together, not a constant from an old session.**

### RETIRE the historical five-number table -- do not reconcile it

    (2026-08-03)     bbox        aspect  centroid        peak  energy  px>2
    reference     469 x 154     3.05:1  (798.6,424.6)   29.9  424242  47328
    Metal         316 x 296     1.07:1  (794.3,478.4)   16.2  347624  54291

Those numbers were taken with the compute blob **mirrored onto different
scene content**, so its thresholded bbox, energy and pixel count were
measuring a different part of the image than the reference's. They are not a
valid target and were never a valid target. The evidence: the energy ratio
**reversed** direction after the fix, from 82% of reference to 132%. A pure
reflection cannot change total energy; a reflection onto different content
can change *thresholded* energy in either direction, which is exactly what a
mirrored measurement looks like in hindsight.

### What remains, and where to look next

With position resolved, Metal's bloom is **broader and stronger**: 34% more
pixels above threshold, 33% more energy, at 86% of the peak, in a square blob
(1.08:1) against the reference's wide one (1.61:1). Aspect is the one metric
whose direction survived the fix unchanged.

Broader + lower peak + more total energy is the signature of **coarser pyramid
levels contributing too much**. Audit round three eliminated tap placement,
the Gaussian exponent, the amount floor, and the resample coordinate
convention by reading, and round two eliminated level count, level-0
resolution, composite weights and the resample kernel. What that leaves is the
**upscale-replace leg** -- whether Metal's walk back up the pyramid replaces
each level as the reference does, or accumulates.

That is a source question again, and it is now narrow enough to be worth one
more read. But **dump the intermediate `bloomA` and per-level textures first**
if it does not fall out immediately; three source reads have been spent on
this effect and the next cheap win is intermediate data, not a fourth read.

### Method notes that cost time this session

- **GZDoom's `-exec` runs before `Init complete.`** and `+screenshot` never
  fires. Neither can drive a capture sequence; the whole script executed with
  no map loaded and no frames rendered. Captures are manual.
- **The console pauses single-player, and `GetScreenshotBuffer` does not
  re-render** -- it re-presents the existing pipeline image
  (`mt_renderdevice.cpp:1004`). So a screenshot taken from an open console
  reflects the last frame rendered with the console DOWN. An earlier attempt
  this session produced seven byte-identical shots across four cvar toggles
  purely from this. Set the cvar, **close the console**, let it render, then
  reopen and shoot.
- **Never use `;` to chain console commands in a capture sequence.** A shot
  that came out identical to bloom-off was almost certainly
  `gl_bloom 1; mt_compute_bloom 0` not applying `gl_bloom`. `Pass1` runs the
  exposure pass whenever `bloomEligible` is true, so *any* gl_bloom-on frame
  differs from a gl_bloom-off frame even before bloom is composited -- which
  makes "identical to bloom off" a reliable tell that `gl_bloom` never took.
- Run `mt_caps` immediately before each shot. It prints the resolved bloom
  path and both cvars, which turns "which config was this file?" from
  inference into a log line.

## dispatchThreads was NOT the bloom cause (measured 2026-08-04, round two of captures)

`29cdcd619` routed all 12 compute dispatches through `MtDispatchThreads`,
which rounds up to whole threadgroups where `dispatchThreads` is unsupported.
Re-measured at the same viewpoint (savegame-pinned) with A/B/C each set from
the command line so no console toggle was involved.

                     bbox       aspect  centroid       peak   energy   px>2
    reference PP   533 x 212    2.51:1  (746.4,409.6)  46.0   695117  67263
    Metal compute  427 x 357    1.20:1  (749.7,416.4)  35.0   955928  90441

Ratios Metal/reference, against the previous round's at a different viewpoint:

                 round 1    round 2
    energy         1.33       1.38
    px>2           1.34       1.34
    peak           0.86       0.76
    aspect         0.67       0.48

**Nothing moved toward 1.0.** The driver was rounding partial threadgroups up
-- the harmless branch -- so the conformance fix changed no pixels here.
Keep it (the old code was outside the API contract on a GPU reporting
Common3 = no / Mac2 = no), but it is **eliminated as the bloom cause**.

### The signature is stable, and it is ANISOTROPIC -- restate it that way

Two independent rounds at different viewpoints agree closely on the ratios
(px>2 1.340 vs 1.345, energy 1.33 vs 1.38). This is a reproducible property,
not a single measurement.

"Broader" was the wrong description and it sent the search toward energy and
gain. The real signature is **directional**: Metal's blob is **68% taller and
20% narrower** than the reference's (427x357 against 533x212). Both rounds
agree on the direction. The reference blob is strongly elongated horizontally;
Metal's is nearly isotropic.

That matters because the blur *should* be isotropic in screen space on both
paths -- level 0 is 360x194 for a 1440x773 viewport, so one texel is ~4 screen
px in x and ~3.98 in y, and both paths step whole texels with identical
weights. **A path that is isotropic where the reference is 2.5:1 elongated is
the anomaly to explain, and the elongation is on the REFERENCE side.** Do not
assume Metal is the one that is wrong about shape until the intermediate
levels say so.

Centroids still agree (X 3.3px, Y 6.8px), so the V-flip fix holds at a second
viewpoint.

### Next step is the intermediate dump, not another A/B capture

Every static candidate is now eliminated: level count, level-0 resolution,
composite weights, resample kernel, pyramid structure (round two); tap
placement, Gaussian exponent, amount floor, resample coordinates (round
three); upscale-replace leg, blur weight normalization, blur tap offsets, and
dispatch conformance (this session). Four source reads have been spent.

Dump `bloomA` and each pyramid level to disk on both paths and compare them
stage by stage. That localizes the divergence to a stage instead of inferring
it from the composite, and the anisotropy gives it a specific question to
answer: **at which level does the vertical extent first exceed the
reference's?**

### Capture protocol that finally worked -- use this one

Three capture rounds were lost to avoidable causes: console-pause staleness,
a reversed toggle order, and viewpoint drift between sessions. What works:

1. **Pin the viewpoint with a savegame.** Walk to the spot once, `save
   capspot`. Absolute pixel numbers are only comparable within a viewpoint,
   and across rounds only the RATIOS survive.
2. **One launch per configuration, cvars set on the command line**, so the
   setting is live from the first rendered frame. The operator types only
   `screenshot`. No cvar typing means no reversed order and no `;` hazard,
   and no console-pause staleness because the frame was drawn before the
   console opened.
3. Compare with `tools/blobstats.py`, and report ratios when comparing across
   rounds.

## LOCALIZED 2026-08-04: the bloom divergence is in the EXTRACT, not the pyramid

Measured with the new `mt_bloom_dump`, savegame-pinned viewpoint (`capspot`,
MAP51 lantern), one launch per config.

    stage                          bbox     aspect   peak     px    energy
    compute extract (pre-pyramid)  85x62    1.37:1   1.27853   797   298.9
    compute L0 (post-pyramid)     109x90    1.21:1   0.14046  7229   288.7
    compute L1                     55x44    1.25:1   0.14190  1788    72.2
    compute L2                     27x22    1.23:1   0.14620   442    18.3
    compute L3                     13x11    1.18:1   0.17068   100     4.7

    reference L0                  130x52    2.50:1   0.21937  4743   239.1
    reference L1                   64x26    2.46:1   0.22623  1173    59.9
    reference L2                   31x13    2.38:1   0.23299   285    15.1
    reference L3                   15x6     2.50:1   0.25706    63     3.9

**The compute extract is already 1.37:1 before the pyramid runs**, and the
pyramid only rounds it further to 1.21:1. A separable Gaussian cannot elongate
a blob, so the reference's 2.50:1 output can only come from an extract that was
already elongated. The pyramid is exonerated on both paths.

### Read the four level rows correctly -- they are ONE number, not four

The up-leg replaces every level with the upscaled level above it, so each
path's final L0/L1/L2 are copies of its L3 at different resolutions. The
constant aspect down each column is guaranteed by construction and is **not**
evidence that the shape is preserved through the pyramid. That is exactly why
the extract snapshot had to be added -- the level rows alone cannot separate an
extract divergence from a down-leg one.

### Ruled out inside the extract

Its arithmetic is identical. `params.threshold` is `1.0f`
(`mt_bloom.cpp:428`), matching `bloomextract.fp`'s hardcoded `- 1`. The uv
mapping is the same expression on both sides: `Offset + TexCoord * Scale`
against `srcOffset + ((gid + 0.5) / bloomRes) * srcScale`, and at this viewport
SceneScale is (1,1) and SceneOffset (0,0). Both minify 4x with a linear
sampler and no mips. Both multiply by the same
`hw_postprocess.exposure.CameraTexture`.

**So the extract math is not the divergence -- what it READS is.** Both name
`PipelineImage[mCurrentPipelineImage]`, but they read it at different points
relative to `hw_postprocess.Pass1`, and the pipeline images ping-pong.

### Next: snapshot the extract's SOURCE, ranked first

1. **Dump `srcTex` itself** on the compute path, and the reference's bound
   input, and compare. If the compute path reads the pre-Pass1 image while the
   reference reads the post-Pass1 one (or the other ping-pong buffer), the two
   extracts see genuinely different images and every downstream number follows.
   This is cheap -- the snapshot mechanism already exists.
2. Add the same extract snapshot to the reference path for a like-for-like
   comparison. Harder, since the extract lives in shared code.

Do NOT go back to reading the pyramid or the blur. Five source reads and three
measurement rounds have now cleared it.

## SOLVED 2026-08-05: the bloom divergence was a shader-cache key, and the COMPUTE path was right

`MtShaderManager::CompileShader` keyed its in-memory `mShaderCache` on the
shader's *name* alone. Several `PPShader`s share a fragment filename and differ
only in their `Defines`, which are baked into the source text but were absent
from that key -- so the second variant requested silently received the first
one's compiled module.

`shaders/pp/blur.fp` is both `BlurHorizontal` and `BlurVertical`.
`PPBloom::RenderBloom` runs the horizontal step first, so **every "vertical"
blur in Metal's PP bloom was a second horizontal blur.**

### The measurement chain, which is the reusable part

    reference stage      bbox    aspect   px
    extract             85x62   1.37:1    797
    after horizontal    78x41   1.90:1    888
    after vertical      78x41   1.90:1    927   <-- bbox UNCHANGED by a blur

A blur that does not move the bounding box at all is not blurring along that
axis. That single row identified the bug; everything before it was narrowing.

After the fix, at the same viewpoint:

    stage / level     compute        reference (before)   reference (after)
    L0              109x90 1.21:1    130x52 2.50:1        112x90 1.24:1
    L1               55x44           64x26                 56x45
    L2               27x22           31x13                 27x22
    L3               13x11           15x6                  13x11

L2 and L3 extents are now identical, peak agrees to 1.9%, energy to 2.4%, pixel
count to 3.0%. **The compute path was correct all along.**

### The reasoning error worth keeping

Between the last two rounds I argued: "a separable Gaussian cannot elongate a
blob, so the reference's 2.50:1 output means its extract must already be
elongated -- therefore the divergence is in the extract." The extracts then
measured *identical* (85x62, 1.37:1, peak within 0.1%), so the premise was
false: the reference's pyramid really was elongating, because it was applying
the horizontal kernel twice.

The deduction was valid and the conclusion was wrong, because it assumed the
code did what it said. **When an elimination argument concludes that the
divergence must be somewhere you have already cleared, suspect the assumption,
not the location.** The earlier note in this file -- "do not assume Metal is
the one that is wrong about shape until the intermediate levels say so" -- was
the right instinct, and I argued past it once before the data settled it.

### Wider blast radius -- NOT yet verified

`shaders/pp/tonemap.fp` has **five** variants differing only by defines
(Uncharted2, HejlDawson, Reinhard, Linear, Palette). All five shared one cache
key, so on Metal every tonemap mode resolved to whichever compiled first --
`gl_tonemap` would have had no effect beyond the first mode used in a session.
Same for any future shader pair sharing a filename. **This is an inference from
the same defect, not a measurement. Verify it before claiming it.**

### Why a cache wipe never helped

Only the in-memory map was wrong. The on-disk cache below it was always keyed
on a hash of the source, defines included, so clearing
`mt_pipelines.bin` or the shader cache could never have surfaced this. Worth
remembering next time "try clearing the cache" is proposed as a diagnostic.

### What this retires

Every compute-vs-reference bloom number recorded on this branch was measured
against a reference path whose vertical blur was missing -- on top of the
Rgba16f-as-8-bit defect fixed the same day. The anisotropy hunt
(energy ratios 1.33/1.38, aspect 0.67/0.48, "68% taller and 20% narrower") was
chasing this bug. Those numbers are history, not targets.

## VERIFIED 2026-08-05 (later): the tonemap inference, and two bugs it uncovered

The "NOT yet verified" inference above is now **settled**, and verifying it
turned up two real defects that the cache bug had been concealing. Fixes:
`ad35e9a52` (Rgba8 format) and `5a9b53e1b` (PP orientation).

### How they stayed hidden -- the reusable part

`gl_tonemap` defaults to `0` (`hw_postprocess_cvars.cpp:41`), and mode 0 makes
`PPTonemap::Render` return before doing anything. **The tonemap pass had never
once executed on the Metal backend.** Same for `gl_lens` (default `false`) and
custom PP shader textures.

This branch is validated by "build it, load a WAD, look at it" -- there is no
test suite. That makes the tested surface exactly equal to *whatever the dev
config happens to enable*. Passes that run by default (bloom, SSAO, present)
have been debugged hard; passes behind a default-off cvar have never had a
single pixel checked. The Metal backend was written by mirroring Vulkan, so this
code gets written correctly-looking and then never runs.

There was a second layer: even if someone had tried `gl_tonemap` before
`b017e7c92`, all five modes rendered identically, so a casual "does this cvar do
anything?" test would have looked *consistent*, merely ineffective. The cache
bug was actively concealing whether the modes worked. Fixing it is what made the
pass observable at all.

**Assume every default-off postprocess setting on Metal is untested until
someone turns it on and looks.**

### Bug 1: every PP pass mirrored V (`5a9b53e1b`)

The shared fullscreen triangle's UVs assume OpenGL's bottom-left texture origin;
Metal's is top-left. Every pass that sampled and wrote flipped V. The chain
survived on flip *cancellation* -- blit flipped once, final present flipped
again -- so only an ODD pass count broke:

    gl_fxaa 1       2 draws   even   upright     <- accidentally worked
    gl_lens 1       1 draw    odd    INVERTED
    gl_tonemap 1-5  1 draw    odd    INVERTED

Fixed by flipping `TexCoord` in the PP vertex shader for all PP shaders except
the four `present*` ones, which own their own `UVScale`/`UVOffset` and were
already correct.

### Bug 2: `PixelFormat::Rgba8` mapped to BGRA8Unorm (`ad35e9a52`)

R and B transposed in every `Rgba8` PP texture. Invisible on the dither noise
texture; visible the moment the palette LUT was used. Also affected custom PP
shader textures, i.e. mods. Sibling of the `Rgba16f` case in the same switch.

### The method lessons

- **A test whose two outcomes look the same is not a test.** `gl_fxaa 1` was
  designed to prove the parity theory and came back upright -- apparently
  falsifying it. FXAA is *two* draws, so an even count was the theory's own
  prediction. Count the draws before predicting the parity.
- **Prefer an effect whose application is self-evident.** `gl_lens` was the test
  that worked, partly because barrel distortion is unmistakable: a null result
  could not masquerade as a pass. FXAA's subtlety made its null undetectable.
- **A mathematical identity is the strongest available probe.** `gl_tonemap 4`
  is `sqrt(c*c) == c`. It removed the shader maths as a variable entirely:
  pre-fix it inverted the frame (proving the pass runs), post-fix it is
  byte-identical to mode 0 (proving the pass is now exact). Both halves were
  needed; either alone is consistent with "the setting did nothing."
- **Byte-identical captures still need the null ruled out.** The final pair
  matched at `326268d2...`, which is equally consistent with "perfect identity"
  and "the pass never ran." What distinguished them was the *pre-fix*
  observation that mode 4 visibly broke the frame.

### Capture-protocol note

The one-launch-per-config rule got relaxed during this session -- cvars were
typed into a running console instead. That left `gl_lens 1` set for every
subsequent capture including the `mt_bloom_dump`, an uncontrolled confound that
made four screenshots uninterpretable. The clean two-launch pair had to be
redone. macOS `Cmd+Shift+4` *does* sidestep protocol item 4 (the console-pause
trap), since it captures the live window rather than re-presenting.

## SOLVED 2026-08-06: the reference AO path composited nothing, and every debug mode was structurally unable to show it

`gl_ssao 0` and `gl_ssao 3` produced byte-identical frames on the reference PP
AO path. Root cause, fixed in `ebc854ebf`:

`depthblur.fp`'s **vertical** pass wrote `0.0` into the depth channel while the
horizontal pass preserved it:

```glsl
#if defined(BLUR_HORIZONTAL)
    FragColor = vec4(fragAO, centerDepth, 0.0, 1.0);   // depth kept
#else
    FragColor = vec4(pow(clamp(fragAO,0,1), PowExponent), 0.0, 0.0, 1.0);  // destroyed
#endif
```

The vertical pass is the last writer of `Ambient0` before the composite, and
`ssaocombine.fp` derives its whole output alpha from that channel *twice*:
`depthSignal = 1 - exp2(-ssao.y * 0.01)` collapses to 0, and the `ssao.y > 2.0`
gate fails. Alpha was zero, so the alpha blend was an exact no-op. **AO was
computed correctly and multiplied by zero.**

Measured with `mt_ao_probe` (new this session), before -> after:

| | before | after |
|---|---|---|
| mean `ssao.x` | 0.86671 | 0.86671 (occlusion, correctly unchanged) |
| max `ssao.y` | 0.000 | 101.375 (mean 64.861, sane world units) |
| gate passing | 0 / 278640 | 278640 / 278640 |
| computed alpha | 0 | mean 0.05532, max 0.21731 |
| SceneColor differing px | 0 | 550267 (49.43%) |

Screenshot pair confirms independently: 50.8% of pixels darker, 0.149% brighter,
largest block delta -15.208 lum.

### Why this survived, and what it does NOT invalidate

**`hw_postprocess.cpp:910` wraps BOTH blur passes in `if (gl_ssao_debug < 2)`.**
So every debug mode that displays the depth channel skips the pass that destroys
it. The `gl_ssao_debug 7` reasoning recorded further up this file was *correct
for what it measured* and structurally incapable of revealing this: the only
modes that show you `.y` are the modes that bypass the vertical blur. Four such
observations were recorded as clearing AO generation. They cleared a code path
that does not contain the defect.

**The compute-path AO work in this file stands.** The compute path
(`MtAOModule`, `mt_ao.metal`) has its own `ssao_combine_vs`/`ssao_combine_fs`
and never touches `depthblur.fp` or `ssaocombine.fp`. Every
`mt_compute_ao_algorithm` comparison, the AlchemyAO grain measurements, the
regression hunts -- all compute-vs-compute, all unaffected.

**What IS invalidated:** any claim about the *reference* PP path producing
correct AO output, and any compute-vs-reference visual parity claim. Those were
comparing against a blank.

### The timeline is the point

- `595ffaf0f` introduced `depthSignal`/`depthMask` -> reference PP AO silently
  dies, on **all three backends**. `master` is unaffected: it composites
  `vec4(fogColor, 1.0 - attenutation)` with no depth dependency, so there is
  nothing to report upstream.
- Before 2026-08-01 the Intel dev machine defaulted to **compute** AO, so AO was
  visible and the algorithm work above was done against real output.
- `2cf256d13` (2026-08-01) switched Intel to the reference path by default. **AO
  silently vanished from the default configuration that day**, which is why the
  byte-identical A/B surfaced when it did.

### Method lesson

**A debug mode that changes the pipeline is not an observation of the pipeline.**
Before trusting a debug view, check what it changes besides the display. This
one gated out two passes, and it cost four sessions.

Corollary, learned the same day: **re-enabling a long-dead string patch is a
behaviour change, not a repair.** The `ssao.fp` sky-dome guard had matched
nothing since `ac0fec5db` renamed `AOStrength` to `effectiveStrength`;
re-deriving it dropped mean `ssao.x` from 0.86671 to 0.99924 -- AO essentially
gone -- and it was reverted the same day (`66b0b6f64`). Its mechanism is still
not understood: it edits only the occlusion expression yet toggling it also
moved `FragColor.y` by three orders of magnitude, with no compile error logged.

## 2026-08-07: captures are now unattended, and the determinism claim was wrong

`+shotafter <frames> [name] [quit]` and `+execafter <frames> <command>`
(`m_misc.cpp`, ticked from `D_DoomLoop` after `D_Display`) arm a countdown from
the command line the way `mt_ao_probe` and `mt_bloom_dump` do. A whole A/B
series now runs from a shell loop with no operator, which removes the three
failure modes that have each cost a capture round: reversed toggle order, `;`
chaining, and the console-pause staleness.

The countdown runs **only on frames where `gamestate == GS_LEVEL` and
`gameaction == ga_nothing`**. That is the load-bearing detail: the picker,
title screens and savegame loading all render a run-dependent number of frames,
so counting from process start would give each launch in a control pair a
different amount of exposure settling — defeating the pair.

**`cl_capfps 1` is required for a meaningful floor, and this corrects a standing
claim in this file.** Two *identical* launches at capspot differ by a mean 1.7
lum over 98% of the frame, block deltas to 174, without it — larger than the
FXAA effect they were meant to measure. Animated content advances per **tic**,
and the tics elapsed by rendered frame N depend on the frame rate. With capfps
the same pair is byte-identical. The branch's "reproduces bit-exactly for the
same elapsed frame count" was true per *tic*; frame == tic only under capfps.

With that floor, both remaining coverage-hole checks closed:

- **FXAA passes.** Floor byte-identical; effect 4.20% of pixels, 2.099% brighter
  against 2.100% darker, mean signed delta +0.0044, largest block delta +1.371.
  Edge-local and symmetric — the prediction — and unlike the earlier false
  positive, which was a +4.12 mean signed wash over 39% of the frame (exposure).
- **Colormap polarity passes.** `tools/polarity.py` decides inversion vs
  desaturation by the **sign** of the luminance correlation: r = -0.546 with
  mean scene luminance 39.85 -> 167.75. Desaturation preserves luminance and
  would give r near +1. The untouched HUD and the nonlinear tonemap depress |r|
  and cannot flip its sign.

**The reusable method note:** both of these were previously "a test whose two
outcomes look the same is not a test" — the recurring failure in this file. The
fix in each case was not a better eye but a statistic whose *sign* differs
between the hypotheses, plus a same-config control pair to establish that the
floor is below the effect. Take the floor first, always.

## 2026-08-07: Tier 2 direct-composite — UNREACHABLE ON THIS HARDWARE, everything around it now tested

The standing item read "Tier 2 direct-composite path has never executed." It
cannot execute here, and that is now measured rather than assumed. `mt_caps`:

    ReadWrite BGRA8 (Tier2): NO (Tier 1 path)
    Common3 no · Mac2 no · Apple1 no          (Intel HD 6000, Metal 2.0, IMR)

`MTL::ReadWriteTextureTier1` covers only r32float/r32uint/r32sint. The scene
colour is RGBA16Float (or BGRA8Unorm), both of which need Tier 2, so
`bloom_combine_rw_all` can never bind its `access::read_write` scene texture on
this GPU. **This is a hardware ceiling, not a missing test.** It merges with the
"Apple Silicon completely untested" item: the same machine blocks both, and one
run on any Tier 2 device would settle them together.

### What WAS testable, and passed — the fallback matrix

`mt_compute_bloom_composite` selects the path, and two of its three modes are
reachable on Tier 1. Predictions stated before capture; all five held.

    config                                   result
    gl_bloom 0                               034adf54   (the null control)
    mt_compute_bloom 0     (reference PP)    f87dbcf2
    composite 0            (auto -> Tier 1)  0792f53b
    composite 1            (force Tier 1)    0792f53b   == composite 0
    composite 2            (Tier 2 required) f87dbcf2   == reference PP

- **composite 1 is byte-identical to composite 0**, which is the correct no-op:
  on Tier 1 hardware "force Tier 1" and "auto" resolve to the same leg.
- **composite 2 is byte-identical to the reference path.** `Execute` returns
  false at the unsupported gate and `MtPostprocess`'s `!computeBloomRendered`
  branch runs `hw_postprocess.bloom.RenderBloom`. The fallback is exercised,
  and it is a real fallback rather than a dropped effect — which the bloom-off
  control is what proves, since bloom is worth 17.1% of pixels and max delta 47.
- Note the fallback still ENCODES the whole compute pyramid before bailing.
  Wasted GPU, no correctness impact (it writes only bloom textures, never
  srcTex). Left alone; the early-out could move above the pyramid if it ever
  matters.

**Bonus result worth recording:** compute bloom against reference bloom over
the whole frame is now **max_delta 1, mean 0.049, zero pixels differing by more
than 2**. Every earlier comparison on this branch was blob statistics; this is
the whole-frame confirmation that the 2026-08-05 shader-cache fix closed it.

### Static parity of the Tier 2 kernel against the verified Tier 1 pair

Since the path cannot run, it was read against the Tier 1 legs, which ARE
verified on screen. `bloom_combine_rw_all` vs `bloom_combine_contrib_all` +
`bloom_fs`:

- Identical uv construction (`(gid + 0.5) / srcRes`), identical sampler, and
  identical zero-strength skips over the same four bound levels.
- **The absent V flip is CORRECT, not a repeat of the round-two bug.** Tier 1
  needs it only because `bloom_vs` derives uv from clip space where +Y is up.
  The read-write kernel never leaves texture order: `gid` is a destination
  texel and the sample uv is derived from that same `gid`.
- Alpha agrees. `bloom_fs` returns alpha 0 under additive blend; the kernel
  reads the scene texel and writes back `scene.rgb + bloom` with `scene.a`
  untouched.
- `viewportOrigin` is applied the same way both legs apply it — `SetViewport`
  passes `mViewportY` to Metal's top-down `originY` with no flip. So Tier 2
  inherits Tier 1's behaviour exactly, including at a non-zero viewport top;
  it does not introduce a new convention.
- The scene texture gets `TextureUsageShaderWrite` only when
  `supportsReadWriteBGRA8` (`mt_renderbuffers.cpp:106-108`), matching the gate,
  so there is no latent case where the tier is present but the usage flag is
  missing.
- `combineRWPSO` builds successfully even on Tier 1 (no failure in the log), so
  a Tier 2 device will not additionally trip over PSO creation.

Eliminated along the way: `EnsureTexture` requires an exact size match and
recreates otherwise, so `mCompositeTex` can never be larger than the viewport
and the Tier 1 raster composite cannot sample a stale margin.

### The method failure, which cost the first whole matrix

The first five-launch run came back with **four configs byte-identical** and
`mt_caps` reporting cvar values that contradicted the command line. The cause
was **zsh**: `$2` unquoted does not word-split, so each config string arrived as
one argv entry and only its FIRST cvar took effect.

The branch rule caught it immediately — byte-identical across a toggle means
the setting did not apply. What made it cheap to catch was `+execafter 100
mt_caps` printing the RESOLVED path in every log: the contradiction was visible
in the log rather than inferable from pixels. **Put a state dump in every
capture launch.** In zsh use `${=VAR}` when splitting is wanted.

## 2026-08-07: BlitCurrentToImage's V flip — VERIFIED CORRECT, by executing the dead branch

The standing item was "BlitCurrentToImage's conversion branch confirmed
unreachable, kept deliberately, its V-flip unverified". The flip is now
verified, and the branch has been executed on hardware for the first time.

### Why the recipe in the source could not work

The comment there proposed forcing `CreateWipeTexture` to BGRA8Unorm, running a
level-exit wipe and looking at it. That cannot be done unattended, and it cannot
be done at all with a frame hook: `PerformWipe` runs to completion inside a
single `D_Display` call (`d_main.cpp:1193`), so there is no frame boundary
during the wipe to capture on. A screenshot after it shows the destination
screen.

### `mt_wipe_probe`, and why it asks a different question

The wipe was never the question — the question is whether the conversion leg
produces the same orientation as the copy leg. So the probe blits the SAME
pipeline image down both legs into two textures, one in the pipeline format
(copy leg, and ground truth since that is what wipes use today) and one in the
other format (conversion leg), and compares them.

They are different formats and precisions, so it compares the SHAPE of their
vertical profile: per-row mean luminance, correlated against the other's
profile and against that profile REVERSED. It also reports
`r(copy, copy REVERSED)` as an explicit statement of discriminating power — a
vertically symmetric source would correlate equally both ways and make the
verdict meaningless. Reporting that instead of assuming it is the direct answer
to this file's recurring "a test whose two outcomes look the same is not a test".

### The result, both halves

    as shipped        r(convert, copy) = +0.9986   r(convert, copy REV) = +0.0967   AGREE
    flip removed      r(convert, copy) = +0.0967   r(convert, copy REV) = +0.9986   INVERTED

Those two runs landed on the SAME frame, so the pair is an exact swap of the
same two numbers rather than two loosely comparable readings. Discriminating
power was +0.0988 -- far from the +1 that would mean a symmetric, useless
source. An earlier run at a different frame agreed (+0.9985 / -0.2582).

**The falsification half was necessary, not decorative.** "AGREE" on its own is
exactly what a probe unable to detect inversion would print, and it is also what
would appear if `BlitCurrentToImage` had silently failed and left both textures
holding the same thing. The mirrored second result rules out both at once and
additionally proves the conversion branch really executes — which the source's
own `PRINT_LOG` marker could NOT prove, because PRINT_LOG does not reach stdout
and its absence from a redirected log means nothing.

### Why it is correct, now that it is known to be

The present shader is deliberately excluded from `PatchVertexShader`'s PP V-flip
(`mt_shader.cpp:1470`), so on Metal it carries an IMPLICIT flip — the same one
the chain used to survive by cancellation before `5a9b53e1b`. The explicit
`Scale.y = -1, Offset.y = +1` is the second flip, and the two cancel, matching
`copyFromTexture`. The direction that shipped from a bug report turns out to be
derivable.

The branch remains unreachable in normal play. That is fine and deliberate; it
is now a guarded conversion path that is known to work rather than one that is
merely hoped to.

## 2026-08-07: custom PP shaders — FOUR defects, and the pass had never run at all

The last never-tested item on the standing list, and the one with the worst
ratio of "looks fine in review" to "does nothing at runtime". Fixed in
`25fbdc24b`.

### No mod needed — build a two-sided one

The obvious move is to download a mod that ships a postprocess shader. A better
one is `tools/pptest/make.py`, which builds a minimal mod DESIGNED as a test,
which no real mod is:

- `pptest_scale` is a CVARINFO cvar bound with `cvar_uniform`, so behaviour is
  set from the command line and the matrix runs unattended.
- At `1.0` the shader is a mathematical identity: the frame must be
  byte-identical to no shader. **That single result proves three things — the
  pass runs, its maths is exact, and it is upright**, since a V flip would show
  as a huge delta rather than a null.
- At any other value it must differ. Without this half the identity result is
  worthless, being equally consistent with "the pass never ran".
- The bound texture is pure red, so a channel-order bug renders BLUE. The null
  cannot masquerade as a pass.

### The four defects

Three prevented the shader from compiling at all, each independently fatal:

1. **`layout(location=...)` only under `IsVulkan()`** (`hw_postprocess.cpp`).
   Metal runs glslang under SPIR-V semantics exactly as Vulkan does, so every
   custom shader was rejected with *"SPIR-V requires location for user
   input/output"*. Stock PP shaders declare locations in the lump; only the
   custom path builds its in/out declarations in C++.
2. **Metal prepended the prolog to the VERTEX stage.** Vulkan passes `""` there
   (`vk_ppshader.cpp:39` vs `:48`). For a custom shader the prolog carries the
   sampler and in/out declarations, so `layout(location=0) in vec2 TexCoord`
   landed in `screenquad.vp` and collided with its own `PositionInProjection` at
   location 0. Invisible for stock shaders, whose prolog is only a uniform block
   that the vertex stage ignores.
3. **`LoadPrivateShaderLump` searched only the engine pk3** — it used the
   `int wadfile` overload with `0`. A MOD's shader lump was never found and the
   fragment source came back empty. Now unrestricted, matching Vulkan.

And one that survives compilation:

4. **The `screen` target was never dispatched on Metal.** Shared `Pass1`/`Pass2`
   run only `beforebloom` and `scene`; Vulkan (`vk_postprocess.cpp:190`) and
   OpenGL (`gl_postprocess.cpp:150`) each run `screen` from their own present
   path, and Metal did not. GLDEFS accepted it, `listshaders` listed it,
   `shaderenable` enabled it, and nothing executed it.

### Measured, all three targets

    identity (scale 1.0) vs no shader   max_delta 0          exact, and upright
    scale 0.5, scene and screen         39.8 -> 20.2 mean lum, 89.2% of px
    scale 0.5, beforebloom              mean delta 26.83
    hardcoded channel rotate            72.3% of px changed
    custom PP texture, pure red         renders RED, not blue

`beforebloom` differing from the other two (26.83 vs 26.36) is a result, not
noise: it runs before bloom, so bloom re-adds energy from the darkened image.
The insertion point is real and distinguishable.

### The trap that hid it, twice in one session

**The engine printed the exact glslang error the whole time — via `PRINT_LOG`,
which does not reach stdout.** Every launch log looked clean while every custom
shader failed to compile. The same trap had just cost time on the wipe probe,
where a missing `PRINT_LOG` marker was nearly read as evidence the branch had
not run.

`PRINT_LOG` is invisible under `> log 2>&1`; it needs `+logfile`. Shader
parse/link failures and missing shader lumps are now `PRINT_HIGH` in red,
because a pass that cannot build is not a debug detail. **When a path
"silently does nothing", run it once with `+logfile` before concluding it is
silent.**
