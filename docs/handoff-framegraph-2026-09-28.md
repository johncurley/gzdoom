# Frame graph status and next work — 2026-09-28

## Current state

The shared renderer has a resource registry and a CPU-side `FrameGraph` in
`src/common/rendering/hwrenderer/frame/`. It records real postprocess passes,
pass resource uses, RAW/WAR/WAW dependencies, backend resource-use
observations, and upload/read observations. Pass2 now records each draw's
state and physical ping-pong index, then dispatches it in graph order through
the existing backend draw path. The graph does not allocate resources or cull
passes; each backend still handles its normal resource transitions.

The Linux GL/Vulkan runtime and observer-cost tranche closed on 2026-09-24; its
evidence is in [`handoff-linux-2026-09-24.md`](handoff-linux-2026-09-24.md).
Both backends produced a live 45-pass / 46-edge graph with the full tested
postprocess configuration. The observer on/off A/B found no measurable cost
for that RX 550 MAP06 route. This is not an Apple Silicon or Metal performance
result.

## CPU graph diagnostics added

The CPU graph now supports explicit required outputs, `keepAlive` pass roots,
dead-pass candidates, and lifetime intervals for resources marked transient in
the `FrameResources` registry. Lifetimes are first/last positions in the
deterministic topological order. They exclude imported resources and registry
resources marked persistent. These intervals are diagnostic only; no memory
aliasing or culling uses them.

The GL presenter and the Vulkan/Metal postprocess presenters declare
`Backbuffer` as an output when their producer pass is recorded. Screenshot
readbacks, wipe captures, and partial GL backbuffer copies are marked
keep-alive so output reachability does not discard their external or
cross-frame effects. The fixed presentation dither texture is named as an
external input so the Vulkan and Metal present pass can be recorded.

Material texture reads now retain their global upload-order observations and
are also attached as deduplicated sampled reads to the active main-view scene
pass on GL, Vulkan, and Metal. If an earlier graph pass writes the same name,
the read creates the corresponding graph dependency. Otherwise the current
texture value is declared as an imported input. Offscreen-only scene traversals
do not yet have equivalent graph scopes. Continue auditing side-effecting and
cross-frame passes and mark them keep-alive before treating any candidate as
unused.

Scene attachment preservation and blended postprocess outputs record logical
reads of their prior contents. RAW edges retain producer dependencies; WAR and
WAW edges constrain ordering without propagating pass liveness. Backend-use
observations continue to describe attachment binding as a write.

### Intel Metal live validation — 2026-09-29

After the attachment-read correction, a real MAP06 run on the Intel Metal
renderer used `gl_bloom 1`, `gl_ssao 3`, `gl_tonemap 1`, `gl_lens 1`, and
`gl_fxaa 1`. The `r_framegraph_selftest` positive and negative attachment
controls passed. `r_resources` reported 38 resources / 48.5 MB; resource
validation emitted no stale-size diagnostics, with only the unused screen/save
shadow maps untouched.

The live graph reported 52 passes / 65 edges, `Backbuffer` as its required
output, and no dead-pass candidates or build errors. The retained chain includes
`scene.target -> scene.opaque -> ssao.combine -> scene.portal_translucent ->
scene.resolve -> present`; `scene.opaque` and `scene.portal_translucent` also
show the sampled material reads observed at runtime. The new edge from
`ssao.combine` to `scene.portal_translucent` carries the blended scene color
forward. `Exposure.Camera` is listed as an imported persistent value for the
blended exposure update. Build used the Intel macOS configuration
(`HAVE_VULKAN=OFF`), so this does not validate GL/Vulkan runtime behavior.
No renderer execution path changed, and no image-parity or performance claim
is made from this diagnostic run.

A second isolated MAP06 run set all five effects to zero. Its graph reported
five passes / six edges and no dead-pass candidates: `scene.target`,
`scene.opaque`, `scene.portal_translucent`, `scene.resolve`, and `present`.
SSAO, bloom, exposure, tonemap, lens, and FXAA were absent as expected. The
self-test passed, and resource validation emitted no stale-size diagnostics.

### Pass2 graph execution — 2026-09-29

