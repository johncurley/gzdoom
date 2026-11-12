# Engine Modernization Plan

This document is the durable roadmap for modernizing the engine while
preserving GZDoom mod, demo, savegame, and rendering compatibility.

`AGENTS.md` contains current operational notes and benchmark procedures. The
Metal renderer README and `gemini.md` are historical architecture references,
not the active roadmap.

## Targets and principles

- Treat the Intel MacBookAir7,2 as the minimum compute-performance target.
- Treat Apple Silicon and modern Vulkan hardware as scaling targets.
- Prefer measured hybrid raster/compute techniques over replacing rasterization
  for its own sake.
- Keep legacy paths available through backend capability selection.
- Preserve simulation ordering unless a new concurrency API is explicitly
  opt-in.
- Establish compatibility tests and stable benchmarks before deep refactors.

## Track A: Rendering foundation

**Prerequisite written 2026-08-16: `docs/frame-analysis.md`** — the actual pass and
resource dependency map (three tiers of resources, the extracted pass I/O table, and
the four implicit couplings a graph would make explicit). Items 3 and 4 below should
start from it; it also proposes the migration order and argues AO should be *last*,
not first.


1. Establish performance and compatibility baselines.
2. Define pointer-free per-view inputs and scene-data ownership before
   introducing any render snapshot. The Linux source audit found that
   `FRenderViewpoint` is a shallow, mutable traversal context with live gameplay
   pointers, not a safe thread-handoff object; see
   [`audit-render-view-snapshot-linux-2026-09-26.md`](audits/audit-render-view-snapshot-linux-2026-09-26.md).
   The shader-facing value slice already exists as `HWViewpointUniforms` and is
   copied synchronously into `HWViewpointBuffer`; scene/BSP data remains serial.
3. Complete the CPU-side frame-graph contract: explicit outputs, resource
   lifetimes, dead-pass candidates, and scene-material reads attached to the
   passes that consume them.
4. Migrate a bounded pass chain to graph-driven execution, then add backend
   synchronization and hazard handling before widening to AO, bloom, and
   presentation.
5. Introduce backend-neutral render packets.
6. Separate BSP visibility discovery from surface generation.
7. Add compute light-list construction and clustered forward lighting.
8. Add GPU culling and indirect submission where measurements justify it.

The frame graph should select effect implementations by capability rather than
creating independent renderers. A node may have a direct-compute, temporary
compute, raster, or disabled implementation.

## Track B: Deterministic visibility

Develop a deterministic fixed/rational visibility kernel independently from
visual shading:

1. Audit coordinate and intermediate numeric ranges.
2. Specify fixed-point formats, overflow behavior, and tie-breaking rules.
3. Build a scalar CPU reference for ray/AABB, ray/plane, and ray/triangle tests.
4. Add golden vectors and deterministic result hashes.
5. Port the kernel to Metal and SPIR-V compute.
6. Start with binary shadow visibility.
7. Extend to portal-aware rays and selected reflections.

The deterministic boundary ends at the hit record. Texture sampling, material
lighting, denoising, and other visual-only shading remain native floating-point
workloads. SPU-13 support is deferred while that project remains experimental.

## Track C: Simulation modernization

Performance acceptance prioritizes smooth gameplay: reproducible long-frame
frequency/severity and doorway/area-entry/level-transition pacing. Average FPS
is secondary. Isolated CPU or API-command savings support implementation choices
but do not, by themselves, qualify a smoothness improvement.

The [engine performance and dependency audit](audits/engine-performance-dependencies-2026-11-08.md)
records current source boundaries, profiler limitations, and the matched-workload
capture contract. Blackwater MAP09 now supplies a manual saved route and one
complete Metal CPU capture; qualified matched hotspot rankings remain pending.
The [profiling tools](../tools/profiling/README.md) now summarize existing
Instruments exports and document the CPU/wait, actor-count, and render-workload
measurements. The first Blackwater capture supplies candidates, not a qualified
GL/Metal baseline or a threading implementation target.

The [instrumentation audit and implementation order](audits/engine-instrumentation-2026-11-09.md)
defines the active sequence for this tranche:

First increment completed: sight-query early-return timer accounting repaired
with a live failing control/passing candidate, and VM history relabeled as
display samples. The next completed increment adds
[native macOS timeline markers](audits/engine-profile-markers-2026-11-09.md),
default-off and live-qualified on Intel Metal/GL with a known-delay control.
Combined CPU samples and scheduler states are readable on one timeline. Those
marker controls alone do not establish Blackwater attribution or observer cost.
The [corrected full-retention Blackwater repeat](audits/blackwater-marked-frame-2026-11-09.md)
now aligns a 261.5 ms long frame with synchronous lightning sound preparation,
repeating the earlier event signature. Resolved sound/cache attribution is next;
matched GL/Metal baselines and observer-overhead qualification remain open.

The [scene-material observer follow-up](audits/framegraph-observer-cost-2026-11-09.md)
retains the graph and adds a cache of completed material observations within
each backend-pass scope, preserving per-binding upload accounting. An isolated
repeated-binding control measures about 94% less observer CPU time with identical
graph reports; a within-process Blackwater check has identical pixels and graph
dumps. Whole-frame timing was too variable to qualify a gameplay speedup.

The [audio worker boundary qualification](audits/audio-worker-boundary-2026-11-09.md)
measures exclusive decoder handoff with the installed ZMusic library. Blackwater
thunder PCM/metadata match between serial and worker reads, with about 98% less
owner CPU during isolated decoding. Decoder construction remains serialized
because of initialization/open-state dependencies. The
[first engine integration](audits/audio-worker-integration-2026-11-09.md) adds
a bounded Vorbis worker, owner-allocated PCM, ready publication and
cancellation/reset handling. Repeated Blackwater controls show ready hits
replacing first-use thunder loads; sanitizer core tests and live loop/sample
metadata checks pass. Synchronous cold-miss fallback is retained. Matched doorway
pacing and long-session acceptance were pending in the initial qualification.
The later scoped doorway checks and current sound defaults are recorded below.

The [post-integration metrics](audits/blackwater-audio-metrics-2026-11-09.md)
now demonstrate stationary weather hitch reduction across three runs per mode:
seven >100 ms intervals disabled, zero enabled; separate CPU captures confirm
thunder decode removal from the main-thread event path. Material-observation
allocation/hash/read-tracking remains a substantial recurring CPU candidate.
Doorway-route and uncapped throughput qualification remain open; inclusive CPU
sample shares are not frame-time savings or a threading mandate.

The [resource-read lookup follow-up](audits/resource-read-lookup-2026-11-10.md)
removes temporary FString allocation on lookup hits while retaining owned,
case-sensitive resource names and every per-binding upload observation. An
allocation control measures 67–69% less isolated lookup CPU time, with identical
Blackwater graph reports and pixels on Intel Metal/GL. Final build, graph tests
and baseline/SSAO cross-backend checks pass. A fresh stationary CPU capture
shows resource-read observation at 2.2% of main-thread sample weight (previously
8.6%); this is supporting attribution, not a whole-frame speedup claim. The
separate material-cache string lookup and movement/AABB work remain candidates;
doorway-route and uncapped qualification are still open.

The [material-cache lookup follow-up](audits/material-cache-lookup-2026-11-10.md)
also removes temporary strings on repeated material-cache hits, with owned
pass-scoped keys and the same alias/pass/reset invalidation points. An isolated
allocation control measures 54–55% less material-observer CPU time; graph reports
and Blackwater pixels match on Intel Metal/GL. Final build, graph tests and
baseline/SSAO comparisons pass. A fresh CPU capture places material observation
at 7.1% of main-thread sample weight (previously 11.9%), alongside thinker and
movement work; different focus and uncontrolled inputs limit that comparison.
Retain dependency validation and bounded postprocess replay. Further string
identity work or movement-query optimization requires separate measurement;
the graph does not provide a frozen world snapshot for concurrent scene traversal.

The [graph caller follow-up](audits/graph-caller-lookups-2026-11-10.md) identifies
Metal material binding as the measured caller and combines upload-read ordering
and scoped material completion in one owned resource entry. Warm bindings now
perform one lookup instead of two; a restored duplicate-lookup control measures
43–47% less isolated CPU work, with matching graph reports and pixels. Build,
graph tests and baseline/SSAO comparisons pass. The fresh material-observer
sample share is 5.4% (previously 7.1%); focus/input limitations prevent a whole-frame
speedup claim. First-observation comparisons remain. The next rendering CPU
investigation is render-state/material/pass/stream application and its shared
hwrenderer callers, alongside movement/blockmap queries. Advanced graph scheduling,
culling and memory reuse still require separate correctness and GPU/memory
measurements; the graph already controls bounded postprocess replay.

