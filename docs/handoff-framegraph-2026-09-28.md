# Frame graph status and next work — 2026-09-28

## Current state

The shared renderer has a resource registry and a CPU-side diagnostic
`FrameGraph` in `src/common/rendering/hwrenderer/frame/`. It records real
postprocess passes, pass resource uses, read-after-write dependencies, backend
resource-use observations, and upload/read observations. It checks consistency
of recorded uses and produces a deterministic topological order for reporting.
GL, Vulkan, and Metal still execute their existing backend command paths in
their existing order. The graph does not schedule or reorder GPU work, issue
barriers, allocate resources, or cull passes.

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

Scene attachment preservation and blended postprocess outputs now also record
logical reads of their prior contents. These feed the existing RAW dependency
builder while backend-use observations continue to describe attachment binding
as a write. The graph still does not model WAR/WAW hazards for scheduling.

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

### Intel Metal conditional validation — SSAO only — 2026-10-01

On the Intel Iris Graphics 6000 machine, `cmake --build build --parallel 4`
passed and `r_framegraph_selftest` reported `selftest: PASS`. A real DOOM2
MAP06 session ran with `gl_ssao 3` and bloom, tonemap, lens, and FXAA disabled;
`r_framegraph` and `r_resources` were issued after 240 rendered frames. The
graph reported 10 passes / 17 edges, `Backbuffer` as the required output, and
no dead-pass candidates. The chain included
`scene.target -> scene.opaque -> ssao.lineardepth -> ssao.occlude ->
ssao.blur.h -> ssao.blur.v -> ssao.combine -> scene.portal_translucent ->
scene.resolve -> present`, with the expected attachment-preservation edges.
`AO.LinearDepth` and `AO.Ambient0`/`AO.Ambient1` were touched for reads and
writes and had the expected first/last-use intervals. `r_resources` reported
19 resources / 45.6 MB with no stale-size diagnostics; only the unused screen
and save shadow maps were untouched. This adds Intel Metal SSAO-only
conditional coverage. It does not validate Linux GL/Vulkan, image parity, or
Apple Silicon.

### Intel Metal compute-bloom graph validation — 2026-10-01

The first forced compute-bloom capture ran on Intel Tier 1 hardware and exposed
an observer gap: `mt_caps` confirmed Metal compute plus raster composite, while
the graph reported `bloom.compute` and the exposure chain as dead. The compute
pass omitted its `Exposure.Camera` read, and the final raster composite into
`PipelineImage[0]` had no graph pass. These were false dead-pass candidates;
the rendered route was active.

The graph observer now records `Exposure.Camera` as a compute input and records
the final composite as a separate `bloom.composite` pass. On Tier 1 it reads
`Bloom.Composite` and preserves/writes the current pipeline image as a color
attachment. On the Tier 2 direct-composite path it reads `Bloom.A` and records
the pipeline image as storage read/write. Backend command order is unchanged.
The self-test now covers both graph shapes and has negative controls that omit
the composite edge or exposure dependency.

After rebuilding, `r_framegraph_selftest` passed. A real DOOM2 MAP06 capture
with `gl_bloom 1`, `mt_compute_bloom 1`, and `mt_compute_bloom_intel 1`
reported 18 passes / 21 edges, `Backbuffer` as output, and no dead-pass
candidates. `mt_caps` confirmed `Metal compute (MtBloomModule)` and
`Tier 1 compute + raster composite`. The graph contains
`exposure.combine -> bloom.compute -> bloom.composite -> present`, with
`Bloom.Composite` live from the compute producer to the raster consumer.
`r_resources` reported 36 resources / 55.3 MB, including `Bloom.Composite`
read and written, with no stale-size diagnostics; only unused screen/save
shadow maps were untouched. This is live Tier 1 coverage; Tier 2 was modeled
in the CPU self-test but remains untested on hardware. No image-parity or
performance claim is made.

### Intel Metal compute-AO graph validation — 2026-10-01

The first forced compute-AO capture confirmed that `MtAOModule` was active but
reported `ssao.compute` as dead. Its graph pass omitted the later raster blend
into `SceneColor`, and listed `AO.DepthPyramid` even with algorithm 0, which
does not build that resource. The runtime observer now records the depth
pyramid as its own producer only when its dispatch runs, reports only AO
intermediates that were dispatched, and records the selected AO result flowing
into a SceneColor-preserving raster composite. No Metal command order changed.
The self-test covers the base algorithm without a pyramid, algorithm 2 with its
pyramid producer, and a negative control that omits the composite.

