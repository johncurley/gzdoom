# Bounded owner-thread sound retention — 2026-11-11

Status: opt-in implementation with scoped warm Intel Metal reload measurements
and native GL/Metal loop/pressure/reset checks. Default remains zero pending
broader gameplay and long-session acceptance. This targets repeat decoding of
previously played effects, not cold misses or asynchronous channel processing.

## Contract

`snd_retainedcache` is a non-archived KiB budget, clamped to 0–65536, default 0.
`snd_retention_stats` reports bounded history, last precache-pass count/payload,
first actual-play reuse hits, and rejected retention candidates. Rejections
include budget and eligibility failures, not dropped playback events.

Keep at most 128 owner-thread history entries for actual backend-channel starts.
Store sound index, canonical buffer handle, resource/loop/raw metadata and
backend-reported payload bytes. No actors or borrowed PCM are queued. Repeated
aliases record their resolved canonical buffer. Most recently played candidates
are considered first; oversized candidates do not prevent older smaller fits.

At existing CacheMarkedSounds maintenance, first retain the existing mandatory
active/explicitly marked path, including its canonical alias resolution. Then
mark optional eligible history entries while their cumulative payload fits the
budget. Original eviction unloads everything still unmarked. There is no
runtime eviction of active voices, no truncation, and no change to synchronous
cache completion, codec decoding, playback timing decisions or worker budgets.

The budget applies to **extra otherwise-unmarked buffers at precache points**.
It is not a limit on all sounds loaded during gameplay, active voices, explicit
precache declarations, driver overhead or total process memory. Budget changes
take effect at the next maintenance pass. Metadata capacity is separately bounded
by 128 entries. OpenAL supplies AL_SIZE; unknown-size backends return zero and
are not retained. No duration-based size estimate is used.

Explicit unload forgets matching history before releasing the buffer. Full
unload clears history and pass statistics; device reset already calls that path
before destroying OpenAL. Local SNDINFO replacement also unloads before parsing
new definitions. Identity/loop/raw metadata mismatches reject old entries.
No VM bindings, sound IDs, actor callbacks, RNG choices or tick order change.

## Loop-safety correction approved during implementation

Initial tests checked metadata changes within one history entry but missed
cross-definition sharing. Existing LoadSound's loaded-lump search compares lump
and some raw-rate information, not all loop/format fields. Extending a buffer's
lifetime could therefore affect a later request through another definition.
This is a native SoundEngine interface edge case, not a demonstrated failure
in the measured Blackwater assets; GZDoom's usual definitions use default
per-sound loop fields and encoded assets may contain intrinsic loop tags.

Work paused under AGENTS.md's halt-and-flag rule, and the user explicitly approved
restricting eligibility. Final policy only retains non-RAW definitions with
RawRate=0 and LoopStart=LoopEnd=-1. It also rejects a resource if any current
definition for that lump has conflicting raw/explicit-loop settings. Intrinsic
loop tags still follow the original decoder. Mandatory active/marked caching
and the legacy loaded-lump sharing implementation remain unchanged.

The dedicated native fixture creates two names for one unique Ogg resource and
sets explicit loop points on the second through a temporary native test command.
Both GL and Metal reproduce the hazard with the guard bypassed: the second
request aliases the retained default buffer and actual OpenAL loop points are
0–49220. With the guard enabled, the old idle buffer is released and actual
OpenAL loop points are 123–5000. All queries return AL error 0. This is a failing
control and passing candidate, separate from active-loop protection.

## Predictions and exploratory budget sweep

Before measurement: 2–4 MiB should avoid most of the repeated resumed sound
loads and reduce median request-to-completed-resumed-loop latency by at least
25 ms, exceeding within-arm spread. Zero/smaller budgets must preserve active
playback, byte bounds and ordinary fallback. Initial decoding cost is not avoided.

Same MAP09 copied auto00 save and Metal 1024×768/cap35 setup as the prior
[post-load attribution](postload-sound-attribution-2026-11-11.md). Audio worker
preparation stays explicitly enabled. No gameplay input, Instruments or video.
Each budget change gets an unmeasured reload plus play before two measured loads.
Temporary clocks use the existing request/core/outer-loop/resumed-loop endpoints;
an owner-thread miss counter distinguishes core and resumed misses. Statistics
print after the measured endpoint. Timer/counter probes are removed at completion.

Exploratory sweep before the loop-safety restriction, two measurements per cell:

| Budget | First resumed misses | Median resumed loop | Extra retained payload |
|---|---:|---:|---:|
| 0 | 8 | 76.02 ms | 0 |
| 256 KiB | 7 | 72.35 ms | 252,900 bytes |
| 1 MiB | 3 | 48.96 ms | 1,032,512 bytes |
| 2 MiB | 1 | 38.06 ms | 1,363,404 bytes |
| 4 MiB | 1 | 36.33 ms | 1,363,404 bytes |

This selects **2 MiB as the next experimental setting**, not a cross-mod default.
An inaudible/virtual sound request does not establish actual-play history; this
policy intentionally does not retain every buffer a loader touches.

