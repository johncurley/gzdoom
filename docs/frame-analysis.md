# Frame analysis — what the renderer's passes and resources actually are

Written 2026-08-16, from the code, as the prerequisite for **Track A items 3 and 4**
of `docs/engine-modernization.md` (build a backend-neutral frame graph; migrate the
bounded postprocess passes into it). You cannot design a graph over a set of passes
whose inputs, outputs and couplings have never been written down. This writes them
down.

Scope: the postprocess chain and its resources, on the Metal backend, with the
shared `hw_postprocess` machinery it is built on. The scene pass itself is
characterised only where it couples to postprocess — which turns out to be the most
important finding here.

Method note: the pass I/O table was extracted mechanically from the `SetInput*` /
`SetOutput*` calls in `hw_postprocess.cpp` rather than read off prose, because prose
and code have already disagreed on this branch. Line numbers are current at the time
of writing.

---

## 1. Resource inventory

Three tiers, and the tiers matter — they have different lifetimes, different owners,
and only the first is visible to any backend-neutral code.

**Shared scene/pipeline targets** (`mt_renderbuffers.h:40-48`), sized to the scene:

| resource | notes |
|---|---|
| `SceneColor` | the frame; format from `GetSceneColorFormat()` |
| `SceneDepthStencil` | depth + stencil, read by AO, stencil used by PP |
| `SceneNormal` | G-buffer normals; **only written under `GBUFFER_PASS`** — see §3.1 |
| `SceneFog` | 8-bit; AO's composite input |
| `PipelineImage[2]` | the ping-pong pair the whole PP chain runs through |

**Per-effect textures owned by shared code** (`hw_postprocess.h`), sized independently:
bloom `VTexture`/`HTexture` per level, exposure `ExposureLevels[]` + `CameraTexture`,
AO's `LinearDepthTexture` / `Ambient0` / `Ambient1` / `AmbientRandomTexture[]`,
tonemap `PaletteTexture`.

**Private to the Metal compute modules**, invisible to shared code and to every
shared diagnostic: AO holds nine (`mt_ao.h:99-107` — `mAOTexture`, `mBlurTexture`,
`mLowresResultTexture`, `mFullresAOTexture`, `mFullresTempTexture`,
`mFullresResultTexture`, `mStencilView`, `mStencilViewSource`,
`mDepthPyramidTexture`), bloom holds five (`mt_bloom.h:68-78`).

That third tier is the shape of the problem: **two parallel resource systems that no
single place describes.**

## 2. The pass chain

`Postprocess::Pass1` and `Pass2` (`hw_postprocess.cpp:1234-1252`) are the whole
ordering mechanism — two functions, a fixed sequence, conditionals inline.

```
scene render ─┬─ opaque pass
              ├─ AmbientOccludeScene        ← MID-SCENE (hw_drawinfo.cpp:1071)
              └─ portals, translucents
   ↓
Pass1:  exposure ─ customShaders("beforebloom") ─ [bloom, unless skipBloom]
   ↓
Pass2:  tonemap ─ colormap ─ lens ─ fxaa ─ customShaders("scene")
   ↓
present
```

Extracted I/O, one row per pass (`current`/`next` are the `PipelineImage[2]` pair):

| pass | reads | writes | conditional on |
|---|---|---|---|
| `exposure` | current | `ExposureLevels[0..n]`, `CameraTexture` | always (chain of N downsamples) |
| `bloom` | current, `CameraTexture` | level `VTexture`s, then current | `gl_bloom`, `!skipBloom` |
| `blur` | current | level `VTexture`s, then current | menu blur path |
| `tonemap` | current, `PaletteTexture` | next | `gl_tonemap` |
| `colormap` | current | next | `fixedcm`/flash |
| `lens` | current | next | `gl_lens` |
| `fxaa` | current | next | `gl_fxaa` |
| `ssao` | `SceneDepth`, `SceneColor`, `SceneNormal`, `SceneFog`, random | `LinearDepthTexture`, `Ambient0/1`, then **`SceneColor`** | `gl_ssao`, mid-scene |
| custom | current (+ user textures) | next | per-shader |
| present | current | backbuffer | always |

Two structural observations fall straight out of the table:

- **The chain is implicit.** `SetInputCurrent`/`SetOutputNext` encode a dependency
  that exists only in call order. Nothing declares that `fxaa` depends on `lens`;
  reordering the calls silently changes the frame, and no validation would object.
- **AO is the outlier in every respect.** It is the only pass that reads G-buffer
  targets, the only one that writes `SceneColor` rather than the ping-pong pair, and
  the only one that runs *inside* the scene rather than after it.

## 3. The couplings a frame graph would make explicit

This is the payload of the analysis. Each item below is a real defect or a real
diagnostic gap that already cost time, and each is an instance of *implicit coupling
between passes* — precisely what a graph with declared inputs and outputs removes.

### 3.1 A pass mutating global state that later passes depend on

`MtPPRenderState` drives the **main** render state (unlike Vulkan, where
`VkPPRenderState` is a separate object). Every PP pass forces a single colour
attachment, and nothing put it back. Because
`MtPipelineStateManager` selects the shader variant as
`DrawBufferCount > 1 ? GBUFFER_PASS : NORMAL_PASS`, and `FragNormal` exists only
under `GBUFFER_PASS`, **every surface drawn after the mid-scene AO pass compiled
against a shader with no normal output** — 134 of 135 wall draws, measured. Colour
and depth still landed in attachment 0, so nothing looked wrong; the scene normal
G-buffer was simply empty, which is what broke SSAO on Metal.

