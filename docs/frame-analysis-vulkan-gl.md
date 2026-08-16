# Frame analysis — Vulkan/GL — what the renderer's passes and resources actually are

Written 2026-08-16, companion to `docs/frame-analysis.md` (Metal-only). Task 6 of
`docs/handoff-linux-2026-08-16.md`: the frame graph that analysis feeds (Track A
items 3-4 of `docs/engine-modernization.md`) is meant to be backend-neutral, and
designing that interface from one backend produces an abstraction shaped like Metal
with holes where the others differ. This is the other two backends, read the same
way, so the two documents can sit side by side before the interface is designed.

Scope and method are identical to the Metal document: the postprocess chain and its
resources, extracted mechanically from `SetInput*`/`SetOutput*` calls and from
reading the code that actually runs, not from prose. Where a claim rests on
measurement rather than reading, it says so — `tools/matrix/crossbackend.py
--backends gl,vulkan` is available on this hardware (task 3, closed) and was used
where reading alone left a question open.

---

## 1. Resource inventory

**Tier 1 transfers with the names unchanged, but the representation does not.**

Vulkan (`vk_renderbuffers.h:29-40`) is the closest match to Metal — the same five
named members plus a pipeline depth/stencil, as public `VkTextureImage` handles:

| resource | notes |
|---|---|
| `SceneColor` | `VkTextureImage`, public member |
| `SceneDepthStencil` | depth + stencil |
| `SceneNormal` | G-buffer normals, format `VK_FORMAT_A2R10G10B10_UNORM_PACK32` |
| `SceneFog` | 8-bit, AO's composite input |
| `PipelineImage[2]` | the ping-pong pair, `NumPipelineImages = 2` |
| `PipelineDepthStencil` | present on Metal too (`mt_renderbuffers.h:63`); the Metal
  doc's table omitted it as out of scope, not because it's absent |

GL (`gl_renderbuffers.h:170-192`) is a **third representation**, not a second copy of
the same idea. Two structural differences, both from targeting old-style GL FBOs
rather than a texture-first API:

- **Every tier-1 target is private**, reached only through `Bind*` methods
  (`BindSceneColorTexture`, `BindSceneNormalTexture`, ...). Vulkan and Metal expose
  the textures as public members; GL exposes access, not the resource.
- **A texture/renderbuffer split per target** — `mSceneMultisampleTex` +
  `mSceneMultisampleBuf`, `mSceneDepthStencilTex` + `...Buf`, and so on for normal
  and fog. GL FBOs distinguish a sampleable texture attachment from a
  non-sampleable renderbuffer attachment (the latter used when multisampled, then
  resolved into the former), so what is one texture on Vulkan/Metal is a pair of
  objects on GL, selected by `mSceneUsesTextures`. `SceneColor` itself is not even
  named that on GL — it is `mSceneMultisampleTex`/`Buf`, bound via
  `BindSceneColorTexture()`.

**Tier 2 (per-effect textures owned by shared code) transfers unchanged, exactly as
predicted.** `hw_postprocess.h`'s `PPTexture : public PPResource` and its owning
effects (bloom, exposure, AO's `Ambient0`/`Ambient1`/`LinearDepthTexture`, tonemap's
`PaletteTexture`) are the same class, same file, same recreation path, for all three
backends. Nothing GL- or Vulkan-specific touches this tier.

**Tier 3 does not exist on Vulkan or GL. This is the headline finding of §1.**
`vk_postprocess.h` declares no per-module texture arrays of its own — its only
texture-shaped member is a forward-declared `VkPPTexture`, the same generic wrapper
tier 2 already uses. GL has no dedicated postprocess-resource class at all; its
postprocess logic runs directly against tier-1 (`gl_renderbuffers.h`) and tier-2
(`hw_postprocess.h`) resources with nothing private in between. Metal's fourteen
private textures (nine in `mt_ao.h`, five in `mt_bloom.h`) exist *because* of its
compute AO/bloom modules — see §3.2: neither backend has an alternate compute
implementation to hold intermediate state for, so neither has grown a third tier
to hold it in.

## 2. The pass chain

