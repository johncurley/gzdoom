# Audio sibling preparation trial — 2026-11-12

Status: **Default enabled — scoped Intel Metal gameplay checks pass; broader acceptance remains open.**
`snd_asyncsiblings` now defaults true, with a disabled console control.
The qualification history below used default false; its earlier dispositions
are superseded by the approved default change recorded at the end. No normal
RNG selection or mod definition has been changed.

## Bounded candidate

Continue from 5dd635c20b. For an unloaded request, the existing resolver first
selects its random variant normally. An optional owner-thread walk can then
prepare one unselected canonical sibling while the selected sound follows the
existing synchronous loader. It never calls ResolveSound/PickReplacement,
publishes an unrequested buffer, borrows actor data or waits during gameplay.

Bounds are 64 visited nodes, one acquisition attempt, at most 128 KiB of
encoded sound payload and a 1 MiB PCM extent. Admission checks the existing
four-job/64 MiB pool before acquisition and excludes duplicate pending keys.
The owner opens/closes decoders, reads resources and publishes OpenAL buffers;
the existing worker performs decoding. RAW/external/tentative definitions,
selected-lump aliases and cycles are excluded. A best-effort resource reader
catches filesystem failures without invalidating the unselected definition.

New backend/client hooks have conservative defaults for other implementations.
The existing macOS decoder gate remains. No sound ID, VM ABI, RNG selection,
playback scheduling or worker-pool limit is intentionally changed.

## Initial timing experiment

Prediction before measurement: at least one extra useful worker hit and a
5 ms lower median resumed maximum, with no additional not-ready fallbacks.

Six fresh processes interleave off/on/on/off/off/on. Sound precache remains
enabled and retention is zero. Same frozen Blackwater MAP09 save/configuration.
No profiler, per-request audit or stall logging is active.

| Setting | First resumed maximum median/range | Warm maximum median/range | First-load worker hits |
| --- | --- | --- | ---: |
| Disabled | 99.93 ms / 99.70–100.71 ms | 74.11 ms / 71.00–74.78 ms | 15 |
| Enabled | 93.28 ms / 90.27–96.62 ms | 70.85 ms / 69.45–73.12 ms | 17 |

The initial prototype meets the 6.65 ms first-window median target and produces
two additional hits in every enabled first visit. Not-ready/failure counts
remain zero; peak reservation remains 55,982,791 bytes below 64 MiB. Warm ranges
overlap, so no warm improvement is established. This is a workload-window
maximum, not exclusive CPU time or an exact first-tick duration.

These timings precede the added keyed-admission/native-validation refinements.
They are exploratory evidence for the candidate, not final-code acceptance.

## Native checks completed

The expanded Metal run passes its five ordinary resource modes, including
texture/audio/input/graph controls. The new explicit sibling diagnostic passes
two exact PCM/hash/loop comparisons, duplicate/full admission checks, selected
and same-lump exclusions, RAW/external/tentative exclusions, a cycle, oversized
encoded input, PCM overflow and cancellation/cleanup. An intentionally corrupt
deflate entry really throws through the ordinary reader; the advisory path
rejects it safely. The new fixture entries are never ordinary precache inputs.

Four fresh cold-playback runs each play the same two-choice fixture eight
times. Each individually passes its publication/readiness/cleanup checks.
Enabled runs consume one prepared sibling with zero late/failure results;
disabled runs use no preparation jobs. Both publish identical PCM/loop data.
The final cross-process selection comparison fails.

## Exact validation blocker

[tools/regression/run.py](../../tools/regression/run.py) compares all four
frame-scheduled selection lists after supplying the same startup RNG seed.
Three runs select:

~~~text
559, 558, 559, 558, 559, 559, 559, 559
~~~

The repeated enabled run selects:

~~~text
558, 559, 558, 559, 558, 559, 559, 559
~~~

IDs and PCM records agree otherwise. The first selection already differs before
the prefetch hook for that request. No earlier request/prefetch audit appears
in those logs. The current fixture does not capture the live client sound RNG
state at the sequence boundary; startup seed equality alone has therefore not
established comparable starting state. This is not evidence sufficient to
blame or exonerate the worker. Preserve the failed run.

Sound selection uses DoomSoundEngine::PickReplacement and the named client
FCRandom RandSound. The implementation must not reseed or alter that generator
in normal gameplay to make a test pass.

## First contract correction — approved, then disproved

Add a fixture-only native paired oracle which snapshots the client sound RNG
and relevant unused fixture sound state, runs the same requests with the
feature off/on from exactly matching state, compares selected IDs and PCM/loop
data, and restores the original state on every exit. Preserve separate
frame-scheduled readiness, overflow, error and cleanup checks and retain this
failed cross-process result. Do not weaken the RNG-invariance requirement or
enable the feature until the corrected gate passes.

