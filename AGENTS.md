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
