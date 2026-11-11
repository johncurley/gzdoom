# Blackwater marked long frame — 2026-11-09

Finding: one observed **129.625704 ms Update gap** contains a 106.137824 ms
G_Ticker invocation dominated by synchronous lightning/thunder sound preparation.
This is event-local evidence for a CPU audio-preparation stall, not a claim that
all doorway glitches or all renderer costs share that cause.

## Capture qualification and limits

The operator repeated the mall doorway route and reported a hitch “early.” The
rebuilt process was launched directly from the project executable, PID 72293,
with native timeline markers enabled, the same starting-save hash, Metal and
1024x768. UUID `AB2DAEF9-85DE-379E-8F66-920C989587BE` matches the archived
profiled executable and live image mapping (base `0x101ca4000`). The original
Blackwater missing-FIRECHECK warning remains; no mod assets were altered.

Artifacts are under
`/private/tmp/gzdoom-blackwater.Y5o1Yg/marked-metal-3wkkyxbk`.
The recording command requested 40 seconds of System Trace plus Time Profiler,
but its exported summary reports only **10.000000 seconds**, recording mode
**Windowed (5 seconds)**, and a requested time limit of 40 seconds. Instruments
also reported **lost events**. Therefore this is not a qualified full-route
capture, and “early” cannot be assigned to an event in the retained interval
without further evidence. All times below are relative to the exported window.

The signpost export has 270 Update points, 968 loop records and 546 tick records.
One unmatched begin and one unmatched end are retained as diagnostics. Update
ordinals are consecutive after normalizing Instruments' integer grouping commas;
there is no missing intermediate Update marker in the selected gap. The global
loss warning still applies: interval coverage alone is not proof of loss-free
scheduler data or unbiased sampling.

## The selected event

| Measurement | Value |
|---|---:|
| Update-to-Update window | 4.562181260–4.691806964 s |
| Update gap | 129.625704 ms |
| Context at both endpoints | GS_LEVEL, menu off, pause off, app active |
| Update ordinals | 2105 → 2106 |
| Loop ordinal containing the slow tick | 3466 |
| Tick invocation | 4.572625061–4.678762885 s |
| Tick interval | 106.137824 ms |

Exported main-thread scheduling intervals cover the selected gap and report:

| State | Duration |
|---|---:|
| Running | 119.208660 ms |
| Blocked | 8.154696 ms |
| Runnable | 0.150071 ms |
| Interrupted | 1.000200 ms |
| Preempted | 1.112077 ms |

The CPU table contains 118 nominal 1 ms main-thread samples in the same gap.
Inclusive engine ancestry, deduplicated by symbol within each stack:

| Stack | Sample weight |
|---|---:|
| TryRunTics | 105 ms |
| G_Ticker / P_Ticker / RunThinkers | 104 ms each, nested |
| DLightningThinker::LightningFlash | 103 ms |
| SoundEngine::StartSound / SoundEngine::LoadSound | 102 ms each, nested |
| OpenALSoundRenderer::LoadSound | 97 ms |
| D_Display | 12 ms |

Do not add these inclusive values. Sample weights are not exact function
latencies or exclusive frame percentages. Vorbis decode, libsndfile read,
archive inflate and OpenAL buffer-storage leaves corroborate audio preparation
on the main thread during the tick. About 119 ms running versus 8 ms blocked
also argues against this particular event being predominantly a GPU/driver wait,
subject to the trace's loss warning. No GPU occupancy or complete filesystem
timeline was collected here.

## What this establishes

The earlier prediction was that an actual frame spike aligned with the
90–200 ms lightning decode candidate would contain a substantial synchronous
sound-loading component. This frame satisfies that event-local prediction.
The high RunThinkers weight is largely downstream audio work, not evidence
that actor-list traversal itself consumed 104 ms. Scene-material bookkeeping
remains a separate steady-cost candidate rather than the dominant cost of this
selected frame. This is not a general exoneration of the CPU renderer or GPU.