**Identical, and verifiably so rather than assumed identical**: `grep -c
"HAVE_METAL\|HAVE_VULKAN\|#ifdef\|#ifndef"` over `hw_postprocess.cpp` returns zero.
`Postprocess::Pass1`/`Pass2` is one function pair, one file, no backend
conditionals — every row of the Metal document's pass table (`exposure`, `bloom`,
`blur`, `tonemap`, `colormap`, `lens`, `fxaa`, `ssao`, custom, present) applies to
Vulkan and GL exactly as written, including the two structural observations (the
chain is implicit call order; AO is the outlier that reads G-buffer targets, writes
`SceneColor` directly, and runs mid-scene). This is the one part of the Metal
analysis that needed no companion work at all — reading it was enough to confirm it,
which is itself worth recording since prose and code have disagreed on this branch
before.

## 3. The couplings a frame graph would make explicit

### 3.1 A pass mutating global state that later passes depend on

**This is where the three backends genuinely diverge, and not along the line the
task's framing predicted.** The prediction was "three disciplines a graph must
accommodate" (Metal's shared main state, Vulkan's separate object, GL's
`FGLPostProcessState` bracket). Reading the code gives a sharper answer: the risk is
not about *how many objects* each backend uses, it is about **which value drives
shader-variant selection**, and only one of the three backends has it wired to
something a PP pass can corrupt.

**Metal is coupled by construction.** `MtPPRenderState::Draw()`
(`mt_postprocess.cpp:284`) calls `fb->GetRenderState()` — the *same* persistent
`MtRenderState` instance scene draws use — and issues its single-attachment PP
output through it. Pipeline selection reads `mRenderTarget.DrawBuffers`
(`mt_renderstate.cpp:769: pipelineKey.DrawBufferCount = mRenderTarget.DrawBuffers`),
a **raw, mutable field on that same shared object**. AO's PP draw sets it to 1 and,
pre-fix, nothing set it back — so the next scene draws read the stale value and
compiled against `NORMAL_PASS`. This is the documented bug (frame-analysis.md §3.1),
and the mechanism is now traced to its exact field, not just its symptom.

**Vulkan is not coupled, and not for the reason "a separate object" suggests.**
`VkPPRenderState::Draw()` (`vk_pprenderstate.cpp:53`) builds its **own**
`VkPPRenderPassKey` from purely local fields (`BlendMode`, `Shader`, `Output.Type`,
...), gets a dedicated render pass and framebuffer from `GetPPRenderPass(key)`, and
never reads or writes `mRenderTarget.DrawBuffers` — the field scene-draw pipeline
selection actually depends on (`vk_renderstate.cpp:517,538`, the identical field
name and mechanism as Metal's, confirming this is the shared architecture Metal's
bug lives in, not a Metal-only concept). The only thing `VkPPRenderState::Draw()`
does to the scene's render state is call `EndRenderPass()` before it starts — the
next scene draw begins its own pass again from the unchanged `mRenderTarget`, still
declaring 3 attachments. **`VkPostprocess::AmbientOccludeScene()`
(`vk_postprocess.cpp:244-252`) calls no restore step at all, and reading the code
says it needs none** — PP rendering is architecturally quarantined from the scene's
render-target tracking, not merely represented by a separate object that happens to
behave.

That reading was checked against a measurement rather than left as reasoning alone:
`crossbackend.py --backends gl,vulkan --scene doom2` (`doom2`/MAP06, the same run
that closed task 3) includes the `ssao` config precisely because it is the pass that
exercises this code path — mid-scene AO, then portals and translucents drawn with
whatever pipeline gets selected next. Result: `ssao OK, tone x1.00, uniform backend
noise (median band mean 0.123)`, no band outlier — Vulkan agrees with GL to the
tool's own noise floor on exactly the frames that would show a wrong-shader-variant
defect if one existed. That is one scene on one map, not an exhaustive proof (see §5).