Three further pre-guard processes each alternate two zero-budget and two 2 MiB
arms, with two measurements after every unmeasured warm reload (12 events per
mode). Resumed-loop medians improve 77.59→38.09, 80.58→36.22 and 80.92→37.40 ms,
each beyond its within-arm spread. Miss ranges are 7–9 disabled and 1–2 enabled.
Whole-interval median gains are 32.60, 42.31 and 45.31 ms, but process 3 has a
61.46 ms within-arm total spread: the predeclared full-interval criterion fails
there. Retain the outlier, do not exclude it as an assumed clock/scheduler event.
The largest loading Update gap shows no qualified improvement. These exploratory
results are not final guarded-implementation qualification.

## Verification and artifacts

### Final guarded timings

Three fresh processes repeat the same interleaved zero/2 MiB contract, including
an unmeasured warm reload per arm and two measured reloads per arm: 12 events
per mode, 24 total. Each process has two arms per mode. No fault guard bypass,
per-sound developer logging, native loop query, profiler or video is active.

| Process | First resumed loop, disabled → 2 MiB | Complete interval, disabled → 2 MiB | Complete saving | Largest within-arm complete spread |
|---|---:|---:|---:|---:|
| 1 | 82.582 → 37.534 ms | 1911.659 → 1855.250 ms | 56.409 ms | 33.880 ms |
| 2 | 82.276 → 38.066 ms | 1905.714 → 1860.724 ms | 44.989 ms | 26.877 ms |
| 3 | 81.546 → 38.488 ms | 1896.728 → 1862.180 ms | 34.548 ms | 18.863 ms |

The >=25 ms complete-interval prediction now passes beyond within-arm spread
in all three final-code processes. Resumed-loop savings of 43–45 ms also exceed
their within-arm spreads (8.1–13.6 ms). Preliminary failed/noisy results remain
in their original directories and are not substituted into this final table.
Full-interval gains are approximately 1.8–3.0%, not a general FPS claim.

Disabled resumed misses range 7–9; enabled range 1–3. Largest selected extra
payload is 1,363,404 bytes, below 2 MiB; all pass/entry bounds hold. Core-stage
miss counts remain zero, so the avoided decode is not merely moved into loading.
Core time and the largest loading Update gap have no qualified improvement.
The approximately half-second loading gap therefore remains a separate target.

Actual resumed tick counts are 16 disabled and 15–16 enabled. Wall latency is
measured through a completed loop, not at an identical final gametic; preserve
the existing scheduler and do not infer replay determinism from this test.

### Correctness and coverage

The temporary sanitizer harness extracts the actual retention/eviction method
bodies, uses engine TArray, and mocks the backend and canonical CacheSound
marking. It tests MRU/fit behavior, entry cap, zero/negative budgets, unknown and
oversized payloads, identity/loop/raw mismatch, explicit unload, reused handles,
full unload and active aliases. Separate faults remove active protection and
cross-definition eligibility; both must fail. This is not a full-engine ASan run.

Pre-guard native GL/Metal checks preserve an actual looping aliased buffer larger
than the optional budget during zero-budget trim: buffer/source/length stay
identical and playback position advances. Device reset clears history and
restores the loop. Stopping then trimming releases its idle buffer. These are
backend-state checks, not an audible-output recording or proof against every
driver/system discontinuity. Final guarded runs repeat these checks on both
backends: the 4,338,396-byte active aliased loop exceeds the optional 2 MiB
budget and survives a zero-budget trim unchanged. Positions advance from
11145 to 38452 on Metal and 12817 to 40124 on GL, with the same buffer/source.
Device reset clears history and restores a source with matching buffer length;
stop followed by zero-budget trim releases the idle buffer.

An overly strict identity diagnostic initially expected no repeated flesh or
monster loads after startup. It fails on the first revised Metal take:
fleshdrop1 and rain still load after the first reload, then only rain after
the second. History grows from eight to nine entries with zero admission
rejections. The GL take similarly has fleshdrop2 plus rain, then only rain.
Keep and report this failed zero-repeat expectation; do not claim that all
variants enter actual-play history during a short initial window. The timing
contract already includes a warm reload and predicts most misses avoided.
Identity reporting was corrected to retain these names rather than abort the
remaining lifecycle/timing checks. No failed capture was discarded or rerun.

Final-source ASan/UBSan policy tests and both deliberate failing controls pass.
No temporary clocks, counters, native fixture commands, direct AL loop query
or guard bypass remain in production source or executable. The final build and
whitespace checks pass. Final baseline/SSAO GL/Metal comparisons with the 2 MiB
setting report OK; all four graph self-tests pass. Only these configurations
are claimed, not the full golden suite. `final-matrix-qualified` isolates their
eight current image/log files from older unrelated matrix scratch captures.
`final-verification.json` records final executable/source hashes and checks.

For the next manual gameplay check, use `snd_retainedcache 2048` and inspect
`snd_retention_stats`. The setting is non-archived and starts disabled on a new
launch. Keep broad/default acceptance pending cold first visits, longer sessions,
different mods and supported-platform runtime checks. Resource loading and
hwrenderer profiling remain separate next targets.

Artifacts: `/private/tmp/gzdoom-sound-retention-20261111`, including initial
INIs/copied saves, all arguments, sweep and repeated raw rows, source/binary
snapshots, sanitizer source/faults, native fixture/control logs and analysis.
`analysis.json` retains the preliminary failed full-interval gate. The original
mod/save/configuration and golden baselines are not edited. Temporary probes
and fixtures are not production code. No CPU/GPU innocence, general FPS,
cross-build demo determinism, cold-cache or long-session acceptance is claimed.
