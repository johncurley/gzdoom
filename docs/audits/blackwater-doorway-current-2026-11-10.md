# Current-build Blackwater doorway capture — 2026-11-10

Status: three unprofiled current-build Metal manual-route captures complete.
The operator confirmed the doorway crossings felt smooth in all three runs.
The scoped three-run smoothness check passes. This is not a matched GL/Metal
comparison, deterministic replay or before/after improvement attribution.

## Setup

Current working-tree build passes. Copy the original Blackwater MAP09 start save
and benchmark INI into an isolated run directory. Metal at 1024×768, cap35,
original vsync setting retained, explicit background activity enabled. Enable
`snd_asyncprepare 1` for this experiment; the engine's default remains off.
The current graph lookup and Metal depth-clip changes are built in. No new engine
code, mod changes, user INI edits or instrumentation are introduced for this run.

Assets, executable UUID/SHA-256, save/config hashes, arguments and current git
state are retained in the run manifest. Archive the actual executable before
launch. Fresh process; OS/driver/shader caches are not explicitly cleared. Do
not call this a cold-cache run. Initial audio stats show three ready jobs, with
no failures or unfinished jobs before release.

Launch paused, with existing exec-file polling only while waiting. A ten-second
countdown allows the operator to focus the game. The route file enables existing
`vid_frametrace 2`, unpauses once, then disables logging after 1,400 rendered
frames (approximately 40 seconds) and pauses automatically. Polling resumes only
after measurement has ended. Native markers, stall dumps, overlays, video and
Instruments are disabled for this light capture. The game is left paused for
operator feedback and subsequent work; it is not an unattended stationary run.

Prediction before release: zero frames above 100 ms during the requested manual
crossing with prepared audio. Any observed longer intervals reject that numeric
hypothesis and require attribution. A successful single run does not prove all
transition hitches are fixed; lower-amplitude pacing defects also matter.

## First candidate results

Artifact directory: `/private/tmp/gzdoom-doorway-current-5392d8pz`.
Control/analysis scripts: `/private/tmp/gzdoom-doorway-20261110`.

| Measurement | Result |
|---|---:|
| Complete two-second windows | 19 |
| Intervals retained | 1,342 |
| Selected exposure from rounded means/counts | approximately 38.302 s |
| Maximum interval | 45.40 ms |
| Intervals above 100 ms | 0 |
| Intervals above 33.3 ms | 77 |
| Per-window p95 range | 31.73–35.99 ms |
| Per-window p99 range | 32.46–37.29 ms |
| Prepared audio ready hits at end | 3 |
| Not-ready fallbacks / failures | 0 / 0 |

The exposure estimate has at most approximately 0.00671 s error from rounding
window means alone. Final partial-window samples are not emitted by the existing
logger and are excluded. Do not pool window percentiles into a whole-run p95/p99.
`frame-summary.json` retains every complete window and the analysis limitations.
All three prepared jobs are consumed and reservation returns to zero. No CPU
stacks, scheduling states, filesystem or GPU execution data were recorded, so
these logs do not establish the cause of any smaller visible hitch.

## Qualification limits

The user confirmed "Confirmed no hitches." after the crossing/smoothness
question. The run's manifest, summary and operator-observation JSON retain that
confirmation. There is no automated crossing marker or
per-frame menu/focus context in this light run. Manual input is not a demo or
world-state equivalence proof. First and warmed crossing timings cannot be
separated from the log alone. The three route confirmations and repetitions are
now complete; profile a remaining reproducible event separately if one appears. Keep first
visits, warmed visits and level transitions labelled separately. Do not add new
optimizations based only on average FPS or a sampled CPU parent percentage.

## Fresh-process repeat

A second process loads the same save and initial INI, with the same executable
and mod hashes (verified across both manifests). It starts paused, reports all
three audio buffers ready, and follows the same countdown/manual-route protocol.
Artifact directory: `/private/tmp/gzdoom-doorway-current-9hs6_5wd`.

| Run | Complete windows | Intervals | Selected exposure | Maximum | >100 ms | >33.3 ms | Operator observation |
|---|---:|---:|---:|---:|---:|---:|---|
| 1 | 19 | 1,342 | ~38.302 s | 45.40 ms | 0 | 77 | Doorway crossing confirmed; no hitches |
| 2 | 19 | 1,344 | ~38.367 s | 57.79 ms | 0 | 45 | Doorways confirmed smooth; no hitches |
| 3 | 19 | 1,341 | ~38.283 s | 38.52 ms | 0 | 39 | Crossing confirmed smooth; no hitches |

Repeat per-window p95 range is 31.57–35.21 ms; p99 range is 31.89–38.42 ms.
No whole-run percentile is derived. The 57.79 ms maximum appears in window 12;
without a crossing marker it cannot be assigned to the doorway. Two prepared
audio variants are consumed, with one still ready at the end, and no failures
or unfinished-result fallbacks. Different ready-hit counts are not replay
failure evidence: these are manual workloads with uncontrolled random variant
selection, not deterministic input/state matches.

There are zero >100 ms intervals across approximately 76.669 selected seconds,
and both route crossings are operator-confirmed smooth. The user reported
"Yes, doorways felt smooth. No hitches." for run 2. The old severe
stall class is absent from these retained timing windows. This does not prove
all gameplay pacing issues are resolved, identify a cause for the 57.79 ms
interval, or isolate the benefit of any individual optimization. Series JSON
and per-run observations are retained in `/private/tmp/gzdoom-doorway-20261110`.

## Three-run disposition

The third fresh-process run is retained at
`/private/tmp/gzdoom-doorway-current-djfz13b4`. Executable, save, mod and initial
configuration hashes match all three manifests. Its per-window p95 range is
31.85–33.69 ms and p99 range 32.44–34.38 ms. Three prepared buffers are consumed,
with no not-ready fallbacks or failures. The user confirmed: "Yes smooth no
hitches, crossed it." The capture application was then queued to quit cleanly.

Across the 57 complete windows: **4,027 intervals, approximately 114.952 selected
seconds, zero >100 ms intervals**, and a maximum of 57.79 ms. All three manual
crossings are confirmed smooth by the operator. The prediction of zero >100 ms
intervals passes in each run, under these conditions. This is a useful scoped
smoothness result, rather than a higher-FPS claim.

The current-build Metal doorway check is complete for this save, mod, cap35 and
explicitly enabled audio preparation, with warm/unspecified OS/driver caches.
It does not qualify uncapped/high-refresh play, cold-cache behavior, other
backends/maps, long sessions or level transitions, and it does not isolate
individual graph/audio/depth-clip contributions. No engine edits were made to
obtain this result. The prior build gate and final documentation whitespace
check pass. All per-run observations and the aggregate are retained in
`route-series-summary.json` in the control directory.

A new Instruments run is not needed to attribute an absent >100 ms event.
The next smoothness workload should exercise heavy gameplay or a reproducible
level transition; an event capture becomes useful when a remaining stall is
observed. The opt-in audio default is unchanged by these runtime checks.
