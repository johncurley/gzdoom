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
maximum channel delta 0 and 0 differing pixels. The GL/Vulkan quality-1/2
routes were closed by the 2026-10-03 follow-up below. Metal runtime and
performance remain open; the native Metal compute AO path does not use this
shared raster wrapper. See
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

## Shared raster AO allocated-sample routing — Linux follow-up 2026-10-02

The Intel Metal multisample implementation exposed a shared postprocess
assumption: `PPAmbientOcclusion::Render()` chose its `sampler2DMS` shaders from
the requested `gl_multisample` cvar. It now accepts the actual allocated scene
sample count instead. GL passes `FGLRenderBuffers::GetSceneSamples()` and
Vulkan passes its render-buffer sample count; the shader choice and AO combine
uniform use that value. This keeps the raster AO route aligned with the actual
attachments when a backend clamps a request to a supported count. The Metal
4× path was exercised live on Intel. The macOS build also compiled the shared
GL/Vulkan call sites, but Linux runtime coverage below predates this change.

**Linux validation requested:** rebuild with Vulkan enabled and run the framegraph
self-test, then exercise GL and Vulkan at 1× and 4× with `gl_ssao 3`, plus the
existing debug-mode-2 route. Confirm the backend reports/allocates the expected
sample count, the single-sample and multisample AO shader routes match that
allocated count, the AO passes remain rooted at `Backbuffer`, and there are no
dead-pass candidates, graph errors, stale-size reports, or Vulkan validation
errors. If the driver clamps a requested count, record both requested and
allocated values and verify AO follows the latter. The earlier Linux 4× graph
and pixel-parity results remain baselines; this follow-up does not require a
new pixel-parity comparison.

**Linux integration results, 2026-10-02:** rebuilt successfully with Vulkan
enabled, then passed `r_framegraph_selftest` on both GL and Vulkan. Ran live
MAP06 with `gl_ssao 3` at GL 1×/4× and Vulkan 1×/4×, then repeated GL and
Vulkan 4× with AO debug mode 2. Every run exited cleanly. Each graph reached
`Backbuffer` and had no dead-pass candidates; normal mode reported 11–12 passes
and 31–32 edges, while debug mode 2 reported 10 passes and 27 edges, matching
the omitted AO blur passes. Resource reports had no stale-size diagnostics;
only expected unused resources were untouched. Vulkan ran with
`VK_LAYER_KHRONOS_validation` and reported no validation errors.

The requested counts were also supported by the live devices: GL reported
`GL_MAX_SAMPLES = 8`; Vulkan's intersected sampled-image color/depth/stencil
limits included 1×, 2×, 4×, and 8×. The GL allocator clamps the request to its
maximum, while Vulkan selects the highest supported count not above the
request; with requests of 1× and 4×, both therefore allocated those exact
counts. The AO call sites pass `GetSceneSamples()` on both backends, and the
shared pass chooses its single-sample or multisample shaders and combine
uniform from that allocated value. This closes the requested Linux sample
routing/graph validation. It does not add a pixel-parity comparison.

## Offscreen and indexed-route follow-up — 2026-10-02

A temporary ZScript/ANIMDEFS canvas fixture now exercises the offscreen canvas
producer/consumer path on both Linux backends. `FGSOURCE` draws `BRICK1`, and
`FGTARGET` samples `FGSOURCE`. MAP06 graph reports showed two ordered
`offscreen.canvas` passes, a RAW edge from the source write to the target read,
`Backbuffer`, and no dead-pass candidates: GL reported 50 passes / 106 edges;
Vulkan reported the same counts and passed with
`VK_LAYER_KHRONOS_validation` enabled. This closes the Linux canvas fixture
gap for GL and Vulkan.

The same fixture also registers a camera actor with `SetCameraToTexture`.
Spawning it at map entry and dumping over the first few frames captured the
one-time producer before the graph reset: GL reported 49 passes / 105 edges,
and Vulkan reported 49 / 105 with `VK_LAYER_KHRONOS_validation` enabled. Both
graphs contained an `offscreen.camera` pass writing the camera texture, reached
`Backbuffer`, and had no dead-pass candidates. Later dumps omit that pass after
the camera texture's initial update. This showed startup production only; it
did not establish a sustained consumer path. That remained open until the
controlled follow-up below.

