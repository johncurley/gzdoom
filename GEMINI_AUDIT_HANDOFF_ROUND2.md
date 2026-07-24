# Handoff: round 2 — review of the round-1 audit's aftermath

Thanks for the round-1 audit (`FINDINGS.md`). This round is different in
kind: round 1 was a blind independent read: this round is an explicit
**peer review of what happened to your findings**, plus finishing the ones
you left unconfirmed, plus a fresh look at the fixes that landed. You now
*should* read `FINDINGS.md` and the "Independent Gemini audit (2026-07-22)"
section of `AGENTS.md` first — full context, not withheld this time.

## What happened since round 1

Before acting on any of round 1's findings, they were checked directly
against the code (not by you — by the other assistant working this repo,
Claude). Result: **6 of your 13 numbered findings were wrong**, including
both CRITICALs and one of the two HIGHs:

- Finding 1 (`RenderPassDescriptor` leak): claimed wrong — argued
  `renderPassDescriptor()` is a Cocoa convenience factory method returning
  an autoreleased object the caller doesn't own, cited `mt_commandbuffer.cpp:33`
  (an explicit `retain()` with a comment explaining why it's needed there)
  and the per-frame `NS::AutoreleasePool` in `mt_renderdevice.cpp` (create
  at 310, `release()` at 382) as evidence the leak doesn't exist.
- Finding 2 (`WaitForCommands` no-op): claimed wrong — argued Metal
  guarantees same-queue command buffers execute/complete in commit order, so
  waiting on a later dummy buffer does transitively wait for earlier work.
- Finding 3 (`PatchFragmentShader` dead code): claimed wrong — found a real
  call site (`mt_shader.cpp:931`, `MtShaderManager::LoadFragShader`, added
  by commit `728c775de`) that a thorough grep should have caught. Also
  claims your own comparison against `AGENTS.md` leaned on a note there that
  was itself stale (predated that commit).
- Finding 8 (`ClearScreen` scissor gap, claimed Vulkan-only): claimed wrong —
  argued Vulkan's `VkRenderState::ClearScreen` (`vk_renderstate.cpp:50`) has
  the identical structure, no reset either.
- Finding 9 (`applyExponent` dead field): claimed wrong — cited a read site
  (`mt_ao.metal:692`) and write site (`mt_ao.cpp:1676`).
- Finding 10 (inflight semaphore never waited on): claimed wrong — cited
  `dispatch_semaphore_wait(mInflightFramesSemaphore, ...)` in
  `MetalRenderDevice::BeginFrame()` (`mt_renderdevice.cpp:490`/`498`).
- Finding 5 was called invalidated as a consequence of Finding 2.

Findings 6 (missing normal-attribute detection) and 7 (dead
`mPassDescriptor` member) were accepted as correct and fixed. Finding 4
(ShadowMap null deref) was accepted as a real inconsistency but downgraded
from HIGH (claimed unreachable given `ShadowMap`'s current allocation
lifecycle) and fixed anyway as cheap insurance. Findings 11 and 12 were
accepted as accurate observations (11 real but left unfixed as
feature-sized; 12 an honest hedge, not a demonstrated bug). Finding 13 and
the four Info notes were explicitly left as **not independently verified**.

**Your job this round:**

1. **Push back if any of the six "FALSE" verdicts above are themselves
   wrong.** Don't defer to Claude's reasoning just because it's detailed and
   cited line numbers — re-derive each from the code yourself. In
   particular the Objective-C autorelease-ownership argument for Finding 1
   and the command-queue-ordering argument for Finding 2 are the kind of
   claims worth being skeptical of; if you find a hole in either, say so
   explicitly and back it with your own citation.
2. **Finish Finding 13** (`mt_renderstate.cpp:595-598`, `Clear()` /
   `EndRenderPass()` interaction) — trace the actual control flow this time
   rather than leaving it as a plausible-sounding observation.
3. **Finish the four Info notes** the same way — confirm or refute each
   with a citation.
4. **Review the fixes that landed** for Findings 4, 6, 7 — check the diffs
   are correct, not just that they exist:
   - `src/common/rendering/metal/system/mt_hwbuffer.h` /`.cpp` — new
     `HasNormal()`/`mHasNormal`, mirrors the existing `HasColor()` pattern.
   - `src/common/rendering/metal/renderer/mt_renderstate.cpp` —
     `ApplyStreamData()` now ORs bit 1 from `HasNormal()` into
     `mStreamData.useVertexData`; also the new null guard around
     `ShadowMap->GetTexture()` in the color-clear path.
   - `src/common/rendering/metal/renderer/mt_renderstate.h`/`.cpp` — removed
     dead `mPassDescriptor` member and its destructor guard.
5. **If you have capacity beyond that**: the original round-1 scope
   (`src/common/rendering/metal/`, ~14k lines, vs. `src/common/rendering/vulkan/`
   for parity) is still only partially covered — pick up new territory you
   didn't get to rather than re-treading what's already been through two
   passes.

## Reporting format

Same as round 1: file:line, category, severity, concrete failure scenario,
suggested fix direction. For items 1-4 above specifically, lead with a
verdict (agree / disagree, with reasoning) rather than a fresh finding
write-up.
