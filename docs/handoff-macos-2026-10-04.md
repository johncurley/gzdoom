# macOS handoff — camera scopes and retained ShadowMap — 2026-10-04

## Purpose and source state

Validate the latest frame-graph changes on Intel Metal:

- Camera-target recording now identifies `offscreen.camera.clear`,
  `offscreen.camera.opaque`, and
  `offscreen.camera.portal_translucent`, with target-specific color and
  depth/stencil resource names.
- `r_framegraph` declares `ShadowMap` external so a retained value is valid
  when no same-frame producer runs. An active same-frame shadow producer must
  still supply the scene reads through RAW edges.

The Linux checkout is at `e4d837701` with local working-tree changes. That
commit alone does not contain the latest camera-scope and ShadowMap changes;
make sure those source edits are present in the macOS checkout before building.
Do not infer validation from an older Intel Metal binary. This handoff covers
Intel Metal correctness and graph recording, not Apple Silicon performance or
TBDR policy.

## Build and graph self-test

Use the Intel Metal configuration with `HAVE_VULKAN=OFF`. Build with:

```bash
cmake --build build --parallel 4
```

Run `r_framegraph_selftest` in the rebuilt app and record `selftest: PASS`.
The self-test includes the retained ShadowMap positive case, the missing
external negative case, and the same-frame producer precedence case. This is a
CPU graph check; it does not replace a live Metal frame.

Launch the bundle through LaunchServices (`open -W -n -a ... --args ...`). On
this Intel Mac, launching the GUI executable directly aborts in
`NSApplication sharedApplication`; `open` does not forward app stdout, so read
the GZDoom log or use the existing temporary capture launcher.

## Live camera fixture

Repeat the temporary MAP09 UDMF camera fixture and frame-1 capture procedure
from [`handoff-macos-2026-10-01.md`](handoff-macos-2026-10-01.md). The camera
producer is one-shot, so collect the graph on its first in-level frame before
the next frame resets it. Confirm:

- `r_framegraph_selftest` passes in the live run.
- Each updated target records clear, opaque, and portal/translucent scopes.
- Pass resources use the camera color's stable Metal texture name and its
  derived `<color-name>.DepthStencil` identity; no camera pass uses main-view
  `Scene*` attachments.
- Sampled camera materials attach to the consuming opaque or
  portal/translucent pass, and the graph reaches `Backbuffer` with no dead-pass
  candidates or build diagnostics.
- `r_resource_validate 1` and `r_resources` show no stale-size diagnostics.

Do not require a fixed total pass/edge count: the new scopes add passes and
dependencies relative to the earlier aggregate-camera fixture.

## Live conditional ShadowMap fixture

Use a temporary MAP06 fixture that spawns a `PointLight` at the player start,
as in the Linux route recorded in
[`handoff-linux-2026-09-29.md`](handoff-linux-2026-09-29.md). Enable
`gl_light_shadowmap 1` and `gl_shadowmap_quality 256`; verify the loaded level
has dynamic lights and does not set `LEVEL3_NOSHADOWMAP`. Keep the fixture and
logs outside the repository.

For the active route, confirm the Metal graph contains a `shadowmap` pass that
writes `ShadowMap`, followed by `ShadowMap` reads on the scene passes that use
it. The producer-to-consumer RAW edges must remain present despite the global
external declaration. Check `Backbuffer` reachability, no dead-pass candidates,
no graph-build errors, and no stale-size diagnostics. `r_resources` should
show `ShadowMap` touched on this route.

Run a control with shadow maps disabled or with no eligible shadow-mapped light.
The conditional producer should be absent. The retained-input case is
explicitly covered by the CPU self-test; do not claim a live no-producer read
unless the captured graph actually contains that read.

## Record the result

Include the Mac model/GPU, source revision plus whether the local changes were
present, build configuration/result, self-test output, exact fixture and
cvars, live pass/edge counts, camera color/depth names, ShadowMap writer and
reader names, output reachability, dead-pass candidates, resource validation,
and any diagnostics. Keep the active and disabled shadow-map controls separate.

## Additional Metal validation — wall-fan batch correction (2026-10-05)

The Linux source audit found that adjacent Metal wall fans were flushing the
pending triangle batch. `HWWall::RenderWall()` submits `DT_TriangleFan`, but the
Metal batch converts fan/list/strip inputs to `DT_Triangles` and stores that
normalized type in `mPendingBatch.dt`. The `Draw()` and `Apply()` checks then
compared the stored normalized type against the unnormalized input type. The
source fix adds `GetBatchDrawType()` and normalizes both comparisons. Line and
point primitives remain distinct, and all other batch and sub-draw state keys
are unchanged.

The fix is in the Linux working tree at
`src/common/rendering/metal/renderer/mt_renderstate.cpp` and is uncommitted.
The existing `e4d837701` revision alone does not contain it; ensure both the
helper and both comparison changes are present in the Mac checkout before
building. A Linux build passed, but its CMake configuration excludes
`METAL_SOURCES`, so this is not a Metal compile or runtime result.

### Build and live fixture

Build the Intel Metal configuration using the command above. Use a view with
many adjacent opaque wall fans and repeated materials; Ashes Hard Reset MAP01
(`Night School`) is preferred if the same package is installed on the Mac. Its
Linux capture had 403 plain walls, 135 masked walls, and a portal subview. Keep
the camera fixture's first-frame capture separate because its camera producer
is one-shot.

For the runtime check, add temporary counters around `MtRenderState::Draw()`,
`UpdateSubDrawState()`, and `FlushBatch()` and remove them afterward. Record
source primitive types, how many source draws enter each pending batch, total
indices, sub-draw count, and flush reason. Distinguish normalized primitive
type, `mNeedApply`, each batch-key field (pipeline, vertex buffer/offsets,
cull, material, translation, clamp, override shader), the 256-sub-draw limit,
and index-buffer capacity. For sub-draws, split the full `PushConstants`
memcmp from matrix-buffer and stream-buffer identity/offset changes.

Expected result: consecutive `DT_TriangleFan` wall calls with a stable
batch-level key accumulate into the same pending batch; changes to push
constants or matrix/stream offsets create sub-draws without flushing that
whole batch. A different batch-level key or non-batchable primitive still
flushes. Confirm the emitted geometry is visually correct with a matched
capture and that graph/resource diagnostics remain clean. Do not make a Metal
performance claim until the native run and an uninstrumented timing comparison
are recorded.

Record the Mac model/GPU, exact fixture and launch settings, whether the source
patch was present, batch starts/flushes and reasons, source draws and indices
per batch, sub-draw counts and reasons, visual comparison, build result, and
any diagnostics. Remove all temporary counters before timing or handoff.