### Sustained camera-texture consumer — 2026-10-05

A temporary MAP99 fixture now compares the same `SetCameraToTexture` actor
with and without a visible `FGCAMERA` wall. The camera sits behind a blocking
partition in a separate room, so its own view cannot sample its render target.
After spawning the actor and waiting 30 tics, the consumer graph retained the
three `offscreen.camera` phases and a RAW edge from the camera target's final
portal/translucent write to `scene.opaque`. The matching no-consumer control
contained no camera producer after the same wait.

On the RX 550, GL reported 9 passes / 22 edges with the consumer and 6 / 13
without it. Vulkan reported 10 / 23 with the consumer and 7 / 14 without it,
under `VK_LAYER_KHRONOS_validation`. Each consumer run added exactly three
camera phases and nine edges. The final camera-target pass wrote the same
texture that `scene.opaque` read, with a RAW dependency between them. Both
graphs reached `Backbuffer`, had no dead-pass candidates, and passed
`r_framegraph_selftest`; reports had no graph diagnostics or stale-size
messages, and the Vulkan validation logs had no errors. This closes sustained
camera-texture consumer coverage on Linux GL and Vulkan. It makes no timing or
pixel-parity claim.

## Indexed texture palette providers — 2026-10-02

The indexed fixture exposed that `FMaterial::GetLayer()` requests two palette
layers, while only Metal had registered the callback that supplies them. GL
and Vulkan therefore indexed past the one-layer indexed material. Added cached
translated and highlighted palette textures to both backends and registered
their callbacks at renderer initialization. The palette data and highlight
calculation mirror Metal's provider. Vulkan records the palette texture upload
with the frame graph; its existing `VkHardwareTexture::CreateImage()` path
records the indexed source upload.

The original temporary `DTA_Indexed` canvas fixture now runs without a crash on
both GL and Vulkan. GL created the two palette textures and reported an
`offscreen.canvas` pass. Vulkan's early-frame report showed both ordered canvas
passes, their RAW dependency, `Backbuffer`, and no dead-pass candidates (50
passes / 106 edges). The Vulkan run used
`VK_LAYER_KHRONOS_validation` and reported no validation errors.

Palette invalidation was also exercised live: the fixture rendered once,
changed `gl_paltonemap_reverselookup`, rendered again, and dumped the graph.
Both canvas passes sampled the recreated palette resources; Vulkan validation
reported no lifetime or descriptor errors. GL clears its palette cache after
dropping the last-material binding. Vulkan waits for submitted work, resets
material descriptor sets, then releases its cached palette images.

GDB confirmed the Vulkan source texture reaches `VkHardwareTexture::CreateImage`
and `CreateTexture()` as a 64×128 `VK_FORMAT_R8_UNORM` image with one byte per
pixel and `mipmap=false`. It then records the transfer-destination to
shader-read-only transition. This closes the indexed non-mip upload/barrier
coverage gap on Vulkan. Linux canvas and camera producer fixtures are also now
live on both GL and Vulkan, as recorded above. Metal scheduling and
performance policy remain outside this Linux handoff.

## Vulkan synchronous upload cost probe — 2026-10-02

Before considering async uploads, the prediction was that the indexed 64×128
R8 upload's staging work would take under 0.1 ms on the RX 550. A temporary
CPU timer around `VkHardwareTexture::CreateTexture()` split staging-buffer
allocation/map/copy/unmap from image/view creation and transfer-command
recording. It was removed after the captures.

Two completed cold Vulkan launches (MAP06 followed by MAP07, validation layer
enabled) each recorded 51 synchronous uploads totaling 10,170,428 bytes.
Staging took 3.137 and 3.319 ms; image/view creation plus barriers and copy
recording took 13.194 and 13.129 ms; the 64 MB threshold wait took 0.014 ms in
each. No Vulkan validation errors were reported. These are CPU-side intervals,
not GPU transfer durations; image allocation and driver command-recording
costs are inside the 13.1 ms segment.

