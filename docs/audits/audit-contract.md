# Audit contract: GZDoom native Metal compute-postprocess path

You are being asked for an **independent, adversarial second opinion** on a
narrow slice of a hobby renderer. This is a read-and-reason audit, not a
build-and-test one.

**Do not read `AGENTS.md` until after you have written your findings.** It
contains the maintainer's own conclusions, and the entire value of this
exercise is that yours are formed independently. There is a step at the end
that asks you to read it and diff your findings against it.

A previous audit of this backend by another model returned 18 findings, of
which **6 were false and the false ones were concentrated in the
highest-severity slots** — the two CRITICALs were both wrong, and the fix
suggested for one of them would have introduced a real bug. One MEDIUM
finding was genuinely valuable (the backend was silently dropping
per-vertex normals on every draw). This contract is written to get more of
the latter and less of the former. Sections 5 and 6 are the parts that
differ most from a generic audit request; read them carefully.

---

## 1. What this project is

GZDoom is a Doom-engine source port (C++17, GPLv3) with OpenGL, Vulkan, and
— in this fork — a **native Metal backend** on macOS, written with
`metal-cpp`. The Vulkan backend (`src/common/rendering/vulkan/`) is the
reference implementation; Metal is being brought to parity with it and then
pushed toward a compute-postprocess architecture.

Backends follow a **manager pattern**: `SystemBaseFrameBuffer` is the
abstract base, and each manager owns exactly one GPU resource type's
lifecycle (command buffers, samplers, textures, buffers, shaders, pipeline
state, render buffers, postprocess).

---

## 2. Scope — read this narrowly

**In scope, in priority order:**

1. `src/common/rendering/metal/renderer/mt_ao.cpp` (~2100 lines) and
   `src/common/rendering/metal/shaders/native/mt_ao.metal`.
   This is the highest-value target. It has absorbed seven rounds of
   bug-fixing over the last two weeks and has never had an outside read
   since. Recently changed areas include view→world position
   reconstruction, world-space noise generation, off-screen sample
   rejection, and distance fade.
2. `src/common/rendering/metal/renderer/mt_bloom.cpp` (~550 lines) and
   `shaders/native/mt_bloom.metal`.
3. The glue those two sit on:
   `mt_compute.cpp/.h`, `mt_postprocess.cpp/.h`,
   `mt_renderbuffers.cpp/.h`, and
   `src/common/rendering/hwrenderer/postprocessing/hw_compute.h`.
4. `mt_renderstate.cpp` **only where it interacts with the compute passes**
   (state save/restore around a compute encoder, resource residency,
   encoder transitions).

**Out of scope** — do not spend report space here: the OpenGL backend, the
Vulkan backend except as a *reference to compare against*, core engine and
gameplay code (`src/playsim/`, `src/scripting/`, `src/g_*.cpp`), the
ZScript VM, the filesystem layer, and general C++ style.

Audit the **working tree as it currently stands** (branch `metal-audit`),
not the last tagged release.

---

## 3. Hardware profile — this is a hard constraint, not context

The primary development machine is an **Intel MacBookAir7,2, Intel HD 6000,
Metal 2.0, macOS 12.7.6**. It is *not* Apple Silicon. A previous planning
round was wasted on advice that assumed otherwise. Verified capabilities:

```
Architecture:            Intel (IMR)      TBDR / memoryless:  no / no
ReadWrite BGRA8 (Tier2): NO               Argument buffers:   no (tier 0)
RGB10A2:                 no               Managed storage:    yes
SIMD-group ops:          no               Non-uniform threadgroups: no
Binary archives:         yes              GPU timestamps:     yes
Stage counter sampling:  no               Max drawables:      3
```

Consequences you must respect:

- **No SIMD-group / wave intrinsics, no non-uniform threadgroups.** Kernels
  must bounds-check their grid. Do not propose optimizations built on
  either. Do not flag the bounds checks as redundant.
- **No Tier 2 read-write textures**, so the bloom "Tier 2" direct-composite
  path is dead code *on this machine* by design — it is gated, not broken.
  `composite 0` and `1` both take the Tier 1 path; `composite 2` falls back
  to the upstream `hw_postprocess` reference implementation.
- **No stage-boundary counter sampling**, so per-pass GPU timings are
  hardware-blocked, not unimplemented. Whole-frame GPU timing works.
- **AO runs at quarter resolution here** — `mt_ao.cpp` forces
  `aoScale >= 4` on Intel. Anything whose correctness depends on the AO
  buffer resolution is under-tested; that is a legitimate thing to flag.
