# UDMF change qualification — 2026-11-17

Status: built and qualified on Intel GL/Metal, **uncommitted**.
The optional lookup is retained following the allocation/timing trial below.
See the resumed
validation below; the original waiting-state description is retained for context.
The maintainer approved adding fixes while LLVM compiles and asked how to
handle verification/rollback. The chosen approach is small uncommitted edits,
separate patches and explicit pending gates. No build, native game launch or
benchmark was started for these edits. The existing executable was not rebuilt.
Prior passing bounds/scanner tests do not qualify the new source state.

## Changes

1. In both ParseSector textual scroll-mode branches, the physical test is now
   an else-if. This preserves the selection of both/visual instead of replacing
   it with zero, while retaining physical/unknown behavior. The reviewed
   correction was approved in the maintainer's request to add the fixes.
2. ProcessLineDefs now creates and fills siderefs only if UDMFWallScrollers is
   nonempty. All wall-scroller definitions already exist at that point. The
   populated lookup path and source bounds remain intact. The allocation and
   stage-timing qualification is recorded below.

Only these source edits were added to src/maploader/udmf.cpp in this tranche.
Existing scanner/input-reader and sidedef-bound changes remain intact. No
renderer, sound-worker, direct-record or shared-container change was made.
The source-review and direct-record documents describe further candidates,
not additional implementation.

## Preserved local patches

/private/tmp/gzdoom-udmf-pending-20261117 contains:

- before.cpp: working source immediately before these two changes, including
  the earlier uncommitted input/scanner-facing and bounds work.
- scroll-fixed.cpp: source after only the scroll correction.
- after.cpp: source after both changes.
- 01-scroll-modes.patch and 02-unused-side-refs.patch: separate source patches.
- combined.patch: exactly these two changes, not the complete diff from HEAD.
- manifest.json: source/patch SHA-256 identities and pending status.

The combined patch passed git apply --reverse --check against the edited
working source. No rollback was performed. The scoped rollback, if later
needed and after checking for intervening edits, is:

~~~sh
git apply --reverse --check /private/tmp/gzdoom-udmf-pending-20261117/combined.patch
git apply --reverse /private/tmp/gzdoom-udmf-pending-20261117/combined.patch
~~~

The individual patches allow reverting either change independently. Never use
a whole-file git restore or reset to undo these edits: that would also discard
earlier pending work. Temporary patches may disappear; the durable change list
and qualification requirements are recorded here, and the normal working diff
also contains the edits.

## Gates after LLVM finishes

- Run the mandatory build and verify the executable-adjacent gzdoom.pk3 and
  required game_support.pk3. Keep compiler/configuration fixed across A/B runs.
- Verify floor and ceiling textual both/visual/physical/unknown modes with
  nonzero speeds, mixed case, numeric aliases and repeated properties. Require
  appropriate live texture motion and physical carry; original controls must
  detect the both/visual behavior difference. See the source review's contract.
- Run no-wall-scroll and wall-scroll fixtures, including shared source sides
  and per-part/global scroll combinations. Require identical geometry and
  scroller mapping for the populated path. Exercise Metal and OpenGL.
- Re-run the permanent sidedef and scanner/input checks and relevant native
  resource/image controls. Python checks support the tooling but cannot replace
  native parser/scrolling validation. No demo recording is assumed.
- For the allocation trial, keep the scroll correction enabled in **both**
  arms (scroll-fixed.cpp versus after.cpp). Verify actual siderefs allocation
  counts and the retained MAP09 source identity. Compare interleaved unprofiled
  stage timings only after LLVM ends and thermal/load conditions settle. The
  old on-disk executable lacks the scroll correction and is not that isolated
  timing control. No benchmark gain is required for the independent behavior
  correction; retain an optimization only with relevant evidence.
- Remove temporary probes and complete final production checks before commit.
  Keep changes reviewable separately. No commit/push was requested or made.

## Resumed validation after LLVM completed

Artifacts: `/private/tmp/gzdoom-resume-20261117-224637`. Source diff, source and
old executable hashes were preserved before the build. The default build passed
with `/usr/bin/c++`, RelWithDebInfo, Vulkan disabled. All five adjacent packages
match the build outputs. The eight regression-tool and thirteen profiling-tool
Python checks pass; existing pngdiff unclosed-file warnings remain.

`tools/regression/udmf_scroll.py` generates a real map and observes live texture
movement and actor carry after twenty game ticks. All 28 floor/ceiling cases
pass on Intel GL and Metal, with identical reported results. Cases cover both,
visual, physical and unknown strings, mixed case, numeric aliases, and repeated
properties in both orders. The preserved original binary fails ten of the 28
cases (both/visual text and repeated visual), while numeric and physical controls
continue to pass. All three runs use byte-identical fixture packages. The old
binary also predates the bounds fix; these rooms use valid source indices.
An initial harness attempt exited before twenty ticks and was rejected for
missing results; capping simulation rendering and delaying exit fixed the
verification setup, without changing engine code.