A focused indexed-canvas capture measured the exact 64×128 R8 route: source
pixel preparation (`CreateTexBuffer`) took 0.0285 ms and staging plus Vulkan
image/command work took 0.3777 ms. The two 256×1 palette uploads took about
0.10 ms each. This confirms the prediction for indexed staging and shows that
async machinery would not pay for this small route by itself.

The same capture recorded 45 source-preparation calls: mean 0.186 ms, maximum
4.012 ms. The 44 RGBA calls averaged 0.190 ms; the maximums were the two
1280×960 startup textures at 3.677 and 4.012 ms. This is one startup/fixture
sample, not a cold-map timing distribution. The two complete-launch totals
were repeatable, but they do not isolate map-transition frames.

The current Vulkan contract gives a safe first async boundary: only detached,
owned pixel data may cross to a worker. Vulkan image/view creation, mapped
staging-buffer writes, image barriers, and command recording remain on the
render thread. The transfer command buffer is submitted before the draw buffer
on the graphics queue; its staging buffers remain in `TransferDeleteList` until
the associated fence wait retires them. Any worker completion must also be
checked against a task/wrapper generation before it can update texture state.

This does not justify an async loader based on the indexed route: that path is
sub-millisecond, and the startup-only source sample does not estimate
transition benefit. The matched cold-transition profile below separates
source access from Vulkan-side stages and shows that forced validation
substantially inflates the command timings. It does not establish a user-visible
stall or justify an async-loader change. The Metal handoff found much larger
cold-precache costs in source bitmap acquisition/PNG decode, but that work
happens before its detached snapshot reaches the worker. Do not move `FTexture*`
access or `GetBgraBitmap()` onto a worker without a separate thread-safety and
lifetime contract.

## Matched Vulkan cold-transition upload profile — 2026-10-05

The prediction before the source/object split was under 8.5 ms of source access
and more than 75 ms of the prior ~111 ms Vulkan object/command bucket in image
and view allocation. Both were wrong: two source-profile runs measured 10.78 ms
in the source-access subinterval, while the finer Vulkan split put image plus
view allocation near 16.9 ms and left about 97.6 ms in the command path when
Khronos validation was explicitly enabled.

Each fresh Linux Vulkan run used the AMD Radeon RX 550/RADV at 1280×720, started
on MAP06, and changed to MAP07 after 151 in-level frames. `gl_precache 1` was
fixed across runs; bloom, SSAO, tonemap, lens, FXAA, and shadow maps were off;
vsync and frame caps were off. The measured interval was gated around the map
transition. Each run captured exactly 471 `CreateTexBuffer`/`CreateTexture`
uploads totaling 9,345,508 bytes. `r_framegraph_selftest` passed. The
100 ms `vid_stalltrace` reported the initial startup/wipe interval only, with
no map-transition loop interval above its threshold. That does not rule out
smaller hitches or cumulative load time.

The source-profile arm, with `VK_LAYER_KHRONOS_validation` forced, averaged
10.78 ms in the `GetBgraBitmap`/`Get8BitPixels` source-access subinterval and
19.23 ms for `CreateTexBuffer` inclusive. The remaining `CreateTexBuffer` work
averaged 8.46 ms. “Source access” includes bitmap acquisition, decoding when
needed, cache access, copying, and conversion; it is not a decoder-only timer.

The finer `CreateTexture` profile was run twice both with the validation layer
forced and with no explicit layer override. These are CPU-side totals across
the 471 uploads, not GPU transfer durations:

| CPU stage | Validation forced | No explicit layer override |
|---|---:|---:|
| Staging allocation/map/copy/unmap | 8.71 ms | 3.74 ms |
| Image allocation | 12.14 ms | 3.87 ms |
| Image-view allocation | 4.76 ms | 0.68 ms |
| Transfer command path | 97.60 ms | 25.67 ms |
| Threshold wait | 0 ms | 0 ms |
| **Total `CreateTexture` stages** | **123.22 ms** | **33.96 ms** |