`PPRenderState` now captures shader, textures, uniforms, viewport, blend
state, debug group, and the physical pipeline-image index for each Pass2 draw.
It builds the subgraph before executing those snapshots in `FrameGraph::Order()`.
FXAA is represented by its two actual draws, even though both keep the `fxaa`
label. Custom scene shaders remain after the scheduled chain. `r_framegraph_pass2`
defaults on; when off, it calls the original immediate pass sequence directly.
Graph replay calls the existing GL, Vulkan, or Metal draw function for every
snapshot, so backend command recording and image transitions stay on those paths.

The graph builder now creates RAW, WAR, and WAW edges. RAW edges alone carry
liveness; anti-dependencies only constrain order. The self-test includes an
imported read followed by overwrite and rewrite, and passed in every capture.
With current declaration-time versioning, the Pass2 dependency edges point
forward, so the computed order matches the original sequence. This establishes
graph-controlled execution and hazard coverage without claiming pass
reordering or a performance change.

The Linux `cmake --build build -j$(nproc)` gate passed. On the RX 550, MAP06
captures compared graph execution against the original immediate path on GL and
Vulkan. Effects-off, tonemap+lens+FXAA, and an active colormap flash produced
identical decoded pixels in all six backend/mode comparisons. The effects-on
Vulkan live graph had 9 passes / 20 edges, a rooted `Backbuffer`, no dead-pass
candidates, and the expected two FXAA draws. The Khronos validation layer
reported no errors. Live captures resized from 640x480 to 800x600 also produced
identical decoded pixels between the immediate and graph paths on both backends.

The standard Linux golden-image effect relations passed for baseline, tonemap,
lens, FXAA, and colormap, covering the single-draw paths ending on the opposite
ping-pong image and FXAA's two-draw return to the starting image. Both repeat
runs produced stable pixels, but all five signatures differed slightly from
the stored Linux baseline (mean luminance shifted by 0.02-0.05); no baseline was
rewritten. The direct legacy-versus-graph pixel comparisons above passed. No
Metal build or runtime was available in this Linux session, and Apple Silicon
scheduling/performance policy remains open.

### Pass1 exposure and bloom graph execution — 2026-09-30

Added `r_framegraph_exposure` and `r_framegraph_bloom` switches so Pass1 can
record and execute each complete exposure or bloom chain through the same
`PPRenderState` graph replay path. Each chain is built independently: exposure
updates the persistent `Exposure.Camera`, then bloom reads it in the same frame.
The immediate path remains available by setting the corresponding switch to
zero. Pass2 and AO were not changed in this tranche.

The mandatory Linux build passed with Vulkan enabled. On the RX 550, OpenGL ran
DOOM2 MAP06 at 640x480 for 120 in-level frames with bloom enabled and Pass2
graph execution disabled. `r_framegraph_selftest` passed. A live `r_framegraph`
dump reported 36 passes / 72 edges, `Backbuffer` as the required output, no
dead-pass candidates, and a connected scene → exposure → bloom → present
chain. Exposure ran nine average reductions between extract and combine;
bloom's extract, blur, downscale, upscale, and combine draws were present. The
graph build emitted no fallback diagnostic.

Vulkan was then run with the Khronos validation layer. Its live graph reported
37 passes / 73 edges, rooted at `Backbuffer`, including `scene.resolve` before
exposure, `Exposure.Camera` feeding `bloom.extract`, and the bloom composite
feeding present. The framegraph self-test passed, no replay fallback appeared,
and the validation layer emitted no errors.

The prediction was exact pixel identity between graph replay and the immediate
path. Matched captures with exposure and bloom replay both enabled versus both
disabled were byte-identical on both backends at 640x480 RGB. GL mean luminance
was 17.491 in both arms; Vulkan was 17.553 in both. Each comparison had maximum
channel delta 0 and 0 differing pixels. No baseline was changed. This validates
output preservation on the tested GL and Vulkan routes, not Metal runtime
behavior or performance.

### Ambient-occlusion graph execution — 2026-09-30

Added `r_framegraph_ao` for the shared raster SSAO path. Each AO graph imports
the scene attachments (`SceneColor`, `SceneDepthStencil`, `SceneNormal`,
`SceneFog`), the possibly precomputed `AO.LinearDepth`, and the three persistent
random inputs (`AO.RandomTexture0` through `AO.RandomTexture2`). A raster
linear-depth draw inside the graph still creates its normal write dependency;
the external declaration covers Vulkan's optional compute producer, which runs
before the AO graph begins. Metal's native compute AO path bypasses this shared
raster wrapper, as before.

