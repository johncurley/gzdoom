# macOS handoff — Metal offscreen, UI, and AO framegraph coverage — 2026-10-05

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
does not cover multisample fallback, and makes no performance claim.

## Intel Metal raster AO debug branches — 2026-10-02

**Prediction:** debug modes 1, 3, and 4–10 on the single-sample raster fallback
should produce stable immediate/replay captures. Mode 1 should retain blur;
modes 3 and 5–10 should retain the raw three-pass AO route. Mode 4 displays
`SceneNormal` in the combine pass, so the graph should identify
`ssao.lineardepth` and `ssao.occlude` as dead-pass candidates in that debug
view. Other modes should have no dead-pass candidates.

The real DOOM2 MAP06 captures used the same Intel Metal raster fallback and
800x572 viewport as the preceding run, with `gl_ssao 3`, `gl_multisample 1`,
and bloom, tonemap, lens, and FXAA disabled. Every launch proved the active
`reference PP (hw_postprocess.ssao)` route, queried the requested debug cvar,
and passed `r_framegraph_selftest`. For each mode, the graph was checked for
`Backbuffer` reachability and the expected liveness result. Mode 4's combine
read was `SceneNormal`; every other tested mode read `AO.Ambient0`.

Mode 1 reported 12 passes / 31 edges and kept all five AO passes, including
both blur passes. Each of modes 3 and 4–10 reported 10 passes / 27 edges and
kept `ssao.lineardepth`, `ssao.occlude`, and `ssao.combine`, with blur absent.
Modes 1, 3, and 5–10 had no dead-pass candidates. Mode 4 reported exactly
`ssao.lineardepth ssao.occlude`, as predicted for its normal-display branch.
All graphs reported `Backbuffer` as output and no build failure.

Two captures per arm were byte-identical for both immediate and replay, and
immediate/replay captures also matched byte-for-byte for every mode. The
first warm-up capture in debug mode 5 replayed a different image, while its
two settled captures matched the immediate arm exactly; only the settled,
interleaved captures were used for parity. This closes the remaining
single-sample raster AO debug branches (debug modes 0–10 across this and the
preceding run). The subsequent section records the Metal multisample
implementation and validation. No performance claim is made.

## Intel Metal multisample scene targets and AO fallback — 2026-10-02

**Prediction:** on this Intel device, requesting 4× should allocate 4-sample
scene color/depth/normal/fog targets plus a single-sample scene-color resolve
texture. The real render graph should carry that resolve into the postprocess
chain and keep every pass live through `Backbuffer`. Repeated 4× captures,
immediate/replay captures, and runtime 1×↔4× toggles should be pixel-identical
within each sample-count route. Compute AO requested with a multisample target
should select the shared raster AO fallback, since Metal compute AO reads
single-sample depth and normal textures.

The final Intel build passed `cmake --build build --parallel 4`; the rebuilt
`gzdoom.pk3` was copied into the app bundle and verified identical with `cmp`.
The self-test passed, including a positive multisample resolve graph case.
Live runs used stock DOOM2 MAP06 on the Intel MacBookAir7,2 / macOS 12.7.6,
with a settled 800x572 capture viewport. `mt_caps` reported requested 4,
allocated 4. The 4× graph reported 12 passes / 35 edges, with
`SceneColor.Resolve` written by the scene target and subsequent scene draws,
then read by `scene.resolve`; it was declared and touched in `r_resources`.
There were no stale-size reports or dead-pass candidates. The 1× graph had 12
passes / 32 edges and no resolve resource.

Two immediate 4× captures and two graph-replay 4× captures were pixel-identical
to one another; immediate and replay also matched exactly. Runtime 1×→4×
matched the steady 4× capture, and runtime 4×→1× matched the steady 1× capture.
With equal-sized 1× and 4× captures, the measured difference had max channel
delta 18, mean delta 0.7026, 2.89% of pixels above delta 2, and no pixels above
the harness's hot-pixel threshold. Requesting Intel compute AO at 4× selected
`reference PP (hw_postprocess.ssao) <- multisample scene target` and matched
the raster-fallback 4× capture exactly. These results establish functional
4× support and repeatability on this Intel Mac; higher sample counts and Apple
Silicon behavior/performance remain unverified.

