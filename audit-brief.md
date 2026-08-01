# Audit brief — Metal backend vs reference, `metal-audit` branch

Written 2026-08-02. For an auditor working **from source only**: no GPU, no
build, no capture session required. Everything below is a reading task.

> **STATUS: this audit has been RUN (2026-08-02). Results below the brief.**
> Two of the brief's own "known live" anchors turned out to be stale — they
> had already been fixed and `AGENTS.md` had not been updated. Corrected in
> place; see "Audit results" at the end. Re-read that section before reusing
> this brief.

## Why this brief exists

Both real defects found on this branch had the same shape: **self-consistent
Metal code that had silently stopped matching the reference implementation.**
Neither was visible as a bug when reading the Metal file alone — each was only
a defect *relative to* the OpenGL/Vulkan path it was supposed to mirror. Both
cost hours to find by hand. That class is mechanically auditable, and this
brief is scoped to it.

## Read these first (non-negotiable)

- `CLAUDE.md` — build, architecture, and the measurement rules.
- `AGENTS.md` — current implementation state. **The most up-to-date source of
  truth for what is actually true about the Metal renderer.**
- `.github/copilot-instructions.md` — Metal renderer field guide.
- `handoff.txt` (repo root) — the 2026-08-01 session state.

Skipping these produces false positives. Specifically, the following are
**known, deliberate, and NOT findings**:

- Metal's `FrontFacingWinding` is Clockwise. The compile-time Y-flip patch in
  `MtShaderManager::PatchVertexShader` inverts winding; CW is the correction,
  not a bug.
- Vertex shaders are regex-patched at compile time to flip Y and remap Z from
  `[-1,1]` to `[0,1]`. Engine-internal space is Y-up, Metal is Y-down.
- The Intel clamps in the AO path are **load-bearing and measured**, not
  conservative guesses. Compute AO costs ~1.95x the reference PP path on the
  reference machine (22.05ms vs 11.42ms FrameGPU, against a 0.36ms noise
  floor), independently reproduced twice. Do not propose removing them, and do
  not describe compute AO as a performance win.
- `mt_compute_ao_intel` and `mt_compute_ao_intel_clamp` are separate on
  purpose. Folding them would make the clamps unreachable.

## What to produce

**Findings with the reasoning shown — not conclusions.** For each finding:

