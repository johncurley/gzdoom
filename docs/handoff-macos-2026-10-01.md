# macOS handoff — Metal offscreen and UI framegraph coverage — 2026-10-01

## Purpose

The 2026-10-01 Linux tranche added graph scopes for offscreen canvas/camera
updates and HUD/2D UI consumers across GL, Vulkan, and Metal. GL and Vulkan
were built and exercised on the RX 550. The Metal source was updated but could
not be compiled or run in that Linux configuration. This handoff asks the
Intel macOS machine to validate the Metal implementation and, if practical,
exercise an actual canvas or camera texture update.

This is correctness and graph-coverage work. It does not authorize pass
reordering, Metal-specific scheduling, transient aliasing, or TBDR policy.
Apple Silicon remains required for Metal performance and scheduling decisions.

## Changes to validate

- `FrameGraph::ObserveSceneMaterialRead()` now attaches sampled material reads
  to active `offscreen.*` and `ui.*` scopes as well as `scene.*` scopes.
- Metal `RenderTextureView()` records `offscreen.canvas` and
  `offscreen.camera` producers by the stable hardware-texture resource name.
  Canvas passes read and write the texture because drawing may preserve
  untouched pixels. Camera passes write the target after viewpoint setup
  clears it. Both producer passes are keep-alive.
- Metal `Draw2D()` records its blended current-pipeline-image read/write and
  stencil clear as `ui.2d`.
- Shared postprocess code records the post-bloom HUD callback as `ui.hud`, and
  `HWDrawInfo::EndDrawScene()` records the optional HUD model as
  `scene.hud_model`.
- `r_framegraph_selftest` covers canvas preservation, a camera producer with
  a sampled material, UI sampling of both textures, output reachability, and
  depth/stencil backend-use validation.

The Linux live MAP06 route exercised `ui.hud` and `ui.2d` on both GL and
Vulkan. Stock MAP06 did not create canvas or camera textures, so no live
offscreen producer result is claimed yet. Full Linux details are in
[`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md); implementation
history is in
[`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md).

## Intel Mac validation record — 2026-10-01

The Intel build passed with `cmake --build build --parallel 4`. It linked all
109 pre-translated MSL stages, rebuilt the app bundle, and refreshed
`gzdoom.pk3` with the 17 new shader files. `r_framegraph_selftest` reported
`selftest: PASS`, including the hazard, offscreen/UI, compute-bloom, and
compute-AO graph controls.

A real DOOM2 MAP06 effects-on run used `screenblocks 10`, `gl_bloom 1`,
`gl_ssao 3`, tonemap/lens/FXAA enabled, Intel compute AO and bloom enabled, and
the Pass2/exposure/bloom/AO graph switches enabled. The live graph reported 26
passes / 58 edges, `Backbuffer` as output, and no dead-pass candidates. It
recorded `ui.hud` after bloom and before tonemap, and `ui.2d` before present.
Resource validation reported 40 resources / 66.5 MB, no stale-size diagnostics,
and only the unused screen/save shadow maps untouched.

A second MAP06 run disabled bloom, SSAO, tonemap, lens, and FXAA while retaining
`screenblocks 10`. Its graph reported 7 passes / 14 edges, rooted at
`Backbuffer`, with no dead-pass candidates. The optional effect passes were
absent while `ui.hud` and `ui.2d` remained. Resource validation reported 16
resources / 42.5 MB, no stale-size diagnostics, and only the unused shadow maps
untouched. The self-test passed in this run as well.

Neither stock MAP06 run created a canvas or camera texture. Those producer
paths have self-test coverage but still need a live fixture. No graph-on/off
pixel comparison was made on Metal; the separate Linux handoff records exact
pixel matches for the tested GL/Vulkan graph-replay routes. This closes the
Intel Metal build, self-test, and live UI/HUD effects-on/off tasks, not the
offscreen fixture or Apple Silicon runtime boundary.

## Tasks

1. Build the current tree with the Intel Metal configuration:

   ```bash
   cmake --build build --parallel 4
   ```

2. Run `r_framegraph_selftest` in the bundled app and record its output. A
   self-test pass checks the graph contract, not live Metal pass recording.

3. Run a real MAP06 frame with bloom, SSAO, tonemap, lens, and FXAA enabled,
   plus `screenblocks 10` so both HUD and 2D UI paths draw. Enable resource
   validation and dump `r_resources` and `r_framegraph` after the scene has
   rendered. Check that:
   - `ui.hud` is recorded between the bloom work and later postprocess passes
     that actually run;
   - `ui.2d` appears before present;
   - the graph is rooted at `Backbuffer`, reports no dead-pass candidates or
     graph-build failures, and resource validation reports no stale sizes;
   - the UI pipeline-image and depth/stencil backend observations match their
     declarations.

4. Repeat with those five effects disabled. Confirm the optional effect passes
   disappear while the scene, UI, and present passes that still execute remain
   represented. Explain any absent HUD pass by the actual runtime condition.

5. If a reproducible canvas/camera-texture fixture is available, force an
   update and dump the graph in a real Metal frame. Confirm the producer uses
   the texture's stable resource name, observed material reads attach to the
   producer, the canvas carries its prior-value dependency, and a later
   consumer reads the updated value. If no fixture is available, report that
   boundary explicitly; do not treat the self-test as a live-render result.

6. If the live UI/HUD recording or offscreen fixture changes rendered output,
   compare against a same-machine capture with graph observation disabled or
   the established Metal/OpenGL capture control. These scopes are diagnostic;
   no image change is expected.

## Acceptance record

Record the build configuration and result, `r_framegraph_selftest` output,
scene/cvar setup, pass and edge counts, required-output and dead-candidate
results, resource validation output, and whether an offscreen fixture actually
ran. Keep Metal correctness evidence separate from Apple Silicon performance
claims. Do not start ordering changes as part of this handoff.

## References

- [`handoff-macos-2026-09-23.md`](handoff-macos-2026-09-23.md) — Intel Metal
  validation history and broader acceptance criteria.
- [`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md) —
  current framegraph state and Linux results.
- [`renderer-methodology.md`](renderer-methodology.md) — renderer evidence
  and measurement controls.
- [`gpu-capture-protocol.md`](gpu-capture-protocol.md) — capture procedure.
