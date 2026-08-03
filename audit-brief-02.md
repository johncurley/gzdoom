# Audit brief 02 — bloom parity, parity tooling, samplers, render state

Written 2026-08-02, after brief 01 was run and closed.

## Contract

**Read `audit-brief.md` first.** Its sections *Read these first*, *What to
produce*, and *Out of scope* apply here **unchanged and in full** — including
the "known deliberate, NOT findings" list (the Clockwise `FrontFacingWinding`
under the Y-flip patch, the compile-time Y-flip/Z-remap itself, the measured
Intel AO clamps, and the deliberate separation of `mt_compute_ao_intel` from
`mt_compute_ao_intel_clamp`). Those are not restated here on purpose: they
lived in one place and drifted once already, which is what brief 01's results
section is about.

Also read brief 01's **results** section. It records what has already been
closed, so you don't re-report it.

Same working contract: **source only** — no GPU, no build, no capture
session. Findings with the reasoning shown, numeric predictions where
possible, confidence stated, and what measurement would settle it. Rank by
"what would this change on screen".

### One added rule, learned from brief 01

**Anchor every finding to code and commits. Treat the living docs as a
hypothesis, not as truth.**

Brief 01 pointed auditors at `AGENTS.md` as the source of truth and named two
"known live" defects from it. Both had already been fixed — `aba5fba35`
restored the `depthMask` ramp, `f3f62c912` deleted the dead `ssao_combine`
kernel — and the doc had not been updated. The audit caught this precisely
because it went to source. So:

- Verify a doc claim against the tree before building on it.
- When a finding contradicts `AGENTS.md`, say so explicitly and say which you
  think is wrong. Those are the most valuable results *and* the most likely to
  be an auditor misreading.
- `git log -S<symbol>` and `git log -L` are in scope and encouraged. A commit
  that already fixed the thing is a finding.

**Do not update `AGENTS.md` or any living doc.** Report only.

---

## Task 1 — `mt_bloom.metal` vs the reference bloom shaders (send first)

**Compare** `src/common/rendering/metal/shaders/native/mt_bloom.metal`
against `wadsrc/static/shaders/pp/bloomextract.fp`, `bloomcombine.fp`, and
`blur.fp`.

**Static parity only.** Brief 01 put bloom out of scope; that exclusion was
about *behaviour* — "does bloom actually fire" is a measurement question that
has defeated two reading attempts and stays on the reference machine. Code
divergence is a reading question and is squarely in scope. Report constants
and structure; **do not conclude anything about whether bloom fires, is
visible, or is correct on screen.**

Look for the same classes that produced the `depthMask` defect:

1. Shared numeric constants that differ (thresholds, blur tap weights, kernel
   radii, mip/level counts, exposure factors).
2. Guards or validity tests present on one side only.
3. Structural branch differences — a `clamp`/`saturate`/early-out with no
   counterpart.
4. Sampling pattern and tap count.

**Important prior, to calibrate confidence:** `AGENTS.md` records that the
compute extract is already byte-matched to the reference and that the two
paths agree to 81 px (0.0089% of frame, mean signed +0.0002) after the
exposure fix. So the **extract stage is well-trodden** — a *new* finding
there is more likely a misread than a defect, and should be reported with
that stated. The **combine and blur stages are the unexamined ground**; weight
your effort there.

### UPDATE 2026-08-03 — the measurement is IN. Task 1 now has a fixed target.

This task no longer runs in parallel with an unknown. The Metal-vs-reference
bloom measurement has been taken, at a **zero noise floor** (two same-config
captures a minute apart were byte-identical), and the two paths **differ**.
Ashes 2063 Enriched MAP51 solar lantern, each path measured against a
`gl_bloom 0` baseline:

                        bbox        aspect  centroid        peak   energy   px>2
    reference PP     469 x 154     3.05:1  (798.6,424.6)   29.9   424242   47328
    Metal compute    316 x 296     1.07:1  (794.3,478.4)   16.2   347624   54291

Compute spreads over **more pixels at a lower peak with ~82% of the total
energy**, in a squarer blob whose centroid sits 54px lower in Y (X agrees to
4px). The reference blob is wide and flat; the compute blob is nearly square.

**Your job on Task 1 is now specific: find the code difference that produces
this signature.** Any candidate you propose should be checked against all five
numbers, not just one — a mechanism that explains the lower peak but predicts
*fewer* affected pixels is contradicted by the data and should be reported as
contradicted, not quietly dropped.