`SoundEngine::LoadSound` prepares a sound only when its handle is invalid and
tries to reuse already-loaded matching lumps. `OpenALSoundRenderer::LoadSound`
decodes the supplied sound into a buffer synchronously. Further investigation
needs the resolved sound ID/asset, cache state and lifecycle: different random
variants on first use and unloading/reloading are not equivalent explanations.
No cache, precache, audio-thread or actor-thread change was made.

## Capture/tool corrections

A separate 12-second paused-game check with `--window 20s` exported **12.330470 s**
and **Windowed (20 seconds)**. This verifies that an explicit larger retention
window avoids the observed ten-second cap in that check. Future 40-second
captures should request `--window 60s`, then verify duration, table ranges and
loss diagnostics before qualification. The short check does not prove that a
heavier full-route capture will be loss-free.

Two offline parsing issues exposed by this data were corrected:

- Archived executable names differ from `gzdoom`. The CPU analyzer now strips
  atos image/source suffixes generically so one function is not split by its
  archive filename or source line. A new regression failed before this fix.
- Instruments displays integer metadata with grouping commas, such as
  `update=2,105`. The signpost analyzer now retains normalized numeric context
  alongside the raw message. Treating that string as `update=2` would falsely
  suggest missing markers. A grouping regression is included.

Twelve analyzer tests pass. No engine instrumentation changed during this
analysis, so the archived binary still matches this recording. Next: repeat with
explicit full retention, then qualify repeated/event-aligned results and sound
asset/cache attribution before choosing a bounded optimization.

## Corrected full-retention repeat

The operator reloaded the starting save, repeated the route, and again reported
the hitch near the mall doorway. `doorway-marked-02.trace` used the same PID,
executable and settings, with `--time-limit 40s --window 60s`. Its TOC reports
**40.391389 seconds**, Windowed (1 minute), and Time limit reached. No explicit
lost-event warning was emitted; its run-issues store has the common “Data stream:
Time Mapping” entry also present in the successful controls, but not the first
marked run's separate “Lost events were detected” record.

The signpost export also contains records beyond the CPU/scheduler endpoint.
Those records must not be treated as jointly measured frames. Analysis was
restricted to **0–40.391389 s**, matching the exported run duration and the CPU/
thread-state ranges. The raw signpost-only 949 ms and 411 ms gaps after 45 s
are excluded from this capture's findings. The helper now accepts `--start`/
`--end`, with a regression proving that post-capture gaps cannot enter a bounded
summary. No assumption is made about why this Instruments version exports them.

Within the common interval there are 1,392 Update points, with consecutive
ordinals, and 1,391 observed gaps. The median is 28.844645 ms, p95 33.557167 ms,
p99 36.287583 ms, and maximum **261.457350 ms**. Exactly one gap exceeds 100 ms.
Mean gap is 28.876203 ms. This is one capped, profiler-on manual-route run;
these values are not peak throughput or an instrumentation-off baseline.

### Repeated event-local attribution

| Measurement | First retained event | Full-retention repeat |
|---|---:|---:|
| Update gap | 129.625704 ms | 261.457350 ms |
| G_Ticker invocation | 106.137824 ms | 238.533333 ms |
| Exported main-thread Running interval weight | 119.208660 ms | 239.657642 ms |
| Exported main-thread Blocked interval weight | 8.154696 ms | 15.301551 ms |
| Main-thread sample weight in gap | 118 ms | 241 ms |
| Inclusive lightning sound-loading ancestry | 102 ms | 232 ms |

The repeated event spans **6.401438465–6.662895815 s**, Update 19653→19654,
loop 36153, tick invocation 19870. Endpoints show GS_LEVEL, menu off, pause off,
native app active. Its complete tick interval is 6.419372854–6.657906187 s.
Thread states cover the gap: Running 239.657642 ms, Blocked 15.301551 ms,
Runnable 0.026435 ms, Interrupted 2.084417 ms and Preempted 4.387305 ms.

Of 241 nominal 1 ms CPU samples, 232 include
`DLightningThinker::LightningFlash → SoundEngine::StartSound/LoadSound`.
216 include `OpenALSoundRenderer::LoadSound`, and 16 include
`DoomSoundEngine::ReadSound`; these are inclusive nested contexts, not an exact
exclusive partition. Vorbis decode dominates the leaves, with archive inflate,
normalization/copy and OpenAL buffer creation also present. This supports
synchronous audio preparation as the dominant cost of this captured long frame.
It does not establish that the doorway geometry itself triggers the sound.

