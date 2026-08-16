# Measuring a renderer: method

How to find out whether a rendering change did what you think it did, on this
fork, with these instruments. Written 2026-08-16 after a session that produced
three false negatives from experiments that were never running, several hours of
readings from an unsettled rig, and a benchmark config that tested the wrong code
path while reporting success.

Every rule below is here because breaking it cost real time. The examples are
kept because a rule without its incident gets optimised away by the next person
who finds it inconvenient — including you, six months from now.

This is the *method*. `CLAUDE.md` has the commands, `AGENTS.md` has the current
measurements, `docs/gpu-capture-protocol.md` is the capture runbook.

---

## 1. The three prime directives

**Predict before you look.** Write the number you expect, then measure. Every
real defect found on this branch was caught this way; several confident readings
of the source were not. A capture or a diff shows so much that it is trivial to
find a story in it afterwards, and that story will be wrong in a way you cannot
detect from inside it.

**Prove the code ran before you believe the result.** A negative result from an
arm that never executed is indistinguishable from a real negative. This is the
single most expensive failure mode in this repository — see §4.

**A measurement is only as good as its control.** Two captures of different
things compare fine and mean nothing. Before interpreting any delta, be able to
say what the *only* difference between the arms was, and prove it.

## 2. What each instrument can and cannot answer

Reach for the cheapest instrument that can answer the actual question, and know
what it is blind to. Nothing here answers "is it correct" — only "is it different
from a reference", which is why the reference matters more than the tool.

| instrument | answers | blind to |
|---|---|---|
| `Frame ... avg=` (`mt_metrics`) | did the frame get slower, overall | which pass; anything that only happens in motion |
| `FrameGPU` | **nothing you should quote** | it reports spans that cannot coexist with the measured frame interval (~220ms against 7ms frames, 2026-08-07). Cause unresolved; the obvious fix is a no-op |
| `ComputeCPU` / `PPCPU` labels | **which code path ran** — their best use | cost. They are encode time, not GPU time |
| `mt_frametrace <seconds>` | hitching, during actual play: p50/p95/p99, >33ms and >100ms counts | anything you do not walk through; Metal only |
| pixel diff (`pngdiff`, `localize`, `cluster`) | did the image change, where, in which direction | whether the change is *correct*; and `localize.py` cannot see the status bar — its region is the central `(40,51)-(760,457)` |
| `crossbackend.py` | does Metal disagree with the GL reference, and in what *shape* | a bug both backends share; it finds divergence, not correctness |
| `run.py` + relations | did a pass stop running at all | whether the baseline it compares to was ever right |
| GPU frame capture | what the GPU actually did: bound PSO, blend, stencil, load actions, every input texture | **cost, on this machine** — no stage counter sampling on the HD 6000, and Xcode's counters return no data (checked in the GUI, 2026-08-15) |
| a person playing the game | temporal grain, hitching, "is this pleasant" | everything quantitative |

Two entries deserve emphasis because they were learned the hard way:

**Per-pass GPU cost does not exist on this hardware.** `mt_caps` reports *"Stage
counter sampling: no  <- per-pass GPU timing gate"*. `CLAUDE.md` used to claim an
Xcode capture's per-encoder timing was the trustworthy fallback; that was never
verified and is wrong. Apple Silicon has the counters and will lift this.

**The benchmark harness cannot see hitching at all.** Measured 2026-08-16: a
configuration that froze constantly in gameplay reported avg 5.5ms, max 90.6ms,
4 stalls — indistinguishable from the smooth reference path. A settled viewpoint
never exercises what a renderer does badly while the camera moves. This is why
`mt_frametrace` exists and why §8 is not optional.

## 3. Running an experiment

1. **Write the prediction**, including what each outcome would mean. Predict the
   outcome that costs more work too, so you are not incentivised toward the
   convenient reading.
2. **One variable.** If you must pin several cvars, pin them in *both* arms.
3. **Cvars on the command line, one launch per configuration.** A console change
   does not survive a restart, and an ini value silently restores it.
4. **Discard the first launch after any configuration change.** It runs cold
   shader/PSO compilation, and `win_w`/`win_h` apply *after* the window is sized,
   so the first capture can come back at the wrong resolution entirely.
5. **Two launches per arm, byte-identical, before comparing arms.** If an arm
   cannot reproduce itself, its differences from another arm are noise wearing a
   finding's clothes.
