# Audit brief 03 — render-state parity, the bloom remainder, and two long-open items

Written 2026-08-04, after brief 02 was run and its three findings were fixed.

## Contract

**Read `audit-brief.md` first**, then brief 01's results, then **brief 02 in
full including its results section**. Everything in those applies here
unchanged — the working contract, what to produce, out of scope, and above all
the *"known deliberate, NOT findings"* list (the Clockwise
`FrontFacingWinding` under the Y-flip patch, the compile-time Y-flip/Z-remap
itself, the measured Intel AO clamps, and the separation of
`mt_compute_ao_intel` from `mt_compute_ao_intel_clamp`). Those are not
restated here on purpose; they have drifted once already.

Same working contract: **source only** — no GPU, no build, no capture session.
Reasoning shown, numeric predictions where possible, confidence stated, and
what measurement would settle it. Rank by "what would this change on screen".

Brief 02's added rule still stands and earned its keep again this round:
**anchor every finding to code and commits, and treat the living docs as a
hypothesis.** `git log -S<symbol>` and `git log -L` are in scope and
encouraged; a commit that already fixed the thing is a finding.

**Do not update `AGENTS.md` or any living doc.** Report only.

### Audit the tree at `3cb088b89` or later

Brief 02's three findings were all confirmed against source and **fixed** on
2026-08-04. If you are reading an older checkout you will re-find them. They
are listed under "Closed" below with the commit; re-reporting any of them is a
sign you are on the wrong revision.

---

## What brief 02 closed — do not re-report

Brief 02 was a good round: three findings, all three real, all three fixed.
For calibration, note that the *most valuable* finding of that round
(finding 3, depth writes) had nothing to do with the task it came from being
the exciting one — it came from the boring, bounded state-surface comparison.

**Closed and fixed in `3cb088b89`:**

1. **Tier 1 bloom composite sampled the contribution upside down.** `bloom_fs`
   now applies `float2(in.uv.x, 1.0 - in.uv.y)`. `bloom_combine_contrib_all`
   writes `mCompositeTex` in texture order; `bloom_vs` derives uv from clip
   space where +Y is up; native libraries bypass `PatchVertexShader`.
2. **Bloom mip chain rounded down** (`width >> i`) where the reference
   ceiling-halves iteratively. Now iterative ceiling-halving.
3. **Depth writes enabled while depth testing disabled.** `pipelineKey.
   DepthWrite` is now `(mDepthTest && mDepthWrite)`, matching
   `vk_renderstate.cpp:217`. The reverse-Z compare mapping was deliberately
   NOT touched and must not be reported.

**Closed and fixed in `9d19739e4`** (the deferred cleanups): the dead AO
plumbing — `combinePSO`, `Execute()`'s unused `fogTex`/`combineTex`, and the
dither-texture gate — and the sampler W-axis divergence, where the reference
hardcodes `REPEAT` at all four `vk_samplers.cpp` `AddressMode()` call sites.

**Closed as a non-issue:** brief 02 flagged the prior 81-pixel bloom-extract
agreement as possibly inconsistent with the Y-flip finding. It is not. The
extract-stage comparison never runs through the composite, so a broken
composite could not have appeared in it. Right instinct, no contradiction.

**Also closed:** Tasks 1, 2 and 3 of brief 02 in their entirety. Task 4's
depth/stencil subsystem is done.

### One correction to carry, because it is a reasoning pattern

Brief 02's finding 1 reported the predicted reflected centroid as *exactly*
matching the measurement: `2 * 451.5 - 424.6 = 478.4`. That axis of 451.5 was
the **midpoint of the two measured centroids** — fitted to the data, not
derived independently from the scene viewport. The numbers were therefore
*consistent with* a vertical flip; they did not confirm one, and the code
evidence is what carried the finding.

This is a good finding weakened by a circular check, and it is the single most
useful habit to bring to this round: **when you offer arithmetic as
confirmation, state where each input came from.** An input derived from the
thing you are testing is not evidence.

---

## Task 1 — `MtRenderState`: push constants vs uniform-buffer binding

**Brief 02's Task 4 said to pick ONE subsystem and do it properly. Depth/
stencil is now done and produced the round's best finding. Blend modes are
done. Do this one.**

Compare Metal's per-draw constant delivery against
`src/common/rendering/vulkan/` as the reference. Specifically: what the
reference sends as push constants versus what Metal sends as
`setVertexBytes`/`setFragmentBytes` versus what it routes through a uniform
buffer or the stream buffer; and whether the *contents and layout* agree
field-for-field at each binding index.

Why this one: `CLAUDE.md` names `MtRenderState` the most critical class in the
backend, this is its least-examined surface, and a layout or index mismatch
produces exactly the class of defect this branch has repeatedly found — silent,
plausible-looking output that is wrong by a constant or a swizzle.

