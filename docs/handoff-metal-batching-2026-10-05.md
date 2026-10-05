# Metal wall-fan batching handoff — 2026-10-05

## Finding and source change

`HWWall::RenderWall()` submits wall geometry as `DT_TriangleFan`. Metal turns
fans, triangle lists, and strips into indexed triangle lists and stores
`DT_Triangles` in `mPendingBatch.dt`. The pending-batch checks in both
`MtRenderState::Draw()` and `MtRenderState::Apply()` compared that stored
normalized type against the original input type. With `Draw()`'s default
`apply=true`, consecutive wall fans therefore failed the comparison and
flushed the pending batch before it could collect the next wall.

`GetBatchDrawType()` maps `DT_TriangleFan`, `DT_Triangles`, and
`DT_TriangleStrip` to `DT_Triangles`; both checks now compare normalized types.
Other primitive types remain distinct. The change preserves the existing
batch-key and sub-draw rules. In particular, stable adjacent wall fans should
accumulate in one batch; a change to per-draw constants or matrix/stream-buffer
identity or offset may begin another sub-draw without flushing that batch.

The source-level batch key includes normalized primitive type, pipeline key,
vertex buffer and both offsets, cull mode, material pointer, translation,
clamp mode, and override shader. The pipeline key covers vertex format,
effects, alpha test, blend, depth/stencil, color mask, culling, depth clamp and
write, sample count, target formats and draw-buffer count, clip mask, and
shadow-pass state. The sub-draw key compares the complete `PushConstants`
value and matrix/stream buffer identity and offset. Non-batchable primitive
types, changes to the batch key, the 256-sub-draw limit, and index-buffer
capacity remain independent flush conditions.

## Linux evidence and limits

The Linux build completed, but its CMake configuration excludes
`METAL_SOURCES`; this confirms no Metal compilation or runtime behavior. The
separate Linux GL `gl_sort_textures` comparison at 1280×720 measured 7.61 ms
versus 5.43 ms in Ashes MAP01 and 1.44 ms versus 1.32 ms in DOOM2 MAP02
(`-compatmode 3`) with sorting off versus on. Those results describe GL draw
reordering only. They do not measure or validate this Metal batch fix, and do
not justify changing the sorting default.