The Linux build passed with Vulkan enabled. On the RX 550, GL rendered MAP06
with `gl_ssao 3`, graph replay on, and the other optional postprocess paths off.
The live graph reported 9 passes / 27 edges, rooted at `Backbuffer`, with no
dead-pass candidates. Its chain included `ssao.lineardepth` → `ssao.occlude` →
horizontal/vertical blur → `ssao.combine`; the composite read and rewrote
`SceneColor` before translucency. There was no postprocess graph fallback
diagnostic.

Vulkan ran the same scene with `gl_ssao 3`, `vk_compute_ssao 1`, and the Khronos
validation layer. The live graph reported 10 passes / 28 edges, no dead-pass
candidates, and `ssao.lineardepth.compute` → `ssao.occlude` → blur → combine.
This confirms the imported `AO.LinearDepth` dependency from the compute pass
before graph replay. The validation layer emitted no errors and there was no
fallback diagnostic.

A second Vulkan route set `vk_compute_ssao 0` to exercise raster linear depth.
Its live graph also reported 10 passes / 28 edges, with the raster
`ssao.lineardepth` draw writing `AO.LinearDepth` before occlusion. The graph was
rooted, had no dead-pass candidates or fallback diagnostic, and the Khronos
validation layer emitted no errors.

For both backends, the prediction was exact pixel identity between graph replay
and immediate execution. Same-route 640x480 captures were byte-identical:
GL mean luminance 16.767, Vulkan compute-depth 16.833, and Vulkan raster-depth
16.829 in both arms; each comparison had maximum channel delta 0 and 0 differing
pixels. This validates the tested quality-3 routes, including both Vulkan
linear-depth producers.

Conditional-route checks then covered 4× multisampling on both backends. Vulkan
disabled its compute-depth option at this sample count and used raster
`ssao.lineardepth`; both GL and Vulkan reported 10 passes / 28 edges, rooted at
`Backbuffer`, with no dead-pass candidates or fallback. Vulkan validation was
clean. Graph-on/off captures were byte-identical at 640x480: GL mean luminance
16.770 and Vulkan 16.832, with maximum delta 0 and 0 differing pixels.

Both backends were also checked with `gl_ssao_debug 2`, which omits the blur
draws. GL reported 7 passes / 22 edges and Vulkan 8 / 23; both graphs remained
rooted at `Backbuffer`, with no dead-pass candidates or fallback. Vulkan
validation was clean. The matching captures were byte-identical at 640x480:
GL mean luminance 240.677 and Vulkan 240.560, with maximum delta 0 and 0
differing pixels. Other quality tiers were open at the time; GL/Vulkan q1/q2
parity is recorded in the 2026-10-03 section below. Metal runtime and
performance remain open.

### Offscreen and UI/HUD coverage — 2026-10-01

Added producer scopes for `RenderTextureView()` on GL, Vulkan, and Metal.
Canvas updates read and rewrite their named texture because 2D canvas drawing
can preserve pixels outside the updated area; camera-texture updates write
their target after the viewpoint setup clears it. Both are keep-alive passes.
Sampled material reads observed during these callbacks now attach to the active
`offscreen.*` pass. Added a `scene.hud_model` scope, a `ui.hud` consumer around
the post-bloom HUD callback, and `ui.2d` scopes that record blended reads and
writes of the current pipeline image plus the stencil clear.

The self-test now exercises a preserved canvas update, a camera producer with
a sampled world material, and a UI overlay that samples both textures before
present. It checks the dependency edges, output reachability, no dead-pass
candidates, and the UI depth/stencil backend observation. The updated
`r_framegraph_selftest` passed.

Live RX 550 MAP06 runs with all tested effects enabled and `screenblocks 10`
exercised HUD and 2D UI on both GL and Vulkan. GL reported 47 passes / 104
edges; Vulkan reported 48 / 105, including its explicit resolve. Both graphs
were rooted at `Backbuffer`, had no dead-pass candidates or graph-build
failures, and showed the bloom → `ui.hud` → tonemap chain and `ui.2d` before
present. Vulkan validation emitted no errors. MAP06 did not create a canvas or
camera texture, so the offscreen producers have self-test coverage but still
need a live fixture that exercises those routes. Metal code was updated but
could not be compiled or run in this Linux configuration.

