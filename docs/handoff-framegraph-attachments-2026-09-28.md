# Session handoff — attachment access and Linux conditional graph checks

Date: 2026-09-28  
Branch: `framegraph-output-lifetimes`  
Latest source commit: `41819e446`  
Latest documentation commit before this handoff: `4abd53f7d`

## What changed

The framegraph had marked `scene.target`, `scene.opaque`, and all five SSAO
passes as dead-pass candidates even though those passes rendered and visibly
contributed to the frame. The graph did not record attachment reads: depth
testing consumes the existing scene depth, scene translucency blends with the
existing scene color, and SSAO composites into the existing `SceneColor`.
Blended postprocess outputs also consume their prior destination contents.

The graph declarations now mark those scene and postprocess attachments as
read/write across GL, Vulkan, and Metal. Scene `Exposure.Camera` is recorded as
an external previous-frame input for its blended update. The graph remains
diagnostic: this change does not reorder rendering, cull passes, add GPU
synchronization, or alter rendered output.

Code commit: `41819e446 framegraph: model attachment read-write access`  
Conditional-results documentation commit: `4abd53f7d docs: record conditional framegraph captures`

## Linux verification

Machine: RX 550, Wayland, Freedoom 0.12.1, MAP01. The full build passed with
`cmake --build build -j$(nproc)`. Real frames were rendered; `r_framegraph`
and `r_resources` were issued after 240 and 250 frames, then the process quit
at frame 260. These were not Xvfb `+quit` smoke checks.

The all-effects-enabled capture changed the initial seven candidates per
backend to none. It reported 46 passes / 52 edges on GL and 47 / 53 on
Vulkan, with no graph build or observed-use errors. The first GL run exposed a
missing `Exposure.Camera` external declaration; that was added before the
final captures.

Conditional configurations also produced no candidates or graph/use errors:

| Configuration | GL | Vulkan |
| --- | ---: | ---: |
| SSAO, bloom, tonemap, lens, FXAA all disabled | 4 passes / 4 edges | 5 / 5 |
| SSAO 3 only | 9 / 13 | 10 / 14 |
| Bloom + tonemap + FXAA; SSAO and lens disabled | 40 / 42 | 41 / 43 |

Vulkan has one additional pass and edge in each configuration for its explicit
scene resolve. The six conditional logs and two final all-effects logs are on
the Linux machine under `/tmp/gzdoom-framegraph-*-{effects-off,ssao-only,no-ssao,attachment-fix}.log`;
they are not repository artifacts.

## Metal handoff — next action

Metal was updated in source but is not built on Linux. On the available Metal
machine, build this branch and run a real MAP01 frame with SSAO, bloom,
tonemap, lens, and FXAA enabled. Then issue `r_framegraph` and `r_resources`.
Check that:

- `scene.opaque` reports read/write scene depth;
- `ssao.combine` reads/writes `SceneColor`, and the portal/translucent scene
  pass reads/writes scene color and depth;
- blended postprocess outputs report read/write, including the persistent
  `Exposure.Camera` update;
- `Backbuffer` reaches the present chain;
- `Build()` has no errors and the enabled-effects graph has no dead-pass
  candidates.

If the enabled-effects capture is clean, repeat at least the all-effects-off
and SSAO-only configurations. Record the actual Metal build/runtime result and
any candidate list here or in the next dated handoff. This checks graph
correctness; it is not an Apple Silicon performance or TBDR-policy result.

## After Metal validation

The next renderer implementation step is to specify RAW, WAR, and WAW
dependencies and backend synchronization for the bounded `Pass2` chain in
`docs/frame-analysis.md` §4. Only after that contract is reviewed should the
graph begin driving execution. Keep scheduling, resource aliasing, and pass
culling out of scope until captured output matches the existing path across
backends and resize/effect-toggle cases.

Related roadmap and existing evidence: [`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md),
[`handoff-linux-2026-09-24.md`](handoff-linux-2026-09-24.md), and
[`renderer-methodology.md`](renderer-methodology.md).
