# Session handoff — the AO session: three suspects dead, one new bug found

Written 2026-08-16 on macOS (Intel MacBookAir7,2, HD 6000, macOS 12.7.6), on
`metal-audit`. Entry point for the next session on the Metal AO work.

`AGENTS.md` carries the measurements. This file is the narrative: what was
settled, what was found, and what must not be repeated.

---

## What was closed

**macOS item 1 — `crossbackend.py` under Metal.** 12/12 OK on MAP06, self-check
REPRODUCIBLE on both backends. No regression from `e1f47ce5b`. Its two
Metal-reaching changes are viewport-shaped and would have shown as a half-frame
or displaced band; nothing of that shape appeared.

**macOS item 2 — Vulkan on Apple.** Decided by the user: auto-detect on the
platforms that want it, explicit `-DHAVE_VULKAN=ON` on Apple, off by default,
because Vulkan-through-MoltenVK is a reference platform there and defaulting it
on defeats the point of the Metal backend. This also fixed a live breakage —
the committed tree could not be configured from scratch on macOS at all.

**The `~2x` premise for the Intel compute-AO guard.** Retired. Both AO paths
cost the same: +0.82ms compute, +0.78ms reference, 0.31ms floor.

---

## What was found: the compute AO bistability

**This is the most important thing in this file and it is unresolved.**

The same command line produces two stable, reproducible regimes:

| regime | vs no-AO | what it means |
|---|---|---|
| **0.31%** of pixels | mean 0.130 | AO essentially absent |
| **14.90%** of pixels | mean 1.526 | AO clearly present |

Both reproduce: the 0.31% regime survived a two-launch byte-identical gate, and
the 14.90% regime ran 8 launches in a row unchanged. Something selects between
them and it was not found.

**Refuted as the cause, each by measurement — do not re-test without new
evidence:**

- the AO algorithm cvar — 0, 1 and 2 all behave alike (14.8-14.9% in that regime)
- the distance fade — `fade 100/500` and `1000/8000` both give 14.90%
- launch ordering — compute-after-reference is present in one script, absent in another
- the archived `gl_ssao` value — it was already 3 during inert runs
- the PSO binary archive — clearing it made no consistent difference
- stale capture files — verified by mtime; every PNG was written after its launch

Deleting `matrix.ini` restored the 14.90% regime once. The only diff between the
inert and fresh inis was the fade pair, and pinning those did not reproduce it.

**Until this is understood, no quality verdict on the compute path is safe in
either direction.** The measurements in `AGENTS.md` are all from the 0.31%
regime, because that is what the settled two-launch gate produced.

## The visual A/B, and the operator's read

Published artifact: three frames (no AO / reference PP / Metal compute) plus
12x-amplified difference maps against the no-AO control.

- reference PP vs no-AO: mean 0.710, **4.80%** of pixels
- Metal compute vs no-AO: mean 0.130, **0.31%** of pixels

**Operator's judgement, 2026-08-16: the PP path is cleaner.** That is a read of
the *stills*, and it is recorded as such.

**In-game appearance is NOT assessed and a still cannot assess it.** A fixed
viewpoint says nothing about temporal stability, which is where screen-space AO
actually fails — grain that crawls under motion, flicker as samples change
between frames. This branch has history there: the AlchemyAO grain regression
was a temporal artefact fixed with forced 4x blur passes. So a path that looks
clean in a screenshot can still be unusable in motion, and vice versa. Judging
this needs someone playing the game, not the harness.

**The default was therefore left alone.** Flipping `mt_postprocess.cpp:531` is a
one-line change and remains available, but nothing measured this session
justifies it: the cost argument for the guard is dead, and the quality argument
against flipping is now stronger than the cost argument ever was.

---

## Do not repeat these

**Do not measure compute AO with an ad-hoc launcher.** Frames swung between
AO-sized differences (~15%) and exposure-sized ones (60%, max channel delta 160)
until the matrix suite's own discipline was adopted: discard the first launch
after any configuration change, then require byte-identical captures across two
launches per arm. Every trustworthy number this session came from arms that
passed that gate; several hours of readings before it did not.

**Do not pass `mt_compute_ao_*` on the command line and then forget it.** They
are `CVAR_ARCHIVE`, so a command-line value is written to the shared
`matrix.ini` and silently changes every later run. This session contaminated its
own measurement that way with `mt_compute_ao_fade_*` — the exact trap already
documented in `AGENTS.md`, walked into anyway.

**`Printf(PRINT_LOG, ...)` at shader-compile time reaches no log.** Shaders are
compiled before the console exists. A diagnostic there must use `stderr`, as
`in_keytrace` does. Two experiments returned a convincing PIXEL-IDENTICAL
verdict while the patch under test was not running at all — once behind an
`mt_debug` gate, once because of this.

**A `.gputrace` is a directory and its encoder labels are greppable.** No Xcode
needed for structural questions:
`grep -ao "PP [a-z]*: [a-z0-9_/.]*" frame-<stamp>.gputrace/capture | sort | uniq -c`

**There is no per-pass GPU timing on this machine.** `mt_caps` says *"Stage
counter sampling: no"*, and Xcode's counters return no data — checked in the GUI.
`CLAUDE.md` claimed the opposite and has been corrected. Use differencing:
`Frame avg(feature on) - Frame avg(feature off)`, several reps, interleaved.

---

## Open, in the order I would take them

1. **The bistability above.** Everything else about compute AO is blocked on it.
   The next measurement I would take is an `mt_aoprobe` run in each regime —
   that probe exists for exactly this class of "AO composited nothing" defect
   and was never pointed at this.
2. **In-game assessment of both AO paths**, by playing, watching for temporal
   grain rather than looking at stills.
3. **The SSAO residual** (~0.047 occlusion units). Compute kernel, `fast::`
   precision and NaN-in-guards are all ruled out with proof of execution.
   `LinearDepthTexture`'s sampler state is the last unmeasured item from the
   original lead — but three dead suspects raise the odds the figure is an
   artefact of the AO-isolation method, so re-derive the measurement first.
4. **Apple Silicon (item 3)** — still blocked on hardware. Note the compute path
   is the *default* there, so an M-series tester will see AO differing from every
   Intel figure in the table before changing anything, and should pin
   `mt_compute_ao 0` for the first comparison.

## Housekeeping

Three GPU traces in `~/Documents/GZDoom/gputrace/` (~317MB each) can be deleted;
their label results are recorded. `mt_msl_precise_math` is a permanent
diagnostic, default off, not archived — its stderr proof line must not be
quietened.
