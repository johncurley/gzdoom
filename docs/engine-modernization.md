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
2. Introduce an immutable render-world snapshot.
3. Build a backend-neutral frame graph.
4. Migrate AO, bloom, presentation, and other bounded postprocess passes.
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
  - The ZScript JIT (`src/common/scripting/jit/`) is written entirely against
    asmjit's `X86Gp`/`X86Xmm` types with no ARM/AArch64 codepath or
    architecture guard found on audit (2026-07-10). Apple Silicon and any
    other ARM64 target currently get either a silent fallback to the VM
    interpreter or a broken JIT path — needs verifying before scoping — and
    either way represents a real, unclaimed performance opportunity. An
    ARM64/AArch64 asmjit backend for the JIT is a candidate Track C /
    per-platform item once VM dispatch profiling above is further along.
- Expose deterministic asynchronous services for pure workloads such as
  pathfinding and sight queries.
- Keep legacy ZScript, ACS, and thinker execution serial and authoritative.
- Do not transparently parallelize existing `Tick()` calls: scripts can observe
  ordered mutations, thinker insertion, destruction, list ordering, and RNG.

## Track D: Per-operating-system optimization (future)

Once the compute-shader postprocess conversion (Track A) covers the
significant passes, and after the ARM64 JIT gap above is scoped, the next
horizon is per-OS/per-architecture optimization work rather than
per-rendering-backend work: e.g. the ARM64 JIT backend, and any
platform-specific paths worth adding for Linux and Windows once the macOS
Metal path is mature. Not yet broken into concrete steps — revisit once
Tracks A/C above are further along.

## Current milestone: compute postprocess

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

1. Compare `gl_sort_textures 0` and `1` in identical stable views.
2. Instrument batch-break and sub-draw reasons.
3. Count unique textures, complete batch keys, and per-wall stream states.
4. Check masked walls, portals, camera textures, and compatibility maps.

The likely long-term solution is a GPU `SurfaceData` table indexed by a
per-vertex or per-primitive surface ID. Moving normals, light indices, fog,
colors, and related wall state out of per-draw push constants would allow walls
sharing a material and pipeline to become genuine combined indexed draws. This
belongs with render packets and the frame-graph work rather than the bloom
stabilization change.

## Near-term order

1. Finish Tier 1/Tier 2 bloom runtime validation.
2. Record stable Intel baseline measurements.
3. Define the render snapshot and frame-graph interfaces.
4. Migrate existing postprocess passes incrementally.
5. Prototype compute light-list construction.
6. Start the standalone deterministic visibility CPU reference.

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