### Intel Metal follow-up — 2026-10-01

Live canvas recording exposed that sampled 2D material reads could be observed
after their offscreen graph scope had closed. Added a backend hook called by
shared `Draw2D()` immediately after material selection; Metal resolves the
material's hardware texture layers and records reads only during an offscreen
view. The other backends use the default no-op hook.

A temporary MAP09 fixture updated `FGSOURCE` and `FGTARGET` canvas textures.
The source drew `BRICK1`; the target sampled the source. The Metal graph
reported 9 passes / 15 edges, `Backbuffer`, and no dead-pass candidates. It
recorded the source canvas's preservation read, sampled input, and stable
resource name, then a later `offscreen.canvas` read of that source with a RAW
edge from its producer. The Metal self-test passed. The camera probe initially
missed the producer because it dumped at frame 60, after the one-time initial
camera update had been cleared and the graph had reset. A follow-up added a
camera thing to temporary MAP09 UDMF, kept it alive with a looping state, and
dumped on frame 1. Intel Metal recorded two `offscreen.camera` passes; the
probe's `FGCAMERA` wrote stable resource `Metal.Texture.1908`. The graph had
10 passes / 17 edges, `Backbuffer`, and no dead-pass candidates. Camera
producer coverage is now live on Intel Metal; Linux camera fixtures remain
open.

On deterministic MAP06 captures with all postprocess effects and Intel compute
AO/bloom enabled, immediate mode and replay mode each reported 29 passes / 60
edges with no dead-pass candidates. Their 1440x900 captures were byte
identical (max channel delta 0); two immediate captures also matched. This
closes the Intel Metal canvas producer/consumer and replay-pixel checks. Live
GL/Vulkan effects-on/off validation of attachment-preserving graph changes
also passed; offscreen-only traversals remain to be exercised on those
backends, as recorded in
[`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md).

### Intel Metal compute-AO conditional coverage — 2026-10-01

Before the subsequent graph-replay merge, Intel Metal captures exercised the
native compute-AO routes with raster postprocess effects disabled. Algorithm 0
reported 7 passes / 16 edges. Algorithm 1 (AlchemyAO/SAO) reported the same
shape. Algorithm 2 matched the predicted 8 passes / 18 edges, including
`ssao.depth-pyramid` writing `AO.DepthPyramid` before `ssao.compute` reads it.
Each route continued through `ssao.compute.composite` into `SceneColor`, had no
dead-pass candidates or stale-size diagnostics, and passed
`r_framegraph_selftest`. Algorithm 2's `AO.DepthPyramid` was touched for both
write and read.

The Intel gate was also checked with `mt_compute_ao 1` and
`mt_compute_ao_intel 0`: `mt_caps` confirmed the reference raster SSAO fallback
and its graph reported 10 passes / 17 edges, with no dead-pass candidates or
stale-size diagnostics. These are Intel observer/dispatch coverage results,
not tests of the graph-driven execution added later in this handoff.

### Intel Metal postprocess shader coverage — 2026-10-01

The stock all-effects route initially found nine misses among 101 native raster
postprocess shader lookups. The exact current-hash MSL was already in the
runtime cache, but the collector only promoted fresh translations. Added
`--from-cache` support to `tools/collect_metal_shaders.py` and bundled 17
current-hash stages, including the updated `ssao.fp` and `lineardepth.fp`.
The hand-written compute kernels in `mt_ao.metal` were already in
`native_shaders.metallib`; the missing AO coverage was the shared raster path.

After rebuilding the Intel app and PK3, CMake reported 109 pre-translated
stages. The all-effects stock MAP01 replay reported 109 lookups / 109 hits,
zero symbol misses, zero unavailable-library lookups, and no `msl_tolib` or
`msl_translate` events. Its observer graph reported 47 passes / 56 edges, no
dead-pass candidates, and a passing graph self-test. These measurements predate
the graph-driven replay changes above.

The user reports that the previously reported Ashes gameplay freeze appears
resolved following recent renderer, shader-coverage, and build changes. There
is no controlled Ashes before/after capture to isolate the cause. The roughly
1.3-second stock-demo map-entry display interval recorded in `AGENTS.md` is a
separate event; reopen the Ashes investigation only if the gameplay freeze
returns.

## Remaining work, in order

1. **Continue validating the CPU graph contract.** Intel Metal and Linux
   GL/Vulkan effects-on/off runs are recorded above. UI/HUD scopes are live on
   all three backends, and canvas producer/consumer and camera-texture
   producers now have live fixtures on GL, Vulkan, and Intel Metal. Vulkan's
   indexed non-mip upload transition and the shadow-map conditional route are
   also covered live. Camera target identities and clear, opaque, and
   portal/translucent graph scopes are implemented; live GL/Vulkan results are
   recorded below and Intel Metal rebuild/runtime validation remains open.
   Remaining coverage includes backend-specific conditional routes and the
   Apple Silicon runtime boundary. Linux results are recorded in
   [`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md).
   Extend conditional-route coverage as fixtures and hardware permit.