- The **capability gating is known-correct**. `supportsRGB10A2`,
  `metalVersion`, `supportsSIMDGroup` and `supportsNonUniformThreadgroups`
  all derive from one `supportsFamily(GPUFamilyMac2)` probe and therefore
  all go false together. This looks like a broken probe and is not one —
  Common1/Common2 yes with Common3/Mac2 no is a coherent profile for an
  older Intel iGPU. Do not report it.

---

## 4. Domain knowledge — intentional decisions, not bugs

Don't rediscover these. Do still scrutinize whether each is *applied
correctly* at every site.

- **Coordinate system.** Engine-internal is Y-up (OpenGL-style); Metal is
  Y-down. `MtShaderManager::PatchVertexShader` regex-patches vertex shaders
  at compile time to flip Y and remap Z from `[-1,1]` to `[0,1]`. That
  inversion also flips winding, which is why `FrontFacingWinding` is
  deliberately **Clockwise**. Downstream: scene textures are stored
  bottom-up, so `uv.y == 0` is the image **bottom**. A previous audit round
  "fixed" a Y-flip in the AO kernel that was already correct and had to
  revert. If you believe there is a Y-flip error, you must show the
  *round trip* — patched clip-space output through to the texel the kernel
  actually fetches — not just point at an asymmetric-looking expression.
- **Native `.metal` files are authoritative over inline C++ shader strings.**
  Several `mt_*.cpp` files contain an inline source string (e.g.
  `SSAO_COMPUTE_SOURCE`, `BLOOM_COMPUTE_SOURCE`). The compiled files under
  `shaders/native/` are what actually ship; the inline copy is a fallback
  used only if the metallib fails to load. **The two copies have drifted
  silently before.** Run `python3 tools/check_shader_parity.py` from the
  repo root (stdlib-only, no deps) and include its output verbatim.
- **CPU/GPU param struct layout.** Any struct passed via `setBytes:` or a
  buffer binding must match its Metal-side counterpart byte-for-byte —
  size, field order, alignment. Someone appending a field "for alignment"
  on one side only is a real, easy-to-miss bug class. Check **both** sides
  every time you see a shared param struct. This is one of the highest-yield
  checks available to a reader who cannot run the code.
- **Lazy state evaluation.** Render state (`SetViewport`, `SetScissor`,
  `SetDepthBias`, …) is accumulated and applied only in `Apply()`, right
  before a draw. "State set but never applied" mid-function is not dead
  code until you have confirmed `Apply()` is unreachable in that sequence.
- **Objective-C ownership.** `metal-cpp` follows Cocoa naming rules: methods
  without `alloc`/`new`/`copy`/`create` in the name return **autoreleased**
  objects the caller does not own. Frames are wrapped in an
  `NS::AutoreleasePool`. The previous audit's top CRITICAL was a
  false "leak" report against exactly this pattern, and its suggested
  `release()` would have caused an over-release crash. Before reporting any
  leak, state which ownership rule the call falls under and cite a
  counter-example in this codebase where the same pattern *is* explicitly
  retained.
- **Sampler key values:** `0`=Repeat, `1`=Mirrored Repeat, `2`=Clamp to
  Border, `3`=Clamp to Edge. Edge seams are usually the wrong one of these.
- **Texture cache key** is (width, height, format, mip count). A missing
  dimension shows up as textures flashing frame-to-frame.
- **Ring buffers.** Per-frame dynamic GPU data uses circular allocation,
  8–16 MB/frame. The GPU must finish frame N before the CPU overwrites that
  region on wraparound. **Missing synchronization here is a genuine
  risk class worth active hunting** — unlike most items in this list, this
  one is not "already handled, move on."

---

## 5. Standard of evidence

Every finding must clear these bars. A finding that cannot is either
downgraded to an explicitly-labelled *observation* or dropped.

1. **Concrete failure scenario.** Name the input or state, and the wrong
   output or crash it produces. "This looks fragile" is not a finding.
2. **Sign and magnitude must be checked, separately from mechanism.** A
   previous audit proposed that a too-tight thickness threshold caused
   *over-darkening*; both branches at the reject `continue`, i.e. discard
   the sample, so a tighter threshold can only *brighten*. The mechanism
   was plausible and the predicted sign was backwards, which killed it in
   one in-game test. If your finding predicts a visible artifact, state
   **which direction** the error goes and why.
3. **Site must be checked, separately from mechanism.** The same audit
   correctly intuited that hard-clamped UVs collapse a sample disk onto
   repeated edge texels — but pointed at a radius clamp that was already
   present and demonstrably not the cause. The real site was the viewport
   border. If you are confident in a mechanism but unsure of the site, say
   exactly that; it is useful, and mis-sited-but-real is a legitimate
   finding class here.
