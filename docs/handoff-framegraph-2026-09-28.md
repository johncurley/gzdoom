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
3. **Widen in measured steps.** Add bloom and exposure, then AO. Keep resource
   aliasing and pass culling disabled until output roots, all relevant reads,
   writes, and lifetimes have been proven on the migrated paths.
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
and positive/negative attachment-preservation cases. On Intel macOS,
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
