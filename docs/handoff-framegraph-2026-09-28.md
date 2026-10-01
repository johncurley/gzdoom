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
differing pixels. Other quality tiers, Metal runtime, and performance remain
open.

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
   all three backends, and the canvas producer/consumer is live on Intel Metal.
   Camera-texture producers now have a live Intel Metal fixture; Linux still
   needs a live camera fixture.
   Extend conditional-route coverage as fixtures and hardware permit.
2. **Widen in measured steps.** Exposure, bloom, and quality-3 raster AO now
   have live Linux graph-replay controls. AO quality-3 multisample and debug
   routes are covered on GL and Vulkan. Intel Metal's reference raster fallback
   now has live single-sample quality-1/2 parity with debug mode 0, plus
   quality-3 debug-mode 1–10 coverage across the recorded runs. Intel Metal
   now has live 4× scene rendering, resolve-resource tracking, and raster-AO
   fallback coverage with repeatable captures and runtime sample-count toggles;
   higher sample counts and Apple Silicon remain open. Keep resource aliasing
   and pass culling disabled until output roots, all relevant reads, writes,
   and lifetimes have been proven on the migrated paths.
3. **Validate Metal policy on Apple Silicon.** CPU graph algorithms and
   backend-neutral contracts can proceed on Linux. Metal scheduling,
   transient aliasing policy, and TBDR performance choices need M-series runtime
   evidence. The Intel Metal compile and live correctness checks for the
   offscreen/UI scopes are listed in
   [`handoff-macos-2026-10-01.md`](handoff-macos-2026-10-01.md). ARM64/AArch64
   JIT work is also deferred until Apple Silicon is available for runtime
   validation, as requested.

## Open validation boundaries

- The Vulkan indexed non-mip sampled-image transition from transfer to shader
  read was not dynamically exercised. Keep it open until a confirmed active
  `DTF_Indexed` draw route reaches that path; details are in the Linux handoff.
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
passed, but live GL/Vulkan output/lifetime validation remains open. Xvfb `+quit`
checks do not substitute for real rendered frames.

## Metal handoff check

Intel Metal effects-on/off, conditional AO fallback, all single-sample raster
AO debug branches, and UI/HUD runs are recorded above. The canvas
producer/consumer has a live fixture; the camera texture producer also has a
live frame-1 fixture. Linux still needs live offscreen fixture validation on
GL/Vulkan. Apple Silicon runtime and TBDR performance policy remain
unvalidated.

The latest Linux build includes GL/Vulkan and passed after the UI/HUD scope
changes. It does not compile the Metal backend. Offscreen-only scene traversals
now have producer and material-read scopes, but still need live fixture
validation on GL/Vulkan.