After rebuilding, `r_framegraph_selftest` passed. A real DOOM2 MAP06 capture
forced `mt_compute_ao 1`, `mt_compute_ao_intel 1`, and
`mt_compute_ao_intel_clamp 1` with algorithm 0 and the other postprocess
effects disabled. `mt_caps` confirmed the Metal compute AO path on Intel. The
graph reported 7 passes / 16 edges, `Backbuffer` as output, no dead-pass
candidates, and no build errors. The retained chain includes
`scene.opaque -> ssao.compute -> ssao.compute.composite ->
scene.portal_translucent`; the composite reads the selected `AO.FullresTemp`
and preserves/writes `SceneColor`. `AO.DepthPyramid` was absent, as expected
for algorithm 0. `r_resources` reported 20 resources / 52.6 MB, with the
selected full-resolution AO output touched for read and write, and no
stale-size diagnostics. This validates the Intel algorithm-0 route. No
image-parity or performance claim is made.

### Intel Metal compute-AO algorithm variants — 2026-10-01

Extended the live compute-AO coverage to the two alternate algorithms using
the same isolated-effects configuration (`mt_compute_ao 1`,
`mt_compute_ao_intel 1`, `mt_compute_ao_intel_clamp 1`, `gl_ssao 3`; bloom,
tonemap, lens, and FXAA disabled).

Algorithm 2 was run on the recorded DOOM2 MAP01 route. The predicted graph
shape was 8 passes / 18 edges: one depth-pyramid producer and its two new
dependencies over algorithm 0. The observed graph matched exactly, with
`ssao.depth-pyramid` writing `AO.DepthPyramid` and `ssao.compute` reading it;
the compute result continued through `ssao.compute.composite` into
`SceneColor`. `mt_caps` confirmed Intel Metal compute AO and algorithm 2.
`r_resources` reported 21 resources / 55.6 MB, with `AO.DepthPyramid` at
1280x1024 R16F touched for both write and read. There were no dead-pass
candidates or stale-size diagnostics, and `r_framegraph_selftest` passed.

Algorithm 1 (AlchemyAO/SAO) was run on DOOM2 MAP06. It reported the expected
7 passes / 16 edges, with no depth-pyramid pass, and retained the same compute
result-to-scene-composite dependency as algorithm 0. `mt_caps` confirmed
algorithm 1 on Intel Metal. `r_resources` reported 20 resources / 53.1 MB;
there were no dead-pass candidates or stale-size diagnostics, and the graph
self-test passed. These runs close Intel runtime graph coverage for algorithms
0, 1, and 2. Linux GL/Vulkan validation and image parity remain open; no
performance comparison was made.

Also exercised the Intel default-policy branch on MAP06 with
`mt_compute_ao 1` but `mt_compute_ao_intel 0`. `mt_caps` reported the expected
reference postprocess SSAO fallback, and the graph returned to the 10 passes /
17 edges shape from the earlier raster SSAO-only run. `r_resources` reported
19 resources / 45.6 MB, with no dead-pass candidates or stale-size diagnostics;
the graph self-test passed. This confirms the Intel opt-in gate selects the
reference path even when compute AO is globally enabled.

### Intel Metal recorded-route diagnostics — 2026-10-01

Added two opt-in diagnostics to investigate the previously reported long
display interval. `mt_shader_report` prints native metallib load status, exact
function-symbol lookup counts, and unique missing cache keys. While
`vid_stalltrace` is enabled, a fixed 128-entry ring retains the pass names from
recent Metal frames and prints the frames falling inside a slow loop interval.
The ring uses fixed storage and is inactive when `vid_stalltrace` is off.

Replayed `build/framegraphdemo.lmp` on Intel Metal with stock DOOM II MAP01,
`METAL_CAPTURE_ENABLED` unset, and stderr captured. The map-entry interval was
1319.69ms. Its framegraph sequence was one 10-pass frame at +155.54ms followed
by 37 present-only updates. `display` accounted for 1228.98ms, of which
1163.91ms was outside the named phases; `nextdrawable` accounted for only
2.39ms across 38 calls. This excludes expensive scene/postprocess passes and
`nextDrawable` as explanations for this particular interval, while leaving the
display-side wait unidentified.

The initial native lookup report recorded 92 hits and nine misses across 101
requests. These were raster postprocess shaders and caused nine `msl_tolib`
calls totalling 4.05ms, with no `msl_translate` work. They did not explain the
1.32s interval. The report counts the raster postprocess lookup path; the
hand-written `mt_ao.metal` and `mt_bloom.metal` compute functions are separate
fixed-name functions and were already in the native library.

### Postprocess MSL coverage closure — 2026-10-01