The [render-state count follow-up](audits/render-state-counts-2026-11-10.md)
shows pipeline/matrix deduplication already working and frequent actual material
key changes. A bounded Metal-only encoder-local depth-clip cache removes 99.4%
of repeated commands in the paused Blackwater fixture, with matching images and
graph reports and passing value/invalidation controls. The isolated command-loop
saving is small in normal-frame terms; no hitch or FPS improvement is claimed.
Unprofiled transition pacing is the next acceptance priority. Further rendering
work should examine material preparation and existing opaque ordering before
introducing worker jobs with explicit input ownership and ordered merge points.

The [current-build doorway check](audits/blackwater-doorway-current-2026-11-10.md)
now passes three fresh-process Metal runs: 4,027 retained intervals over about
115 seconds, zero intervals above 100 ms, and all three manual doorway crossings
confirmed smooth by the user. This closes the scoped MAP09 doorway check with
cap35 and audio preparation explicitly enabled; it does not qualify cold caches,
other backends, uncapped play, long sessions or level transitions, or attribute
the result to an individual optimization. Heavy-gameplay and level-transition
smoothness are the next workloads; no profiler capture is needed for an absent
long-frame event. Sound preparation now defaults on through the macOS worker
gate, as recorded in the precache follow-up below.

The [combat/reload follow-up](audits/blackwater-combat-reload-2026-11-10.md)
records smooth fighting reported by the user, but rejects the mixed
combat/death/menu take as a steady-combat baseline. A confirmed normal
death/reload CPU capture attributes an 863 ms Update gap primarily to level
reconstruction, UDMF parsing, decompression and preparation. A separate 101 ms
post-load gap contains 29 ticks plus synchronous actor-triggered sound loading;
it is not one expensive actor tick or established thunder work.

The [bounded key-cache follow-up](audits/udmf-key-cache-2026-11-10.md), verified
through 2026-11-11, caches repeated property-key FNames within each UDMF/USDF
parser with strict entry/length limits. Real MAP09 controls resolve 99.956% of
keys from the cache and reduce parser-body wall time by 32.5–35% (roughly
55–61 ms in this stage). Reference-identity checks, a failing fault control,
static UDMF pixel comparisons and final build/regression checks pass. This is
owner-thread algorithmic work. The
[matched warm reload measurement](audits/blackwater-reload-end-to-end-2026-11-11.md)
now qualifies 65–72 ms less request-to-completed-resumed-loop latency
(3.3–3.7%) across three Metal processes, with all per-process gains exceeding
repeated-run variation. Median per-reload maximum Update gap falls from 570 to
510 ms; substantial pauses remain. The resumed iteration itself shows no
consistent gain. The sound follow-up below resolves the repeated asset identities;
remaining resource/load work and a survivable combat/normal level-exit fixture
are still open. Tick ordering
and the renderer baseline remain unchanged; cold/GL/long-session qualification
is still open.

The [post-load sound attribution](audits/postload-sound-attribution-2026-11-11.md)
identifies nine short first-use sounds per resumed batch: flesh impacts,
monster activity, Geiger clicks and rain. Six reloads show 38–41 ms of
decode/buffer work and less than 1 ms of OpenAL upload. The prediction of a
single >=10 ms decode is rejected; the cost is cumulative. A temporary existing
PrecacheSounds declaration preserves all ten possible variants (1.43 MiB PCM)
across reloads and eliminates the synchronous loads in six control reloads.
This is functional evidence, not an end-to-end speedup or a shipped engine fix.
Three ready thunder jobs already reserve about 51 MiB of the 64 MiB worker
budget, preventing simple admission of another fixed-size job. Next specify
bounded owner-side retention/invalidation and measure it; cold preparation and
wider worker admission remain separate. No Blackwater names, diagnostic hooks,
VM concurrency or tick-policy changes are added to production code.