All 471 textures used mipmaps. In the no-explicit-layer arm, the transfer
command path averaged 0.53 ms for the initial barrier, 4.10 ms for base-level
copy recording, 20.89 ms in `GenerateMipmaps`, and 0.06 ms to retire staging
buffers. Inside mipmap generation, barrier setup/recording took 3.06 ms across
5,877 calls; blit setup/recording took 17.47 ms across 2,703 calls. The
validation-forced arm measured 84.31 ms in mipmap generation, split nearly
evenly between barriers (41.68 ms) and blits (42.25 ms).

Forcing `VK_LAYER_KHRONOS_validation` raised the measured total from about
34 ms to 123 ms, so the validation-enabled command timings substantially
overstate the default-layer profile on this machine. No optimization should be
chosen from the validation-forced number. The no-explicit-layer result is
still a timer-instrumented, aggregate transition profile, and the source
profile was a separate pass; it is not proof of a user-visible frame hitch or
a matched total for the whole load. No async upload or mipmap change is
justified by these measurements. If a transition hitch is observed, the next
measurement should capture per-frame load cost with the user's normal Vulkan
layer configuration before choosing a target. The worker boundary remains
detached, owned pixel data only.

## Attachment-preservation graph validation — 2026-10-02

The newer logical reads for attachment-preserving scene draws and blended
postprocess outputs were validated with real MAP06 frames on GL and Vulkan at
effects-on and effects-off settings. Each run executed `r_framegraph_selftest`,
`r_resource_validate 1`, `r_resources`, and `r_framegraph` after 180 frames.
Effects-on graphs were rooted at `Backbuffer`: GL reported 47 passes / 104
edges and Vulkan 48 / 105, including the preserved `SceneColor` chain through
opaque draw, SSAO composite, and portal/translucent draw. Both also recorded
blended reads/writes for `Exposure.Camera` and `PipelineImage[0]`. Effects-off
reported 6 / 13 on GL and 7 / 14 on Vulkan; optional AO, bloom, tonemap, lens,
and FXAA passes were absent while scene attachment dependencies remained.
Every self-test passed, there were no dead-pass candidates or stale-size
diagnostics, and Vulkan validation reported no errors.

The saved live offscreen-camera probes show that `RenderTextureView()` keeps an
`offscreen.camera` producer active while camera materials are selected, so
their sampled reads attach to that pass. The 2026-10-03 follow-up below splits
that aggregate into target-specific clear, opaque, and portal/translucent
passes on GL and Vulkan.

## Shadow-map conditional route — 2026-10-02

A temporary ZScript fixture spawned a `PointLight` at the MAP06 player start,
activating the shadow-map path against the map's existing geometry. With
`gl_light_shadowmap 1` and quality 256, GL reported 43 passes / 86 edges and
Vulkan reported 44 / 87. Both graphs were rooted at `Backbuffer`, recorded the
`shadowmap` pass writing `ShadowMap`, and recorded `ShadowMap` reads in both
`scene.opaque` and `scene.portal_translucent`. `r_resources` showed ShadowMap
written and read on both backends. The self-test passed, there were no
dead-pass candidates or stale-size diagnostics, and Vulkan validation emitted
no errors. The fixture and logs live under `/tmp`; no fixture content was
added to the repository.