2. **Widen in measured steps.** Exposure, bloom, and raster AO qualities 1–3
   now have live Linux graph-replay controls. AO quality-1/2 single-sample
   routes and Vulkan compute/raster linear-depth branches now have matched
   direct/replay captures. Quality-3 multisample and debug routes are also
   covered on GL and Vulkan. Intel Metal's reference raster fallback
   now has live single-sample quality-1/2 parity with debug mode 0, plus
   quality-3 debug-mode 1–10 coverage across the recorded runs. Intel Metal
   now has live 4× scene rendering, resolve-resource tracking, and raster-AO
   fallback coverage with repeatable captures and runtime sample-count toggles;
   the 2026-10-05 Intel follow-up also verified 8× replay parity, runtime
   1×↔8× toggles, and a 64× request falling back to 8×. Its retained captures
   used 300 frames after rejecting a 120-frame startup-screen capture. See the
   macOS handoff for the controls. Apple Silicon remains open. Keep resource
   aliasing and pass culling disabled until output roots, all relevant reads,
   writes, and lifetimes have been proven on the migrated paths.
3. **Validate Metal policy on Apple Silicon.** CPU graph algorithms and
   backend-neutral contracts can proceed on Linux. Metal scheduling,
   transient aliasing policy, and TBDR performance choices need M-series runtime
   evidence. The Intel Metal compile and live correctness checks for the
   offscreen/UI scopes are listed in
   [`handoff-macos-2026-10-01.md`](handoff-macos-2026-10-01.md). ARM64/AArch64
   JIT work is also deferred until Apple Silicon is available for runtime
   validation, as requested.

## Open validation boundaries

- The cold MAP08 capture still misses strict byte-repeatability. It is not a
  closed parity result; the 2026-09-24 Linux handoff records the capture limits.
- Apple Silicon has not run the renderer. Maintain correctness-first Metal
  behavior until an M-series baseline and runtime validation are available.

## Verification standard

Follow `CONTRIBUTING.md` for renderer changes: prove the control can detect the
failure, capture real rendered frames, and report noise and coverage limits.
The self-test covers an output-rooted chain, a dead-candidate negative control,
missing-output detection, screenshot keep-alive, transient lifetime intervals,
positive/negative attachment-preservation cases, graph hazards, offscreen/UI
consumption, compute-bloom topology, and compute-AO pyramid selection. On Intel
macOS, `cmake --build build --parallel 4` passed and a real Metal frame passed the
self-test and output/liveness checks described above. The prior Linux build
passed. Live GL/Vulkan attachment-preservation output/lifetime validation then
closed on 2026-10-02; see the Linux handoff. Xvfb `+quit` checks do not
substitute for real rendered frames.

## Linux attachment-preservation validation — 2026-10-02