The [bounded retention implementation](audits/sound-retention-2026-11-11.md)
now adds opt-in `snd_retainedcache` (KiB, default zero) and diagnostic statistics.
It keeps most-recently played eligible canonical buffers at existing precache
maintenance, within an extra payload budget and 128-entry history. Active and
explicitly marked buffers retain their existing protection. Explicit unload,
definition/device reset and metadata mismatch invalidate history. A user-approved
guard excludes raw/explicit-loop and conflicting definitions; native GL/Metal
negative controls reproduce the wrong-loop alias without it and correct actual
OpenAL loop points with it. Active-loop pressure/reset checks and policy sanitizer
tests pass. In three guarded warm Metal comparisons, 2 MiB reduces first resumed
loop medians from roughly 82 to 38 ms and complete reload intervals by 35–56 ms,
each beyond within-arm spread. Extra selected payload stays at or below 1.30 MiB;
7–9 resumed misses become 1–3. Core/loading-gap improvement is unqualified.
Keep default off pending broader gameplay, cold/long-session and cross-platform
acceptance; 2 MiB is a measured fixture setting, not a universal budget. Wider
worker preparation, resource loading and hwrenderer profiling remain separate.

The [current warm reload profile](audits/blackwater-current-load-profile-2026-11-11.md)
records three 522–559 ms Update gaps, with 432–475 ms of main-thread sample
weight under `G_DoLoadGame`. UDMF parsing accounts for 116–132 ms. Validation
and setup reopen the same compressed MAP09 WAD and together account for 45–50 ms
of sample weight. Check the ownership and validation order for reusing the
opened map, then compare repeated unprofiled reloads. The
[validated-map handoff](audits/validated-map-reuse-2026-11-11.md) now reuses
the compressed map reader during savegame loading. The warm Metal comparison
recorded one open per reload and 22–40 ms shorter median longest Update gaps
across three processes. For the renderer, collect
BSP, clipping, setup and worker-wait counters on a moving gameplay route.

The [first MAP09 visit capture](audits/blackwater-first-visit-textures-2026-11-11.md)
measured 2.74–2.88 s Update gaps in fresh Metal processes. Main-thread texture
snapshots account for 489–494 ms of sample weight, including 356–369 ms in PNG
inflate and unfilter work. The subsequent upload burst accounts for 603–611 ms.
Test owned PNG decoding on a bounded worker during the intervening sound
precache, with ready-at-first-use counts and pixel parity. Measure the upload
burst alongside the full first-visit gap.

The [standalone PNG decode test](audits/png-worker-decode-prototype-2026-11-11.md)
matches the serial IDAT output on 192 Blackwater RGB/RGBA fixtures. Two
workers reduce median decode time from about 83 to 51 ms with job storage
below 16 MiB. The [Metal integration](audits/metal-png-precache-2026-11-11.md)
keeps that work behind `mt_async_png`. On the first Blackwater MAP09 visit,
the enabled median Update gap was 29 ms shorter across three runs per
setting. All 92 accepted jobs per enabled visit uploaded, and all four
observed first binds used worker-prepared textures. The worker defaults on
for Metal. Set `mt_async_png 0` to run the synchronous decode control.

[Metal upload batching](audits/metal-texture-upload-batching-2026-11-11.md)
reduces the extra command buffers in the Blackwater first-visit burst from
1,966 to 40 with the same 1,965 texture uploads. Across six interleaved Intel
Metal runs, the median longest Update gap fell from 2.73 to 2.37 s. The
batching path now defaults on for Metal. Set `mt_batch_texture_uploads 0`
for the single-upload control. A separate post-load hitch remains visible
in the timing windows.
The [current frame-start profile](audits/metal-frame-start-attribution-2026-11-11.md)
locates 161–183 ms of main-thread sample weight at Metal texture creation,
96–108 ms in staging row copies, and 8 ms in completion cleanup. Level sound
precaching still accounts for 841–894 ms in the first-visit gap. A later
97–117 ms resumed interval includes 35 ticks and 37–48 ms under synchronous
sound loading. The [bounded sound precache worker](audits/sound-precache-worker-2026-11-11.md)
now overlaps an eligible direct sound decode with an owner-thread cache
operation. Native PCM, loop/publication order, and resident payload controls
match. Across six Intel Metal runs the median first-visit gap falls by
58 ms, with 15 worker hits per enabled visit and a peak reservation below
54 MiB in the existing 64 MiB pool. This missed the 100 ms prediction.
Both `snd_precache_async` and `snd_asyncprepare` now default on, with the
worker enabled through its existing macOS platform gate. The console
switches retain the serial controls. Broader random-group coverage,
long-session acceptance and native Linux/Windows decoder qualification
remain open.