The fix is `RestoreSceneRenderTargetAfterAO()` (`mt_postprocess.cpp:594`) — a manual
"put it back" that works and that nothing enforces. In a graph, "the scene pass
writes `SceneNormal`" is a declaration; a pass that invalidated it would fail
validation rather than produce a plausible frame.

### 3.2 Two implementations of one node, chosen at the call site

AO and bloom each have a compute implementation that **bypasses the PP chain
entirely**: `if (useComputeAO && fb->mAOModule->Render(...)) { ...; return; }`
(`mt_postprocess.cpp:535`). Consequences already measured:

- The compute path issues no `ssaocombine` draw, so `mt_aoprobe` — which matches on
  that shader lump — arms and never fires. **The backend's own diagnostic cannot see
  half of the backend.**
- A GPU capture of the compute arm carries no `PP` encoder labels at all, so pass
  structure has to be inferred from absences (verified with a positive control).
- Selection is a chain of conditionals at the call site (`mt_compute_ao`,
  `mt_compute_ao_intel`, architecture check), not a declared capability. The suite's
  `bloom_compute` config passed `mt_compute_bloom 1` and was silently routed to the
  reference path for months, comparing reference against reference.

The roadmap already names the right shape: *"A node may have a direct-compute,
temporary compute, raster, or disabled implementation."* The value is not
abstraction for its own sake — it is that **implementation selection becomes
inspectable**, so a harness can assert which one ran instead of a label being the
only clue.

### 3.3 Resource lifetimes managed per-effect, by hand

Every effect owns its own `EnsureTextures(width, height)`-style recreation, keyed on
a size comparison (`mt_ao.cpp:1382`). Nothing knows the *set* of live resources at a
given frame size, so nothing can alias them, report them, or notice that AO's
quarter-res buffer rounds two different scene heights (773 and 776) to the same 194
rows while the full-res consumers do not.

A graph that owns resource allocation gets three things this cannot: aliasing of
non-overlapping lifetimes (real memory on a 1.5GB integrated GPU), a dump of "what
exists at this size", and size-mismatch validation at declaration time rather than
as a silently wrong image.

### 3.4 Ordering that is a comment, not a constraint

The mid-scene AO placement is documented in a 15-line comment explaining *why* it
must be there and what breaks otherwise. That comment is load-bearing: it is the only
thing preventing someone from moving the call. Declared dependencies (`ssao` reads
`SceneNormal` and `SceneDepth`, writes `SceneColor`; the translucent pass reads
`SceneColor`) would make the legal positions derivable instead of remembered.

## 4. What this suggests for the first migration

Not "port everything". The order that follows from the analysis:

1. **Declare resources first, keep the existing execution.** Sketched in
   `docs/frame-graph-resources.md` — interface, registration sites, what it reports
   and validates, and the phase split.
   Original statement of the step: A registry describing
   the tier-1 and tier-2 textures with sizes and formats, built where they are
   created today. Zero behaviour change, immediately useful: it can print the live
   set, and it gives the size-mismatch validation §3.3 lacks.
2. **Migrate `Pass2` (tonemap → colormap → lens → fxaa) first.** Four passes, all
   pure `current → next`, no G-buffer reads, no mid-scene placement, all individually
   toggleable by cvar and already covered by the matrix suite's relations. It is the
   cheapest possible end-to-end exercise of a graph, and the suite can prove each one
   still acts.
3. **Then bloom/exposure**, which add a multi-level resource chain and a second
   implementation (compute) — i.e. the first real test of capability selection.
4. **AO last.** It is the hardest node (mid-scene, G-buffer reads, writes
   `SceneColor`, two implementations, a stencil view) *and* the one whose current
   behaviour is least settled. Migrating it while its output is still unexplained
   would confuse two problems.

**Explicitly not now:** GPU culling, indirect submission, clustered lighting. The
roadmap has them behind measurement, and the measurement instrument for this hardware
does not exist yet (no per-pass GPU timing; see `docs/renderer-methodology.md` §2).

## 5. What would make this analysis wrong

Recorded so it can be checked rather than trusted:

- It is **Metal-only**. The companion analysis is done —
  `docs/frame-analysis-vulkan-gl.md`, closed 2026-08-16, task 6 of
  `docs/handoff-linux-2026-08-16.md`. Read it before designing the graph interface.
  It corrects the two guesses this note made: Vulkan's `VkPPRenderState` not
  touching the shared render-target state is closer to *architectural isolation*
  than "a separate object", and `FGLPostProcessState` is not what protects GL's
  §3.1-shaped risk — GL's shader selection reads a logical field PP passes never
  touch, a third mechanism again, and `FGLPostProcessState` is an unrelated raw-GL
  state bracket used only at other call sites (backbuffer copy, stereo present).
- The pass table is **static extraction**. It reflects the `SetInput*`/`SetOutput*`
  calls present in the source, not what executes on a given frame; the custom-shader
  path in particular is data-driven.
- Sizes and formats were **read, not measured**. `GetSceneColorFormat()` and friends
  vary at runtime with quality settings.
- The frame does more than this document covers — shadow maps, the wipe/transition
  path, 2D/HUD, stereo present variants. They are excluded deliberately; a graph
  that handles the PP chain and the scene's G-buffer coupling is already the
  interesting problem.