The user approved this correction. Its implementation compiled, but the first
paired Metal run failed: only four of eight selections matched, despite the
captured RandSound state being restored. PCM hashes/loops matched and the
bounds diagnostic still passed.

### Actual resolver discovery

The prior attribution above to RandSound was incorrect for ordinary requests.
DoomSoundEngine::ResolveSound delegates to SoundEngine::ResolveSound, whose
nonvirtual SoundEngine::PickReplacement uses C rand(). The separately named
DoomSoundEngine::PickReplacement uses RandSound but is not the method reached
by this fixture. Capturing RandSound therefore does not establish the starting
state of the actual selector. This explains why the first paired test is not a
valid RNG-invariance oracle; it does not establish a worker-induced defect.

The initial paired run was paused under AGENTS.md's halt-and-flag rule. Do not
reseed the production C generator, make PickReplacement virtual, or change the
normal resolver merely to satisfy a test. A proposed correction is a test-only
controlled C RNG shim for the macOS paired run, allowing the actual rand()
stream to be captured/restored and its consumption compared, with the shim
absent from shipping builds. Preserve independent unmodified-engine readiness
and PCM checks. The user approved this tooling correction before implementation.
The experimental feature remains default false.

## Evidence

- /private/tmp/gzdoom-sibling-audio-20261112-093420: initial contract, original
  sources, prototype source/binary and six-run timing rows.
- /private/tmp/gzdoom-sibling-native-metal: expanded native logs and four
  cold-case result.json files, including the failed repeated sequence.

- /private/tmp/gzdoom-sibling-paired-metal: failed first paired oracle, logs,
  source snapshots and hashes; the incorrect RNG assumption is retained.

## Approved shim qualification

The runner builds a test-only macOS interposer in its artifact directory.
Shipping targets do not link it. Only the default/repeated default correctness
processes load it; standalone cold playback and performance runs do not.
Outside the native paired command, calls pass through to libc. During the
command a thread-local controlled stream permits exact snapshot/restore and
draw counting without touching libc's hidden state. Other threads pass through.
The actual StartSound resolver consumes this stream; no production resolver,
virtual dispatch, generator choice or gameplay RNG seed is changed.

Prediction: eight identical selected IDs, eight draws in both arms, identical
final stream state and ordered PCM/loop outputs; one extra real C RNG draw must
be detected. The first Metal paired check now passes all eight comparisons,
equal 8/8 draw counts and the negative control. Python parser controls also
reject altered choices, PCM hashes, draw counts, missing boundaries and failed
restoration. Standalone shim checks pass eight passthrough calls before/after,
eight repeated scoped draws and sixteen calls from another thread without
advancing the scoped counter. Full suite/backend results follow below.

## Native backend results

Intel Metal: PASS, all five resource modes and four fresh cold sound controls.
Both default-mode paired oracles pass eight comparisons, equal 8/8 draws,
ordered PCM/loop records, the extra-draw negative control and named-generator
restoration. Sibling bounds diagnostics pass two exact PCM/loop comparisons
and the actual corrupt-reader negative control. Resource pixels, existing
precache PCM/order, reset, budgets, cancellation and input controls pass.
Cold enabled runs consume one prepared sibling; disabled runs consume zero,
with no not-ready/failure results and zero endpoint jobs/reservations.

Artifacts: `/private/tmp/gzdoom-sibling-shim-metal` (including manifest, shim
hash, shim controls, per-case argv/logs, result and summary JSON). The native
executable's dependency list contains no RNG shim. The separately executed
shim control script is now retained as `tools/regression/check_rand_shim.py`
and future runs invoke it before launching correctness cases.

Intel OpenGL: PASS, the same five resource modes and four fresh cold sound
controls. Both paired oracles pass 8/8 choices/draws and exact ordered PCM/loop
records with detected extra draws. Cold enabled/disabled worker hit controls
are one/zero, with zero late/failure results or endpoint jobs/reservations.
Artifacts: `/private/tmp/gzdoom-sibling-shim-gl`. This qualifies the shared
macOS audio path under GL and Metal; Linux/Windows and Apple Silicon remain
unmeasured. The original failed runs remain separate and unchanged.

## Final-code timing method

Repeat the same six-run contract on the final native executable with the
normal system RNG and no injected library. The public benchmark now accepts
`--control sound-siblings`, holding sound precache on and retention at zero.
All assets/settings/save hashes and actual argument vectors are retained.
The workload is two frozen MAP09 save loads per process, not a demo, combat
route or manual doorway crossing. OS filesystem caches are not purged.