Durable [resource regression controls](../tools/regression/README.md) now
cover exact audio output/order/loops, bounded preparation failure and
cancellation, PNG worker/fallback pixels, retention, sound reset and live
Metal upload batching. The five Intel Metal controls pass; see the
[qualification record](audits/resource-preparation-regressions-2026-11-11.md).
A portable save-load runner retains hashes and uses three interleaved
repetitions per arm. Native Linux/Windows and Apple Silicon runs remain open.
The [macOS performance handoff](handoff-macos-2026-11-11.md) records the
published baseline, exact input identities, reproduction commands and next
post-load attribution task; it distinguishes default-zero retention in the
load benchmark from the native suite's explicit 2 MiB control.
The [post-load continuation](audits/postload-input-axes-2026-11-12.md) confirms
CPU-heavy resumed intervals with cumulative sound loading, and identifies
8–15 ms in repeated joystick binding scans. Direct per-axis lookup removes
those scans; 1,984 native legacy-oracle comparisons pass and matching traces
have zero axis input samples. Interleaved unprofiled resumed-window medians
decrease by about 10/6 ms for first/warm workloads, with explicit spread limits.
Sound-precache discovery remains the larger follow-up; no VM threading is
implied by these measurements.

The [bounded random-sound sibling trial](audits/sound-sibling-preparation-2026-11-12.md)
adds optional lookahead after the normal resolver chooses a cold sound. It
shares the existing worker budget and never waits or publishes a sibling early.
`snd_asyncsiblings` defaults on following explicit maintainer approval on
2026-11-12; `snd_asyncsiblings 0` retains the disabled control. Wider gameplay
and platform acceptance remains pending. Its corrected native oracle controls
the actual C `rand()` stream through an external test-only macOS shim; the
separate `RandSound` method is not the ordinary resolver. Initial failed tests
and this correction are preserved. Matched backend correctness and final-code
timing qualification pass on Intel macOS: the first resumed-window median
falls 5.72 ms (103.71 to 97.99 ms), with 2–3 additional worker hits and zero
late/failure results. The audit retains the spread and failed validation
history. Warm ranges overlap; no gameplay determinism or broad performance
claim follows from the controlled fixture alone.
Enabled-default Intel Metal manual checks now confirm smooth doorway crossings
and fighting with normal sound. Three further minutes of varied play record
6,284 intervals, maximum 53.64 ms and zero >100 ms at cap35; the operator
confirms no perceived frame lag, deaths or menu use. A separate mixed death/
reload run retains one 623.83 ms interval requiring attribution. These scoped
acceptance runs do not establish an incremental speedup without disabled-fix
controls; longer soak and wider backend/platform gameplay remain open.
The [reload attribution](audits/blackwater-reload-attribution-2026-11-12.md)
now isolates two warm automatic-load gaps at 875/590 ms: scheduler running time
is 86–89%, with 139–161 ms sampled under UDMF parsing and 325–369 ms under the
broader map construction pipeline. Separate post-load tick catch-up retains
36–38 ms of synchronous decode samples. The manual death trace failed to save;
these are scoped background shared-loader observations, not exact attribution
of the earlier 623.83 ms manual pause. Next inspect parser allocation/copy and
content-identified map reuse boundaries; preserve simulation order and validate
any proposed serial change before adding concurrency.





1. Validate existing timer meanings and a common frame/tick/profiler timeline;
   repair only the instruments needed for the next experiment.
2. Qualify and repeat the gameplay, area-entry and level-transition workloads
   on GL/Metal, separating CPU execution from waits and observer overhead.
3. Optimize one demonstrated critical-path cost with a disabled-fix control.
   Sound preparation and scene-material bookkeeping are current candidates.
4. Improve serial algorithms/data access and ownership where measurements warrant.
5. Introduce bounded asynchronous computations only after lifetime, observation
   order, deterministic results and measured net benefit are established.

Hold speculative new rendering optimization campaigns and generic concurrent
actor ticks during baseline qualification; preserve renderer correctness/parity
work. Actor count must be related separately to thinker time, visible geometry,
sprites, lighting and material work before choosing an actor optimization.

