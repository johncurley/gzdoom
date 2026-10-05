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
controller checks when hardware is available.

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
fallback, and 4× scene targets. The target-specific camera clear/opaque/
portal-translucent scopes still need an Intel Metal rebuild and live fixture;
higher Metal sample counts and Apple Silicon runtime behavior also remain open.
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
recorded, with GL and Vulkan live fixtures; Intel Metal still needs a rebuild
and fixture. The direct-use and cross-frame input audit found and fixed the
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
   compatibility-map views. The Linux follow-up reduced sampler calls and
   material-key runs, while draw submissions stayed fixed and wall-stream
   breaks fell only modestly. Program-pointer changes were counted, but a full
   pipeline key was not. The camera producer was observed only on startup, so
   sustained consumer coverage remains open; see the Linux handoff.
3. Count complete batch keys and sub-draw reasons in those views. The source
   audit found and corrected a Metal primitive-key mismatch that flushed each
   wall fan: the pending batch stores `DT_Triangles` after fan conversion, but
   the flush checks compared it with the original `DT_TriangleFan`. The helper
   now normalizes both checks. The Linux build does not compile Metal; validate
   the patch on Intel Metal and collect actual batch sizes and flush reasons
   using [`handoff-macos-2026-10-04.md`](handoff-macos-2026-10-04.md).

The likely long-term solution is a GPU `SurfaceData` table indexed by a
per-vertex or per-primitive surface ID. Moving normals, light indices, fog,
colors, and related wall state out of per-draw push constants would allow walls
sharing a material and pipeline to become genuine combined indexed draws. This
belongs with render packets and the frame-graph work rather than the bloom
stabilization change.

## Near-term order

1. Camera target identities and clear/opaque/portal-translucent graph scopes
   are implemented. GL and Vulkan live fixtures pass; rebuild and repeat the
   camera fixture on Intel Metal, as recorded in
   [`frame-graph-camera-resources.md`](frame-graph-camera-resources.md).
2. The source audit of keep-alive roots, external side effects, and
   cross-frame inputs is complete for the reviewed paths. The persistent
   `ShadowMap` boundary has positive, negative, and same-frame-producer
   self-test coverage. Rebuild and validate the conditional shadow-map route
   on Intel Metal using
   [`handoff-macos-2026-10-04.md`](handoff-macos-2026-10-04.md).
3. The bounded `scene.target`-before-`shadowmap` order experiment is complete
   on GL and Vulkan, with same-backend pixel-identical controls. Require a new
   candidate and a measurable benefit before another order change. Its Linux
   RX 550 A/B found no measurable frame-time benefit; see
   [`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md).
4. Rebuild and validate the Metal wall-fan batch correction on Intel macOS,
   capturing batch size, sub-draw count, and flush reasons in a masked/portal
   view. Then complete the batch-key audit. The matched 1280×720 Linux
   measurements, temporary state probe, camera fixture limitation, source fix,
   and Mac validation steps are recorded in
   [`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md) and
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