For each first/warm phase, the derived resumed maximum is the largest recorded
one-second FrameTrace window maximum below 300 ms, asserting exactly one
window at or above 300 ms contains the core load. All original windows remain
in the result; the excluded load window is still reported by the public
benchmark. This is an elapsed workload-window proxy, not exact first-tick
latency, exclusive CPU time, audio latency or a frame percentile. The final
runner and its hash are preserved with the trial artifacts.

## Final-code timing results

All six final-code runs completed in the declared order, three per arm:

| Setting | First resumed maximum median/range | Warm maximum median/range | First-load worker hits |
| --- | --- | --- | ---: |
| Disabled | 103.71 ms / 103.58–118.93 ms | 77.36 ms / 68.07–80.08 ms | 15 |
| Enabled | 97.99 ms / 96.76–98.25 ms | 72.18 ms / 69.87–72.95 ms | 17–18 |

The first-window median falls 5.72 ms, meeting the declared 5 ms target.
Enabled first-window maxima are all below the disabled range, whose 15.35 ms
spread exceeds the median effect. Retain the 118.93 ms result rather than
exclude it. Alongside the independent earlier 6.65 ms prototype result and
2–3 additional useful first-load worker hits, this supports a small bounded
benefit in this workload, not a precise general speedup. Warm ranges overlap;
no warm improvement is established. Ordinary random sound choices are not a
deterministic demo sequence, and repeatability beyond this save remains open.

All not-ready/failure counts stay zero and peak reservation remains
55,982,791 bytes, below the unchanged 67,108,864-byte pool. The three ready
thunder preparations already present at checkpoints retain 53,741,839 bytes
in both arms; they are not a new leak introduced by this lookahead. Native
fixture endpoints separately prove zero retained jobs/reservations.
Core load medians are approximately 2.27 seconds in both arms: this is not a
claim that the whole level load became materially faster.

Evidence: `/private/tmp/gzdoom-sibling-audio-20261112-093420/final-timing`,
`final-timing-result.json`, `final-timing-runner.py`, runner/source hashes,
argument vectors and the final native source snapshot. No shim, per-sound audit
or Instruments recording is active during these timings.

## Disposition and next step

Keep `snd_asyncsiblings` false by default. To qualify gameplay, enable it
explicitly and repeat the original doorway and active-combat workloads,
including long sessions, sound resets and mod-specific random groups. Retain
serial fallback and existing decoder/OpenAL ownership. Broader native platform
qualification is pending; no actor/VM threading change is justified here.
A whole-audio-thread migration is not part of this change.

## Final verification

Mandatory `cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"`: PASS.
Engine-adjacent `gzdoom.pk3` matches the built package. Python regression
controls: 8 PASS; profiling parser controls: 13 PASS (existing pngdiff resource
warnings remain). Matched DOOM2 MAP06 baseline and SSAO Metal/GL image oracle:
both OK with only the existing backend noise classification. This is selected
cross-backend coverage, not a full golden sweep or Apple Silicon result.
Final build/oracle logs and captures are retained in the trial's `final-checks`
directory along with the final executable hash. A preliminary matrix selfcheck
used the local default mod scene and failed IWAD discovery; it did not qualify
a frame. The explicit DOOM2 scene checks above supersede that launch attempt.

## Maintainer-approved default change — 2026-11-12

After reviewing the scoped results, the maintainer explicitly requested
`snd_asyncsiblings` on by default and to begin gameplay checks. The CVAR now
defaults true, remains non-archived and can be disabled with
`snd_asyncsiblings 0`. Earlier default-off dispositions above describe the
qualification tranche; this approved policy change supersedes them without
claiming that long-session or wider-platform qualification has completed.
Existing backend preparation gates, budgets, serial fallback and resource/
OpenAL ownership remain unchanged. Prediction for the first manual gameplay
check: no >100 ms frame interval in the observed doorway crossing, no audible
missing/cut-off/incorrectly looping effects. A failure requires attribution;
a pass only qualifies the observed run, not every mod or workload.

Enabled-default verification: mandatory build and 8 Python regression tests
PASS. A fresh Intel Metal run passes all five resource modes and four cold
playback controls. The harness queries the CVAR without setting it in enabled
modes and confirms `"snd_asyncsiblings" is "true" (default: "true")`; the serial
control reports false with the same compiled default. Paired RNG/PCM, bounds,
cache/reset and leak checks continue to pass. Artifacts:
`/private/tmp/gzdoom-sibling-default-metal`, with build log at
`/private/tmp/gzdoom-sibling-default-build.log`.
The manual doorway session is staged under
`/private/tmp/gzdoom-sibling-gameplay-20261112`; loading paused is not a route
pass. Operator readiness and observations are required before reporting one.