- Profile and optimize VM dispatch, allocations, and native/script transitions.
  - Begin after the active renderer milestones and their validation gates are
    closed. Start with attribution, not a presumption that ZScript is the
    bottleneck.
    Use a repeatable gameplay route and read `stat think` alongside GPU timing
    to distinguish simulation cost from renderer cost. `profilethinkers -t 20`
    ranks thinker classes by total time; `acsprofile` reports ACS instruction
    counts, not CPU time.
  - Capture optimized builds with symbols in a sampling profiler before adding
    per-function timers. Use Linux `perf`, macOS Instruments Time Profiler, or
    Windows Performance Recorder/Analyzer to find hot stacks across native
    code, the ZScript VM/JIT, ACS, allocation/GC, and render submission. Record
    the route, mod set, settings, warm-up, sample window, and repeated results;
    do not assume `demo1.lmp` is available.
  - If sampling attributes material time to script execution, add focused
    per-function CPU time and call-count instrumentation, then profile VM
    dispatch, allocations, and native/script transitions separately. Keep the
    instrumentation bounded and compare it against an uninstrumented control.
  - Preserve the serial, ordered simulation contract. Only consider async
    execution for isolated pure workloads after defining their deterministic
    inputs and result handoff.
  - The ZScript JIT (`src/common/scripting/jit/`) is written entirely against
    asmjit's `X86Gp`/`X86Xmm` types with no ARM/AArch64 codepath or
    architecture guard found on audit (2026-07-10). Apple Silicon and any
    other ARM64 target currently get either a silent fallback to the VM
    interpreter or a broken JIT path — needs verifying before scoping — and
    either way represents a real, unclaimed performance opportunity. ARM64 /
    AArch64 JIT work is deferred until Apple Silicon hardware is available for
    implementation and runtime validation; reassess the scope after VM dispatch
    profiling above is further along.
- Expose deterministic asynchronous services for pure workloads such as
  pathfinding and sight queries.
- Keep legacy ZScript, ACS, and thinker execution serial and authoritative.
- Do not transparently parallelize existing `Tick()` calls: scripts can observe
  ordered mutations, thinker insertion, destruction, list ordering, and RNG.

## Track D: Platform optimization and parity

Intel Metal remains an active target while Apple Silicon is unavailable.
Continue correctness work and measured Intel GPU tuning on the hardware that is
available. Apple Silicon gates claims about TBDR scheduling, memory aliasing,
and ARM64 runtime performance; it does not gate Intel Metal work.

