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

## Linux work, in order

The Linux GL/Vulkan build and runtime tranche recorded in
[`handoff-linux-2026-09-24.md`](handoff-linux-2026-09-24.md) remains useful
baseline evidence: both backends ran on the RX 550, their prior live graph and
resource checks passed, and the observer A/B showed no measurable overhead in
that MAP06 route. Those runs predate this attachment-preservation amendment;
they do not validate the changed dependency model.

1. Check out the commit containing this handoff and read `CONTRIBUTING.md`,
   `AGENTS.md`, and [`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md).
2. Build with Vulkan enabled and run real MAP06 frames on the RX 550 using GL
   and Vulkan separately. `+quit`/Xvfb self-tests alone do not exercise these
   live scene dependencies.
3. Run `r_framegraph_selftest`, then capture `r_framegraph` and
   `r_resources` after rendering begins with the effects enabled and disabled.
   Confirm the output-rooted scene/AO/translucency/present chain, material
   reads, and blended postprocess dependencies; require no unexplained dead
   candidates, graph build errors, or stale-size reports.
4. Confirm backend resource touches still agree with graph reads and writes.
   Attachment prior-value reads are logical data dependencies; the backend
   observer should continue to report the actual attachment binding as a
   write. Check for GL/Vulkan validation or driver errors.
5. If the exercised route reaches relevant conditional paths, record them.
   Keep offscreen-only scene traversals and paths not reached by the capture
   listed as open rather than inferring coverage.

The 2026-09-24 observer-cost A/B does not need to be repeated unless these
changes produce a measurable regression or the Linux session changes the
observer path. The existing Vulkan indexed non-mip sampled-image barrier
coverage boundary also remains open until a confirmed indexed draw reaches
that path; see the 2026-09-24 handoff for the failed route probes.

## Later Metal testing

The current Intel Metal pass is complete for the tested effects-on/off cases.
Further Metal validation remains expected as coverage grows: conditional paths
on Intel, Apple Silicon runtime behavior, and platform-specific graph behavior
should each be recorded when hardware and a reproducible route are available.
Keep Metal execution correctness-first until Apple Silicon has a measured
baseline; this diagnostic work makes no TBDR scheduling or performance claim.
