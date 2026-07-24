# Independent Audit Findings

**Auditor:** Gemini  
**Date:** 2026-07-22  
**Scope:** `src/common/rendering/metal/` — system, renderer, shaders, textures  
**Reference:** `src/common/rendering/vulkan/` (parity comparison)  
**Shader parity tool output:** All 18 kernels (10 AO + 8 bloom) MATCH between `.metal` and `.cpp` fallback.

> **Verification pass (Claude, 2026-07-22):** checked every finding below against
> the actual code before anything was acted on. 6 of 13 numbered findings were
> wrong — concentrated in the highest-severity claims (both CRITICALs, one of
> two HIGHs) — full reasoning and citations in `AGENTS.md` under "Independent
> Gemini audit (2026-07-22)". Verification status is noted inline per finding
> below (`CONFIRMED — fixed`, `CONFIRMED — not fixed`, `FALSE`, or
> `NOT INDEPENDENTLY VERIFIED`). Treat anything not marked CONFIRMED/FALSE as
> unverified, not as an accepted finding.
>
> **Round 2 (Gemini, same day):** asked to peer-review the six FALSE
> verdicts above rather than defer to them, and finish what was left
> unverified. Independently agreed with all six FALSE verdicts. Finding 13
> confirmed real but near-zero-impact (a `!filled` safety net plus
> accidental correctness at the next frame's clear absorb it). All four
> Info notes confirmed with fresh citations. Fixes for 4/6/7 reviewed and
> confirmed correct. Extended into `mt_compute.cpp`, `mt_pipelinestate.cpp`,
> `mt_streambuffer.cpp` — no additional bugs found. Audit thread closed;
> see `GEMINI_AUDIT_HANDOFF_ROUND2.md` and `AGENTS.md`.

---

## Summary

18 findings: 2 critical, 2 high, 5 medium, 5 low, 4 info. The backend architecture (manager pattern, lazy state machine, compute AO/bloom) is sound, but there are concrete resource leaks, a GPU-sync no-op, dead code with important shader patches, and several null-safety gaps.

---

## Finding 1 — CRITICAL: `MTL::RenderPassDescriptor` leaked every `BeginRenderPass`

**Status: FALSE.** `renderPassDescriptor()` is a Cocoa convenience factory
method — returns an autoreleased object the caller doesn't own. This
codebase already retains explicitly when it needs to (`mt_commandbuffer.cpp:33`)
and wraps every frame in an `NS::AutoreleasePool` (`mt_renderdevice.cpp:310/382`),
so `pRPD` is already correctly cleaned up once per frame. The suggested fix
(`pRPD->release()`) would have been a genuine over-release/use-after-free.
Not applied.

**File:** `mt_renderstate.cpp:1387`  
**Category:** correctness / resource leak

`BeginRenderPass()` allocates `pRPD` via `renderPassDescriptor()` (+1 retained) but never calls `pRPD->release()`. At 60fps × multiple render passes, this leaks ~43-108 MB/hour.

**Fix:** Add `pRPD->release()` before function return.

---

## Finding 2 — CRITICAL: `WaitForCommands(true)` doesn't wait

**Status: FALSE.** "Commit an empty dummy buffer after the real work, wait
on the dummy" is a correct Metal idiom — command buffers on the same queue
execute and complete in commit order, so waiting on the later dummy buffer
transitively waits for the real work committed before it. Not applied.

**File:** `mt_commandbuffer.cpp:74-86`  
**Category:** correctness / GPU sync

```
finish=true: Flush (commit, NO wait) → create empty buffer → commit & wait on empty
```

The actual work's command buffer is committed without waiting; the wait is on a worthless empty buffer. Callers expecting real GPU sync (texture re-upload at `mt_texture.cpp:325`, stream-buffer stall at `mt_renderstate.cpp:1206`) get none.

**Fix:** `if (finish) FlushCommands(true);` — remove the dummy-buffer pattern.

---

## Finding 3 — HIGH: `PatchFragmentShader` never called (dead code, shadows broken)

**Status: FALSE.** `MtShaderManager::LoadFragShader` calls it directly
(`mt_shader.cpp:931`), added by commit `728c775de`. The AGENTS.md line cited
as corroboration was itself stale (predated that commit) — corrected there.
Not applied.

**File:** `mt_shader.cpp:776-827`  
**Category:** correctness / parity-gap

`static PatchFragmentShader()` contains critical fragment-shader patches for Reverse-Z shadow maps (far-plane sentinel `1e20`), linear-depth inversion, and SSAO orientation — but it's never called. Zero call sites in the entire codebase.

**Failure scenario:** Shadow maps clear to `1.0` instead of `1e20` (Reverse-Z far plane), causing incorrect shadow testing at distance. Linear-depth reconstruction is not inverted. PP-fallback SSAO has wrong coordinate orientation.

**Fix:** Call `PatchFragmentShader()` from the same pipeline as `PatchVertexShader()`.

---

## Finding 4 — HIGH: ShadowMap null dereference in color clear

**Status: CONFIRMED — fixed, but downgrade from HIGH.** The missing guard is
real, but `ShadowMap` is allocated synchronously and unconditionally in
`CreateScene()` before any `BeginRenderPass()` can run, and never reset
afterward — not a reachable crash in the current code. Fixed anyway (cheap,
matches the existing guarded pattern at line ~1474) as insurance against a
future lifecycle change.

**File:** `mt_renderstate.cpp:1433`  
**Category:** correctness / crash

```cpp
fb->GetBuffers()->ShadowMap->GetTexture()  // nullptr crash when ShadowMap unallocated
```

No null check before dereference — inconsistent with the guarded check at lines 1472-1476.

**Fix:** `if (auto* sm = fb->GetBuffers()->ShadowMap.get(); sm && targetTex == sm->GetTexture())`

---

## Finding 5 — MEDIUM: Texture re-upload races due to Finding 2

**Status: INVALIDATED.** Built entirely on Finding 2, which is false — the
sync this relies on does actually happen. Not applied.

**File:** `mt_texture.cpp:321-325`  
**Category:** correctness / race condition

Calls `WaitForCommands(true)` before `replaceRegion()`, but due to Findng 2 this is a no-op. GPU may still read the old texture content during `replaceRegion`, producing undefined content.

**Fix:** Fix Finding 2, or switch to blit-encoder path for re-uploads.

---

## Finding 6 — MEDIUM: Missing normal-attribute detection

**Status: CONFIRMED — fixed, and likely underrated.** Verified Vulkan's bit
scheme exactly (`vk_renderpass.cpp:122-128`). Fixed: added
`HasNormal()`/`mHasNormal` to `MtVertexBuffer` (`mt_hwbuffer.h`/`.cpp`),
`ApplyStreamData()` now sets bit 1 from it. This plausibly caused visibly
wrong lighting on any geometry with per-vertex normals, not just a
structural gap — not yet visually re-verified in-game.

**File:** `mt_renderstate.cpp:875-879`  
**Category:** parity-gap

`ApplyStreamData()` only checks `HasColor()`, not `HasNormal()`. Every Metal draw takes the uniform-fallback normal path. Vulkan sets `VATTR_NORMAL` in `vk_renderpass.cpp:127`.

**Fix:** Add `HasNormal()` to `MtVertexBuffer` and detect it in `ApplyStreamData()`.

---

## Finding 7 — MEDIUM: `mPassDescriptor` (h:107) is dead member

**Status: CONFIRMED — fixed.** Removed the member and the now-empty
destructor guard.

**File:** `mt_renderstate.h:107`, `mt_renderstate.cpp:67-71`  
**Category:** architecture

Member declared and destructor tries to release it, but `BeginRenderPass()` allocates a local `pRPD` instead — `mPassDescriptor` is never assigned.

**Fix:** Remove the member or make `BeginRenderPass()` use it.

---

## Finding 8 — MEDIUM: `ClearScreen()` missing scissor/viewport reset

**Status: FALSE.** Vulkan's `VkRenderState::ClearScreen` (`vk_renderstate.cpp:50`)
has the identical structure — no explicit scissor/viewport reset either. No
Metal-specific asymmetry as claimed. Not applied.

**File:** `mt_renderstate.cpp:73-87`  
**Category:** correctness

`ClearScreen()` calls `Apply(DT_TriangleStrip)` which applies the *current* viewport/scissor — potentially a smaller rect leftover from previous scissored draws. Vulkan's equivalent resets scissor to full viewport before the clear.

**Failure scenario:** Stale scissor rect from a previous draw restricts the clear to a sub-region, leaving the rest of the screen uncleared. Most visible during loading screen transitions or resolution changes.

**Fix:** Call `SetScissor(0, 0, -1, -1)` and `SetViewport(0, 0, -1, -1)` at the start of `ClearScreen()`.

---

## Finding 9 — LOW: `AOBlurParams.applyExponent` dead field

**Status: FALSE.** Written in `mt_ao.cpp:1676`, read in both `mt_ao.metal:692`
and the `.cpp` fallback at 729. Not dead. Not applied.

**File:** `mt_ao.cpp:79-90`, `mt_ao.metal:45-56`  
**Category:** architecture

Declared in both CPU and GPU structs, never read or written by any code path. Pure struct bloat.

---

## Finding 10 — LOW: `Dispatch_semaphore` created, signaled, never waited on

**Status: FALSE.** `dispatch_semaphore_wait(mInflightFramesSemaphore, ...)`
runs every frame in `MetalRenderDevice::BeginFrame()` (`mt_renderdevice.cpp:490`
and 498) — real, functioning frame-throttling. Not applied.

**File:** `mt_renderdevice.cpp:104, mt_commandbuffer.cpp:96-103`  
**Category:** architecture

`mInflightFramesSemaphore` is created with count = `maxDrawableCount` and signaled in EndFrame's completion handler, but never consumed via `dispatch_semaphore_wait()`. Actual frame throttling comes from `nextDrawable()` drawable-pool exhaustion. The semaphore maintenance is harmless but wasteful.

---

## Finding 11 — LOW: `EnableMultisampling` and `EnableLineSmooth` are no-ops

**Status: CONFIRMED — not fixed.** Both function bodies are genuinely empty.
Real parity gap, but implementing actual MSAA/line-smoothing state is a
feature-sized task, not a quick correctness fix — left as a documented gap
rather than attempted here.

**File:** `mt_renderstate.cpp:628-630`  
**Category:** parity-gap

```cpp
void MtRenderState::EnableMultisampling(bool on) {}
void MtRenderState::EnableLineSmooth(bool on) {}
```

These should configure sample count and line-smoothing state. Vulkan equivalents are not no-ops. Multisampling is partially handled elsewhere via `mRenderTarget.Samples`, but `EnableLineSmooth` is completely unimplemented.

---

## Finding 12 — LOW: `ApplyDepthBias` z-bias scale factor is arbitrary

**Status: CONFIRMED accurate as an observation, not a demonstrated bug.**
Citation checks out exactly. Gemini's own hedge ("if it works empirically
it's fine") stands — left alone.

**File:** `mt_renderstate.cpp:835-837`  
**Category:** correctness

```cpp
float unitsScale = 1.0f / 16777216.0f;
mCurrentBiasUnits = -mBias.mUnits * unitsScale * 128.0f;
```

The `128.0f` magic multiplier for "Reverse-Z precision" has no documented derivation or justification. Correctness is scene-dependent; different engines use `2 * maxSlope` or configurable constants. If it works for this codebase's content, it's fine empirically but fragile.

---

## Finding 13 — LOW: `Clear()` sets `mClearTargets` but `EndRenderPass()` drops them silently

**Status: NOT INDEPENDENTLY VERIFIED.** Code location citation checks out
(`mt_renderstate.cpp:595-598` is exactly `Clear()`), but the described
edge-case control-flow scenario wasn't traced through. Treat as unconfirmed.

**File:** `mt_renderstate.cpp:595-598`  
**Category:** correctness

```cpp
void MtRenderState::Clear(int targets) {
  mClearTargets = targets;
  EndRenderPass();
}
```

If `EndRenderPass()` succeeds (ends the encoding), but `BeginRenderPass()` is not called before the next `Apply()` (e.g., if the encoder stays ended), the `mClearTargets` value is consumed by the next `BeginRenderPass()` which clears it at line 1603. If `BeginRenderPass()` never runs again (e.g., render-to-texture with no new pass), the clear is lost. The `mFilledTargets` tracking continues to think textures are filled.

**Failure scenario:** Infrequent — only during edge-case pass sequences — but would cause stale framebuffer content in intermediate render targets.

---

## Finding 14 — LOW: `RecycleTexture` not declared in `mt_renderdevice.h` (compiler warning)

**File:** `mt_renderdevice.h vs .cpp`  
**Category:** architecture

`mt_renderdevice.cpp:786` defines `MetalRenderDevice::RecycleTexture()` but the header declaration at line 155 was found on re-check. WITHDRAWN — declaration exists.

---

## Info / Observations

**Status: NOT INDEPENDENTLY VERIFIED** (items 1-4 below).

1. **SSAOParams struct layout** matches byte-for-byte between CPU (`mt_ao.h:10-36`) and GPU (`mt_ao.metal:12-38`). AOBlurParams and AOFullresParams also match. Good hygiene.

2. **ClearTarget consumption path** is unusual: `ApplyRenderPass()` checks `mClearTargets` and calls `EndRenderPass()` (line 721-724), but `mClearTargets` is only consumed to zero at line 1603 inside `BeginRenderPass()`. Between these two calls, another `ApplyRenderPass()` could see the same pending clear flags again.

3. **`mFrameCount < 100` heuristic** (mt_texture.cpp:317) for disabling mipmaps during early frames is a reasonable startup optimization but means the first ~1.7s at 60fps of gameplay has no mipmaps on newly loaded textures.

4. **Shader parity script (`check_shader_parity.py`)** passes cleanly — all 10 AO and 8 bloom kernels match between `.metal` and `.cpp` fallback. This was a known drift risk that is currently under control.

---

## Comparison with Claude's AGENTS.md findings

*The following is noted after completing the independent audit above:*

After reading AGENTS.md, these of my findings are **new / not documented there**:

- **Finding 1** (RenderPassDescriptor leak) — not mentioned.
- **Finding 2** (WaitForCommands no-op) — not mentioned. The `streambuffer` stall recording is mentioned but the root cause (sync is a no-op) isn't.
- **Finding 3** (PatchFragmentShader dead code) — AGENTS.md notes `PatchFragmentShader` as dead code (line "Dead code: `PatchFragmentShader` (lines 760-827) defined but never called"), and my finding concurs. Correctly identified by both.
- **Finding 4** (ShadowMap null deref) — not mentioned.
- **Finding 8** (ClearScreen scissor reset) — not mentioned.
- **Finding 11** (EnableMultisampling/EnableLineSmooth no-ops) — not mentioned.
- **Finding 12** (ApplyDepthBias arbitrary scale) — not mentioned.

Findings 5, 6, 7, 9, 10, 13 are either partially noted or are lower-severity observations that AGENTS.md doesn't separately list.

The two audits are broadly complementary — Claude's work is heavily focused on the compute AO/bloom performance investigation (real GPU timing, algorithm selection, Intel bottlenecks, texture upload instrumentation), while this audit examines structural/correctness issues in the render state machine, command buffer, and shader compilation pipeline. Neither duplicates the other's territory significantly.
