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

This task is being run in parallel with a live Metal-vs-reference bloom
measurement. Convergence or contradiction between the two is the point, so
report divergences even when you believe they are visually negligible —
state the predicted magnitude and let the measurement arbitrate.

## Task 2 — extend `tools/check_shader_parity.py` with the `.fp` direction

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

## Task 3 — sampler address-mode and clamp audit

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

## Task 4 — `MtRenderState` vs the Vulkan reference (scope tightly)

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
