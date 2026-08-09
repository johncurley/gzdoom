# Contributing

Contributions are welcome, from humans and from AI agents. There is one rule
that matters more than the rest, and it is unusual enough to state first.

## A change is not done until something measured it

Not "it compiles". Not "it looks right". Not "the code clearly does X".

This project has a long history of confident, well-reasoned conclusions that
were wrong — a broken postprocess pass that matched its own golden image
perfectly because the baseline had recorded the broken frame; three separate
"the launcher is fixed now" claims that changed nothing observable; several
hypotheses disproved twice because nobody wrote down that they were dead the
first time. `docs/history/agent-log.md` is largely a record of this.

So the standard here is:

**1. State a numeric prediction before you look.** "Rows 380-620 should move
from -25 to within a few units of the reference." Every real defect found on
this branch was caught this way; several careful readings of the source were
not.

**2. Measure against something.** OpenGL is the reference implementation the
Metal backend was written against. `tools/matrix/crossbackend.py` compares them
frame-by-frame and reports the *shape* of any difference, which is what
identifies the cause — a uniform delta is backend noise, one band differing is a
vertical offset, half the frame is a viewport bug.

**3. Prove your measurement can detect the defect.** This is the step people
skip. Before claiming a fix works, run the same measurement with the fix
disabled and confirm it *fails*. A test that passes both ways proved nothing.
When SceneFog was verified, the control showed 91.4% of pixels differing without
the fix and a 1-LSB maximum with it — that pair is the evidence, not the second
number alone.

**4. Check the effect is larger than the noise.** Take two readings of an
identical configuration first. Captures here are byte-deterministic for a fixed
config, so pixel noise should be zero; frame timing has a ~0.5ms floor on the
reference machine. And check the effect under test is bigger than unrelated
differences — in every scene tried, the Metal-vs-OpenGL background difference
was larger than the entire visual effect of SSAO, so a naive comparison of the
shipped frame proves nothing about SSAO at all.

If you cannot measure it, say so plainly in the PR. "I believe this is correct
but could not verify it because I have no Apple Silicon hardware" is a useful,
honest contribution. A confident claim that turns out to be untested is not.

## Before you start

Read `AGENTS.md` — specifically the traps section. Most of it is about the build
and cache pipeline silently serving you stale artifacts:

- The engine loads the `.pk3` **next to the executable**, not `build/gzdoom.pk3`.
- A shader edit — or a shader *revert* — is not live until `zipdir` runs.
- The MSL cache and `mt_pipelines.bin` will both happily serve you yesterday's
  shader. Clear them.
- Archived CVARs leak between runs. `vid_preferbackend` in particular will
  silently turn a "Metal" test into an OpenGL one.

Then grep `docs/history/agent-log.md` for whatever you are about to investigate.
It is 6,000 lines and nobody expects you to read it, but it records what has
already been disproved, and that is cheaper to find than to rediscover.

## Tooling

```bash
python3 tools/matrix/run.py                 # golden-image regression, 11 configs
python3 tools/matrix/run.py --update-baseline
python3 tools/matrix/crossbackend.py        # Metal vs OpenGL oracle
python3 tools/matrix/crossbackend.py --selfcheck
python3 tools/localize.py a.png b.png       # where and in which direction
```

Both harnesses use their own config file and a pinned window, so they cannot
contaminate your `gzdoom.ini` or drift when your display changes. `run.py`
refuses to record a baseline while any pass is broken — that guard exists
because a baseline recorded from a broken frame makes the bug permanent and
invisible.

`tools/pngdiff.py`, `localize.py` and `cluster.py` decode PNGs with the standard
library only; the development machine has neither PIL nor ImageMagick.

## Platforms

Changes are verified on what the maintainer can actually run: **macOS (Intel)**
and **Windows 10**. Linux/BSD and Apple Silicon are covered by CI and by
contributors. If your change touches a platform nobody here can test, say so —
it will be merged on that understanding rather than on a false assurance.

## ZWidget is a subtree

`libraries/ZWidget` tracks a fork of dpjudas/ZWidget. Do not hand-edit it and
leave it there — fixes that are not specific to this fork should go upstream:

```bash
git subtree pull --prefix=libraries/ZWidget zwidget <branch> --squash
git subtree push --prefix=libraries/ZWidget zwidget <branch>
```

A previous plain-directory copy let three lineages drift until eight API
families had silently diverged, none of which surfaced until the halves were
compiled together.

## Commits

Explain **why**, and include the measurement. A commit message here should let
someone six months later know what was tried, what the numbers were, and what
was ruled out — the commit log is the primary record, and it is used that way.
If a commit corrects an earlier claim, say so explicitly rather than quietly
changing it.

## AI-assisted contributions

Explicitly welcome. Agents have done substantial work in this tree and the
documentation is written with them in mind.

Two conditions:

**You are responsible for what you submit.** By opening a pull request you
certify that you have the right to contribute the code under the GPL v3 and that
you have reviewed it. How it was produced is your business; whether it is
correct and licensable is yours to stand behind. This is the usual
[DCO](https://developercertificate.org/) position and it is not special to AI.

**The verification standard above applies unchanged.** Agents are, if anything,
more prone to confident wrong conclusions — the history in this repository is
substantially a record of that. Generated code that has not been measured
against the reference will be asked for measurements, not merged on plausibility.

If you are an agent reading this: `AGENTS.md` is your starting point, the traps
section will save you hours, and the single most useful habit is refusing to
believe your own fix until a control run says it failed without it.
