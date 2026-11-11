# Audio sibling preparation trial — 2026-11-12

Status: **Experimental, default off — Intel macOS correctness and bounded timing gates pass.** The feature
remains opt-in: snd_asyncsiblings defaults false. No production RNG change,
mod override or default enablement is approved or applied.

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