**Already ruled out by inspection. Do NOT re-report these:**

- Level count — both 4 (`NumBloomLevels == 4`; Metal is bloomA + 3 mips).
- Level 0 resolution — both quarter-res of `mSceneViewport`. Reference
  `((w+1)/2+1)/2` at `hw_postprocess.cpp:53-58`, Metal `(srcW+3)/4` at
  `mt_bloom.cpp:387-390`.
- Composite weights — level 0 only at unit gain on **both** sides.
  `mt_bloom.cpp:536-541` sets `strength[1..3] = 0`.
- Resample kernel — `downsample_box` (`mt_bloom.metal:111-122`) is a single
  bilinear sample despite its name, functionally identical to
  `bloomcombine.fp`, and both sides use one kernel for the down and up legs.
- Pyramid structure — blur-then-downscale walking down, blur-then-upscale
  *replacing* the level below walking up, then a final blur of level 0.

**Unexamined candidates, in rough order of suspicion:** the blur weight
computation and whether the Metal path reads `gl_bloom_amount` at all (the
reference feeds it to `ComputeBlurSamples(7, blurAmount, ...)` at
`hw_postprocess.cpp:104-106`); per-level texture dimension rounding as the
pyramid descends; filter and edge/clamp behaviour at level boundaries; and the
composite alpha difference — **narrowed 2026-08-03**: the parity script
confirms the *raster* combine already matches `bloomcombine.fp` including the
alpha clear, so this candidate applies **only** to the compute kernel
`bloom_combine_contrib_all` (`mt_bloom.metal:124-143`), which writes `1.0`.

**A cautionary note that is part of this task.** Two source-reading
conclusions about this exact code were wrong on 2026-08-03. The second claimed
Metal sums all four bloom levels — derived from reading
`bloom_combine_contrib_all` **without reading its caller**, which zeroes three
of the four weights. The first proposed a normalized-UV vs texel-offset unit
mismatch, disproved by `blur.fp` using `ivec2` `textureOffset`. Both were
plausible from the shader alone. **Read every kernel together with its binding
site, and check `mt_bloom.cpp`'s comments — they record prior fixes**
(e.g. `mt_bloom.cpp:430-442` documents the all-levels summation being fixed on
2026-07-30 at ~2.1x the reference).

## Task 2 — DONE (`6524a7f6b`). Read the result before doing Task 1.

The reference-GLSL invariant direction now exists and passes. **Its output is
evidence for Task 1** — run `python3 tools/check_shader_parity.py` first and
treat every `MATCH` as a hypothesis already eliminated. In particular it
confirms the bloom extract bias, threshold, exposure guard, Gaussian exponent,
blur-amount minimum, 7+7 tap count, and the raster combine's
RGB-with-cleared-alpha contract all agree.

Its limits are stated in its own docstring and matter here: it checks named
invariants, not semantic equivalence. Coordinate conventions, resampling
strategy, control-flow shape and per-level behaviour are explicitly outside
it. **A clean run does not mean the two paths render the same image** — the
measurement in Task 1 proves they do not.

Extending it further is welcome but secondary. If you find the Task 1 cause
and it is mechanically checkable, add it as an invariant so it cannot
regress. Do not broaden it into token-set diffing; the docstring explains why
a noisy checker is a worthless one.

### Original brief for Task 2, kept for context

The standing highest-leverage tooling item in `AGENTS.md`, and **not**
addressed by brief 01. That audit reported the script "passes", which covers
only its two existing directions:

1. native `.metal` vs the inline C++ fallback raw-string copy, and
2. native `.metal` vs the CPU-side struct declaration in the C++ header.

The `.fp` comparison is a **third direction that does not exist yet**. Read
the script's docstring before writing anything — it already documents why each
direction exists, which one matters more and why, and what the comparison
deliberately cannot see (`alignas`, `packed_*`, implicit tail padding). Follow
that convention: whatever you add must state its own blind spots in the same
voice, because a clean run that is quietly incomplete is worse than no script.

Scope it to what is mechanically checkable across a GLSL/MSL boundary —
shared numeric literals and named constants, guard presence, tap counts.
Do not attempt full semantic equivalence. A high-signal narrow check beats a
broad one that cries wolf; the script's value is that it runs every time and
is believed.

