# Post-load attribution and axis input lookup — 2026-11-12

## Scope and prediction

Continue from 9d24d62371 on engine-performance-regressions, using the frozen
Blackwater MAP09 save and input identities in the
[macOS handoff](../handoff-macos-2026-11-11.md). Recordings began on November 11;
analysis and qualification continue November 12. Retention is explicitly zero,
sound precache/PNG/upload batching keep their defaults, and no mod/save is
modified. This is automatic load/resume work, not combat/demo replay.

Prediction before the baseline captures: each of two fresh processes has a
post-load interval with at least ten actual ticks and 25 ms of inclusive
synchronous sound-loading sample weight. Both pass.

## Current attribution

Two 25-second System Trace recordings include Time Profiler, Points of
Interest and Filesystem Activity. The exact executable, UUID, ASLR mapping,
source snapshots, package/input hashes and raw exports are retained. CPU
samples exclude waiting threads; scheduler intervals from the same recording
supply running/blocked time. The target main thread is filtered by process
PID as well as thread name.

| Interval | Update gap | Completed ticks | Main running | Main blocked | OpenAL LoadSound samples | Axis input samples |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Process 1, first resume | 121.38 ms | 35 | 119.05 ms | 0.40 ms | 53 ms | 11 ms |
| Process 1, warm 1 | 91.25 ms | 18 | 89.49 ms | 0.24 ms | 50 ms | 11 ms |
| Process 1, warm 2 | 80.85 ms | 18 | 79.58 ms | 0.09 ms | 43 ms | 8 ms |
| Process 2, first resume | 146.64 ms | 35 | 132.63 ms | 11.24 ms | 61 ms | 15 ms |
| Process 2, warm 1 | 92.38 ms | 18 | 90.74 ms | 0.09 ms | 53 ms | 8 ms |
| Process 2, warm 2 | 89.09 ms | 17 | 87.32 ms | 0.09 ms | 53 ms | 10 ms |

Running/blocked rows leave interrupted/runnable/preempted time; all state
durations together cover each interval without overlaps. Rows identify the
first tick-bearing resumed interval with sound loading; an earlier Update can
have no completed tick. DecodeSound accounts
for 33–48 ms of inclusive sample weight. RenderView has 2–5 ms and BeginFrame
0–1 ms. Inclusive call stacks overlap; do not sum them as exclusive costs.
The first-load texture burst remains a separate renderer/resource workload.

Five intervals have 0.26–0.40 ms of filesystem syscall wall time. Process 2's
first resume has 13.94 ms, alongside its longer blocked period. These are
logical file calls, not a physical disk throughput measurement. Most measured
post-load time is CPU execution, while occasional I/O waits still matter.

Both exports have no duplicate marker begins or unknown event types.
Incomplete scopes lie at recording/window edges; completed interior tick
intervals and scheduler coverage are used for the table. No blanket claim of
loss-free OS collection or GPU innocence follows from this.

## Sound discovery control

A temporary PK3 provides only the existing GameInfo declaration:

~~~text
GameInfo
{
    PrecacheSounds = "special/fleshdrop", "gasbag/active",
                     "special/geigermed", "weather/rain"
}
~~~

Six unprofiled diagnostic processes use off/on/on/off/off/on order, each
loading the save three times. Existing snd_precache_audit records successful
decoded-buffer publications after each CacheMarkedSounds boundary.
Predeclared prediction: enabled runs have zero deferred publications in all
nine load windows, and disabled controls have positive counts.

All nine enabled windows report zero; disabled windows report 9–10. This is
evidence for missing precache discovery/timing, and agrees with the older
[sound attribution](postload-sound-attribution-2026-11-11.md). The windows also
contain later gameplay; these are not exact first-tick miss counts or new
per-asset identity probes. Global hints preload the staging map too, and
diagnostic hashing/logging adds overhead. No cold-load speedup, audio parity
or world-state equivalence is inferred from this control.

The fixture is not installed, committed or hardcoded into the engine.
Generic sound discovery/retention remains separate follow-up work.

## Bounded input change

A second hotspot sits outside the thinker/VM work:
G_BuildTiccmd -> ButtonMap::GetAxes -> FButtonStatus::AddAxes ->
FKeyBindings::GetKeysForCommand. Each button constructs positive/negative
key lists by scanning every binding, even when every joystick axis is idle.