Operational note: this macOS build aborts in `NSApplication sharedApplication`
when its GUI executable is launched directly. Live runs must launch the app
bundle through LaunchServices (`open -W -n -a ...`); the temporary capture
launcher collected GZDoom's own logfile because `open` does not forward the
app's stdout to its caller.

## Intel Metal 8× scene targets and request fallback — 2026-10-05

**Prediction:** an 8× request should allocate the highest supported sample
count, retain the scene-color resolve dependency, and produce identical
immediate/replay images. Repeated captures and runtime 1×↔8× changes should
match their steady-state routes. A request above the device's supported counts
should fall back without changing the image; compute AO requested with MSAA
should continue to select raster AO.

The clean `main` checkout at `09d90efed6` passed
`cmake --build build --parallel 4` with `HAVE_VULKAN=OFF`. The generated
`gzdoom.pk3` was copied beside the app executable and compared equal. Runs used
LaunchServices, a dedicated temporary config, stock DOOM2 MAP06,
`screenblocks 12`, quality-3 raster AO/debug 0, and bloom/tonemap/lens/FXAA
disabled. `mt_caps` confirmed requested 8 / allocated 8. Requesting 64 also
allocated 8, establishing this device's highest supported power-of-two count
through the existing allocator. No renderer code changed.

The retained set used 300 rendered level frames and an actual 800×572 viewport.
Two interleaved captures each of 1× immediate, 8× immediate, and 8× replay
were byte-identical within each route; 8× immediate/replay also matched exactly
(maximum channel delta 0, zero differing pixels). The 8× graph reported
12 passes / 35 edges, with `SceneColor.Resolve` feeding `scene.resolve`; 1×
reported 12 / 32 and no resolve resource. Every retained graph had
`Backbuffer` as its output, no dead-pass candidates, a passing self-test,
and no postprocess replay fallback diagnostic. Runtime 1×→8× matched steady
8×, and 8×→1× matched steady 1×. Requesting compute AO at 8× selected the
multisample raster fallback and matched steady raster 8×. Requesting 64× also
matched it.

Separate final replay captures explicitly enabled `r_resource_validate 1` and
matched the retained 8× and 1× images. Validation emitted no stale-size
diagnostics. The registry reported 20 resources / 72.9 MB at 8× and
19 resources / 19.2 MB at 1×; only the unused screen/save shadow maps were
untouched. The resolve entry disappeared at 1× as expected.

The same-size 1× versus 8× positive control differed by maximum channel delta
16, mean delta 0.8398, and 32,371 pixels above delta 2 (7.074%); the harness
reported zero hot pixels. Thus the zero-noise parity measurements can detect
the sample-count change. The retained 8× image was also inspected directly.

**Rejected captures:** the initial 120-frame set included a 1× startup-screen
image (2 passes / 1 edge, only UI and present), despite logging MAP06 and
requested 1 / allocated 1. Another 1× warm-up differed from later settled
captures. Those images do not establish scene parity and were excluded. The
300-frame rerun explicitly required scene and AO passes and repeated each
steady-state route before comparison. The first cold 8× image also had different
dimensions and was excluded. Temporary scripts, logs, and captures live under
`/private/tmp/gzdoom-metal-msaa-high` and `/private/tmp/gzdoom_metal_high_samples*`.
This closes the tested Intel 8× route and above-limit request fallback;
Apple Silicon correctness and performance remain open.

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
7. **Complete.** Single-sample raster AO debug modes 0–10 passed the recorded
   graph and pixel-repeat checks. Intel Metal 4× scene rendering and AO fallback
   passed resolve-graph, replay-repeatability, runtime-toggle, and same-size 1×
   versus 4× checks. The 2026-10-05 continuation also passed Intel 8× replay,
   runtime toggles, and above-limit request fallback. Apple Silicon remains open.

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
