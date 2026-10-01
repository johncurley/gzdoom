# Linux handoff — attachment-preserving framegraph dependencies — 2026-09-29

## Current state

The Intel Metal framegraph validation tranche is complete for now. The
2026-09-29 change makes prior attachment contents visible to graph liveness
analysis: scene color/depth/G-buffer draws record logical reads alongside
writes, and blended postprocess outputs read their prior value. Backend bind
observations remain attachment writes. This records diagnostic dependencies;
the graph still does not schedule or reorder rendering.

The defect was visible in a live MAP06 graph: scene passes and the SSAO
composite could be reported dead because the graph saw only the later write to
an attachment, not the draw's dependence on its existing contents. The
self-test now has both a positive output-rooted attachment chain and a
negative control that omits the translucent attachment read and expects the
SSAO composite to be reported dead.

## Intel Metal evidence

On 2026-09-29, the Intel macOS build passed with `HAVE_VULKAN=OFF`. A real
Metal MAP06 run with bloom, SSAO, tonemap, lens, and FXAA enabled reported 52
passes / 65 edges, `Backbuffer` as the required output, no dead-pass
candidates, and no graph build errors. The retained chain included
`scene.target -> scene.opaque -> ssao.combine -> scene.portal_translucent ->
scene.resolve -> present`; runtime material reads appeared on the scene
passes. The resource report showed 38 resources / 48.5 MB, with only unused
screen/save shadow maps untouched and no stale-size diagnostics.

A second real run with all five effects disabled reported 5 passes / 6 edges,
with the expected scene and present passes only. There were no dead-pass
candidates or stale-size diagnostics. The self-test passed in both runs.
These checks validate Intel Metal graph recording and liveness reporting; they
do not establish Apple Silicon behavior, conditional Metal path coverage,
image parity, or performance.

## Linux RX 550 validation — 2026-09-29

The Linux build passed at `93671600a` with `HAVE_VULKAN=ON`:

```bash
cmake --build build --parallel $(nproc)
```

The build completed successfully. Runtime captures used an AMD Radeon RX 550 /
RADV POLARIS12 (Mesa 26.2.3-arch3.1; GL 4.6 and Vulkan 1.4.354) on Wayland.
They rendered DOOM2 MAP06 at 640x480 for 180 in-level frames before
`r_resources` and `r_framegraph`. Each run executed
`r_framegraph_selftest` first. GL used `+vid_preferbackend 0`; Vulkan used
`+vid_preferbackend 1`. Effects-on enabled bloom, SSAO 3, tonemap, lens, and
FXAA; effects-off set all five to zero. Vulkan runs used
`VK_LAYER_KHRONOS_validation`.

| Backend | Effects | Graph | Resources | Untouched resources |
| --- | --- | --- | --- | --- |
| GL | On | 45 passes / 54 edges | 28 / 2.8 MB | `PipelineDepthStencil` |
| GL | Off | 4 passes / 5 edges | 5 / 1.1 MB | `PipelineDepthStencil`, `PipelineImage[1]` |
| Vulkan | On | 46 passes / 55 edges | 29 / 4.8 MB | `ShadowMap`, `PipelineDepthStencil` |
| Vulkan | Off | 5 passes / 6 edges | 8 / 3.3 MB | `ShadowMap`, `PipelineDepthStencil`, `PipelineImage[1]`, `SceneNormal`, `SceneFog` |

All four self-tests passed. Every live graph had `Backbuffer` as its required
output, no dead-pass candidates, and no graph build errors. Resource validation
reported no stale-size diagnostics. GL and Vulkan both showed the attachment
prior-value chain from `scene.target` through `scene.opaque` and
`scene.portal_translucent`; the effects-on runs also showed the SSAO composite
feeding translucency, the exposure/bloom chain, and final postprocess/present
dependencies. Backend observations continued to describe bound attachments as
writes, with sampled material reads recorded on the scene passes. Vulkan's
explicit `scene.resolve` accounts for its additional pass and edge. The
Khronos validation layer emitted no errors in either Vulkan run.

The captures close the Linux effects-on/effects-off validation for the
attachment-preservation change. They do not cover offscreen-only scene
traversals or every intermediate effect-toggle combination. Vulkan's indexed
non-mip sampled-image barrier remains open until a confirmed `DTF_Indexed`
draw route reaches it; the 2026-09-24 handoff records the unsuccessful route
probes.