6. **Establish the noise floor from the data you just took** — the worst
   within-arm spread. A delta smaller than that is not a result.
7. **Interleave arms** (A,B,A,B) so thermal drift is shared. The reference
   MacBookAir7,2 throttles under sustained load; identical configs drift over a
   long session.
8. **Check the label**, every launch. See §4.

## 4. Proof of execution — the gate that matters most

Before reading any result, prove the thing you were testing actually ran. Three
incidents, all on 2026-08-15/16:

- An experiment stripping `fast::` intrinsics returned a clean `PIXEL-IDENTICAL`
  twice while the patch was **not running** — first because its proof line sat
  behind an `mt_debug` gate, then because `Printf(PRINT_LOG, ...)` at
  shader-compile time is emitted before the console exists and reaches no log.
  Diagnostics on that path must use **stderr**, as `in_keytrace` does.
- The suite's `bloom_compute` config passed `+mt_compute_bloom 1` but not
  `+mt_compute_bloom_intel 1`, so the Intel gate routed it to the **reference**
  path. It compared reference against reference and came back byte-identical —
  which reads exactly like "the two paths agree".
- Sweeping `mt_compute_ao 1` against `0` produced bit-identical frames because
  the Intel guard forces compute off regardless; the cvar could not do anything.

The pattern: **byte-identical results and "no difference" findings are suspicious
by default.** They are what both a perfect agreement and a dead experiment look
like. Distinguish them before interpreting:

- a label from `mt_caps`/`mt_metrics` naming the *resolved* path, not the cvar
- a stderr line from inside the code path under test
- a count of work done (shaders patched, pixels dispatched)

Prefer a proof that a *harness* can read. If proving it needs an operator at the
keyboard, it will not be checked on the run that matters.

## 5. Contamination

Things that silently make two "identical" launches different:

- **`CVAR_ARCHIVE` cvars written by your own experiment.** Passing
  `mt_compute_ao_fade_*` on a command line wrote them to the shared ini and
  changed every later run in the session. Walked into on 2026-08-16 by the person
  who had documented the trap.
- **`vid_preferbackend` is archived** — an unspecified backend inherits the last
  launch's, which silently invalidates Metal-vs-GL comparisons.
- **The PSO binary archive** (`~/Library/Application Support/zdoom/cache/mt_pipelines.bin`)
  can serve a pipeline the current source never built. Clear it before captures.
- **The translated MSL cache** serves the previously translated shader. A patch
  applied *before* the cache is invisible on a cache hit — apply post-cache, at
  the compile choke point, or clear it.
- **Two pk3s.** The running app loads the one beside the executable. Updating the
  other leaves the engine running the old shader with no error and no sign; a
  correct AO fix was measured as a failure this way.
- **A stale CMake cache** can hide that the *committed* tree does not configure.
  The stash-and-build check does not catch it; only configuring a throwaway
  directory does.
- **`gzdoom.ini` from earlier experiments.** Non-default cvars produce artifacts
  indistinguishable from broken code. An entire AO investigation resolved to
  config.

Corollary: **use a dedicated config file** for measurement runs, and for
gameplay tests use a *copy* of the real one so the test cannot mutate it.

## 6. Techniques

**Differencing, for a pass cost you cannot time directly.** When per-pass
counters do not exist, subtract the same frame with the feature off:

```
pass cost = Frame avg(feature on) - Frame avg(feature off)
```

Several reps per arm, interleaved. This removes the unrelated frame work the pass
is hiding inside — at ~200fps on a small window, a 0.7ms pass is invisible in the
total and obvious in the difference. Resolution matters: the same AO difference
that resolves at 1600x776 does not clear the noise at 800x600.

**Shape, not magnitude, for cross-backend differences.** Two backends never agree
byte for byte; filtering, dithering and float precision produce small *spatially
uniform* noise. So the question is never "is there a difference" but "what shape":
a uniform low delta is expected; one horizontal band is a vertical offset or flip;
half the frame is a viewport or scissor bug; delta only where an effect appears
means that pass differs. This is why `crossbackend.py` reports per-band numbers
instead of one figure — a whole-frame mean hides a displaced effect, because the
pixels that gained and lost average out.