Real MAP06 effects-on and effects-off frames validated the logical reads for
preserved scene attachments and blended postprocess outputs on GL and Vulkan.
Both effects-on graphs reached `Backbuffer` and retained the
`scene.opaque` → `ssao.combine` → `scene.portal_translucent` SceneColor chain,
plus the blended `Exposure.Camera` and `PipelineImage[0]` dependencies. Effects
off removed the optional effects while retaining scene attachment ordering.
The self-test passed in every run; there were no dead-pass candidates,
stale-size diagnostics, or Vulkan validation errors. Full pass counts and run
conditions are recorded in [`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md).

This validates the dependency contract for the tested routes. The captured
shadow-map graph exposed a candidate ordering freedom: `shadowmap` and
`scene.target` write separate resources, and both precede `scene.opaque`. The
follow-up section below records the completed backend-specific order experiment.
Keep aliasing and culling disabled until all relevant routes are represented
and validated.

## Scene clear and shadow-map ordering — 2026-10-02

The approved order experiment now clears the main scene attachments before the
shadow-map update on GL and Vulkan. The graph records `scene.target` at the
actual clear operation, followed by `shadowmap`; both remain inputs to
`scene.opaque`. The render target is rebound and scene viewport/depth state is
reapplied after shadow rendering, since the shadow path changes backend state.
Vulkan consumes the deferred clear through a clear-only render pass on the
existing draw command stream. Metal reports that it cannot flush its deferred
clear, so it retains its original operation order.

The mandatory Linux build passed. Live MAP06 runs with the shadow fixture,
SSAO/bloom/tonemap/lens/FXAA enabled, and `r_resource_validate 1` passed the
framegraph self-test on GL and Vulkan. Both graphs placed `scene.target` before
`shadowmap`, reported no dead-pass candidates or stale-size diagnostics, and
Vulkan validation reported no errors. The GL reordered capture was pixel
identical in the analysis region to its saved old-order capture
(0 differing pixels over threshold 2). For Vulkan, a same-build old-order
control was made by temporarily disabling clear flushing; that image was also
pixel identical to the reordered capture. The earlier Vulkan screenshot was
not a matched control: it showed a different MAP06 view state, so it was not
used to claim parity. GL and Vulkan reordered captures differed by a mean
0.126 channel values, with 0.106% of analysis pixels over threshold 2.

This is a fixed, backend-guarded order change, not graph-scheduled scene
execution. Metal order and Metal clear-flush behavior remain unchanged. Keep
aliasing and pass culling disabled.

## Scene-clear ordering performance check — 2026-10-04

Measured the old `shadowmap -> scene.target` order against the current
`scene.target -> shadowmap` order on the RX 550 at 1280×720. Each measured
launch used MAP06, a temporary PointLight to activate the shadow-map pass,
shadow quality 256, SSAO quality 3, bloom, tonemap, lens, and FXAA enabled,
with vsync and frame caps disabled. The interleaved sequence was old/new/old/new
on each backend. Each launch recorded two 5-second `vid_frametrace` windows;
an initial GL and Vulkan warm-up was excluded. The trace measures Update to
Update intervals and includes present wait.

| Backend | Order | First-window averages (ms) | Second-window averages (ms) | Combined mean (ms) |
|---|---|---:|---:|---:|
| GL | old | 5.83, 5.78 | 5.09, 5.01 | 5.43 |
| GL | clear first | 5.91, 5.68 | 5.08, 5.01 | 5.42 |
| Vulkan | old | 5.99, 6.05 | 5.39, 5.39 | 5.71 |
| Vulkan | clear first | 5.99, 5.92 | 5.46, 5.36 | 5.68 |

These differences are below the run-to-run movement: GL's second-window
averages settled near 5.0 ms in both arms, while Vulkan's did so near 5.4 ms.
This A/B shows no measurable frame-time benefit from changing the order on
either backend. It does not justify further order changes without a separately
measurable candidate.

Image controls used the same effects and fixture. The two old-order GL images
and the second clear-first image agreed exactly or within one channel level.
The first clear-first image was the sole outlier: 127 of 589,824 analysis
pixels exceeded threshold 2 (0.0215%), with max channel delta 52. This capture
set therefore does not strengthen strict pixel-parity evidence; the earlier
matched parity run remains the correctness result. Vulkan's first old/new pair
was exactly identical; the second pair differed by at most one channel level
and had no pixels over threshold 2. Both backend graphs recorded the requested
order, passed the framegraph self-test, and reported no dead-pass candidates.
The temporary A/B cvar and screenshots were removed from the source tree; logs
and captures remain under `/tmp/gzdoom_clearorder` for this session.

## Metal handoff check

Intel Metal effects-on/off, conditional AO fallback, all single-sample raster
AO debug branches, and UI/HUD runs are recorded above. The canvas
producer/consumer has a live fixture; the camera texture producer also has a
live frame-1 fixture. Linux GL/Vulkan also have live canvas and camera producer
fixtures, including sampled camera-material reads. Camera target clear,
opaque, and portal/translucent scopes are now covered on GL/Vulkan; the Intel
Metal source implementation needs its rebuild and live fixture recorded below
before that backend is closed. Apple Silicon runtime and TBDR performance
policy remain unvalidated.

The latest Linux build includes GL/Vulkan and passed after the camera-target
scope changes. It does not compile the Metal backend. Offscreen camera/canvas
producer and material-read scopes have live GL/Vulkan fixture coverage,
including camera attachment subpasses. A temporary dynamic-light fixture now
activates the shadow-map producer on GL and Vulkan; its route is recorded in
the Linux handoff.

## Linux shared raster AO quality 1/2 — 2026-10-03

GL and Vulkan quality-1/2 raster AO routes now have live direct-versus-replay
parity on MAP06 at single sample. Both AO qualities change pixels against the
same-backend AO-off controls; a repeated GL quality-1 capture matched exactly
in the viewport analysis region.
Vulkan was also checked with both raster and compute linear-depth production.
All graphs remained rooted at `Backbuffer`, retained the five expected AO
passes, and had no dead-pass, fallback, stale-size, or Vulkan validation
diagnostics. Detailed pass counts and image differences are in
[`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md).