1. File and line, both sides of the comparison.
2. The exact divergence, quoted.
3. What observable difference it would produce, *as a numeric prediction where
   possible* ("Metal's AO fades in at half the rate with distance, so at depth
   D the Metal frame is N% lighter in the AO term").
4. Your confidence, and what measurement would settle it.

Rank by "what would this change on screen", not by how odd the code looks.

**Do not update `AGENTS.md` or any living doc.** Findings are unverified until
measured on the reference machine. Three claims that were entirely plausible
from reading the code were retracted against measurement in the last session
alone (that HDR enables bloom; that compute AO is faster than the reference;
a predicted portal-bleedthrough signature that did not exist). Assume some
fraction of this audit's output will die the same way — that is expected and
fine, but it is why findings land in a report and not in the docs.

Flag explicitly if a finding contradicts something already written in
`AGENTS.md`. Those are the most valuable results and also the most likely to
be an auditor misreading; say which you think it is.

---

## Task 1 — Metal compute shaders vs reference `.fp` shaders (highest value)

The core parity question, and the one that has actually produced defects.

**Compare:**

| Metal (native) | Reference |
|---|---|
| `src/common/rendering/metal/shaders/native/mt_ao.metal` | `wadsrc/static/shaders/pp/ssao.fp`, `ssaocombine.fp`, `depthblur.fp`, `lineardepth.fp` |
| `src/common/rendering/metal/shaders/native/mt_bloom.metal` | `wadsrc/static/shaders/pp/bloomextract.fp`, `bloomcombine.fp`, `blur.fp` |

Also present: `src/common/rendering/metal/shaders/compositor.fp` and
`ssao_simple.compute.glsl`.

**Look for, in priority order:**

1. **Shared numeric constants that differ.** This is the confirmed-live class.
   One known instance, already logged in `AGENTS.md` and *not yet resolved*:
   the `depthMask` ramp is `1.0 - exp2(-ssao.y * 0.01)` in `ssaocombine.fp`
   but `exp2(-... * 0.005)` in Metal's `ssao_combine_fs` (both branches) —
   half the rate, so Metal's AO fades in more slowly with distance. **Do not
   "fix" it**; it is unresolved whether it is deliberate tuning or drift. Your
   job is to find out whether it is alone or one of a family. That answer
   largely determines the verdict: an isolated tweak reads as tuning, a
   cluster reads as drift.
2. **Guards and validity tests present in one side only.** E.g. the
   reference's `ssao.y > 2.0` validity guard.
3. **Structural branch differences** — a `if`/`clamp`/`saturate` on one side
   with no counterpart.
4. **Sampling pattern, tap count, kernel radius, blur weights.**

Coordinate-space differences (Y-flip, Z range) are expected — see the
known-deliberate list above. Distinguish those from real divergence.

**Deliverable option:** the highest-leverage form of this task is extending
`tools/check_shader_parity.py` to perform comparison automatically. Read its
docstring first — it already compares native `.metal` against inline C++
fallback strings and against CPU-side struct declarations, and its docstring
records why each direction exists and what it deliberately cannot see. The
`.fp` comparison is a third direction and is noted in `AGENTS.md` as the
highest-leverage tooling work available. A hand-written report is acceptable;
a script that finds the same things every time is better.

## Task 2 — Unreachable and dead code in the Metal path

Mechanical, verifiable, and directly actionable.

Find every Metal kernel / shader function with **no live caller or no PSO ever
compiled from it**, and every C++ branch with no reachable caller.

Two known instances to confirm and use as the pattern:

- `ssao_combine` (`mt_ao.metal:1259`) is never compiled into a PSO — only
  `ssao_combine_fs` is (`mt_ao.cpp:1340`). It is *missing* the reference's
  validity guard and `depthMask` ramp, which makes it look like a live bug in
  the shipping path when it is not reachable at all. Slated for deletion; it
  will mislead the next person debugging AO bleed.
- `BlitCurrentToImage`'s format-conversion branch currently has no reachable
  caller: `CreateWipeTexture` is the only caller and now always matches the
  pipeline format.

The value here is that unreachable code **actively misleads future debugging**
— both instances above did. Report each as: reachable / unreachable / could
not determine, with the evidence.

## Task 3 — CVAR surface

An entire AO investigation on this branch resolved to stale config, and dead
CVARs still tab-complete after their feature is removed. Audit which `mt_*`
and `gl_ssao*` CVARs are **declared**, which are **actually read**, and which
are **stale**.

Known stale: `mt_compute_ao_temporal*` (inert, feature removed, still present
in the local `gzdoom.ini`).

Flag any CVAR whose default in code disagrees with what the docs claim, and
any that is declared but never read.

## Out of scope

- **Compute bloom Metal-vs-reference behaviour.** This is a measurement
  question, not a reading question, and reading has already failed on it
  twice. It stays on the reference machine. You may report *code* divergence
  in `mt_bloom.metal` under Task 1 — just don't conclude anything about
  whether bloom fires.
- Anything requiring Apple Silicon. Untested everywhere; not auditable here.
- Performance claims of any kind. `FrameGPU` from `mt_metrics` is the only
  valid cost signal and it requires the hardware.

---

# Audit results — 2026-08-02

Run against HEAD. Every claim below was re-verified independently before
being recorded here.

## The brief's own anchors were stale (verified)

Both "known live" AO items in Task 1 and Task 2 had **already been fixed**;
`AGENTS.md` still listed them as open, and this brief inherited that error.
Verified at HEAD:

- **`depthMask` ramp** matches the reference: `1.0 - exp2(-x * 0.01)` at
  `mt_ao.metal:1097` against `ssaocombine.fp:33`, and the low-resolution
  branch also uses `0.01` at `mt_ao.metal:1131`. Restored by `aba5fba35`.
- **`ssao_combine`** no longer exists in the native shader; every remaining
  AO entry point has a PSO construction path at `mt_ao.cpp:1339`. Deleted by
  `f3f62c912`.
- `tools/check_shader_parity.py` passes for all AO/bloom functions and
  shared structs.

**Method lesson, worth more than the findings:** the stale anchors came from
`AGENTS.md`, which the brief correctly names as the source of truth. It was
wrong for two items because fixes landed without the doc being updated. A
source audit catches this; a doc-driven audit propagates it. Anchor findings
to code and commits, and treat the living docs as a hypothesis.

## Confirmed findings

**1. Effective-config help text is wrong about archiving.** `mt_debug.cpp:509`
states every listed setting is `CVAR_ARCHIVE`. Two are not:
`mt_compute_bloom_composite` (flags `0`, `mt_postprocess.cpp:37`) and
`gl_ssao_debug` (`hw_postprocess_cvars.cpp:74`). *Prediction:* those two reset
after restart while the archived ones persist. Confidence high. Settle by
changing each, restarting, checking. **This one bites during A/B sessions** —
a debug cvar silently reverting across a restart is exactly the "cvar did not
apply" failure mode that has cost this branch multiple invalid data sets.

**2. Low-impact dead AO plumbing.** All confirmed from source, no screen
impact in normal operation:
- `combinePSO` declared at `mt_ao.h:90`, released at `mt_ao.cpp:1387`, never
  assigned or used.
- `Execute()` takes `fogTex` and `combineTex` (`mt_ao.h:65`) and never reads
  them.
- The dither texture is allocated and *gates* compute AO even though the
  three sample kernels explicitly do not use it (deferred cleanup noted at
  `mt_ao.metal:259`). A dither allocation failure would unnecessarily force
  the PP path.

## Clean

All declared `mt_*` and `gl_ssao*` CVARs have live source reads; no
declaration/default mismatch. `BlitCurrentToImage`'s conversion branch
remains unreachable through its only caller, as already documented.
