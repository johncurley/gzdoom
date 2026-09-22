# Handoff to macOS — Intel Metal framegraph validation — 2026-09-23

This is the next macOS task after the Linux framegraph and renderer-tuning
tranche. The Linux side has established the shared contracts and verified the
OpenGL/Vulkan observers. The macOS side should now validate the same contracts
against the native Metal renderer on the Intel reference machine.

This handoff is intentionally about correctness and observation first. It does
not authorize Metal-specific scheduling, transient aliasing, memoryless
attachments, or other TBDR policy. Apple Silicon has never run this renderer;
those decisions remain gated on real M-series hardware.

## Current state

The framegraph is no longer only a design document:

- `FrameResources` is a shared backend-neutral registry for declared resources,
  dimensions, formats, handles, per-frame reads/writes, and validation.
- `FrameGraph` records pass reads/writes, computes deterministic RAW
  dependencies, reports lifetimes/dead-pass candidates, and describes upload
  readiness. It does not reorder work, allocate resources, emit barriers, or
  change execution.
- The shared postprocess path records the pass names used by the graph. The
  graph covers the real bloom, SSAO, exposure, tonemap, colormap, lens, and
  FXAA paths when those paths actually run.
- Metal already declares its scene/pipeline targets, AO resources, and bloom
  resources; its scene, postprocess, AO, and bloom code also feeds the graph's
  backend-observation hooks.
- Metal material binding already gives sampled world textures stable graph
  identities and records their observed reads.
- Vulkan records upload readiness for hardware and named postprocess textures.
  OpenGL's ordinary world-texture path remains synchronous, so it observes the
  resource read without inventing a separate transfer record.
- The Metal texture loader already performs CPU texture processing on a GCD
  worker queue. Metal GPU texture creation and upload remain on the render
  thread. This is not yet a shared loader and should not be made one
  speculatively.

The important boundary is:

```text
CPU decode/processing -> backend GPU upload -> first material/postprocess read
        worker              Metal render thread              framegraph consumer
```

The graph should describe that relationship. It should not own `FTexture`,
`DObject`, or backend texture lifetimes.

## First macOS objective

Validate that Intel Metal produces the same truthful graph and resource report
as the existing Linux backends, without changing the renderer's execution
policy.

Use a fixed viewpoint and a fixed configuration for capture comparisons. Test
at least:

1. Stock scene rendering with postprocess disabled.
2. Bloom and SSAO enabled, so the full postprocess resource set is exercised.
3. A cold material/texture precache, followed by a warm repeat.
4. A level transition or new-area traversal, where asynchronous texture work
   can complete while normal rendering continues.

For each case, collect:

- `r_resources` after a real rendered frame;
- `r_resource_validate 1` output, checking for stale-size diagnostics;
- `r_framegraph` after a real rendered frame, checking `ok=true` and an empty
  report;
- `mt_metrics` and, for cold-load runs, `mt_frametrace`/`mt_stalltrace` as
  appropriate;
- a deterministic screenshot or harness capture against the existing Metal
  baseline.

Do not use `+quit` as proof that a real framegraph ran. Command-line `+`
commands execute before the main frame loop. Use an actual short play session,
an existing save, or a temporary in-game console command after the first frame.

## Metal upload work

The first code-level gap is upload observability, not a new loader.

`MtTextureManager::PerformAsyncGPUUpload()` is the natural point to record a
`FrameGraphUploadDesc` for the Metal texture's stable graph resource name, but
the fields must describe the actual ordering guarantees:

- preparation: worker, because `CreateTexBuffer()` runs on the GCD worker;
- staging ownership: true only while the staging data is retained until the
  blit has consumed it;
- transfer recorded: true only after the Metal blit encoder has been recorded;
- completion ordered: true only if the command-buffer submission/queue
  ordering guarantees the first draw cannot sample before the upload. Do not
  equate `commit()` with GPU completion without checking the existing command
  queue model.

The existing loader captures a raw `FTexture *` in a background block. Before
making this more general or increasing concurrency, establish that the source
texture cannot be destroyed or mutated for the duration of the task. If that
cannot be proven, stop and propose a stable texture identity/generation or
retained source-data contract. Do not silently introduce a `DObject` lifetime
race.

Measure before and after any upload change:

- cold first-use latency;
- number and total CPU time of texture uploads;
- warm-run frame time;
- whether the first material read occurs only after the recorded upload;
- visual parity and absence of partially uploaded textures.

An upload record that merely makes `r_framegraph` look complete is not enough;
the control run must demonstrate that the instrument detects an intentionally
missing ordering flag or equivalent invalid state.

## What is explicitly deferred

Do not implement these on the Intel-only pass:

- transient resource aliasing or memoryless Metal attachments;
- pass reordering or graph-owned scheduling;
- tile-memory load/store policy;
- replacing Metal's command-buffer ownership with a graph allocator;
- moving GPU texture creation or Metal API calls to worker threads;
- a common loader that captures raw engine texture objects without a lifetime
  contract;
- changing default draw sorting or material order as part of this handoff.

Stable wall/flat sorting is shared scene code and should mechanically apply to
Metal, but it remains an independently measured opt-in experiment until Metal
captures confirm the same correctness and transition behavior.

## Acceptance criteria for this handoff

The macOS tranche is complete when all of the following are recorded with
commands, configuration, and output in the next handoff or commit:

1. Intel Metal builds cleanly from the current branch.
2. `r_framegraph_selftest` passes.
3. A real Metal frame reports declared and touched scene/postprocess resources
   without stale-size errors.
4. The graph reports the passes that actually ran, rather than a static
   template; disabled effects are absent for the expected reason.
5. Cold and warm texture-load runs show whether Metal upload records are
   complete and correctly ordered. If upload recording is not yet implemented,
   record that as the measured gap instead of claiming completion.
6. Existing Metal-vs-OpenGL capture parity remains within the established
   baseline for the tested scene/configuration.
7. Any Metal-only change includes a control result and does not claim Apple
   Silicon behavior.

After this acceptance pass, Linux work can continue independently on Vulkan and
OpenGL tuning. Apple Silicon remains the gate only for the policy layer that
exploits TBDR behavior.

## Useful references

- `docs/frame-graph-resources.md` — registry vocabulary and validation policy.
- `docs/frame-analysis.md` — Metal pass/resource inventory.
- `docs/frame-analysis-vulkan-gl.md` — Linux backend counterpart.
- `docs/renderer-methodology.md` — measurement and falsification rules.
- `docs/gpu-capture-protocol.md` — capture procedure.
- `docs/handoff-framegraph-2026-09-21.md` — Linux implementation history.
- `src/common/rendering/metal/textures/mt_textureloader.{h,cpp}` — current
  worker-side texture processing contract.
- `src/common/rendering/metal/textures/mt_texture.cpp` — current render-thread
  GPU upload path.