Both of this branch's real defects were in this class and both cost hours by
hand. Task 1 is the one-time report; this is the version that re-runs forever.

## Task 3 — CLOSED. Do not re-run.

The sweep was done and produced two real divergences, both fixed in
`fde91b90b`: `CLAMP_CAMTEX` clamped where the reference repeats, and
`CLAMP_XY_NOMIP`/`CLAMP_NOFILTER*` were treated as address modes only while
the reference also pins filtering and mipmapping. The sampler cache key was
confirmed complete for the state Metal actually creates.

**One divergence found but deliberately NOT fixed**, still open: Metal derives
the W address mode from the clamp mode, while the reference hardcodes `REPEAT`
for W on every HW sampler. Only affects 3D sampling and touches the
postprocess/bloom sampler paths, so it wants its own change. Not an audit
task — it needs a build.

### Original brief for Task 3, kept for context

Mechanical, high value per unit effort, and it produces *visible* artifacts
when wrong.

`.github/copilot-instructions.md` calls out seam leaking from incorrect
sampler clamp modes as a recurring Metal gotcha. `AGENTS.md:992` notes
`ssao_combine_fs` uses `address::clamp_to_edge` for depth/normal/AO. **Nobody
has swept the whole surface.**

Enumerate every sampler the Metal backend creates and every sampler a shader
declares, with its address mode, filter, and mip mode; compare against what
the corresponding reference path binds. Flag any `repeat`/`mirrored_repeat`
on a full-screen or postprocess target, and any mismatch between what a
shader assumes and what the C++ side binds.

The sampler cache-key composition is also documented in the field guide as a
gotcha — flag any key that omits a field it should include, since that
silently returns the wrong cached sampler.

## Task 4 — CONTINUE with the next subsystem (blend modes are done)

**Blend modes were audited and produced the session's best finding**:
`STYLEOP_Shadow` used `Zero`/`SourceAlpha` where the reference takes the fuzz
mapping, making Metal 42% too dark on shadow sprites. Fixed in `fde91b90b` and
**verified on screen** — Metal and Vulkan shadow-darkening ratios now agree to
0.003. Do not re-audit blend modes.

That hit rate is the argument for continuing. **Pick ONE of the remaining
subsystems** and do it as thoroughly as the blend-mode pass was done:

- depth/stencil state,
- the culling/winding path,
- push-constant vs uniform-buffer binding.

Two notes carried from the blend-mode result. First, the reference's own
mapping tables are the authority and can be counter-intuitive — `renderops[]`
holding `-1` for everything above `STYLEOP_RevSub` is what routes
`STYLEOP_Shadow` into the fuzz branch, and that is invisible unless you follow
the index. Trace the table, don't infer from the enum name. Second, quantify:
"42% darker, alpha-dependent, crossing at As = 0.6" is what made that finding
actionable and testable, where "the blend factors differ" would not have been.

For the culling/winding subsystem specifically, re-read the known-deliberate
list in brief 01 first — the Clockwise `FrontFacingWinding` under the Y-flip
patch is correct and must not be reported.

### Original brief for Task 4, kept for context

`CLAUDE.md` names `MtRenderState` the most critical class in the backend, and
Vulkan parity is the branch's whole premise — but the class is large and an
unscoped pass will produce an unreadable report.

**Pick ONE subsystem** and do it properly: depth/stencil state, *or* blend
modes, *or* the culling/winding path, *or* push-constant vs uniform-buffer
binding. Compare against `src/common/rendering/vulkan/` as the reference
implementation.

State which you chose and why at the top. A thorough report on blend modes is
worth more than a shallow sweep of all four. If you finish one and have room,
start a second under its own heading rather than merging them.

Note the field guide's warnings on push constants vs uniform buffers and on
ring-buffer GPU/CPU sync before reporting anything in those areas as a defect.

---

## Not in this brief

- Compute bloom **behaviour** (see Task 1) — measurement, stays on hardware.
- The dead AO plumbing from brief 01's finding 3 (`combinePSO`, unused
  `Execute()` params, the dither-texture gate). Confirmed already; the fix
  needs a build to verify reachability safely and is being done on the
  reference machine.
- Anything Apple Silicon — untested everywhere, not auditable from source.
- Performance claims of any kind. `FrameGPU` from `mt_metrics` is the only
  valid cost signal and it requires the hardware.
