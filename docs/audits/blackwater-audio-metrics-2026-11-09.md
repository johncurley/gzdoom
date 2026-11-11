# Blackwater audio metrics and remaining CPU candidates — 2026-11-09

Finding: bounded thunder preparation removes the large frame spikes in the
stationary saved-world control. The remaining repeated CPU candidate is material
dependency/upload-read bookkeeping. This is not a completed doorway-route,
uncapped throughput, GPU occupancy or cross-backend baseline.

## Frame-interval acceptance

Prediction before measurement: enabled runs consume ready buffers and remove
the >100 ms thunder-associated spikes reproduced by disabled controls. Do not
infer an uncapped FPS gain from a 35 fps capped fixture.

Six fresh processes, order off1/on1/on2/off2/off3/on3, same original MAP09 save,
mod, dedicated config, Metal at 1024×768, and warm OS caches. Sound buffers start
fresh in each process. Background activity/pause settings are explicitly matched.
No route input, developer logging, native markers, stall dumps or Instruments
are active in these runs. Existing `vid_frametrace 2` starts after the first
in-level rendered frame; preparation counters are printed twice.

Compare the first **nine complete two-second windows** per run. Final partial
windows are not available from the existing logger and are excluded. Two off
runs logged a tenth complete window; exclude it too for equal exposure. Selected
durations, estimated from rounded window means and counts, are 18.126–18.175 s.
Do not pool window p95/p99 values into a fictitious whole-run percentile.

| Run | Maximum selected interval | Frames >100 ms | Ready hits at final counter |
|---|---:|---:|---:|
| off1 | 240.47 ms | 2 | no worker |
| on1 | 38.04 ms | 0 | 2 |
| on2 | 42.00 ms | 0 | 3 |
| off2 | 202.67 ms | 2 | no worker |
| off3 | 241.90 ms | 3 | no worker |
| on3 | 39.58 ms | 0 | 3 |

Across approximately 54.5 selected seconds per arm: **7 intervals >100 ms in
disabled controls, zero enabled**. Enabled maximum variation is 3.96 ms, much
smaller than the removed 100–240 ms tail. All enabled runs initially have three
ready jobs, and no not-ready fallback or preparation failure. Final counters
cover the full process run, not exactly the nine-window selection.

Medians of each run's window p50 values remain 28.54–28.74 ms disabled and
28.55–28.65 ms enabled, consistent with the 35 fps cap. The improvement is in
hitching; this does not establish a higher rendering/simulation ceiling.
Stationary weather reproduces this class of stall without crossing the doorway;
other crossing-triggered costs are not ruled out. Different random variants and
ordering appear between runs, so this is not a deterministic demo replay.

Artifacts: `/private/tmp/gzdoom-audio-frames-t34y0pd7`, including per-run arguments,
binary/save hashes, raw logs and `summary.json`. No engine code or user config
was changed for these measurements.

## CPU attribution, separate from timing acceptance

System Trace attempts did not qualify: one sampler timed out before recording;
another target exited before a complete trace saved; a notification-gated
30-second recording reached its limit but saving exceeded the harness timeout.
Its terminated trace was malformed on export. Do not use these as evidence of
an engine stall or assert that the recorder could never finish given more time.
Rejected roots: `gzdoom-audio-metrics-20261109-201748`, `...-202739`, `...-204114`
under `/private/tmp`. Fresh scheduler running/blocked attribution remains open.

A lighter **Time Profiler + Points of Interest** capture succeeded once per
setting. Each target starts paused. An existing `execafter`/exec-file gate keeps
it paused until the recorder emits its started notification, then unpauses once.
No temporary engine barrier/instrumentation is added. `vmmap` collects the image
mapping while paused. Postprocessing uses the archived executable, its recorded
UUID/hash, and the actual per-process load address; ASLR bases are not reused.

Both captures end normally at the time limit: 30.304633 s enabled, 30.188249 s
disabled. Analysis selects 15 s starting 100 ms after the first observed
paused→unpaused Update transition. Require continuous Update ordinals, GS_LEVEL,
menu off, pause off, and exclude post-capture marker spill. Both issue stores
contain only the common `Data stream: Time Mapping` entry, with no explicit
lost-event warning. Save load/profiler setup/paused work is outside selection.

| Fifteen-second selection | Enabled | Disabled |
|---|---:|---:|
| Update points | 525 | 512 |
| Gametic delta between selected endpoints | 524 | 522 |
| Largest Update gap | 41.91 ms | 247.32 ms |
| Largest complete tick interval | 13.24 ms | 224.53 ms |
| Main-thread sample weight | 7,290 ms | 6,843 ms |
| LightningFlash inclusive sample weight | 15 ms | 482 ms |
| LightningFlash **and synchronous OpenAL LoadSound** ancestry | 0 ms | 447 ms |

The three largest disabled gaps are 247.32, 184.24 and 123.33 ms; their sampled
stacks include LightningFlash → synchronous loading with Vorbis decode leaves.
The enabled lightning path consumes prepared buffers and has no sampled
synchronous decoder ancestry. Other synchronous sounds remain: 119 sampled ms
under generic OpenAL LoadSound in the enabled window, distributed across smaller
events. Do not interpret this as all audio having moved off main.

CPU sample weights are inclusive, nested and not wall/frame-time percentages.
No blocked duration is inferred from missing samples. The enabled native focus
context is active for 409 Updates/inactive for 116; disabled is active for all
512. Explicit background settings maintain activity and cadence, but this plus
single-pair sampling prevents a precise whole-CPU-cost on/off comparison.
Use these captures for event attribution and candidates, not net CPU speedup.

## Remaining candidates

Enabled selection, denominator 7,290 ms main-thread sample weight:

| Path | Inclusive sample weight | Share |
|---|---:|---:|
| Scene-material graph observation | 1,368 ms | 18.8% |
| Resource-read observation, nested within the above | 627 ms | 8.6% |
| Thinker iteration including invoked work | 558 ms | 7.7% |
| P_TryMove, overlapping thinker/native work | 391 ms | 5.4% |
| Scene creation | 252 ms | 3.5% |
| P_CheckPosition, nested movement work | 207 ms | 2.8% |
| FrameGraph reset | 159 ms | 2.2% |
| Other synchronous generic sound loading | 119 ms | 1.6% |

Rows overlap; do not add them. Disabled material observation also remains large:
1,121 sampled ms (16.4%). Within enabled material-observation ancestry, leaf
weights include strcmp 195 ms, allocation/free/pointer-size routines, and
SuperFastHash 90 ms. This is direct caller evidence of allocation/string/hash
overhead, not a claim from a hot allocator name alone.

The earlier cache avoids repeated dependency searches, but upload-read tracking
and cache lookup still construct temporary `FString`s on bindings. The next
bounded candidate is reducing that allocation/lookup work while preserving
upload observation sequence and canonical resource identities. Stable IDs or
allocation-free lookup need an explicit lifetime/alias contract before edits.
Movement/spatial queries are a secondary candidate; these captures do not
justify parallel actor ticking or a GC/VM redesign. GPU cost was not measured.

CPU artifacts: `/private/tmp/gzdoom-audio-cpu-20261109-210058`, including archived
binary, raw trace/exports, image maps, per-process symbol files, bounded sample
JSON and summaries. The lighter captures are separate from unprofiled frame
timing acceptance. No new optimization is implemented in this measurement turn.