The same fixture exposed a separate pre-existing wall-part defect. Four lines
share one source side with global X=0.25, top X=0.5 and middle Y=0.75. Expected:
top X twice bottom X, middle Y only, all four decompressed copies identical.
Actual on original/current GL and current Metal: every part moves by
`(14.25, 14.25)` after nineteen scroller ticks. In `ParseTextMap`, the wall
`CreateScroller` call omits `scroll.where`, defaulting to `scw_all` for every
scroller. History traces this omission to `45c103220b` (2023-10-29),
which introduced UDMF wall scrolling; it was not introduced by this fork work.
Minimal correction: pass `EScrollPos(scroll.where)` as its seventh argument.
The maintainer approved this scoped correction after the halt-and-flag report;
it is implemented and passes the default rebuild and native GL/Metal checks.
The pre-fix executable, packages and source are preserved under `pre-wall-fix/`
in the resumed artifact directory, alongside the isolated `03-wall-parts.patch`.
The final runner additionally rejects missing probe actors and unexpected case
order/indices. Identical final fixtures were run against all three binary states:

| Binary | Sector cases | Shared-wall part rates |
| --- | --- | --- |
| Original, GL | 10 of 28 fail | Fail |
| Sector fix, before wall-mask fix, GL | All 28 pass | Fail |
| Final, GL and Intel Metal separately | All 28 pass | Pass |

The fixed wall offsets are top `(9.5, 0)`, middle `(0, 14.25)`, bottom
`(4.75, 0)`, identical on all four expanded sides and both backends. Final
artifact directories are `verified-scroll-gl`, `verified-scroll-metal`,
`verified-pre-wall-gl` and `verified-original-gl`. Final executable SHA-256:
`ead0eef474b5c1b40e7c2ad62c4fb65efa0bf8f728f7ce50a20d3e5849d9f93c`.
All adjacent packages still match after the final rebuild.

Both broader native regression suites pass: five resource/image controls and
four cold-sound controls per backend, exact per-backend capture equality, PCM/
loop comparisons, reset/cleanup budgets, scanner/input and their negative
controls. They ran before the independent one-argument wall-mask correction;
after that correction the final scrolling and seven sidedef-boundary fixtures
pass on both backends. No renderer or audio implementation was changed.
`resources-gl`, `resources-metal`, `final-sides-gl` and `final-sides-metal`
retain the detailed results. The final 8+13 Python tests and whitespace checks
pass. No temporary C++ probes were introduced.

Both timing arms must retain **both** scrolling corrections, changing only
optional siderefs creation/filling. Do not compare against `pre-wall-fix/gzdoom`
as an isolated optimization baseline. No commit or push has been made.

## Optional sidedef lookup trial

Artifacts: `/private/tmp/gzdoom-side-refs-omzkua7g`. Both scrolling fixes and the
compiler/configuration were held constant. One temporary probe binary compared
forced lookup creation/filling against the conditional production path. Numeric
predictions: zero lookup allocation requests without wall scrollers and at least
5% lower median `ProcessLineDefs` elapsed time. No LLVM compiler process was
present in the pre-trial CPU check.

The original Ashes Blackwater MAP09 TEXTMAP was verified byte-for-byte against
the retained source: SHA-256
`b1dd53b49928b9372d47cf6a96bfd15965850f86aa826a00ef1f5a5e3dbb1e74`.
The mod SHA-256 is
`af078fab322be816dfc79d50f3ca487f4de27a12384791799be45995add3b2bb`.
Native loads confirmed no wall scrollers, 28,726 source/expanded sides and
16,911 lines in both arms. The isolated allocation pass observed **28,727
lookup allocation requests and 2,298,080 bytes of array capacity versus zero**.
These count actual TArray growth requests reaching `M_Realloc`: one outer and
28,726 inner arrays. Bytes exclude allocator overhead and other map allocations;
this is not an RSS measurement. Counting was disabled during timing.

| Independent GL pass | Forced lookup median | Conditional median | Difference |
| --- | --- | --- | --- |
| First order | 18.24 ms | 11.62 ms | 6.62 ms |
| Reverse order | 14.12 ms | 11.50 ms | 2.62 ms |

Each pass preserves ten loads, five per arm. The first four loads comprise
same-arm repeat pairs; the remaining six interleave three per arm. Using only
those six warm loads gives the same medians as using all ten. Consecutive
same-arm repeats in those warm windows differ by 0.47–2.46 ms. Both passes
favor the conditional path, but baseline drift and overlapping full-run ranges
rule out one precise speedup percentage. This stopwatch measures elapsed
`ProcessLineDefs` only, excluding lookup destruction and the rest of map loading.
No CPU profiler was attached; no whole-load, frame-rate or gameplay-smoothness
gain is established. `timing-analysis.json` retains the full and warm summaries.

Forced/conditional native shared-wall fixtures pass on GL and Intel Metal:
28 sector cases and all four expanded wall copies agree exactly. With wall
scrollers present both arms retain **114 lookup allocation requests, 116 entries
and 9,040 capacity bytes**. All seven sidedef-boundary fixtures also pass with
the old lookup forced on, on both backends; the conditional production path
passed these in the preceding validation. This confirms that the condition
removes the unused lookup while preserving its populated behavior.

All temporary C++ controls/counters/timers were removed. The exact pre-trial
production source was restored and the mandatory default build passed; all five
adjacent packages match. Final production GL/Metal scrolling checks exercise
the restored build. Source/binary identities, probe source/diff, logs and runner
are preserved in the artifact directory. Retain the conditional lookup; no
permanent diagnostic API or new benchmark target was added.
