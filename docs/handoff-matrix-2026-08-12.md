# Session handoff — the matrix suite made trustworthy

Written 2026-08-12, on macOS (Intel MacBookAir7,2), after pulling the Linux work
(`c22ae47bf..0deb2c8db`, 22 commits) into `metal-audit`.

Read `AGENTS.md` first for the measurements. This file is the narrative: what
changed, what was wrong before, and what is deliberately left open.

---

## The headline: the suite was reporting `ok` on a dead pass

`gl_ssao 3` changes **0.000%** of MAP12, the map the suite had been running.
The `ssao` must_differ_from relation was being satisfied entirely by RNG noise
from the status bar face, so it would have reported `ok` with the AO pass
completely dead — the one failure the suite exists to prevent, quoting its own
docstring. It only became visible after the face was removed.

The same hole existed on the `doom1` scene: `gl_bloom` changes 0.01% of E1M1
against a noise floor of 0.00%. Two scenes, two disarmed relations, both from
maps chosen by assumption rather than measurement.

**Do not choose a map for this suite by looking at it.** Both requirements have
to be measured: every pass must visibly act, *and* repeated launches must be
pixel-identical. Of eight Doom 2 candidates, exactly one satisfied both.

## Settled, with measurements in AGENTS.md

- **Capture nondeterminism is the status bar face.** `ST_updateFaceWidget` turns
  it with `M_Random` on an idle timer. Both previously-suspected causes (the
  shared `matrix.ini`, `gl_exposure_speed`) are **wrong** — measured, 8 samples
  per arm. Stock scenes now pin `screenblocks 12`.
- **Maps chosen by measurement.** doom2 = MAP06, doom1 = E1M3. E1M3 is the only
  map found in either IWAD that satisfies both requirements alone, so doom1
  needs no per-config override.
- **A config may carry its own map, keyed by scene.** No stock Doom 2 map does
  both jobs, so the bloom pair sits on MAP12 with its own `baseline_bloom`.
  `check_relation_maps()` hard-exits if a relation ever spans two maps.
- **Three silent failures now fail loudly**: a missing `-file` segfaulted in
  `Files.back()` before the console existed; a wrong IWAD loaded a fallback and
  captured the wrong game; a must_differ failure printed "PIXEL-IDENTICAL" for
  two different conditions.

## Do not repeat these

- **`localize.py` cannot see the status bar.** Its region is the central
  `(40,51)-(760,457)`. Two captures with different pixel hashes localize as
  *identical, zero differing pixels*. This is what sent earlier investigations
  toward "flake" twice.
- **Driving the binary by hand mixes resolutions.** The first launch against a
  fresh config captures at 1152x720, not the pinned 800x572 — `win_w`/`win_h`
  apply after the window is sized. `run.py`'s warmup covers it; a hand-rolled
  loop does not, and the mixture reads as "more nondeterminism". It produced a
  wrong intermediate finding here (that exposure mattered).
- **Fetch every remote before comparing against one.** A stale `zwidget` ref
  made the subtree look like it had drifted; the fix was already published.

## Correction to a commit message

`6ebd03671` says a wrong-IWAD run "reaches the harness as NO CAPTURE". **That is
wrong.** With no map loaded the engine falls to the title screen and plays
attract demos, which are `GS_LEVEL`, so `shotafter` fires and captures a frame
of the wrong game. The suite would have compared those and reported ordinary
DIFFs. The guard added in that commit is worth *more* than its message claims.

## Open

- **`crossbackend.py` has not been looked at.** It shares `configs.json` and
  knows nothing about per-config maps or `screenblocks`. (It *does* take
  `--scene`; read 2026-08-12, the specific gap is that `crossbackend.py:158`
  calls `matrix.launch()` without the `scene` argument, so the bloom trio's
  scene-keyed `MAP12` is dropped and those configs run on MAP06, where bloom
  changes nothing. Written up as a Linux task in `AGENTS.md`.) It is also the
  check most worth investing in: two independent implementations agreeing is a
  stronger invariant than one implementation agreeing with its own past, and it
  needs no baseline file and no determinism across time.
- **Whether to keep `baseline.json` as a gate.** Undecided. Its true positives
  this session were zero; the relations did all the work. Note `baseline.json`
  holds one scene at a time by design, so doom1 has no golden image.
- **`shotafter` counts rendered frames, not tics.** Measured: `maptime` is 121
  on some launches and 122 on others. This is real but is *not* what makes
  pixels differ — at the same tic, two states still occur. Something ticks that
  `maptime` does not index. Unresolved.
- **A residual in-scene element** at `x[455..458] y[249..267]` on MAP12 (a green
  torch, ~1 sample in 8) survives `screenblocks`. Bloom amplifies it, which is
  why the bloom trio is `relations_only`.

## Every measurement here is from one machine

All of it was measured on the Intel MacBookAir7,2 (HD 6000), on Metal. The code
changes are platform-neutral, but the *choices* they encode are not proven to
be: `screenblocks 12`, MAP06 and E1M3 were each selected from measurements taken
here. The Linux box may have a different noise profile — different GL driver,
different timing — and if a map that is 8/8 pixel-identical here is not there,
that is information about the engine, not a broken suite. Re-run the two
measurements (`--only baseline --scene <name>`, eight samples; and the pass
scan) before trusting a scene on other hardware.

The same applies in reverse to the noise floor quoted throughout: 0.00% is what
this GPU produced, not a guarantee.

## Not covered here

The standing items — removing the input diagnostics (`in_keytrace`,
`ZWIDGET_TRACE_REPEAT`), X11 raw input via XInput2, `check_shader_parity.py`
into CI, and Apple Silicon validation — are untouched by this session and live
in `CLAUDE.md` under Outstanding. This file is the matrix suite only.

## Parked, not abandoned

`upstream-gl-blackframe` (one commit off `master`) and
`0001-gl-clear-the-cached-active-shader-...patch` hold the GL black-frame fix
rewritten to stand alone. `cocoa-modal-fixes` (2 commits on `zwidget/master`)
builds clean against current dpjudas master with no new warnings — verified, 12
warnings before and after, all pre-existing OpenGL deprecations. Whether any of
it is offered anywhere is undecided and is not a task.