**GL is not coupled either, but for a third reason again: its shader-selection value
and its GPU state are two different things.** `gl_renderstate.cpp:100` reads
`mPassType` directly for shader lookup — the same logical field
`hw_renderstate.h:701-709` (`GetPassDrawBufferCount`/`SetPassType`) exposes to every
backend, set once per eye/view in shared code (`hw_entrypoint.cpp:148`) and never
touched by a PP draw. `OpenGLFrameBuffer::AmbientOccludeScene()`
(`gl_framebuffer.cpp:446-456`) *does* call `EnableDrawBuffers(1)` before AO and
`EnableDrawBuffers(GetPassDrawBufferCount())` after — but that pair exists to keep
the **raw `glDrawBuffers` binding** correct (old-style GL FBOs are genuinely global,
sticky state, unlike Vulkan's per-pass objects), not to protect shader selection,
which was never at risk on GL in the first place. Removing that restore call would
plausibly break *where pixels land*, not *which shader compiles* — a different
failure mode than Metal's bug, wearing the same shape from a distance.

`FGLPostProcessState` (`gl_postprocessstate.h/.cpp`) is a **fourth, unrelated
mechanism** worth separating from the above: an RAII bracket that saves and restores
raw global GL state (blend, scissor, depth test, multisample, current program,
texture/sampler bindings). It is not used around `Pass1`/`Pass2`
(`FGLRenderer::PostProcessScene`, `gl_postprocess.cpp:56-65`, uses only
`GLPPRenderState renderstate(mBuffers)` — the same shape as Vulkan's
`VkPPRenderState renderstate(fb)` and Metal's `MtPPRenderState renderstate(fb)`, all
three constructed identically at their respective `PostProcessScene` entry points).
`FGLPostProcessState` brackets only backbuffer copy, stereo present, and two sites in
`gl_renderbuffers.cpp` — call sites that touch the GL context's global state directly
rather than going through the shared render-state abstraction at all. The Metal
document's own §5 caveat ("GL brackets with `FGLPostProcessState`") conflated this
with the Pass1/Pass2 discipline; it is a real GL mechanism, just not the one guarding
against §3.1's bug class.

**What this means for a graph:** the invariant worth declaring is not "PP passes may
mutate scene state, accommodate three styles of doing so" — it is "a PP pass's output
target selection must not be able to leak into what a later scene draw's pipeline
key reads." Vulkan enforces that by construction (separate objects, nothing shared).
GL enforces it by keeping the two values (`mPassType` vs raw `glDrawBuffers`)
independent. Metal did not, until the manual fix. A graph with declared
producer/consumer edges for "scene draw pipeline selection" as its own tracked value
— not folded into whatever a render-target struct happens to hold — makes Metal's
bug class structurally unreachable rather than merely fixed once.

### 3.2 Alternate implementations of a node

**Answered definitively, not just for AO/bloom: neither Vulkan nor GL has any
compute-shader path in the renderer at all.** `grep -rln` for `mt_compute_ao`,
`mt_compute_bloom`, `useComputeAO`, `useComputeBloom` over both backends' source
returns nothing, and neither `vkCmdDispatch` nor `glDispatchCompute` appears
anywhere in `src/common/rendering/vulkan/` or `src/common/rendering/gl/`. GL cannot
— GL 4.1 on macOS has no compute shaders, and while the Linux GL here is 4.6, nothing
uses the capability (`CLAUDE.md` already recorded this). Vulkan *could* — the API
supports it — but nothing in this tree does.

So "a node may have direct-compute / raster / disabled implementations" is confirmed
a **Metal-only requirement today**, exactly as the task specified checking. A graph
interface should still model capability selection as a first-class concept — Metal
needs it now, and Vulkan could grow a compute path later with nothing structural
stopping it — but GL and Vulkan supply no second example to design against, only
Metal does.

### 3.3 Resource lifetimes managed per-effect, by hand

**Tier 1 recreation is the identical pattern on all three backends**, keyed on a
size comparison inside each backend's per-frame setup: GL's `Setup()`
(`gl_renderbuffers.cpp:138-165`, `if (width != mWidth || height != mHeight || ...)
CreateScene(...)`) and Vulkan's `BeginFrame()`
(`vk_renderbuffers.cpp:64-83`, the same shape) mirror Metal's `mt_ao.cpp:1382`
exactly — nothing here is backend-specific in kind, only in which fields feed the
comparison (GL also checks `gl_multisample`/`mSceneUsesTextures`; Vulkan also checks
sample count).

**Vulkan carries one extra dependency the other two don't**: a resize or sample-count
change also calls `fb->GetRenderPassManager()->RenderBuffersReset()`
(`vk_renderbuffers.cpp:69-70`), because Vulkan render pass objects are cached keyed
on attachment format/sample count, and a resized target can invalidate that cache.
Neither GL nor Metal has an equivalent cache to invalidate — this is a real,
Vulkan-specific fourth resource-adjacent system (the render pass cache) that a
registry migration would need to know to poke, not just the textures themselves.

**Tier 3 has no Vulkan/GL instance to generalize from** (§1) — the aliasing,
reporting, and size-mismatch validation a registry would add apply identically to
tier 1 on all three backends, but the motivating pain (module-private textures
invisible to shared code) is Metal-only, tied directly to §3.2's finding. Lifting
the registry to `hwrenderer/` loses nothing by modeling only tiers 1-2 for now;
tier 3 stays a Metal-side extension point until a second backend actually needs one.

### 3.4 Ordering that is a comment, not a constraint

**Shared code, so this applies identically and needed no separate discovery.** The
mid-scene AO call and its governing 15-line comment live in `hw_drawinfo.cpp:1055-
1085` — `screen->AmbientOccludeScene(...)` is called from the one shared
`HWDrawInfo::CreateScene` path all three backends funnel through
(`hw_drawinfo.cpp:1077`, gated on `applySSAO && RenderState.GetPassType() ==
GBUFFER_PASS`), followed unconditionally by `portalState.EndFrame` and
`RenderTranslucent` — with whatever draw-target state the backend's own
`AmbientOccludeScene()` override left behind, which is exactly §3.1's finding
restated at the call-site level. The comment is the only thing constraining where
this call can move, on every backend equally.

One more shared reset worth being precise about, since it could otherwise look like
it contradicts §3.1: `hw_entrypoint.cpp:183-185` resets `mPassType` to `NORMAL_PASS`
and calls `EnableDrawBuffers(1)`, also identically for all three backends — but only
**after** `ProcessScene()` returns, i.e. after opaque, AO, portals *and* translucents
are all done, immediately before `PostProcessScene()` starts. It guarantees the PP
chain itself always begins in a known-good state; it does nothing for the
opaque-AO-portals-translucent window §3.1 is actually about, which is exactly the
window between the two resets — the mid-scene one each backend's own
`AmbientOccludeScene()` override is responsible for, and the only one Metal was
missing.

## 4. What this means for the resource registry and the graph interface

- **The registry (currently `metal/renderer/mt_resources.{h,cpp}`, phase 1) does not
  need a tier-3 concept to serve Vulkan or GL** — they have none (§1, §3.2, §3.3).
  Lifting it to `hwrenderer/` is simpler than the Metal side alone would suggest:
  model tiers 1-2, leave tier 3 as a Metal-side extension that stays local until a
  second backend grows a compute path.
- **The §3.1 coupling is not a three-way accommodation problem.** It is one backend's
  design choice (pipeline selection reading a raw, shared, PP-writable field) that
  the other two avoid by different, unrelated means (Vulkan: architectural isolation;
  GL: a decoupled logical field). A graph should declare "the value that selects a
  scene draw's pipeline" as its own tracked, single-writer piece of state — not
  reachable from a PP pass's own output-target bookkeeping — which makes Metal's bug
  class unreachable **by the interface**, not just fixed by a function nobody is
  required to call. `RestoreSceneRenderTargetAfterAO()` is a patch; a graph is what
  makes the patch unnecessary.
- **Capability selection (§3.2) should stay in the interface even though only Metal
  uses it today.** Removing it because two of three backends have no example would
  make the same mistake in reverse — designing against the majority and leaving a
  hole exactly where Metal already lives.
- **The Vulkan render-pass cache (§3.3) is a real fourth system a migrated registry
  needs to know about**, specific to Vulkan, with no analogue to generalize from GL
  or Metal.
- **Nothing here changes the migration order `frame-analysis.md` §4 already
  recommended** (resources first, `Pass2` next, bloom/exposure after, AO last) — if
  anything it strengthens the case for AO last: §3.1 is now understood precisely
  enough on all three backends to migrate it correctly, not just cautiously.

## 5. What would make this wrong

Recorded so it can be checked rather than trusted, mirroring `frame-analysis.md` §5:

- **Vulkan's structural immunity in §3.1 rests on reading `VkPPRenderState::Draw()`
  and `vk_renderstate.cpp`, corroborated by one `crossbackend.py` result on one
  scene** (`doom2`/MAP06). That is a real measurement, not just reasoning, but it is
  one map exercising one set of portal/translucent geometry after AO — not every
  path through `RenderTranslucent`/portal recursion that could theoretically read a
  stale pipeline key a different way.
- **The pass table and §3.4's ordering claim are static-extraction results**, exactly
  as the Metal document notes for its own table — they reflect what the source
  declares, not what a specific frame's custom-shader or portal-recursion path
  actually executes.
- **Sizes and formats were read, not measured**, same caveat as the Metal document —
  `GetSceneColorFormat()`-equivalent values vary at runtime with quality settings on
  every backend.
- **Scope exclusions are identical to the Metal document**: shadow maps, the
  wipe/transition path, 2D/HUD, and stereo present variants are outside this
  analysis on all three backends, for the same reason — the PP chain and the scene's
  G-buffer coupling is already the interesting problem.