**Read the field guide's warnings on push constants vs uniform buffers and on
ring-buffer GPU/CPU sync before reporting anything here as a defect**
(`.github/copilot-instructions.md`). Some of the divergence is deliberate,
because Metal's argument model is genuinely different from Vulkan's. A finding
that amounts to "Metal does not use push constants the way Vulkan does" is not
a finding. A finding that a specific field arrives at a different offset, in a
different unit, or stale by one frame, **is**.

Carry both lessons from the blend-mode result:

- **Trace the table, don't infer from the name.** `renderops[]` holding `-1`
  above `STYLEOP_RevSub` is what routed `STYLEOP_Shadow` into the fuzz branch,
  and that was invisible without following the index.
- **Quantify.** "42% darker, alpha-dependent, crossing at As = 0.6" is what
  made that finding actionable. "The bindings differ" would not have been.

If you finish and have room, the culling/winding path is the remaining
subsystem — under its own heading, not merged into this one. Re-read the
known-deliberate list first; the Clockwise `FrontFacingWinding` is correct.

---

## Task 2 — the bloom remainder, framed narrowly

**Read this framing before starting; the task is smaller than it looks.**

Two mechanisms in the bloom path are now fixed (the composite V-flip and the
mip rounding). Neither is predicted to account for the full measured
difference between the compute and reference paths. But **the re-measurement
has not happened yet** — it is the next hardware session — so this task is
*not* "explain the remaining gap." You do not yet know what remains.

What is in scope is the source ground that round two identified as unexamined
and did not cover:

1. **Filter and edge/clamp behaviour at level boundaries** as the pyramid is
   walked down and back up. The blur reads outside its level's extent at the
   edges; compare what each side does there.
2. **Per-level behaviour of the blur itself** — round two verified the
   Gaussian exponent, the amount floor, and the 7+7 tap count as named
   invariants, but not that the taps land on the same texels at every level.
3. The **upscale-replace leg** specifically. Round two verified the pyramid
   *structure* matches, and that `downsample_box` serves both legs, but the
   upscale is where a half-texel convention error would show up and it was not
   examined at that resolution.

**Explicitly out of scope for this task**, because it is measurement and it
stays on the reference machine: whether bloom fires, whether it is visible,
whether it is correct on screen, and any claim about the five-number signature
in brief 02. Report source divergence only.

**Do not re-report** anything in brief 02's "Properly eliminated by this
round" list: `gl_bloom_amount` is read and passed unchanged on both sides; the
compute-only alpha `1.0` is consumed by a fragment shader returning alpha
`0.0` under additive blend; the reference blur's nearest sampler versus the
native blur's linear cannot matter where all seven coordinates are texel
centres. Also do not re-report the level count, level-0 resolution, composite
weights, resample kernel, or pyramid structure — all eliminated in round two.

Run `python3 tools/check_shader_parity.py` first. Every `MATCH` is an
eliminated hypothesis, and it gained an invariant this round — the composite
V-flip — so a clean run now covers that too. Its limits are in its docstring
and they matter here: it checks named invariants, not semantic equivalence.
**A clean run does not mean the two paths render the same image.**

If you find something mechanically checkable, propose it as an invariant.
Do not broaden the script into token-set diffing; the docstring explains why.

---

## Task 3 — three long-open items, one report each, briefly

These have been open across several sessions with nobody assigned. They are
small. Give each a short verdict rather than a full treatment; the value is in
closing them or promoting them, not in depth.

1. **`BlitCurrentToImage`'s conversion branch has no reachable caller.** Is
   that true at this revision? If so, is the branch dead code to delete, or a
   path that *should* be reachable and is being missed? Note the recorded trap
   that `BlitCurrentToImage` silently changes vertical orientation — given the
   composite V-flip found this round, that neighbourhood deserves a look.
2. **Texture-upload cold-load instrumentation has never fired.** Added
   2026-07-15. Either the instrumentation is misplaced, the path is not taken,
   or the condition is wrong. Which?
3. **Round-1 Findings 11 and 12**, both still standing and both judged minor:
   `EnableMultisampling`/`EnableLineSmooth` are empty no-ops, and
   `ApplyDepthBias` carries an undocumented `128.0f` reverse-Z scale factor.
   For 11: does the reference actually do anything, so that the no-op is a
   real gap rather than a documented one? For 12: is `128.0f` derivable from
   the depth format and reverse-Z range, or is it a fitted constant? A derived
   number wants a comment; a fitted one wants a measurement.

---

## Not in this brief

- **All bloom and AO behaviour** — measurement, stays on the reference
  machine. The next hardware session runs the bloom re-capture against
  predictions already written down in `AGENTS.md`; do not try to anticipate
  its result, and do not adjust any prediction.
- The compute AO path's algorithm quality. Parity is the question, not whether
  the AO looks good.
- Anything Apple Silicon — untested everywhere, not auditable from source.
- Performance claims of any kind. `FrameGPU` from `mt_metrics` is the only
  valid cost signal and it requires the hardware.
- On-screen verification of `CLAMP_CAMTEX` and the `NOFILTER` sampler changes.
  Those rest on reading alone and are known to; they need a map with a camera
  texture whose UVs cross the edge, which is a capture task, not an audit one.
