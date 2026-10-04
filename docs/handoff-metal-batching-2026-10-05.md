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