Keep Linux and Windows in the active platform loop alongside Metal. Shared
OpenGL/Vulkan changes need Windows builds and runtime checks, and each native
controller backend needs its own input and haptics validation. A build on one
OS does not establish runtime parity on another. The next Windows step is a
Visual Studio build and a recorded OpenGL/Vulkan runtime baseline, followed by
controller checks when hardware is available. Use the
[Linux and Windows texture upload handover](handoff-framegraph-2026-09-28.md#texture-uploads-on-linux-and-windows)
for the transition baseline and backend-specific measurements.

Controller capability is backend- and device-dependent: Linux evdev exposes
force feedback only when the event node is writable and advertises rumble;
the macOS Cocoa GameController path includes haptics for supported devices on
macOS 11+, while the older IOKit path reports none; Windows XInput has rumble,
while DirectInput reports none. Record the actual backend, OS, controller,
connection type, and tested capabilities for each result.

Haiku is a later portability target. Start with a platform/API audit and a
small build probe after the current Linux and Windows baselines are established;
coordinate contribution form with Haiku and GZDoom maintainers before proposing
upstream changes. Revisit this work as the platform APIs and maintainers' needs
become concrete.

## Current milestone: frame graph coverage and bounded replay

The resource registry, diagnostic pass graph, backend-use and upload
observations are implemented across Metal, OpenGL, and Vulkan. The CPU graph
tracks required outputs, keep-alive passes, dead-pass candidates, transient
lifetimes, sampled material reads, and attachment-preservation dependencies.
See [`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md) for
the current evidence and [`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md)
for Linux follow-ups.

Graph replay now covers bounded Pass2 work, exposure, bloom, and the shared
raster AO path. GL/Vulkan graph-on versus immediate captures were byte-identical
for the tested routes; Intel Metal replay also matched its immediate capture.
The computed order still matches the authored draw order because current
dependencies point forward. This is graph-controlled replay, not a pass-sorting
policy or a performance claim.

Live coverage includes GL/Vulkan main-view effects, AO qualities 1–3, debug and
sample-count routes, HUD/2D UI, offscreen canvas/camera producers, the Vulkan
indexed non-mip upload route, and shadow-map activation. Intel Metal coverage
includes effects, compute-AO conditions, canvas/camera producers, raster-AO
fallback, and 4×/8× scene targets. Intel Metal target-specific camera
clear/opaque/portal-translucent scopes and active/disabled shadow controls
passed on 2026-10-05. Apple Silicon runtime behavior remains open.
Linux allocated-sample AO routing closed on 2026-10-02; Linux quality-1/2
replay parity closed on 2026-10-03.

The matched Linux Vulkan MAP06→MAP07 upload profile closed on 2026-10-05.
`CreateTexture` accumulated about 34 ms across the transition with no explicit
validation-layer override, versus 123 ms with Khronos validation forced; the
100 ms stall trace reported no map-transition loop above threshold. No async
upload or mipmap change is justified without a user-visible hitch in the
normal layer configuration. See
[`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md).

Camera-target identities and clear/opaque/portal-translucent scopes are now
recorded, with GL, Vulkan, and Intel Metal live fixtures. The direct-use and
cross-frame input audit found and fixed the
retained `ShadowMap` boundary; the remaining explicitly persistent inputs and
scene-material fallbacks already have external declarations. The bounded
`scene.target`-before-`shadowmap` ordering experiment is complete on GL and
Vulkan with same-backend pixel-identical controls. Any further ordering change
needs a new candidate, a stable baseline, image equivalence, and a measurable
benefit. The 2026-10-04 RX 550 A/B found no measurable frame-time benefit on
either backend; results are recorded in
[`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md). Do not
enable culling or transient aliasing until output reachability, resource reads,
lifetimes, and keep-alive roots are complete and validated.
Metal/TBDR policy remains gated on Apple Silicon hardware.

## Deferred milestone: compute postprocess

Metal AO and bloom are the first compute vertical slices. Complete visual and
fallback validation before starting another renderer-wide refactor.

Bloom composite modes:

- `mt_compute_bloom_composite 0`: automatic capability selection.
- `mt_compute_bloom_composite 1`: force the Tier 1 high-precision compute plus
  raster-composite path.
- `mt_compute_bloom_composite 2`: require the Tier 2 direct read/write path;
  unsupported hardware falls back to the postprocess implementation.

Validation must cover stable-view comparisons, resizing, fullscreen changes,
portals, camera textures, and unsupported-capability fallback.

The current `mt_metrics` AO/bloom timings measure CPU command-encoding cost.
True per-pass GPU timings require Metal counter sample buffers and must be
reported separately when implemented.

## Deferred investigation: opaque surface batching

The engine already provides `gl_sort_textures`, which sorts plain and masked
walls by texture and clamp flags. Benchmark this option before adding another
sort.

Texture sorting can reduce material bindings, sampler changes, pipeline work,
and batch flushes. It does not automatically collapse thousands of walls into
dozens of Metal draw calls. Walls commonly have different normals, light-list
indices, fog, colors, glow/gradient state, push constants, and stream-buffer
offsets. The current Metal batch emits a separate indexed draw for each
resulting sub-draw.

Before implementation:

1. Compare `gl_sort_textures 0` and `1` in identical stable views. The
   matched 2026-10-05 Linux RX 550 follow-up found a 28.7% lower mean interval
   in Ashes MAP01 and an 8.2% lower interval in DOOM2 MAP02 under
   `-compatmode 3`, both at 1280×720. The busy view contains masked walls and a
   portal; the compatibility spawn view did not expose a masked surface. See
   [`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md). These are two
   views on one machine and do not justify changing the default.
2. Extend draw-state accounting to pipeline, sampler, and per-wall stream
   state on representative masked-wall, portal, camera-texture, and
   compatibility-map views. The 2026-10-05 Linux probe now records normalized
   GL program/raster/target keys, material and effective sampler keys, buffer
   bindings, geometry ranges, and render-state uniform changes. Across Ashes
   MAP01, DOOM2 MAP02 compatibility mode, and the controlled MAP99 camera
   consumer, each opaque list held one pipeline key and zero pipeline-key
   transitions. Texture sorting cut adjacent batch-key runs while leaving the
   distinct key set and draw count unchanged. See the Linux handoff.
3. Count complete batch keys and sub-draw reasons in those views. The source
   audit found and corrected a Metal primitive-key mismatch that flushed each
   wall fan: the pending batch stores `DT_Triangles` after fan conversion, but
   the flush checks compared it with the original `DT_TriangleFan`. The helper
   now normalizes both checks. Intel Metal validation on 2026-10-05 measured
   124 versus 593 flushes/frame and up to 78 fans sharing a batch. It also
   corrected binding the next pipeline before flushing pending triangles;
   the old-order control had seven pipeline mismatches/frame, versus zero
   with the correction. Linux's temporary GL probe now separates the
   candidate batch key from per-draw geometry ranges and uniform state.
   Metal's three-frame 64-bit reason accounting is now complete for this
   Ashes view: corrected sub-draw boundaries numbered 319/315/322, with
   stream-data index changes in 307/301/308 and overlapping lighting/fog
   changes. The primitive control had no within-batch state boundaries.
   The follow-up also exercised point/line draws, the real 256-sub-draw
   limit, and small/full-capacity boundaries. An approved pre-write reservation
   fixed an index-buffer overrun hazard found during review. Static DOOM2 MAP02
   corrected/control captures matched exactly; Intel timings found no benefit
   above their measurement floor. Other primitive/state combinations and
   allocation-failure limits remain untested. See
   [`handoff-metal-batching-2026-10-05.md`](handoff-metal-batching-2026-10-05.md)
   and the Linux handoff.

The likely long-term solution is a GPU `SurfaceData` table indexed by a
per-vertex or per-primitive surface ID. Moving normals, light indices, fog,
colors, and related wall state out of per-draw push constants would allow walls
sharing a material and pipeline to become genuine combined indexed draws. This
belongs with render packets and the frame-graph work rather than the bloom
stabilization change.

## Near-term order

1. Camera target identities and clear/opaque/portal-translucent graph scopes
   are implemented and live-validated on GL, Vulkan, and Intel Metal. Linux's
   controlled fixture also confirms sustained camera-texture consumption on
   both GL and Vulkan; see
   [`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md) and
   [`handoff-macos-2026-10-04.md`](handoff-macos-2026-10-04.md).
2. The source audit of keep-alive roots, external side effects, and
   cross-frame inputs is complete for the reviewed paths. The persistent
   `ShadowMap` boundary has positive, negative, and same-frame-producer
   self-test coverage. Intel Metal active/disabled shadow controls passed on
   2026-10-05. A later controlled producer omission established live retained
   reads and a missing-external negative control; naturally occurring producer
   omission remains unestablished. See
   [`handoff-macos-2026-10-04.md`](handoff-macos-2026-10-04.md).
3. The bounded `scene.target`-before-`shadowmap` order experiment is complete
   on GL and Vulkan, with same-backend pixel-identical controls. Require a new
   candidate and a measurable benefit before another order change. Its Linux
   RX 550 A/B found no measurable frame-time benefit; see
   [`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md).
4. Intel Metal wall-fan validation and initial aggregate batching counts
   completed on 2026-10-05, including the pipeline-order correction and a
   positive failure control. Fine-grained flush/sub-draw masks and per-field
   distributions are recorded for three settled frames of the Ashes view;
   additional static MAP02 and controlled limit/primitive/capacity routes were
   validated afterward. Uninstrumented Intel timing comparisons found no
   benefit above their measurement floor. Further primitive/state combinations,
   allocation-failure limits, and Apple Silicon remain untested. The matched
   1280×720 Linux measurements, temporary
   state probe, and sustained camera-consumer control are recorded in
   [`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md); the Metal
   source fix and Mac validation are in
   [`handoff-macos-2026-10-04.md`](handoff-macos-2026-10-04.md). Do not change
   sort behavior or defaults based on the current Linux results alone.
5. Revisit Metal/TBDR ordering policy and ARM64 JIT work when Apple Silicon is
   available for runtime validation; keep Intel Metal correctness and measured
   tuning active in the meantime.
6. Establish the Windows Visual Studio build and OpenGL/Vulkan runtime baseline,
   then validate XInput/DirectInput input and haptics independently from Linux
   and macOS.
7. Prototype compute light-list construction.
8. Start the standalone deterministic visibility CPU reference.

## Follow-on (not started): public developer wiki

Once Apple Silicon is validated (Tasks — macOS item 3) and the frame graph
scheduler has landed, consider a GitHub wiki distilling `AGENTS.md`, this
document, and the Metal/Vulkan field guides into public-facing engine
architecture reference. Scope is architecture documentation for this fork
specifically, not modding tutorials (Ultimate Doom Builder, ZScript-for-
beginners, etc.) — that ground is already well covered by the existing Doom
community and isn't worth re-treading. Deliberately not scheduled earlier:
writing it against an architecture that's still moving (frame graph,
Apple Silicon TBDR differences) means rewriting it as soon as it's done.