AddAxes now reads only the binding of each active axis. It compares the sign
and case-insensitive command suffix, then accumulates in the original axis
order with the original add/subtract and clamp operations. There is no cache,
new threading or invalidation policy. Bind/unbind changes are visible on the
next call. Neutral axes still clear the analog flag/value; digital fields
remain untouched. Controller polling and VM/serialization formats are unchanged.

The explicit input_axes_selftest compares the actual method against the
original full-key-list membership algorithm. Its fixed local integer sequence
does not consume game RNG. Sixteen binding maps cover mixed case, unrelated
keys, positive/negative commands, compound/whitespace commands, single axes,
neutral/negative inputs, simultaneous axes and clamp behavior. It checks
digital fields and detects a deliberately corrupted analog result. Bindings
are restored by an owner-thread RAII guard, including exception exits.

Both profiled launches pass 1,984 comparisons with zero failures and 1,984
negative controls. The permanent native regression runner now requires this
diagnostic alongside its existing texture/audio/graph controls.

## Matched profile result

Prediction before measuring: at most 1 ms of axis input sample weight per
resumed interval, at least 5 ms below each baseline interval. Two matching
recordings pass: all six resumed intervals have zero AddAxes/GetAxes binding
scan samples, versus 8–15 ms before. Zero samples means below sampling
resolution, not zero CPU execution.

Post-load gaps remain: first resumes are 100.30/140.73 ms, and warm resumes
80.44–89.70 ms. Tick counts and other costs vary; these profiled totals are
not a clean end-to-end speedup estimate. Synchronous sound loading remains.

## Unprofiled comparison and regression gates

Six processes compare the archived baseline executable with the candidate,
interleaved old/new/new/old/old/new. Both keep sound preparation enabled and
retention at zero. The baseline runs in a copied app with identical packages;
actual argv and both executable hashes are retained. No Instruments, native
markers, PCM audit or stall logging runs in this timing arm.

The metric is the largest non-loading Update maximum in the one-second
frame-trace windows during the first/warm workload. Exactly one window at
least 300 ms must identify the load in each phase; all other maxima are
retained. Earlier traced loads exceed 500 ms and resumed gaps are below
150 ms. This is a workload-window metric, not an exact first-tick duration.

| Arm | First resumed maximum median/range | Warm resumed maximum median/range |
| --- | --- | --- |
| Original scans | 114.82 ms / 114.00–124.06 ms | 79.65 ms / 78.30–83.43 ms |
| Direct lookup | 105.09 ms / 100.91–107.16 ms | 74.03 ms / 66.89–76.02 ms |

Median reductions are 9.73 and 5.62 ms. All three candidates are below all
three controls in these windows, but the within-arm spread is material,
particularly for warm loads. Keep the ranges and small sample size with the
result; no universal saving or hitch-free outcome is claimed. One control's
first loading interval is 3,394.84 ms versus roughly 2.24–2.27 seconds in the
other runs. It is retained; no cold-loading improvement is attributed to this
input change.

All five native modes pass on Metal and OpenGL. Each launch reports all 1,984
input comparisons and negative controls, with zero failures. Decoded fixture
pixels remain identical to the pre-change captures within each backend; audio,
retention, PNG and upload-counter controls still pass. All 20 Python tests pass,
and MAP06 baseline/SSAO GL-versus-Metal comparisons pass. The full Intel macOS
build, adjacent package comparison and whitespace checks pass. No full golden
matrix or other-platform runtime pass is claimed.

## Artifacts and remaining work

- /private/tmp/gzdoom-postload-current-20261111-213517: baseline executable,
  source, traces, postload-summary.json and filesystem-summary.json.
- /private/tmp/gzdoom-postload-hints-current-20261111-220124: six metadata
  controls, temporary fixture, manifests and publication records.
- /private/tmp/gzdoom-postload-axes-20261111-225017: candidate executable,
  source, traces and native diagnostic output.
- /private/tmp/gzdoom-axis-timing-2z_etneh: old/new unprofiled comparison.
- /private/tmp/gzdoom-axis-regression-metal and -gl: five-case native suites.

Keep original executable UUID/hash for symbolication. Native Windows/Linux
builds and physical controller acceptance remain pending. The software input
test covers computed axis semantics, not device polling/haptics. Next address
generic sound-precache coverage or measured repeated-load retention under a
separate ownership/compatibility contract. Do not assume moving OpenAL
publication or parallelizing the VM solves the remaining misses.
