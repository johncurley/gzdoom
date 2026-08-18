# Handoff: frame graph resource registry — 2026-08-18

Written at the end of a long Linux session (item 14's audit tranche, the UDB
companion-project scoping) as the entry point for whoever picks up gzdoom
next, on any platform. `origin/metal-audit` is current.

## The decision this handoff exists to record

**Apple Silicon hardware is not in hand yet** (the plan is a cheap/broken-
screen M1 Mac, timeline uncertain — see `docs/handoff-macos-2026-08-18.md`
for the full macOS priority order, unchanged by this doc). Rather than either
a full hiatus or starting broad engine-refactor work speculatively, the
scoped-in next step is:

**Start `docs/frame-graph-resources.md`'s resource registry — and stop
there.** That document is a sketch, not committed code; turning it into a
real, reviewed implementation is the actual task. It's staged specifically
to need nothing Metal-specific: no scheduler, no memory aliasing, no
behavior change, pure declarative bookkeeping over what resources exist and
their size rules. Fully verifiable on Linux via the existing matrix suite
(`tools/matrix/run.py`, `tools/matrix/crossbackend.py --backends gl,vulkan`)
— no GPU capture, no Apple hardware, nothing this machine can't already do.

**Do not go past the registry.** The actual graph/scheduler, and
specifically anything doing memory aliasing or barrier placement on Metal,
needs Apple Silicon validation first — that's where TBDR-vs-IMR risk
actually lives (memoryless storage, store actions; see
`docs/engine-modernization.md`'s "Targets and principles" and item 3 in
Tasks — macOS). Building scheduling logic against an unvalidated Metal
target is exactly the kind of speculative work this branch's whole
discipline has been built around avoiding — see item 13 in Tasks — Linux for
a fresh example of what that costs when skipped: a full implement-rebuild-
retest cycle on a plausible, well-reasoned fix for a bug that turned out to
be entirely outside this codebase.

## Concrete starting point

`docs/frame-graph-resources.md` §"Shape" has the sketch: a backend-neutral
`ResourceFormat`/`SizeRule` description, backend-owned memory, no allocation
in phase 1 — the registry records what the backend already made, keyed by a
stable name. Three things it buys immediately, already argued in that doc
and worth re-reading before starting:

1. A memory answer — this fork's reference machine has a 1.5GB integrated
   GPU and currently cannot say how much of it a frame's targets occupy.
2. Size-mismatch validation at declaration time — the AO quarter-res/full-res
   rounding mismatch (773 vs 776 both rounding to 194 rows) that nobody could
   check today because nobody holds both numbers at once.
3. A proof-of-execution instrument — "which resources were touched this
   frame" answers "which path executed" structurally, which is exactly what
   the compute AO path has lacked all along per `renderer-methodology.md`.

Read `docs/frame-analysis.md` first if it hasn't been re-read recently — it's
the actual pass/resource dependency map this registry needs to match, not a
design done in isolation from what the renderer currently does.

## What else is open, for completeness

- **Linux**: `AGENTS.md` Tasks — Linux is fully closed as of item 14
  (2026-08-18's audit tranche) except three findings deliberately left
  reasoned-not-verified (2, 3, 4 — narrow, none live-broken under any WM
  anyone actually runs) and items 10/11 (publishing two ZWidget fixes
  upstream — the user's own task, separately).
- **macOS**: see `docs/handoff-macos-2026-08-18.md` in full. Item 3 (Apple
  Silicon validation) gates everything downstream; item 5's freeze cause is
  found and mitigated but not eliminated (the wipe explanation is closed —
  confirmed animating correctly, not a freeze); item 4 (SSAO residual) is
  small and lowest priority.
- **UDB companion project**: scoped, not started. Separate repo,
  `~/Projects/desktop/udb`, planning docs only as of this session — see that
  repo's own `CLAUDE.md` and `docs/planning-2026-08-18.md`. Not part of this
  repo's task tracking; mentioned here only so the connection (ZWidget
  dogfooding, shared license family) isn't lost.
