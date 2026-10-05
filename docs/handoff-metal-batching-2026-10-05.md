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