The whole run has 18,786 main-thread sample-weight ms. Scene-material observation
accounts for 4,253 ms (22.6% of that sample-weight denominator), while RunThinkers
accounts for 1,611 ms (8.6%, including synchronous sound work). Keep steady
bookkeeping cost separate from the selected audio-driven long frame; do not add
inclusive rows or interpret these shares as wall-frame percentages.

### Sound-preparation follow-up

Blackwater's Sndinfo.txt maps `world/thunder` to three random variants backed by
`DSAMTHN1.ogg`, `DSAMTHN2.ogg`, and `DSAMTHN3.ogg`. Reading their Ogg/Vorbis
headers and final granules (without decoding playback) gives:

| Asset | Encoded bytes | Channels / rate | Duration |
|---|---:|---|---:|
| DSAMTHN1.ogg | 1,214,907 | stereo / 48 kHz | 22.596 s |
| DSAMTHN2.ogg | 639,987 | stereo / 48 kHz | 13.806 s |
| DSAMTHN3.ogg | 1,555,297 | stereo / 48 kHz | 29.020 s |

These are a candidate variant set, not identification of the specific asset in
either stack sample. Neither repeated event proves re-decoding of the same
variant: first-use misses among different variants remain plausible. Next
measure resolved sound IDs, cache misses and source/decode/buffer stages, then
evaluate existing precache coverage and memory cost before async redesign.
A stationary-from-save control can distinguish weather timing from a spatial
doorway trigger; it would be a causal diagnostic, not a matched gameplay-route
performance comparison. No sound-cache, resource-loader or threading change was
made in this analysis.

Repeat artifacts use the `marked-02-` prefix in the same temporary session
directory; the original executable is archived as `gzdoom-profiled-binary`.
Thirteen offline analyzer tests pass after the new bounded-summary control.
Matched repeated GL/Metal qualification and marker-overhead measurements remain
open; this repeat establishes a reproducible CPU audio-preparation signature,
not a completed cross-backend baseline or a measured optimization.

### Cache identity controls and worker boundary

Developer logging from a save-start run observed three lightning events and
first-use loads of thunder variants during play, without intervening in-play
unloads. This identifies cache misses, rather than proving repeated decoding of
one already-cached sound. A temporary additional MAPINFO package declared
`PrecacheSounds = "world/thunder"`; neither the original mod nor engine audio
code was changed. The first comparison is rejected: its prototype arm had zero
lightning events, so zero sound loads was not a valid control result.

A repeat with explicit matching background settings retained three lightning
events in each arm. The prediction was that all three variants would load before
play in the prototype and zero thunder variants would first-load during play.
That identity prediction held: control loaded thunder2 and thunder1 during play;
prototype loaded thunder1/2/3 beforehand and no thunder variant during play.
Other sound sequences differ between arms, so these runs are not deterministic
replays or timing comparisons. They establish precache coverage only, with no
claim of hitch removal or performance improvement. Artifacts:
`/private/tmp/gzdoom-blackwater.Y5o1Yg/sound-identity-background-controls-diwa6qik`.

The threading candidate is the encoded-byte to PCM decode loop inside
`OpenALSoundRenderer::LoadSound`. Its existing `BackgroundProc` services audio
streams; it does not perform this first-use effect decode. A worker design must
own its input bytes, decoder and output PCM, and validate decoder-library
concurrency before implementation. Initially keep sound selection/RNG, cache
mutation and OpenAL buffer publication on their existing owner thread. Queue
preparation ahead of playback and retain decoded buffers: dispatching a cold
decode and immediately waiting preserves the stall. Cancellation, duplicate
requests, memory limits and shutdown also need explicit ownership. A cold miss
still requires a defined policy; synchronous fallback preserves current onset
semantics but cannot promise stall-free first use. No async audio implementation
or changed playback policy has been applied.