4. **Falsification test.** For each finding, give the single cheapest check
   that would prove you *wrong* — ideally a cvar to toggle or a
   fixed-camera A/B, since those are what the maintainer can actually run.
   Findings arrive with this attached or they don't get acted on. For
   `performance` findings the test must be expressible in whole-frame GPU
   time or CPU encode time (`mt_metrics_reset` / `mt_metrics`), because
   per-pass GPU timing is hardware-blocked here — see §3 and the bottleneck
   axis requirement in §7.
5. **Don't guess at runtime behavior you cannot observe.** You almost
   certainly cannot build this (macOS-only, Metal hardware required). Say
   "I cannot verify this without running it" rather than asserting.
6. **No severity inflation.** CRITICAL means crash, corruption, or
   visibly-broken rendering in a default configuration. The previous audit
   spent both of its CRITICAL slots on false positives, which is worse for
   the maintainer than having filed nothing.

An empty or short report is an acceptable outcome. Do not manufacture
findings to fill space. Three well-evidenced findings beat eighteen.

---

## 6. Two specific questions

Answer these explicitly, in addition to whatever you find on your own.

**Q1 — Bloom exposure divergence.** The compute bloom extract and the
reference `hw_postprocess` extract are not equivalent:

```
reference (bloomextract.fp):  max((color.rgb + 0.001) * exposureAdjustment - 1, 0)
compute   (mt_bloom.metal):   max( color.rgb - threshold + 0.001,             0)
```

`mt_bloom.cpp` hard-codes `params.threshold = 1.0f`, and
`MtBloomModule::Execute(cmdBuf, srcTex, gl_bloom_amount)` never receives the
exposure texture. They agree only when `exposureAdjustment == 1`, and
exposure is live by default (`gl_exposure_base`, `gl_exposure_min` 0.35,
`gl_exposure_scale` 1.3). The maintainer's prediction is: compute bloom is
dimmer than reference in dark scenes and does not adapt to scene luminance.

Confirm or refute that reading, and say whether the correct fix is to
multiply by exposure *before* thresholding or to fold exposure into the
threshold — they are not the same operation and the difference matters for
which pixels survive the extract at all.

**Q2 — AO residual.** After a recent fix that changed off-screen samples
from clamped to rejected, a soft darkening remains low in the frame.
Reading `mt_ao.metal` and the AO blur path, identify every mechanism that
could produce a darkening **fixed to the lower viewport region** rather than
to scene geometry. The maintainer's planned test is to strafe and see
whether it tracks the world or the viewport; tell them what each outcome
would implicate, so one test discriminates rather than merely observes.

---

## 7. Reporting format

Write findings to `FINDINGS.md` in the repo root. Rank most-severe-first.
Per finding:

- **File and line range**
- **Category** — exactly one of:
  - `correctness` — wrong output, crash, corruption, race, or a
    CPU/GPU struct-layout mismatch
  - `visual-quality` — renders without error but looks wrong: artifacts,
    noise, banding, divergence from the reference `hw_postprocess` path
  - `performance` — costs more time than it should, with output unchanged
  - `maintainability` — dead code, silent drift between the inline and
    native shader copies, undocumented workarounds, structural violations
    of the manager pattern

  If a finding is *also* a Vulkan/Metal parity gap, append `(parity)` — but
  still pick one of the four above as its primary category, since that is
  what determines who fixes it and when.
- **Bottleneck axis** — **required for every `performance` finding**, one of:
  texture bandwidth / ALU / encoder transition / dispatch count /
  synchronization / CPU-side encode. Name the axis you expect to dominate
  and say why. A performance finding without a named axis is not
  actionable: this machine has whole-frame GPU timing but **no
  stage-boundary counter sampling** (§3), so the axis determines whether the
  claim can be measured at all, and how. If you genuinely cannot tell which
  axis dominates, say so — that is a legitimate answer and more useful than
  a guess.
- **Severity**: critical / high / medium / low / info, per §5.6
- **Confidence**: high / medium / low — and low-confidence entries are
  welcome as long as they are labelled
- **Failure scenario**: input/state → wrong behavior, with direction of
  error per §5.2
- **Falsification test** per §5.4
- **Fix direction**: the shape of the fix, not necessarily a patch

Then a short section listing anything you *looked at hard and concluded was
fine* — the negative space is genuinely useful and cheap for you to
produce.

---

## 8. Only after you've written your findings

Read `AGENTS.md` (long; the AO and bloom sections are what matter) and mark
each of your findings **new** vs. **already documented there**. That
comparison is the actual deliverable — this is a second opinion, not a race
to find the most bugs. If `AGENTS.md` contradicts one of your findings,
don't automatically defer to it: say which of you you think is right and
why. It has been wrong before.