**Ratios need an absolute floor.** A band 69x the median sounds decisive; it was
0.035 of 255 on 0.01% of pixels, against bands that agreed almost perfectly. Any
"N times the baseline" test needs a minimum absolute magnitude ANDed in — as a
gate that can only suppress, never as a retune of the ratio, which would stop the
check running at all.

**Assert identical dimensions before comparing two captures.** Measured 2026-08-16:
frames rendered at 1600x773 and 1600x776 — the same requested window, on the same
machine — differ by **14.90%** of pixels, while two frames at the same size differ by
0.03%. A viewport three pixels taller re-renders the scene; the images are not
misaligned, they are *different renders*. Several scripts here zipped flat pixel
arrays without checking, and the artefact was mistaken for a renderer state for a
whole session. `zip()` silently truncates to the shorter array, so this fails quietly.

**Amplify differences for the eye, state the gain.** A 12x-amplified absolute
difference map against a control makes "where does this pass act" obvious at a
glance. Always label the gain; an unlabelled amplified diff is a rumour.

**Read artifacts directly when you can.** A `.gputrace` is a directory, and its
encoder labels are greppable without Xcode:
`grep -ao "PP [a-z]*: [a-z0-9_/.]*" frame-<stamp>.gputrace/capture | sort | uniq -c`
That answered "which passes ran in each AO path" in seconds. But verify the
*absence* of a label means something — a positive control (enable another pass,
confirm its label appears) distinguishes "not present" from "not serialised".

## 7. Negative results are results

Record refuted hypotheses with the same care as confirmations, in the durable
docs, with the measurement that killed them and the words **do not re-test without
new evidence**. The SSAO residual has consumed multiple sessions partly because
each one re-derived the same dead ends.

Three specific habits:

- **Cite the commit, not the date.** "Fixed on <date>" written from the machine
  that made the fix is not evidence the fix is in the branch. This branch has
  published a feature upstream with its second half uncommitted.
- **Record what a result does *not* establish.** "Both paths cost the same at
  frame level" is not "both paths cost the same"; "the counters show nothing" is
  not "there is no cost difference".
- **Bound the claim.** With a 0.42ms floor and a 0.7ms pass, you can exclude a 2x
  difference but not a 1.2x one. Say which.

## 8. Know when to stop measuring and play the game

Some defects are structurally invisible to every instrument here. Two, both
confirmed on 2026-08-16 and both found in minutes of play after hours of
measurement:

- **Temporal grain** — screen-space AO that looks clean in a still and crawls
  under motion. A fixed viewpoint cannot show it.
- **Hitching** — a configuration that froze constantly while the harness reported
  it healthy on every number it produces.

So: **any change to a path the player looks at gets played before it is judged.**
Measurements decide *what* changed and by how much; they do not decide whether it
is acceptable. Run `mt_frametrace 5`, play for a minute, read the terminal — that
converts "feels bad" into p99 and a >100ms count, which is a finding rather than
an impression.

## 9. Choosing the scene

A benchmark scene has two independent requirements, and **both must be measured,
never assumed**:

1. **Every pass must visibly act on it.** `gl_ssao 3` changes 0.000% of MAP12 —
   the map the suite ran on for months, satisfying its relation entirely on RNG
   noise from the status bar face. It would have reported `ok` with the AO pass
   completely dead: the one failure the suite exists to prevent.
2. **Repeated launches must be pixel-identical.** Of eight Doom 2 candidates,
   exactly one satisfied both.

Scene choices are **not portable**: the current ones were selected from
measurements on one machine, on one backend. Re-run both measurements before
trusting a scene on other hardware. If a map that is 8/8 identical here is not
there, that is information about the engine, not a broken suite.

---

## The short version

Before believing any renderer measurement:

- [ ] Prediction written down first, including what the expensive outcome means
- [ ] One variable; everything else pinned in both arms
- [ ] Cvars on the command line, one launch per configuration, dedicated config file
- [ ] First launch after a config change discarded
- [ ] Each arm reproduces itself byte-identically
- [ ] Noise floor taken from within-arm spread; delta clears it
- [ ] Arms interleaved; machine allowed to cool
- [ ] **A label proves which code path actually ran**
- [ ] Caches considered: PSO archive, MSL cache, pk3, ini, CMake
- [ ] Claim bounded — what it establishes, and what it does not
- [ ] If a player would see it: played, with `mt_frametrace` running