The 2026-10-05 Linux follow-up also measured normalized GL state keys on live
Ashes MAP01, compatibility-mode DOOM2 MAP02, and a controlled MAP99 camera
consumer. Every measured opaque list had one GL pipeline signature and zero
pipeline-key transitions. Sorting reduced adjacent candidate batch-key
transitions (for example, Ashes plain walls 196→50 and plain flats 206→28)
without changing the distinct candidate key set or draw count. Geometry ranges
and uniform changes remained per-draw payload. These results narrow the
cross-platform question but do not map GL keys onto Metal PSOs or predict Metal
flush counts; full measurements and fixture details are in
[`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md).

## Intel macOS validation

Build the Intel Metal configuration with `HAVE_VULKAN=OFF`. Ensure the source
revision includes `GetBatchDrawType()` and both normalized comparisons. Use a
view with many adjacent opaque wall fans and repeated materials; Ashes Hard
Reset MAP01 (`Night School`) is preferred if available. Keep the one-shot
camera fixture separate.

Add temporary counters around `MtRenderState::Draw()`,
`UpdateSubDrawState()`, and `FlushBatch()`, then remove them after the run.
Record source primitive types, source draws and total indices per batch,
sub-draw counts, and flush reasons. Separate normalized-type checks,
`mNeedApply`, every batch-key field, the 256-sub-draw limit, and index-buffer
capacity. For sub-draws, distinguish full `PushConstants` changes from
matrix/stream buffer identity or offset changes.

For the next accounting pass, retain per-field counters and also record a
64-bit reason mask for each flush and sub-draw boundary. This preserves cases
where multiple fields change on the same draw without assigning an arbitrary
single cause. Capture three settled live frames for the corrected route and,
where practical, the existing primitive-mismatch control; report per-frame
counts and the reason-mask histogram. Keep the pipeline-order failure control
as a separate invariant check. The Linux GL results above are context only:
the Mac report must use Metal's actual batch and sub-draw decisions.

Acceptance: consecutive `DT_TriangleFan` wall calls with a stable batch key
accumulate in the same pending batch; legitimate sub-draw state changes do not
flush the whole batch; other batch-key changes and non-batchable primitives
still flush. Confirm the emitted geometry against a matched image capture and
check frame-graph/resource diagnostics. Record the Mac model/GPU, build result,
fixture and settings, batch sizes, indices, sub-draws, flush reasons, image
comparison, and diagnostics. Do not claim a Metal performance gain until the
native run and an uninstrumented timing comparison are complete.

## Intel validation and pipeline-order correction — 2026-10-05

Validated source `8121da4449` plus a scoped, user-approved correction on the
Intel Iris Graphics 6000 / Core i5-5350U reference Mac (macOS 12.7.6).
`ApplyRenderPass()` previously bound the next pipeline/depth state before
`Apply()` noticed the changed key and flushed pending triangles. The flush
now happens before either state is bound. The late check in `Apply()` was
removed. This preserves the batch's pipeline through its actual submission.

**Prediction:** stable wall fans should share batches, while restoring the
old primitive comparisons should split the default-`apply=true` route. A
pending batch should always be emitted under its recorded pipeline; restoring
the old pipeline-bind order should fail that check. Matched geometry captures
were initially expected to be pixel-identical.

Temporary counters ran for three settled frames per launch in Ashes Hard Reset
1.05 MAP01 (`Night School`), with `gl_sort_textures 1`, single-sample targets,
and bloom/SSAO/tonemap/lens/FXAA/shadow maps disabled. The native Metal viewport
was 1440×900. Each measured frame submitted 612 source fans, one triangle list,
and 68 strips, totaling 17,028 accumulated indices; there were no point/line
draws. Both builds used `HAVE_VULKAN=OFF` and passed
`cmake --build build --parallel 4`.

| Route | Batch flushes/frame | Batches with multiple source draws | Largest fan batch | Pipeline mismatches/frame |
| --- | ---: | ---: | ---: | ---: |
| Corrected | 124 | 57 | 78 fans | 0 |
| Old primitive comparisons, pipeline correction retained | 593 | 2 | 78 fans | 0 |
| Normalized primitives, old pipeline-bind order | 124 | 57 | 78 fans | 7 |

The old primitive control still merges two batches: not every call uses the
default apply path. Its 581 primitive-only flushes explain the loss on that
path. In the corrected first measured frame, whole-batch key changes caused
106 flushes (76 material-only, 22 material plus clamp, eight clamp-only).
Pipeline changes caused seven; `mNeedApply` caused five; switching to indexed
geometry and ending the encoder caused three each. Vertex buffer/offset,
cull, translation, and override-shader differences were counted separately
but did not cause flushes in this view. Neither the 256-sub-draw limit nor
index capacity was reached; point/line behavior was not dynamically exercised.

The corrected non-frozen run recorded 429–430 emitted sub-draws/frame.
Full push-constant changes caused 305–306 sub-draw breaks; stream-offset
changes caused one or two (these categories can overlap). Matrix buffer,
matrix offset, and stream-buffer identity changes were zero. This verifies
that per-draw state changes can split sub-draws without flushing their batch.

Two interleaved frozen launches per arm (`freeze` after the first level frame,
`cl_capfps 1`, capture after 300 rendered level frames) separated animation
noise from the correction. The corrected and old-pipeline arms each repeated
exactly. Restoring the old pipeline order produced seven mismatches in every
measured frame and an image difference with maximum channel delta 180, mean
delta 7.8764, and 64,862 pixels above delta 2 (5.005%). This is a positive
failure control for the pipeline invariant, well above the observed noise.
Corrected versus old-primitive-control geometry differed by at most one
channel level, with no pixels above delta 2; the old primitive arm itself
also varied by one level between repeats. Therefore the initial exact-parity
prediction is not established for that comparison. The retained corrected
image was inspected directly.

All captures passed `r_framegraph_selftest`, reported a rooted 8-pass / 16-edge
graph, and had no dead-pass candidates, replay fallback, graph errors, or
stale-size diagnostics with resource validation enabled. All temporary
counters and control CVARs were removed, the final source was rebuilt, and
two uninstrumented frozen captures matched the corrected diagnostic image
byte-for-byte. Artifacts and scripts remain under
`/private/tmp/gzdoom-metal-handoff-1005` and
`/private/tmp/gzdoom_metal_handoff_1005.py`. No timing comparison or performance
claim is made; Apple Silicon remains untested.

### Final regression checks

The stock DOOM2 matrix ran ten available configurations on the uninstrumented
build. All eight effect/identity relations passed. All applicable golden
signatures matched except SSAO; the suite therefore returned `FAIL` for that
stored signature. The current SSAO pixel hash
`32bd4cb802dc6afc5b1e59e545070eb1` is identical to the pre-pull capture from
the earlier `09d90efed6` Intel 8× validation session (`single_2.png`). That
comparison establishes that this correction did not introduce the stored
SSAO mismatch. No baseline was rewritten. The two optional custom-PP fixtures
were unavailable and skipped.

`crossbackend.py --scene doom2 --only baseline,ssao --selfcheck` passed:
both OpenGL and Metal reproduced both configurations exactly. The subsequent
OpenGL/Metal comparison reported `OK` for both routes, classifying the
differences as uniform backend noise (median band mean 0.013 for baseline,
0.609 for SSAO). This is stock-scene cross-backend coverage, separate from the
Ashes pipeline failure control. Both harnesses used the temporary LaunchServices
wrapper and dedicated configs. `git diff --check` passed.

## Fine-grained Metal reason accounting — 2026-10-05 follow-up

Completed the accounting requested by remote revision `ae07e452e3`, using
the same Iris 6000 / i5-5350U Mac, macOS 12.7.6, Ashes 1.05 MAP01,
1440×900 native viewport, sorting on, single-sample targets, and effects/shadows
off. Freeze was enabled after the first level frame; temporary counters
captured three consecutive settled frames after rendered level frame 200.
Each arm used a dedicated config. This is Metal's actual triangle-batch
decision path; Linux GL signatures are not used as a substitute.

**Prediction before measurement:** corrected/control flush counts should remain
124/593 with 17,028 indices/frame; histogram totals should equal flush and
sub-draw boundary counts, and every mask bit's total should equal its separate
field counter. Corrected pipeline mismatches should be zero, versus seven
with the old binding order. All these predictions passed in all three frames.

The diagnostic emitted a 64-bit mask at each actual triangle-batch flush and
each changed-state boundary in `UpdateSubDrawState()`. Flush masks combine the
triggering predicate/call site with every observed batch-key difference at
that boundary. Pipeline-triggered flushes additionally compare every pipeline
key member. These are simultaneous observations, not exclusive causal labels:
for example, a material difference observed during an indexed-switch flush
does not mean material comparison triggered that flush. Sub-draw masks compare
all 16 push-constant components bytewise, matching the full-value comparison,
and independently compare matrix/stream buffer identities and offsets.

### Per-frame totals

Each frame in every arm retained 612 source fans, one triangle list, 68 strips,
and 17,028 indices. Corrected batches still included 57 multi-source batches
and up to 78 fans in one batch; the primitive control retained two such batches.
Emitted sub-draws include one initial sub-draw per batch. Boundary counts below
count subsequent changed-state boundaries rather than initial sub-draws.

| Route / measured frame | Flushes | Emitted sub-draws | State boundaries | Pipeline mismatches |
| --- | ---: | ---: | ---: | ---: |
| Corrected / 1 | 124 | 443 | 319 | 0 |
| Corrected / 2 | 124 | 439 | 315 | 0 |
| Corrected / 3 | 124 | 446 | 322 | 0 |
| Primitive control / 1 | 593 | 593 | 0 | 0 |
| Primitive control / 2 | 593 | 593 | 0 | 0 |
| Primitive control / 3 | 593 | 593 | 0 | 0 |
| Pipeline-order failure control / 1 | 124 | 443 | 319 | 7 |
| Pipeline-order failure control / 2 | 124 | 442 | 318 | 7 |
| Pipeline-order failure control / 3 | 124 | 452 | 328 | 7 |

### Flush masks and field counts

Both corrected and primitive-control histograms were identical across their
three measured frames. Every count in this table therefore applies separately
to frames 1, 2, and 3. Masks retain a full 16-digit hexadecimal representation.

Flush bits: 0 primitive mismatch, 1 `mNeedApply`, 2 vertex buffer,
3/4 vertex offsets, 5 cull, 6 material, 7 translation, 8 clamp,
9 override shader, 10 pipeline, 11 non-batchable primitive,
12 indexed switch, 13 encoder end, 14 frame end, 15 sub-draw limit,
16 index capacity. Pipeline-member bits 32–50, in order: vertex format,
special effect, effect state, alpha test, blend mode, depth function,
stencil op, stencil function, color mask, cull mode, depth clamp,
depth write, stencil test, sample count, draw-buffer count, color format,
depth/stencil format, clip mask, shadow-pass state. All other bits are reserved.

| Flush mask | Corrected, each frame | Primitive control, each frame |
| --- | ---: | ---: |
| `0x0000000000000001` | 0 | 475 |
| `0x0000000000000002` | 2 | 0 |
| `0x0000000000000003` | 0 | 2 |
| `0x0000000000000006` | 1 | 0 |
| `0x0000000000000007` | 0 | 1 |
| `0x0000000000000040` | 78 | 0 |
| `0x0000000000000041` | 0 | 75 |
| `0x0000000000000100` | 8 | 0 |
| `0x0000000000000101` | 0 | 8 |
| `0x0000000000000140` | 20 | 0 |
| `0x0000000000000141` | 0 | 23 |
| `0x0000000000000142` | 2 | 0 |
| `0x0000000000000143` | 0 | 2 |
| `0x0000000000001040` | 1 | 2 |
| `0x0000000000001140` | 1 | 0 |
| `0x0000000000001144` | 1 | 1 |
| `0x0000000000002000` | 3 | 3 |
| `0x0000000400000400` | 2 | 1 |
| `0x0000000a00000400` | 1 | 0 |
| `0x0000001a00000400` | 4 | 0 |

| Observed field / condition | Corrected, each frame | Primitive control, each frame |
| --- | ---: | ---: |
| `needApply` | 5 | 5 |
| `vertexBuffer` | 2 | 2 |
| `material` | 103 | 103 |
| `clamp` | 32 | 34 |
| `pipeline` | 7 | 1 |
| `indexedSwitch` | 3 | 3 |
| `encoderEnd` | 3 | 3 |
| `pipeline.SpecialEffect` | 5 | 0 |
| `pipeline.EffectState` | 2 | 1 |
| `pipeline.AlphaTest` | 5 | 0 |
| `pipeline.BlendMode` | 4 | 0 |
| `primitive` | 0 | 586 |

All other instrumented flush fields/conditions were zero. Notably, the
primitive control observes 586 primitive mismatches: 475 alone and 111
alongside other fields. The earlier 581 primitive-only *trigger-site* count
did not preserve those combinations. Likewise, the new vertex-buffer count
is two simultaneous differences at other trigger sites; it does not contradict
the earlier absence of vertex-buffer-triggered flushes. Material and clamp
appear in 103 and 32 corrected masks, respectively, including overlap and
observations at non-material trigger sites. These totals must not be summed
as mutually exclusive causes.

### Corrected sub-draw masks and field counts

Sub-draw bits: 0 full push constants, 1 matrix buffer, 2 matrix offset,
3 stream buffer, 4 stream offset. Bits 5–20 correspond to the 16 physical
push-constant components: clip split X/Y, glossiness, specular level,
light level, fog density, light factor, light distance, texture mode,
alpha threshold, fog mode, light index, bone index, stream-data index,
clip mask, reserved final component. All other bits are reserved.

| Sub-draw mask | Frame 1 | Frame 2 | Frame 3 |
| --- | ---: | ---: | ---: |
| `0x0000000000000201` | 8 | 9 | 9 |
| `0x0000000000010001` | 3 | 4 | 4 |
| `0x0000000000010201` | 1 | 1 | 1 |
| `0x0000000000040001` | 204 | 200 | 207 |
| `0x0000000000040011` | 1 | 1 | 1 |
| `0x0000000000040201` | 69 | 68 | 68 |
| `0x0000000000048601` | 30 | 30 | 30 |
| `0x0000000000048611` | 1 | 1 | 1 |
| `0x0000000000050001` | 2 | 1 | 1 |

| Changed field | Frame 1 | Frame 2 | Frame 3 |
| --- | ---: | ---: | ---: |
| `pushConstants` | 319 | 315 | 322 |
| `streamOffset` | 2 | 2 | 2 |
| `lightLevel` | 109 | 109 | 109 |
| `fogDensity` | 31 | 31 | 31 |
| `fogMode` | 31 | 31 | 31 |
| `lightIndex` | 6 | 6 | 6 |
| `streamDataIndex` | 307 | 301 | 308 |

Matrix buffer/offset and stream-buffer identity, and every other push component,
were zero. Every boundary changed push constants; the two stream-offset changes
per frame overlapped those boundaries. Most boundaries changed the stream-data
index, often together with light level or fog fields. For example,
`0x0000000000048601` combines full constants, stream-data index, light level,
fog density, and fog mode. The primitive control had no state boundaries
inside its batches and an empty sub-draw histogram in all three frames.
Sub-draw counts vary despite frozen game ticks: the per-draw Metal stream index
is allocator payload, not a static geometry/pipeline signature. No timing or
surface-data redesign follows from these counts alone.

### Separate invariant control and final verification

The old pipeline-order arm retained 124 flushes and seven pipeline mismatches
in every frame. It is a deliberate failure control, not an accepted batching
route. Its seven pipeline masks matched the corrected arm: two
`0x0000000400000400`, one `0x0000000a00000400`, and four
`0x0000001a00000400`. Its emitted sub-draw counts were 443, 442, and 452.
Detailed masks and counters for that arm are retained with the local artifacts.

Corrected versus primitive-control captures had maximum channel delta one and
zero pixels above delta two. The pipeline failure capture had maximum delta
180, mean per-pixel maximum-channel delta 7.8764, and 64,862 pixels above
delta two (5.005%). All three arms passed the CPU self-test and rooted
8-pass / 16-edge graph diagnostics, with no dead-pass candidates, graph errors,
or stale resource sizes.

All temporary source instrumentation and control CVARs were removed; the
renderer source matches `ae07e452e3` exactly. The uninstrumented Intel build
passed `cmake --build build --parallel 4` and the adjacent PK3 comparison.
Both final uninstrumented captures passed live graph/resource diagnostics.
The first matched the previous session's corrected reference hash, but the
second failed that exact-hash assertion. Comparing both against this session's
diagnostic image showed maximum delta one, with zero pixels above delta two;
exact byte parity is therefore not established for this follow-up. This is
within the one-level repeatability noise already observed in the primitive
control and far below the pipeline failure signal. No golden baseline changed.

Artifacts: `mask_{fixed,old_type,old_pipeline}.{log,png}` and final captures
under `/private/tmp/gzdoom-metal-handoff-1005`; decoded counters, legends,
per-frame histograms and image comparisons in
`/private/tmp/gzdoom_reason_mask_results.json`. Temporary scripts remain in
`/private/tmp`, not in the repository. This closes reason accounting for the
tested view only. Additional views, limit/non-triangle routes, uninstrumented
timing comparisons, and Apple Silicon behavior remain open.

## Approved capacity correction and boundary follow-ups — 2026-10-05–06

The follow-up capacity review found a real defect in `MtRenderState::Draw()`:
the CPU wrote converted indices before testing the `> 1000000` threshold.
`FlushBatch()` did not reset the frame's write offset, despite the old comment
suggesting reclamation. With 1,048,576 allocated indices, subsequent geometry
could therefore overrun the mapping. The AGENTS halt-and-flag rule was followed;
the user approved a pre-write reservation and safe backing-storage replacement.

`Draw()` now computes fan/strip expansion in 64 bits, checks the mapped range
before writing, and flushes before growing the index buffer. Growth uses the
existing `MtIndexBuffer::Lock()` allocation and device frame recycle ring;
queued draws retain their old backing storage. Only after allocating fresh
storage does the offset return to zero. A single draw larger than the current
capacity grows to at least its required size. Requests beyond the unsigned
byte-count interface, an invalid offset, exhausted growth, or allocation failure
produce an explicit fatal error. `MtIndexBuffer::CreateBuffer()` also checks
each allocation in its internal ring. The old post-write threshold is removed.
No shader, game-state, graph-order, or sorting-default change is included.
Growth retains larger buffers and can increase peak memory in very large scenes;
allocation-failure and interface-maximum requests were not forced on this Mac.

### Controlled live boundaries

Temporary diagnostics reduced the initial physical allocation to one index.
The same Metal draw path crossed seven growth boundaries:

| Old capacity (indices) | Write offset | Required indices | New capacity |
| ---: | ---: | ---: | ---: |
| 1 | 0 | 378 | 378 |
| 378 | 378 | 378 | 756 |
| 756 | 378 | 768 | 1512 |
| 1512 | 768 | 768 | 3024 |
| 3024 | 2304 | 768 | 6048 |
| 6048 | 5376 | 768 | 12096 |
| 12096 | 11508 | 768 | 24192 |

The first and third transitions exercise a single draw larger than the current
allocation. Every completed frame checked that written and submitted triangle
indices agreed; no overrun or dropped-index report occurred. A safe negative
control omitted reservation and intercepted the would-be overrun before the
write, rather than executing undefined behavior. It detected the first draw
and its deliberately rejected geometry differed from the matched image at
942,267 pixels above delta two (72.706%).

A second temporary probe replayed a valid 128-vertex fan 300 times, changing
only the shader's unused final push-constant component to create sub-draw
boundaries. It reached the real 256-entry limit. The correct route submitted
all 113,778 accumulated indices at the probe checkpoint. Disabling only the
limit flush submitted 96,768 and lost 17,010 indices. Since the omitted fans
duplicated earlier geometry, pixels alone did not detect this defect; the
written-versus-submitted counter did. This is why that counter is the acceptance
measurement. The probe also emitted a point and a two-vertex line through the
real immediate primitive route. It does not establish coverage for indexed
point/line batching or every primitive/state combination.

The first captures used different frame-cap settings from the earlier pinned
fixture and showed 229 pixels above delta two, maximum delta seven. They were
not accepted as parity. With `cl_capfps 1` and `vid_maxfps 0` pinned in both arms,
the normal, forced-small-capacity, and limit/primitive settled captures matched
exactly. All routes passed the CPU graph self-test and rooted 8-pass / 16-edge
graph checks with no stale resource sizes. Temporary probes and altered initial
allocation were removed before timing.

### Full-size capacity boundary

A separate native probe also crossed the production 1,048,576-index capacity.
It used 350,000 real triangle-list `Draw()` calls with valid vertex references,
then made the temporary fixture's triangles zero-area before submission to
avoid excessive pixel overdraw. After rendered level frame 200, the next
three-index draw at offset 1,048,575 triggered pre-write growth. The capacity
request doubled to 2,097,152 indices. Both repeated probe launches submitted
all 1,050,378 indices at the probe checkpoint and all 1,067,028 indices in the
stressed frame; subsequent ordinary frames retained 17,028 indices. This
extends the small-buffer test to the actual production boundary. No counters
or degenerate-geometry injection remain in the source.

Initial frame-one images failed within-arm repeatability and were rejected.
The settled 201-frame captures had maximum delta six: the baseline repeat
differed at 12 pixels above delta two, and the probe repeat at 16 pixels.
The two matched baseline/probe comparisons differed at 12 and 16 pixels,
respectively, with the same maximum delta. Thus the observed difference was
within the measured repeat variation, but exact byte parity was not established
for this large-index fixture. The written/submitted-index and overrun checks
passed independently; forced-small-capacity settled parity remains exact.
Every graph was rooted at `Backbuffer`, 8 passes / 16 edges, with no dead-pass
candidates or stale sizes. Artifacts are `native_settled_*.{log,png}` and
`/private/tmp/gzdoom_native_settled_analysis.json`; rejected startup captures
are `native_capacity_*` under the same temporary artifact directory.

Artifacts are `boundary_*.{log,png}` under
`/private/tmp/gzdoom-metal-handoff-1005`; temporary injectors and launchers are
under `/private/tmp`. The safe capacity control rejects geometry intentionally;
the limit control drops duplicate geometry intentionally. Neither is a shipped
route, and neither was used for timing.

### Uninstrumented timing and a second world-geometry view

Two Intel binaries were built with the approved capacity correction and the
earlier pipeline-order correction. The only behavioral difference in the
control source was `GetBatchDrawType()` returning the raw input type. Neither
binary contained the temporary counters or probe CVARs. Captures used the same
adjacent PK3, native Metal backend, viewport, effects-off settings, sorting on,
dedicated configs, and interleaved corrected/control launches. Binary SHA-256
values and source snapshots are retained with the local artifacts.

**Prediction/gate before measurement:** a timing benefit must exceed both
0.5 ms and the observed launch-to-launch spread. The instrument is the stock
`mt_metrics` rolling 120-frame renderer bracket, not per-pass GPU timing.
After discarding the first launch of each arm, three non-overlapping windows
were sampled at rendered frames 350/500/650, after resetting metrics at 200.
Vsync and `vid_maxfps` were zero; measured windows used `cl_capfps 0`.

| Accepted/context route | Launches/arm after warmup | Corrected mean | Primitive control mean | Within-arm launch-mean spread |
| --- | ---: | ---: | ---: | --- |
| Ashes MAP01 frozen view, context only | 4 | 16.5844 ms | 17.4233 ms | corrected 1.6307 ms; control 1.1783 ms |
| Static paused DOOM2 MAP02 world fixture | 2 | 2.1573 ms | 2.2553 ms | corrected 0.0100 ms; control 0.1480 ms |

The Ashes mean difference was 0.8388 ms, smaller than its run-to-run spread.
Paired differences were 2.4007, 0.1270, 0.1970, and 0.6307 ms. Its images
also missed exact repeatability: maximum delta five, with at most two pixels
above delta two in the measured within-arm comparisons. These samples do not
establish a performance gain. The static DOOM2 difference was 0.0980 ms,
below the predeclared 0.5 ms floor and its control spread. No Metal speedup
is claimed for either route; further optimization needs a new measurable case.

The initial DOOM2 `freeze` rig failed image repeatability, and pausing alone
still captured different monster/pickup animation states. Those attempts are
retained but are not accepted as parity or speedup evidence. The final temporary
fixture used `-nomonsters`, destroyed 43 ownerless world inventory pickups in
`WorldLoaded`, and hid player sprites with `r_drawplayersprites 0`. It paused
after level frame one, then removed the frame cap at 50 before measurement.
All four measured corrected/control captures matched exactly. This supplies
additional world-geometry correctness coverage for the normalized draw route
in MAP02 (`-compatmode 3`), not actor/sprite coverage or a general gameplay
benchmark. Every launch passed the graph self-test and rooted 8-pass / 16-edge
diagnostics with no dead-pass candidates or stale sizes.

Artifacts: `timing_ashes_*`, rejected `timing_doom2_*` and `paused_*` attempts,
and accepted `static_fixture_doom2_*` captures/logs under
`/private/tmp/gzdoom-metal-handoff-1005`. Numeric windows, binary hashes, and
image comparisons are retained in `/private/tmp/gzdoom_timing_results.json`,
`/private/tmp/gzdoom_followup_timing_analysis.json`,
`/private/tmp/gzdoom_static_fixture_timing_results.json`, and
`/private/tmp/gzdoom_static_fixture_analysis.json`. The inventory fixture is
`/private/tmp/gzdoom-static-batch-probe.pk3`; it is not shipped.

### Final production build and regression boundary — 2026-10-06

All temporary counters, producer/declaration switches, initial-capacity changes,
and geometry probes were removed. The final Intel `HAVE_VULKAN=OFF` build passed
`cmake --build build --parallel 4`, and the generated and executable-adjacent
PK3 files compared equal. Only the approved Metal pre-write capacity guard and
index-allocation failure checks remain as source changes.

The stock DOOM2 matrix ran ten available configurations; the two optional
custom-PP fixtures were unavailable. All eight effect/identity relations
passed. The sole golden-signature failure was the previously recorded SSAO
mismatch: its decoded pixel hash remained
`32bd4cb802dc6afc5b1e59e545070eb1`, unchanged from the pre-capacity-correction
record. No baseline was rewritten. OpenGL and Metal each reproduced baseline
and SSAO exactly in the final cross-backend self-check.
The subsequent OpenGL/Metal comparison returned `OK` for both, classifying
the differences as uniform backend noise: median band means 0.013 for baseline
and 0.609 for SSAO. The final `git diff --check` passed.

This closes the requested tested Intel follow-ups with the stated limits:
timing establishes no speedup above the floor, large-index image parity is
bounded by its repeat variation, and retained-shadow coverage forces producer
omission. Indexed point/line routes, other primitive/state combinations,
allocation failure/interface-maximum requests, naturally occurring retained
shadow reads, and Apple Silicon runtime/performance remain unestablished.