## Camera target attachment scopes — 2026-10-03

Camera `RenderTextureView()` now scopes the active target color and a derived
depth/stencil identity through the callback on GL, Vulkan, and Metal source.
The aggregate camera producer was replaced by clear, opaque, and
portal/translucent graph scopes, with attachment-preservation reads in the two
scene phases and the terminal phase marked keep-alive. Main-view `Scene*`
attachments are excluded. The full contract and validation record are in
[`frame-graph-camera-resources.md`](frame-graph-camera-resources.md).

The self-test and full Linux build pass. Live GL reports 52 passes / 114 edges;
Vulkan reports 12 / 24 with validation enabled. Both camera fixtures show the
target color/depth chain, material reads, output reachability, and no dead-pass
candidates or graph diagnostics. Intel Metal still needs a macOS rebuild and
camera fixture run. Apple Silicon/TBDR scheduling policy remains a separate
hardware-gated question; this work only records existing operations and does
not authorize scheduling across Metal attachment boundaries.

## ShadowMap retained-resource boundary — 2026-10-04

The keep-alive and cross-frame audit found that scene passes can read
`ShadowMap` when its level AABB tree is attached even if this frame does not
run the conditional shadow producer. The existing texture then supplies the
retained value. `r_framegraph` now declares `ShadowMap` as an external input;
`BuildEdges()` still binds a read to an earlier same-frame writer first, so an
active shadow update retains its RAW edge.

`r_framegraph_selftest` covers all three cases: a retained input without a
current-frame producer succeeds, omitting the external declaration fails with
the expected read-before-write diagnostic, and a same-frame producer keeps a
single RAW dependency and remains live. The mandatory Linux build and the
self-test passed. The runtime used Xvfb with llvmpipe, so this validates the
CPU graph contract rather than an accelerated dynamic-light frame. The earlier
RX 550 dynamic-light fixture still covers the live producer/consumer route;
Intel Metal conditional-route validation remains open.

This change only corrects graph input classification. It does not change
rendering or command order. The related source audit and completed GL/Vulkan
order experiment are recorded below; conditional Metal routes still need live
validation.

The follow-up source review traced the direct `FrameGraphAccess::Read` sites
and found no second retained input with the same gap in the reviewed paths.
`Exposure.Camera`, eye textures, `PaletteTexture`, AO noise textures,
`Backbuffer`, and route-specific `Present.Dither` reads have external-input
declarations. Partial canvas updates declare their previous contents external;
scene-material reads are declared external by `ObserveSceneMaterialRead()` when
there is no earlier producer in the current graph. Wipe captures are rooted by
their keep-alive passes, and wipe texture reads use the scene/UI material-read
observer. This closes the source audit for the reviewed Linux routes; the
conditional Metal routes still need live validation on Intel Metal.
