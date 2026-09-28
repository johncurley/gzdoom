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

## Corrections to the graph status

An audit of the current `hw_framegraph.{h,cpp}` and backend call sites found
that the following pieces are **not implemented**, despite appearing in a
local draft handoff:

- explicit declarations of the final outputs whose producers must be kept;
- resource lifetime intervals derived from complete pass uses;
- reporting of passes that are unreachable from required outputs;
- scene material texture reads attached to the scene passes that sample them.

The upload observer's resource reads are global facts used to validate upload
ordering. They do not form scene-pass dependencies. Likewise, the existing
read/write lists imply the present graph's outputs for current postprocess
passes; they do not constitute a final-output contract. These gaps prevent
reliable dead-pass analysis, lifetime conclusions, and safe allocation reuse.

## Remaining work, in order

1. **Finish the CPU graph contract.** Add explicit required outputs, derive
   resource first/last use only from complete declarations, report dead-pass
   candidates by walking dependencies back from required outputs, and attach
   scene material reads to the actual scene pass. Define how external,
   persistent, aliased, and conditionally active resources participate.
2. **Validate the diagnostic model on GL and Vulkan.** Exercise real frames
   with representative scene materials, postprocess enabled and disabled, and
   conditional paths. Confirm the output walk retains required producers,
   marks only genuinely unused passes, and computes stable lifetimes across
   ping-pong resources and aliases. Keep live rendering in backend order during
   this stage.
3. **Move one bounded chain to graph-driven execution.** Start with the
   `Pass2` chain identified in `docs/frame-analysis.md` §4. Specify RAW, WAR,
   and WAW handling and per-backend synchronization before changing execution.
   Compare captured output against the existing path and exercise resize,
   enabled/disabled effects, and ping-pong direction.
4. **Widen in measured steps.** Add bloom and exposure, then AO. Keep resource
   aliasing and pass culling disabled until output roots, all relevant reads,
   writes, and lifetimes have been proven on the migrated paths.
5. **Validate Metal policy on Apple Silicon.** CPU graph algorithms and
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
The `r_framegraph_selftest` and the Xvfb `+quit` smoke command are useful
structural checks, but neither substitutes for a live frame on each backend.
