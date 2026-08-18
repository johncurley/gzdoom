# Handoff to macOS — 2026-08-18

Written at the end of a Linux session, as the entry point for the next macOS
one — mirrors how `docs/handoff-linux-2026-08-17.md` was written from the
macOS side. `origin/metal-audit` is current; nothing is sitting on a local
branch. Start with `git pull`.

## Nothing here changes macOS behavior

This session's work was entirely Linux-side (an independent audit of the
native POSIX platform layer, item 14 in Tasks — Linux, plus process-hygiene
cleanup). One shared file changed — `libraries/ZWidget/src/core/theme.cpp`
gained a GTK/GNOME light-theme detection fix, but it's entirely inside the
`#if defined(UNIX) && !defined(__APPLE__)` branch and does not touch the
Cocoa theme path. Nothing else touched a file the Metal renderer, Cocoa
platform layer, or macOS build depends on. Confirm this yourself with `git
log --oneline --stat <last-macOS-commit>..HEAD -- src/common/platform/posix/cocoa
src/common/rendering/metal libraries/metal-cpp` if you want to double-check
before trusting that claim.

## Priority order for macOS — unchanged from Tasks — macOS, restated here

**Item 3, Apple Silicon validation, is the one that matters most and the one
blocking on hardware.** Everything Metal-side has been developed and measured
on a macOS 12.7 Intel Mac. TBDR vs IMR differences — memoryless storage, store
actions, `didModifyRange:` — mean Intel-correct code can be silently wrong on
M-series, and nothing here has ever actually run on Apple Silicon to find out.
This is genuinely gating, not just next on a list: everything downstream
(item 5's freeze investigation, and eventually the frame graph — see below)
needs a known-good Apple Silicon baseline to measure against, or a
correctness bug discovered later can't be attributed to the right cause.

**Item 5, intermittent freezing, is resolved to a cause but not a fix.**
`nextDrawable()` blocking below the renderer's control is confirmed with a
full evidence chain (`AGENTS.md` Tasks — macOS item 5) — drawable leak, late
retirement, nil timeout, and pool exhaustion are all excluded by
measurement. Late acquisition (Apple's recommended structure) reduced the
rate but did not remove it, because the constraint is in
`CAMetalLayer`/WindowServer, not this codebase.

The level-transition wipe was a candidate second explanation — `cl_capfps`
selects a ~25ms-per-tic pacing branch that makes a ~43-tic wipe take ~1.1s
by design, long enough to read as a freeze if it were presenting a static
frame instead of animating. **Confirmed by eye during the last macOS
tranche: the wipe animates correctly, not a freeze.** That explanation is
closed. Any freeze still reported is therefore a mid-play one, which is not
a new unknown — it's the `nextDrawable()` block above, still mitigated, not
eliminated. The open work on this item is closing that gap, not finding a
second cause.

**Item 4, SSAO attenuation residual, is smallest and lowest priority.** ~0.047
in occlusion units, the last open row of the Metal-vs-OpenGL parity table.
Worth closing but not worth delaying the above for.

## After Apple Silicon validation: the frame graph, not before

`docs/engine-modernization.md` Track A is the next durable-roadmap work once
item 3 has a real answer — not because of an arbitrary ordering, but because
the frame graph has to be correct across GL, Vulkan, *and* Metal on both Intel
and Apple Silicon, and there's no point designing or building it against a
renderer whose Apple Silicon behavior is still unknown. A frame-graph-level
bug and a TBDR-correctness bug would be genuinely hard to tell apart without
that baseline established first.

When that work does start, it's staged, not a single big change — see the
roadmap's "Near-term order" and `docs/frame-graph-resources.md` (a sketch, not
committed code): step one is a resource *registry* with no scheduler, no new
allocator, and no behavior change, specifically so the diagnostic value (what
resources are live, catching size mismatches like AO's quarter/full-res
rounding) can be had and verified before any compiler/scheduler logic is
written on top of it. Don't skip to the graph itself — the whole point of
that staging is to keep each step small enough to verify the way everything
else on this branch has been.

One thing worth carrying over from the Linux side's experience this session,
generalized: a plausible, well-reasoned mechanism is not evidence. This
session sank a full implement-rebuild-retest cycle into a fix for a bug that
turned out to be entirely outside the codebase (`AGENTS.md` item 13) before a
30-line reproducer not involving any of this project's code proved it
wrong. The frame graph work is exactly the kind of change where that lesson
matters more, not less — wide blast radius, still no automated test suite
beyond the matrix golden-image tool, and correctness that's easy to assert
and hard to actually check. Measure before believing, same as always.

## What this Linux session actually did, for context

Ran a fresh independent audit of the native POSIX platform layer (the same
methodology as the original 2026-08-12 one — a fresh agent, blind to
`AGENTS.md` until its findings were written, contract revised for the
current state of the tree). 7 findings; fixed the ones with real evidence and
low fix-risk (a genuine input-loss bug on X11 focus-loss, leftover debug
diagnostics including an unbounded `/tmp` log file, a GTK theme-detection
gap, and three silent GLX failure paths); left the rest — a narrow
already-mitigated `BadMatch` race, a possibly-latent state-machine gap, and
an EWMH timestamp nicety — documented rather than chased, after a genuine
attempt to build a non-EWMH test harness ran into a string of environment
problems unrelated to this codebase and stopped being worth the setup cost.
Full detail in `AGENTS.md` Tasks — Linux item 14 and
`docs/audits/findings-linux-2026-08-18.md`.