## Pass1 exposure and bloom graph execution — 2026-09-30

The Linux build passed with `HAVE_VULKAN=ON`. A GL run on the RX 550 rendered
DOOM2 MAP06 at 640x480 with bloom enabled, `r_framegraph_exposure 1`,
`r_framegraph_bloom 1`, and `r_framegraph_pass2 0`. After 121 in-level frames,
`r_framegraph` reported 36 passes / 72 edges, a required `Backbuffer`, no dead
pass candidates, and the live scene → exposure → bloom → present chain. The
framegraph self-test passed. No graph replay fallback was reported.

Vulkan with the Khronos validation layer reported 37 passes / 73 edges,
including `scene.resolve` → exposure → bloom → present. It had no dead-pass
candidates, no replay fallback, and no validation errors. Matched graph-on and
immediate captures were byte-identical on both backends at 640x480: GL mean
luminance was 17.491 in both arms; Vulkan was 17.553. Each comparison had
maximum channel delta 0 and 0 differing pixels. Metal runtime validation and
performance remain open. Detailed evidence is in
[`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md).

## Shared raster AO graph execution — 2026-09-30

The build passed with `HAVE_VULKAN=ON`. GL quality-3 SSAO on MAP06 reported
9 passes / 27 edges; Vulkan quality-3 SSAO with `vk_compute_ssao 1` reported
10 / 28, including its earlier `ssao.lineardepth.compute` producer. Both
graphs were rooted at `Backbuffer`, had no dead-pass candidates, and emitted
no graph replay fallback diagnostic. The Khronos validation layer reported
no Vulkan errors.

Graph-on versus immediate captures were byte-identical at 640x480 (0 differing
pixels, maximum delta 0): GL raster depth mean luminance 16.767, Vulkan
compute-depth 16.833, and Vulkan raster-depth 16.829 in both arms. Vulkan's
compute and raster graph reports both retained the linear-depth producer before
occlusion.

Conditional routes were also checked on both backends. At 4× multisampling,
GL and Vulkan each reported 10 passes / 28 edges, rooted at `Backbuffer`, with
no dead-pass candidates or fallback; Vulkan validation was clean. Captures
were byte-identical (GL mean luminance 16.770, Vulkan 16.832). With
`gl_ssao_debug 2`, GL reported 7 passes / 22 edges and Vulkan 8 / 23, correctly
omitting both blur passes; again there were no dead-pass candidates or
fallback, Vulkan validation was clean, and captures were byte-identical (GL
mean luminance 240.677, Vulkan 240.560). All comparisons were 640x480 with
maximum channel delta 0 and 0 differing pixels. Other quality tiers and Metal
runtime remain open; the native Metal compute AO path does not use this shared
raster wrapper. See
[`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md) for the
full pass and image evidence.

The 2026-09-24 observer-cost A/B does not need to be repeated unless these
changes produce a measurable regression or the Linux session changes the
observer path.

## Offscreen and UI/HUD graph coverage — 2026-10-01

GL and Vulkan live MAP06 runs with all tested effects enabled and
`screenblocks 10` exercised the HUD and 2D UI graph scopes. GL reported 47
passes / 104 edges; Vulkan reported 48 / 105. Both graphs were rooted at
`Backbuffer`, had no dead-pass candidates or graph-build failures, and included
`ui.hud` between bloom and tonemap plus `ui.2d` before present. The
`r_framegraph_selftest` passed; Vulkan validation emitted no errors.

Canvas/camera texture updates now have `offscreen.canvas` and
`offscreen.camera` producers, and material reads inside those callbacks attach
to the producer. The self-test covers preserved canvas contents, a camera
producer, sampled material reads, UI consumption, and present reachability.
Intel Metal has live canvas and frame-1 camera fixtures; Linux GL/Vulkan still
need live offscreen fixture validation because stock MAP06 creates neither
texture. The Metal implementation was not compiled or run on this Linux
machine. Full details are in
[`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md).

## Later Metal testing

The current Intel Metal pass is complete for the tested effects-on/off cases.
Further Metal validation remains expected as coverage grows: conditional paths
on Intel, Apple Silicon runtime behavior, and platform-specific graph behavior
should each be recorded when hardware and a reproducible route are available.
Keep Metal execution correctness-first until Apple Silicon has a measured
baseline; this diagnostic work makes no TBDR scheduling or performance claim.
