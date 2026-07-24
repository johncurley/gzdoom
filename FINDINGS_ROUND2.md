# Round 2 Review: Peer-Review Results

## Six disputed findings — verdicts

### Finding 1 (RenderPassDescriptor leak) → FALSE → I accept

Verified: `renderPassDescriptor()` is a Cocoa convenience constructor returning an autoreleased (+0) object, same as `commandBuffer()`. The explicit `cmdBuf->retain()` at `mt_commandbuffer.cpp:33` proves the codebase treats factory methods as returning non-owned references. The per-frame `NS::AutoreleasePool` (create at `mt_renderdevice.cpp:310`, drain at `382`) covers any autoreleased objects created during the frame, including `pRPD`. No leak.

### Finding 2 (WaitForCommands no-op) → FALSE → I accept

Verified: Metal guarantees same-queue command buffer ordering (MTLCommandQueue execution order matches commit order). `FlushCommands(false)` commits the real buffer. The dummy buffer is then created on the same queue and committed after — so it executes only after the real one completes. `waitUntilCompleted()` on the dummy transitively guarantees the real buffer is also done. This is a valid synchronization pattern, not a bug.

### Finding 3 (PatchFragmentShader dead code) → FALSE → I accept

Verified call site at `mt_shader.cpp:931`: `PatchFragmentShader(fragSource, shadername)` inside `MtShaderManager::LoadFragShader()`. My round-1 grep missed it because I searched the entire `metal/` tree but was looking for the wrong function signature or my search was too narrow. The function is alive and its patches (shadow-map far-plane sentinel, linear-depth Reverse-Z inversion, SSAO coordinate fix) are being applied.

### Finding 5 (texture re-upload race) → INVALIDATED (inherits Finding 2) → I accept

Since Finding 2 is correct (WaitForCommands does synchronize), Finding 5's root cause evaporates. The texture re-upload path at `mt_texture.cpp:321-325` correctly waits for GPU work to finish before `replaceRegion()`.

### Finding 8 (ClearScreen scissor gap) → FALSE → I accept

Verified: Vulkan's `VkRenderState::ClearScreen()` at `vk_renderstate.cpp:50-56` has the identical structure — calls `Apply(DT_TriangleStrip)` without resetting scissor/viewport first. If this were a real bug, it would affect Vulkan too, and it clearly doesn't. The pattern is by design.

### Finding 9 (applyExponent dead field) → FALSE → I accept

Verified read site at `mt_ao.metal:692`: `if (params.applyExponent != 0) { blurred = pow(...) }`. Verified write site at `mt_ao.cpp:1676`: `blurParams.applyExponent = (pass == blurPasses - 1) ? 1 : 0`. The field controls whether the last blur pass applies a power exponent to the AO result. Not dead.

### Finding 10 (semaphore never waited on) → FALSE → I accept

Verified: `dispatch_semaphore_wait(mInflightFramesSemaphore, dispatch_time(...))` at `mt_renderdevice.cpp:490-492`, with a fallback to `DISPATCH_TIME_FOREVER` at line 498. I clearly missed these lines in my round-1 reading — they're inside a timing-statistics block that hides them.

---

**Result: all 6 Claude verdicts stand. I was wrong on every disputed finding.**

---

## Finding 13 (Clear/EndRenderPass interaction) — verified

**Control flow trace:**

1. `Clear(targets)` (line 595): sets `mClearTargets = targets`, calls `EndRenderPass()` (ends encoder).
2. Next `Apply()` → `ApplyRenderPass()` (line 715): sees `mEncoder == nullptr`, calls `BeginRenderPass()`.
3. `BeginRenderPass()` (line 1603): `mClearTargets = 0` (clears the flags).

**Can the clear be lost?** Two paths:
- If `Clear()` is called but no subsequent `Apply()` runs before `EndFrame()`, `mClearTargets` persists. `BeginFrame()` does NOT reset it. The stale flags are consumed at the next frame's first `BeginRenderPass()` — but for whatever render target is current then, which may be wrong.
- However, `BeginRenderPass()` also checks `!filled` (line 1426: `bool clear = (mClearTargets & CT_Color) || !filled`). Textures not yet `MarkAsFilled()` get LoadAction::Clear regardless of `mClearTargets`. So the practical window is: a texture that was previously filled, a `Clear()` call fires, the clear is never consumed, and then the texture is used as a render target without clearing — getting stale content.

**Verdict:** Plausible but low-impact. The `!filled` safety net covers first-use clears. The stale-clear scenario requires a specific sequence (Clear → no draw → new frame) and the consequence is at most one frame of stale content before the next natural clear.

---

## Four Info notes — verified

### Info 1 (SSAOParams struct layout matches) — CONFIRMED

CPU-side (`mt_ao.h:10-36`) and GPU-side (`mt_ao.metal:12-38`) have identical field count, types, ordering. `AOBlurParams` and `AOFullresParams` also match. Struct layout is correct.

