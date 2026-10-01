# macOS handoff — Metal offscreen, UI, and AO framegraph coverage — 2026-10-02

## Purpose

The 2026-10-01 Linux tranche added graph scopes for offscreen canvas/camera
updates and HUD/2D UI consumers across GL, Vulkan, and Metal. GL and Vulkan
were built and exercised on the RX 550. The Metal source was updated but could
not be compiled or run in that Linux configuration. This handoff records the
Intel macOS validation and live canvas, camera, and AO fallback coverage.

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

Neither stock MAP06 run created a canvas or camera texture. The continuation
below records separate live canvas and frame-1 camera fixtures, as well as the
graph-replay comparison. Apple Silicon runtime and performance remain
hardware-specific open boundaries.

## Final coverage completion — 2026-10-01/02

The Metal encoder-binding observer ran after batched 2D canvas commands had
left their offscreen graph scope, so it missed those sampled inputs. Added a
no-op backend hook to `FRenderState`, called from shared `Draw2D()` after a
texture material is selected. Metal resolves the material's hardware texture
layers and records them only while rendering a canvas or camera view. GL and
Vulkan retain the no-op implementation.

The Intel build passed with `cmake --build build --parallel 4`. A final live
MAP09 probe used a temporary PK3 outside the repository: `FGSOURCE` drew
`BRICK1`, and `FGTARGET` sampled `FGSOURCE`. Metal reported 9 passes / 15
edges, `Backbuffer` as output, no dead-pass candidates, and no graph-build
failure. The source canvas used stable resource `Metal.Texture.92` in the final
run and carried its preservation read; the later target canvas sampled that
resource, producing a RAW edge between the two `offscreen.canvas` passes. The
source pass also recorded its sampled texture layers. `r_framegraph_selftest`
reported `selftest: PASS`. Resource validation reported no stale-size
diagnostics; only the unused screen/save shadow maps were untouched.

The separate Metal replay comparison used the deterministic DOOM2 MAP06 scene,
`screenblocks 12`, bloom/SSAO/tonemap/lens/FXAA enabled, Intel compute AO and
bloom enabled, and 120 settled frames. Immediate mode had all four
`r_framegraph_*` switches disabled; replay mode enabled them. Both final-build
graphs reported 29 passes / 60 edges, `Backbuffer`, and no dead-pass
candidates. The 1440x900 captures were byte identical: max channel delta 0,
zero differing pixels. A repeated immediate capture also matched exactly.
The change therefore closes the live canvas producer/consumer and
graph-replay pixel coverage. Camera coverage initially appeared absent because
the dump ran at frame 60, after the one-time initial update and its graph had
been reset. A corrected temporary MAP09 UDMF fixture placed a camera thing,
kept it alive with a looping state, and dumped on frame 1. The graph recorded
two `offscreen.camera` passes, including `FGCAMERA` writing stable resource
`Metal.Texture.1908`; the graph had 10 passes / 17 edges, `Backbuffer`, and no
dead-pass candidates. This verifies the Intel Metal camera producer path.

## Intel Metal raster AO fallback replay — 2026-10-02

**Prediction:** single-sample quality 1 and 2 each retain the five-pass AO
chain (`lineardepth`, `occlude`, horizontal/vertical blur, `combine`). Raw
debug mode 2 should omit both blur passes, leaving three. Immediate and graph
replay captures should match exactly within each route; repeated launches of
each arm should also match.

The Intel build passed with `cmake --build build --parallel 4`. CMake refreshed
`build/gzdoom.pk3`, so it was copied beside the app executable before launch;
the bundle copy and generated archive compared equal. Real DOOM2 MAP06 captures
used `screenblocks 12`, 120 settled frames, and an actual 800x572 viewport.
`mt_caps` identified Intel Metal and the resolved `reference PP
(hw_postprocess.ssao)` path with compute AO off. Bloom, tonemap, lens, and FXAA
were disabled. An initial cold warm-up returned 1152x720 and was discarded
before the final interleaved set; its warm-ups and all retained captures had
matching 800x572 dimensions.

`r_framegraph_selftest` passed in every launch. Quality 1 and 2 each reported
12 total frame passes / 32 edges, with the five expected AO passes. Quality 3
with `gl_ssao_debug 2` reported 10 passes / 27 edges; its graph omitted both
blur passes and retained linear depth, occlusion, and combine. Every graph was
rooted at `Backbuffer`, had no dead-pass candidates, and reported no build
failure. The live cvar query printed `r_framegraph_ao=false` for immediate and
`true` for replay. Two captures per arm were pixel-identical for immediate
mode and for replay; immediate and replay were also pixel-identical for all
three routes.
The low and medium captures differed deterministically by max channel delta 3,
mean delta 0.2199, with 8 pixels above delta 2 in the harness analysis region.
This closes single-sample low/medium fallback and raw-debug replay parity; it
does not cover multisample fallback or other debug modes, and makes no
performance claim.

## Validation checklist

1. **Complete.** Built the current tree with the Intel Metal configuration:

   ```bash
   cmake --build build --parallel 4
   ```

2. **Complete.** Ran `r_framegraph_selftest` in the bundled app. A self-test
   pass checks the graph contract, not live Metal pass recording.

3. **Complete.** The effects-on MAP06 run used `screenblocks 10`, resource
   validation, and a live graph dump. It recorded `ui.hud` in the expected
   post-bloom position and `ui.2d` before present; the graph was rooted at
   `Backbuffer`, had no dead-pass candidates or graph-build failures, and
   resource validation reported no stale sizes. UI pipeline-image and
   depth/stencil backend observations matched their declarations.

4. **Complete.** With the five effects disabled, their passes disappeared while
   scene, UI, and present passes remained represented. The HUD remained because
   `screenblocks 10` kept that runtime path active.

5. **Complete.** The two-canvas fixture verified stable
   producer names, preservation reads, sampled inputs attached to the producer,
   and a later canvas consumer dependency. The frame-1 camera fixture recorded
   two `offscreen.camera` producers; its first-frame timing is required because
   the initial camera update is one-shot and later frames reset the graph.

6. **Complete.** Same-machine MAP06 captures in immediate and replay modes,
   plus a repeated immediate control. All compared images matched byte for
   byte.

## Acceptance record

For future handoffs, record the build configuration and result,
`r_framegraph_selftest` output,
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