The global graph exposed an ordering freedom here: `shadowmap` and
`scene.target` write separate resources, and both precede `scene.opaque`. The
bounded GL/Vulkan experiment has since moved the scene clear before the shadow
update while restoring the scene target and viewport state before geometry.
Same-backend pixel comparisons passed exactly; the contract and results are
recorded in [`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md).
Metal retains its original order because its deferred clear is not yet
flushable through this interface. This remains an explicit backend call order,
not a graph-scheduled scene pass.

## Shared raster AO quality 1/2 coverage — 2026-10-03

Using the existing Linux build on the RX 550, real MAP06 frames exercised
`gl_ssao` quality 1 and 2 with debug mode 0, all other postprocess effects off,
and single-sample scene targets. Each configuration ran with direct execution
(`r_framegraph_ao 0`) and graph replay (`r_framegraph_ao 1`). Captures were
1920x1080; graph and resource reports were collected after 100 in-level frames
and the image after 120. Every launch passed `r_framegraph_selftest`.

GL reported 11 passes / 31 edges for either quality, with the five AO passes
`ssao.lineardepth`, `ssao.occlude`, `ssao.blur.h`, `ssao.blur.v`, and
`ssao.combine`. Its AO-off control reported 6 / 13. Vulkan reported 12 / 32
for both qualities and both linear-depth routes: the raster route recorded
`ssao.lineardepth`, while `vk_compute_ssao 1` recorded
`ssao.lineardepth.compute` writing `AO.LinearDepth` as storage. Its AO-off
control reported 7 / 14. Quality 1 selected `AO.RandomTexture0`; quality 2
selected `AO.RandomTexture1` on both backends.

Every graph reached `Backbuffer`, had no dead-pass candidates, and had no
replay fallback. Resource reports had no stale-size diagnostics. Vulkan runs
set `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`; their logs contained no
validation errors. GL direct and replay captures matched exactly in the
viewport analysis region at both qualities. Vulkan direct and replay captures
also matched exactly there for raster and compute linear depth at both
qualities. A repeated GL quality-1 direct capture had zero differences in that
region, establishing a zero-difference repeat control for this route.

The AO-on controls prove the effect was active: compared with each backend's
AO-off capture, pixels above the two-level threshold changed by 0.84%/1.73%
for GL quality 1/2, 0.82%/1.70% for Vulkan raster, and 0.80%/1.68% for Vulkan
compute. Vulkan compute-versus-raster captures stayed within two channel
levels (quality 1 max delta 1; quality 2 max delta 2, with zero pixels above
threshold). Image differences were measured in the viewport analysis region
used by `tools/pngdiff.py`. This closes Linux GL/Vulkan q1/q2 replay parity and
the Vulkan compute/raster linear-depth route at single sample. It does not
cover Metal runtime, performance, or higher multisample counts.

## Camera target attachment graph scopes — 2026-10-03

The camera graph contract is implemented in shared scene traversal and the GL,
Vulkan, and Metal backend source. `RenderTextureView()` scopes the active
target's color identity and a derived depth/stencil identity around its camera
callback. `Clear3DViewport()` records attachment clears; camera opaque and
portal/translucent traversal record their own logical attachment-preserving
passes. The terminal portal/translucent pass is a keep-alive root. Camera
passes do not borrow main-view `Scene*` resources or include main-view G-buffer
attachments.

The self-test covers clear → opaque → portal/translucent RAW dependencies for
both attachments, material reads in both scene phases, distinct identities for
multiple targets, exact target writes, and the preserved-content canvas path.
The full CMake build and self-test pass.

Live GL validation with the camera-visible fixture reported 52 passes / 114
edges. Live Vulkan reported 12 / 24 with
`VK_LAYER_KHRONOS_validation` enabled. Both showed the three camera passes on
the target color and derived depth/stencil names, connected attachment edges,
sampled reads, `Backbuffer` reachability, and no dead-pass candidates or graph
diagnostics. The Intel Metal source changes have not yet been rebuilt or
validated on macOS; that is the remaining backend check. These runs validate
diagnostic graph identities and dependencies only, not a scheduler or Metal
memoryless attachment policy.

## Existing opaque texture-sort A/B and list probe — 2026-10-04

The first Linux performance candidate is the existing `gl_sort_textures`
option. The tested code sorts plain/masked wall lists by texture and clamp
flags, and flat lists by their texture sort key; it does not merge the indexed
draws. The run used the native GL backend on the AMD Radeon RX 550 / Mesa
26.2.4, Intel i5-2400, and MAP06. The initial benchmark passed `+win_w` and
`+win_h`, but those do not set the native POSIX startup size. `V_InitScreenSize()`
reads `-width` and `-height` and stores the values in `vid_defwidth` and
`vid_defheight`; the native window uses those dimensions. Re-running with
`-width 1280 -height 720` produced 1280×720 frame captures and matching engine
dimensions. No renderer resolution defect was reproduced; the initial
640×480 timing below is retained as a superseded diagnostic result.

Both arms used an unpaused, stationary player view, `vid_vsync 0`,
`vid_maxfps 0`, `cl_capfps 0`, and all listed postprocess effects off. Runs
were interleaved off/on/off/on. Each launch passed `r_framegraph_selftest`.
For each launch the startup interval was discarded; four steady five-second
`vid_frametrace` windows remained per setting. Across those windows:

| `gl_sort_textures` | Mean update interval | p50 range | p95 range | p99 range |
|---|---:|---:|---:|---:|
| 0 | 1.015 ms | 0.92–0.98 ms | 1.45–1.58 ms | 1.85–2.24 ms |
| 1 | 0.895 ms | 0.84 ms in all four windows | 1.33–1.36 ms | 1.66–1.76 ms |

This original 640×480 result suggested a 12% mean improvement, but it is
superseded by the correctly sized comparison below. The separate paused off/on
captures were exactly identical, but their timings were rejected: source
inspection of `TryRunTics()` showed that the paused loop waits on `pauseext` at
the 35 Hz tic cadence, masking the uncapped renderer cost.

The corrected 1280×720 A/B repeated the unpaused, stationary, uncapped setup
with sorting off/on/off/on. Every launch passed `r_framegraph_selftest`; the
first `vid_frametrace` window was discarded as startup. Four steady five-second
windows remained per setting:

| `gl_sort_textures` | Mean interval by window | p50 range | p95 range | p99 range |
|---|---:|---:|---:|---:|
| 0 | 1.57–1.60 ms (mean 1.58) | 1.54–1.57 ms | 1.98–2.04 ms | 2.54–2.66 ms |
| 1 | 1.44–1.47 ms (mean 1.45) | 1.39–1.44 ms | 1.87–1.92 ms | 2.44–2.50 ms |

Sorting reduced mean interval about 8%, p95 about 6%, and p99 about 5% in
this view. Captures were all 1280×720. Phase-matched cross-arm pairs
(`sort-off-01`/`sort-on-02` and `sort-off-02`/`sort-on-01`) were byte-identical
in the viewport analysis region. The other cross-arm pairs differed in 168
analysis-region pixels, with maximum channel delta 31, so retain that small
view-repeatability caveat. This is still one view and does not justify a
default change.

A temporary one-frame list/state probe was built, run, and removed. Before
sorting, the visible view contained 36 plain walls (7 textures, 9 texture/clamp
keys, 33 adjacent-key runs) and 13 plain flats (4 textures/keys, 11 runs);
masked and masked-offset lists were empty. Sorting reduced plain-wall runs to
9 and plain-flat runs to 4, matching their unique key counts. GL still issued
49 draw calls (13 indexed, 36 array; 1,206 indices) in either arm, confirming
that this option reorders draws rather than merging them. Across those draws,
material requests stayed at 49 while material-key changes fell 44→13,
same-key requests rose 5→36, texture changes fell 41→11, clamp changes
10→6, translations remained 0, and shader changes remained 1. These are
render-state cache-key and application counts, not a direct measurement of raw
driver texture binds. No masked geometry was visible, so that route still needs
coverage.

Next, extend the corrected-resolution A/B and the same accounting to views
with masked walls, portals, camera textures, and compatibility maps. Record
pipeline, sampler, and per-wall stream-state breaks there before proposing
another sort or changing the default. Keep any new instrumentation temporary
and verify it does not perturb the measured path.

## Opaque sort coverage and matched follow-up — 2026-10-05

The broader Linux pass used native GL on the RX 550 / Mesa 26.2.4 at 1280×720,
with vsync and frame caps off. Each arm used a stationary spawn view and four
steady five-second `vid_frametrace` windows; the startup window was discarded.
The matched INIs were cloned from one file and differ only in
`gl_sort_textures`. An earlier timing attempt with the temporary state probe
compiled in was discarded. A first post-removal attempt also exposed an
unrelated mod-HUD setting difference between its saved INIs and was discarded.
The state probe was then removed, the full build completed, and the final timing
runs used the matched pair. `vid_frametrace` is an update-to-update wall-time
measurement, not isolated GPU time.

| View | `gl_sort_textures` | Mean of four window means | p50 range | p95 range | p99 range |
|---|---:|---:|---:|---:|---:|
| Ashes Hard Reset MAP01, Night School | 0 | 7.61 ms | 7.44–7.52 ms | 8.65–8.73 ms | 9.51–12.89 ms |
| Ashes Hard Reset MAP01, Night School | 1 | 5.43 ms | 5.32–5.42 ms | 6.48–6.72 ms | 7.21–7.51 ms |
| DOOM2 MAP02, `-compatmode 3` | 0 | 1.44 ms | 1.36–1.42 ms | 1.95–1.99 ms | 2.36–2.45 ms |
| DOOM2 MAP02, `-compatmode 3` | 1 | 1.32 ms | 1.25–1.28 ms | 1.81–1.86 ms | 2.24–2.32 ms |

Sorting lowered the mean update interval by 28.7% in the busy Ashes view and
8.2% in the lighter stock-map view. The latter is a small absolute difference
(about 0.12 ms). These are two spawn views on one machine, so they characterize
the existing option but do not support changing its default.

A separate temporary one-frame probe covered real lists and bind state. In
Ashes MAP01, the main view had 403 plain walls (51 texture/clamp keys), 288
flats (29 keys), and 135 masked walls (18 keys). Adjacent key runs fell from
197→51 for plain walls, 207→29 for flats, and 70→18 for masked walls. The
portal subview contained one masked wall. The compatibility MAP02 view had 63
plain walls (12 keys) and 35 flats (5 keys), with runs falling 43→12 and
21→5; no masked surface was visible in that view.

For Ashes plain walls, sampler bind calls fell 788→204 and sampler-index
changes 348→76; wall-stream-state breaks fell 279→262. For masked walls those
counts were 284→76, 184→56, and 121→105. Shader-program pointer changes were
few, but this probe did not count the complete pipeline key. The GL draw
submission counts did not fall: the 403 plain-wall submissions and 136 masked
wall submissions (one masked item emits two draws) stayed the same. Sorting
reduces state churn but does not merge geometry, and the remaining
wall-stream-state breaks are still substantial. All probe hooks were removed
before the measured runs.

The camera-texture fixture did activate an offscreen camera producer, but only
on its startup update. It was absent from the second-frame and delayed
captures, including with the camera texture assigned to a candidate consumer
surface. That does not establish a sustained camera-texture consumer path, so
there is no camera timing claim.

### GL pipeline-key and sub-draw-state accounting — 2026-10-05

A temporary, runtime-gated GL probe captured one live frame per arm at
1280×720 on the RX 550 / Mesa 26.2.4. It covered Ashes MAP01 with its portal
and masked walls, DOOM2 MAP02 under `-compatmode 3`, and the controlled MAP99
camera-texture consumer with `vid_preferbackend 0`. Each view ran with
`gl_sort_textures` off and on. These are state counts, not timing samples.

GL has no single pipeline object, so the probe used a normalized pipeline key:
active program, vertex-array layout, draw framebuffer, topology/pass and draw
buffer count, plus fixed depth, stencil, blend, cull, scissor, clip, sample,
polygon, and color-mask state. The fixed state baseline was read once, then
updated at the render-state setters; the bound draw framebuffer was sampled at
each draw to cover camera-target switches. Candidate batch keys combine that
pipeline signature with material/translation/clamp, effective sampler mode,
and vertex/index buffer bindings and offsets. Per-draw geometry ranges and
render-state uniform values are counted separately as sub-draw state. The
reported distinct keys are 64-bit signatures. Sampler-key changes mean a
change in effective clamp/sampler selection, not raw `glBindSampler` calls;
the earlier bind-call measurements remain in the preceding section. These
temporary counters do not model GL driver PSO internals or measure timings.

| View/list | Draws | Pipeline signatures off/on | Candidate batch signatures off/on | Batch-key transitions off→on | Sampler-key transitions off→on | Geometry-range changes | Uniform-state changes off→on |
|---|---:|---:|---:|---:|---:|---:|---:|
| Ashes MAP01 plain walls | 403 | 1 / 1 | 51 / 51 | 196→50 | 87→19 | 402 / 402 | 298→272 |
| Ashes MAP01 plain flats | 288 | 1 / 1 | 29 / 29 | 206→28 | 0→0 | 287 / 287 | 221→139 |
| Ashes MAP01 masked walls | 135 | 1 / 1 | 18 / 18 | 69→17 | 44→12 | 134 / 134 | 121→105 |
| DOOM2 MAP02 plain walls | 63 | 1 / 1 | 12 / 12 | 42→11 | 20→7 | 62 / 62 | 45→47 |
| DOOM2 MAP02 plain flats | 35 | 1 / 1 | 5 / 5 | 20→4 | 0→0 | 34 / 34 | 31→23 |
| MAP99 camera consumer plain walls | 1 | 1 / 1 | 1 / 1 | 0→0 | 0→0 | 0 / 0 | 0→0 |
| MAP99 camera consumer plain flats | 2 | 1 / 1 | 2 / 2 | 1→1 | 0→0 | 1 / 1 | 1→1 |

Every measured list had one pipeline signature and no pipeline-key transition.
Buffer-binding changes were zero in every row. Geometry ranges changed on
nearly every draw, but did not alter the candidate batch key; they are draw
payload, not pipeline or material incompatibilities. The Ashes portal subview
also produced four plain-flat draws and one masked-wall draw, with one
pipeline/batch signature in each small list. Sorting left the distinct
candidate key sets and draw submissions unchanged while reducing adjacent
material/sampler key runs. Uniform-state counts are single-frame observations;
the compatibility MAP02 wall count rose by two with sorting, so the probe
does not imply uniform-state changes always follow material sorting.

The MAP99 probe saw only one plain wall and two flats, so it establishes the
fixture's small draw-state footprint, not camera performance. The separate
consumer graph check above remains the data-flow proof. The temporary source
hooks were removed after capture; the full build was rerun without them. No
timing claim or sort-default change follows from this accounting.

### Metal wall-fan batch correction — 2026-10-05

The Metal source audit found a primitive-type mismatch in the pending triangle
batch. `HWWall::RenderWall()` submits each wall as `DT_TriangleFan`
(`hw_walls.cpp:95`), while `MtRenderState::Draw()` converts fans, triangle
lists, and strips to a triangle list and stores `DT_Triangles` in
`mPendingBatch.dt`. Both the pre-`Apply()` check in `Draw()` and the batch-key
check in `Apply()` compared that normalized stored type with the caller's
unnormalized `dt`. Since `Draw()` defaults to `apply=true`, every subsequent
wall fan mismatched and flushed the pending batch before it could accumulate
the next wall. This prevented the intended adjacent wall-fan batching.

Fixed in `src/common/rendering/metal/renderer/mt_renderstate.cpp` with
`GetBatchDrawType()`: `DT_TriangleFan`, `DT_Triangles`, and
`DT_TriangleStrip` compare as `DT_Triangles`, matching the batch's emitted
primitive. Other primitive types remain distinct and still end a triangle
batch. Both comparisons use this normalization; material, vertex-buffer,
culling, pipeline, push-constant, and stream-offset rules are unchanged.

The source-level batch contract is:

- The batch key includes normalized primitive type, `MtPipelineKey`, vertex
  buffer and both offsets, cull mode, material pointer, translation, clamp mode,
  and override shader. `MtPipelineKey` covers vertex format, special effect,
  effect state, alpha test, blend mode, depth/stencil state, color mask, culling,
  depth clamp/write, sample count, target formats/draw-buffer count, clip mask,
  and shadow-pass state.
- Within a batch, a sub-draw starts when the full `PushConstants` value changes
  or the matrix/stream buffer identity or offset changes. The constants contain
  clip split, specular values, light parameters, texture mode, alpha threshold,
  fog mode, light index, bone index, stream-data index, and clip-distance mask.
- Non-batchable primitive types, batch-key changes, the 256-sub-draw limit, and
  the batch-index capacity remain separate flush paths. The runtime count and
  distribution of these reasons have not yet been captured on Metal.

The full Linux build passed, but this Linux configuration excludes
`METAL_SOURCES`; it does not compile or runtime-validate the changed Metal
source. Rebuild and validate on Intel Metal using
[`handoff-macos-2026-10-04.md`](handoff-macos-2026-10-04.md). Instrument there
that several consecutive wall fans enter one pending batch, while legitimate
sub-draw state changes remain separate, then verify image correctness. Keep the
camera consumer as an open fixture issue. The Linux sorting data still does
not justify a default change.