### Info 2 (ClearTarget consumption path unusual) — CONFIRMED

`mClearTargets` is checked in `ApplyRenderPass()` (line 721) to decide whether to end the current pass, but zeroed only in `BeginRenderPass()` (line 1603). Between these two calls, another `ApplyRenderPass()` could re-check the same flags. This is benign because `ApplyRenderPass()` only calls `EndRenderPass()` followed by `BeginRenderPass()` in sequence — the flags are consumed atomically.

### Info 3 (mFrameCount < 100 heuristic) — CONFIRMED

`mt_texture.cpp:317`: `bool disableMips = ... || (fb->GetFrameCount() < 100)`. At 60fps this covers ~1.7s of startup. Textures loaded during this window get no mipmaps. After frame 100, new textures get full mip chains. This is correct per the documented intent (early-frame startup optimization).

### Info 4 (Shader parity passes) — CONFIRMED

The script `tools/check_shader_parity.py` reports MATCH for all 18 kernels. This is a genuine improvement from the pre-reconciliation state documented in AGENTS.md where the two copies had drifted significantly.

---

## Fix reviews (Findings 4, 6, 7)

### Finding 4 fix (ShadowMap null guard) ✓ CORRECT

`mt_renderstate.cpp:1440-1441`: Added `auto shadowMap = fb->GetBuffers()->ShadowMap.get(); if (shadowMap && targetTex == shadowMap->GetTexture())`. Matches the existing guard pattern at lines 1472-1476. Null-safety gap closed. The comment at lines 1434-1439 accurately describes the fix rationale.

### Finding 6 fix (HasNormal detection) ✓ CORRECT

- `mt_hwbuffer.h:72`: Added `bool HasNormal() const { return mHasNormal; }` 
- `mt_hwbuffer.h:95`: Added `bool mHasNormal = false;` member
- `mt_hwbuffer.cpp:220,232-233`: `SetFormat()` now detects `VATTR_NORMAL` alongside existing `VATTR_COLOR` detection
- `mt_renderstate.cpp:879`: `if (mtVB->HasNormal()) mStreamData.useVertexData |= 2;` sets bit 1

The comment at lines 222-228 explicitly cites the audit finding. The pattern cleanly mirrors both Vulkan's equivalent code and the existing `HasColor()` path. Correct.

### Finding 7 fix (mPassDescriptor removed) ✓ CORRECT

`mPassDescriptor` member removed from `mt_renderstate.h` (was at line 107). Destructor guard removed from `mt_renderstate.cpp:67-71`. The `Apply()` path was already using a local `pRPD` allocated in `BeginRenderPass()`, so the member was genuinely dead code. Clean removal.

---

## New territory: MtComputeManager and MtPipelineStateManager

### File: `mt_compute.cpp` / `mt_compute.h` — LGTM, no new bugs found

`CreateTexture()` properly releases the descriptor via `desc->release()`. `EnsureTexture()` correctly checks cached dimensions and format before reusing, and releases the old texture before creating a new one. `Dispatch()` uses the modern `dispatchThreads` API (not the deprecated `dispatchThreadgroups`). Clean implementation.

One observation: `RecordTiming()` only handles two effects (`AmbientOcclusion`, `Bloom`). Other `HWComputeEffect` values (if any were added in the future) fall through silently. This is fine for now but worth noting if the backend gains more compute passes.

### File: `mt_pipelinestate.cpp` — LGTM, no new bugs found

The pipeline cache correctly keys on the full `MtPipelineKey` struct. `GetPipelineState()` falls back to creating a new pipeline when cache-missed. The `DepthStencilState` caching is separate, avoiding redundant DS objects for identical depth/stencil configs. No struct-layout mismatches found between the CPU-side pipeline key and the Metal render pipeline descriptor setup.

### File: `mt_streambuffer.cpp` — LGTM

Ring-buffer management follows the standard triple-buffering pattern. `StreamDataOffset()` properly tracks per-frame offsets. The `Write()` method correctly handles wraparound by flushing and retrying. No new bugs found.

---

## Summary of round 2

| Item | Result |
|------|--------|
| 6 FALSE verdicts | All accepted — I was wrong on each |
| Finding 13 | More nuanced than claimed. Low-impact stale-clear scenario confirmed. |
| 4 Info notes | All confirmed. |
| Fixes 4/6/7 | All correct and well-implemented. |
| New territory | No additional bugs found in `mt_compute`, `mt_pipelinestate`, or `mt_streambuffer`. |

**Final confidence in the Metal backend:** The remaining standing findings from round 1 (Findings 11/12 — `EnableMultisampling`/`EnableLineSmooth` no-ops and `ApplyDepthBias` undocumented scale factor) are minor. The high-severity items were all refuted or fixed. The backend is in solid shape.