## First enabled-default doorway gameplay check

The maintainer confirmed repeated mall-doorway crossings, smooth gameplay and
normal sound in the roughly 40-second Intel Metal check. This is a scoped
manual pass. Nineteen complete two-second FrameTrace windows contain 1,340
reported intervals: zero over 100 ms, maximum 56.93 ms, at the existing 35 fps
cap. The final partial window is unavailable. Worker hits increase by 12,
with no added not-ready results, failures or waits; those hits include existing
preparations and are not all attributable to sibling lookahead. Peak reservation
is 55,861,213 bytes, with one ready job retaining 2,114,027 bytes at the end.
No disabled-fix comparison was taken in this run; do not infer an incremental
speedup solely from this acceptance check. Combat, death/reload and long-session
acceptance remain pending.

Evidence: `/private/tmp/gzdoom-sibling-doorway-2rmc_cbl`, including
`doorway-summary.json`, operator observation, launch manifest and original logs.

## Enabled-default combat candidate — mixed death/reload workload

The operator reported dying. Logs independently show a second MAP09 load inside
the timed section; one 623.83 ms interval occurs in the reload-adjacent timing
window. Preserve it. This is not a clean combat-only pass and the unprofiled
window cannot identify the exclusive cause of that interval. Before the logged
reload, complete timing windows have no intervals over 100 ms and a maximum
45.22 ms; that reporting boundary is not an exact death timestamp. Audio counters
add no not-ready results, failures or waits. Operator fighting/audio observations
and the reload sequence are pending. Game ended automatically paused.
Evidence: `/private/tmp/gzdoom-sibling-combat-lbtj1v6r`, including original logs,
`combat-summary.json` and the host-observed death report (not engine event time).

The operator subsequently confirmed fighting was smooth with normal sound and
that the map reloaded after death. This supports scoped combat/audio acceptance;
it does not convert the mixed run into a pure combat benchmark or eliminate
the retained reload interval. Sustained play, a longer soak and any targeted
reload attribution remain pending.

## Three-minute enabled-default sustained gameplay capture

The current paused session was resumed for 6,300 scheduled frames at cap35.
It completed and automatically paused. Eighty-nine complete two-second timing
windows contain 6,284 reported intervals, zero over 100 ms and maximum 53.64 ms.
No map loads appear inside this section. Worker counters add 13 queued jobs
and 14 hits (including previously ready work), with zero added not-ready
results, failures, cancellations or waits. Peak reservation remains 55,909,711
bytes below 67,108,864 bytes; two ready jobs retain 4,232,316 bytes at the end.
Those counters alone do not establish a leak or complete worker teardown.

The operator confirmed varied situations, normal sound, no perceived frame lag,
and no deaths or menu use. This sustained run is a scoped gameplay/audio pass.
This is acceptance evidence for one enabled-default run with two-second window
and cap35 limits, not a disabled-fix performance comparison or a long-session
soak. The terminal partial timing window is unavailable. Keep the earlier
623.83 ms death/reload interval in its separate mixed-workload record.
Evidence: `/private/tmp/gzdoom-sibling-combat-lbtj1v6r/sustained-summary.json`,
`sustained-plan.json`, route script and original engine/stdio logs.

## Current gameplay disposition

| Workload | Operator observation | Recorded intervals | Maximum | >100 ms |
| --- | --- | ---: | ---: | ---: |
| Repeated mall doorway crossings | Smooth, sound normal | 1,340 | 56.93 ms | 0 |
| Combat followed by death/reload | Fighting smooth, sound normal; reload confirmed | 2,655 | 623.83 ms | 1 |
| Three minutes of varied sustained play | No perceived frame lag, normal sound, no deaths/menus | 6,284 | 53.64 ms | 0 |

All are Intel Metal with cap35 and complete two-second timing windows only.
These acceptance checks have no matched disabled-fix arm; do not claim the
entire observed smoothness is an incremental sibling-lookahead speedup.
Audio preparation late/failure/wait counters do not increase in any section.
The two gameplay maxima below 100 ms do not imply all frames fit the 28.6 ms
cap35 budget, and the terminal partial windows are unavailable. The 623.83 ms
reload interval remains a separate attribution candidate, not a discarded
outlier or a demonstrated audio failure.

Keep the approved enabled default and its disabled control. Next acceptance
work is a longer varied-play soak, live sound-reset/reload checks and matched
GL gameplay; Apple Silicon/Linux/Windows runtime qualification remains open.
Profile the reload interval separately before selecting another optimization.
No new engine change is justified solely by the successful manual runs.