The current MSL text for the misses was already present in the exact-keyed
runtime cache. `tools/collect_metal_shaders.py` previously collected only fresh
translations written to the cache's `generated` directory, so cached AO and
postprocess stages were never promoted into source control. Added a targeted
`--from-cache` mode that accepts exact keys printed by `mt_shader_report`.
Promoted 17 current stages: nine first exposed by the SSAO route and eight
additional exposure/bloom-extract/tonemap/lens/FXAA stages exposed by enabling
all postprocess effects. This includes the new source hashes for `ssao.fp` and
`lineardepth.fp`; the raster AO coverage gap was due to those stages having
changed since collection. The hand-written compute AO shaders were already
compiled from `mt_ao.metal`.

Fixed the build dependencies so changed/new PK3 files rebuild the archive, and
a metallib-only update reruns the app-bundle copy step. The Make source list
continues to omit bracketed asset names that Make treats as patterns; `zipdir`
still packs those files by walking the source tree. CMake reports 109 generated
stages after the additions.

A fresh Intel Metal build linked the 109 stages and refreshed `gzdoom.pk3`.
A real stock MAP01 replay with bloom, SSAO, tonemap, lens, and FXAA enabled
reported 109 native lookups / 109 hits, zero symbol misses, and zero
unavailable-library lookups. No `msl_tolib` or `msl_translate` event appeared.
`r_framegraph` reported 47 passes / 56 edges, no dead-pass candidates, and
`r_framegraph_selftest` passed. This closes the observed stock postprocess
shader coverage. Mod-provided shader keys remain runtime-generated by design.

The steady route remained around 28.4-28.6ms per frame, consistent with the
earlier 28.55-28.71ms windows at the precision of this instrument. The snapshot
observer therefore has no visible timing cost in this replay. This was a stock
demo and did not reproduce the reported Ashes freeze. The user later reported
that the gameplay freeze appears resolved after recent renderer, shader-coverage,
and build changes. Close it as an active task; no controlled Ashes before/after
capture isolates which change resolved it. The map-entry display wait recorded
above is a separate event. Reopen the freeze investigation only if it returns.

## Remaining work, in order

1. **Validate the completed CPU graph contract.** Intel Metal effects-on and
   all-effects-off cases are now recorded above. Conditional paths remain open
   on Metal; run representative live GL/Vulkan frames on Linux hardware.
   Confirm retained side effects stay rooted, candidates are interpreted
   correctly, and transient lifetimes match ping-pong uses and aliases. Keep
   live rendering in backend order during this stage.
2. **Move one bounded chain to graph-driven execution.** Start with the
   `Pass2` chain identified in `docs/frame-analysis.md` §4. Specify RAW, WAR,
   and WAW handling and per-backend synchronization before changing execution.
   Compare captured output against the existing path and exercise resize,
   enabled/disabled effects, and ping-pong direction.
3. **Widen graph-driven execution in measured steps.** The diagnostic graph
   now observes bloom/exposure and AO, but moving those chains to graph-driven
   execution remains future work. Keep resource aliasing and pass culling
   disabled until output roots, all relevant reads, writes, and lifetimes have
   been proven on migrated paths.
4. **Validate Metal policy on Apple Silicon.** CPU graph algorithms and
   backend-neutral contracts can proceed on Linux. Metal scheduling,
   transient aliasing policy, and TBDR performance choices need M-series runtime
   evidence. ARM64/AArch64 JIT work is also deferred until Apple Silicon is
   available for runtime validation, as requested.

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
positive/negative attachment-preservation cases, Tier 1/Tier 2 compute-bloom
composite topology with missing-edge controls, and compute-AO composite
topology with algorithm-specific pyramid coverage. On Intel macOS,
`cmake --build build --parallel 4` passed and a real Metal frame passed the
self-test and output/liveness checks described above. The prior Linux build
passed, but live GL/Vulkan output/lifetime validation remains open. Xvfb `+quit`
checks do not substitute for real rendered frames.

## Metal handoff check

The Intel Metal effects-on run confirms sampled material resources for
`scene.opaque` and `scene.portal_translucent`, a rooted `Backbuffer` present
chain, and no graph build errors or dead-pass candidates. The all-effects-off
run confirms only the five scene/present passes remain and are rooted. Material
inputs without a prior graph writer remain expected imported inputs; names
with a prior graph writer produce edges. Conditional paths remain open. These
runs check the Metal hook and graph contract, not Apple Silicon/TBDR
performance policy.

The latest Linux build includes GL/Vulkan and passed after the scene-material
hook was added. It does not compile the Metal backend. Live output/lifetime and
material-edge validation remains open on GL/Vulkan hardware. Offscreen-only
scene traversals remain outside the new material-read scopes.
