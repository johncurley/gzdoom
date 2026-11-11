# Blackwater same-save reload latency — 2026-11-11

Status: matched warm Intel Metal reload comparison passes. The bounded UDMF
key cache reduces complete reload-to-resumed-loop latency by 65–72 ms in the
three process comparisons. This is a modest loading improvement; substantial
reload gaps and post-load work remain. No further engine optimization or tick
policy change is introduced by this experiment.

## Measurement contract

Prediction, recorded before measurement: at least 30 ms less median wall time
in both G_DoLoadGame and the request-to-completed-resumed-loop interval. Qualify
each separately against repeated-run variation. Measure the longest Update gap
independently rather than infer smoothness from parsing speed.

Temporary owner-thread steady-clock probes mark:

1. G_LoadGame request, before queuing the load action.
2. G_DoLoadGame entry and successful exit, including local destruction through
   an outermost scope guard.
3. The next outer D_DoomLoop boundary after loading: the load iteration,
   including D_Display and any nested wipe Updates, has finished.
4. The following outer boundary: the first resumed gameplay iteration,
   including its actual tick batch and display, has finished.

The primary endpoint is request to boundary 4. This is engine wall latency,
not physical presentation latency, input-to-photon latency, a single frame,
or time until every later resource is ready. Record actual G_Ticker calls in
the resumed iteration, because elapsed-time catch-up changes that count.
Also record the largest consecutive Update-hook interval observed across the
event; its leading endpoint may precede the request. Nested wipe Updates do
not count as resumed gameplay. Output occurs after recording the endpoint.

The control bypasses only ResolveKey's bounded cache and calls the original
FName(text). A runtime switch in the same executable selects each arm; the
request record captures the selected mode. No input processing, scheduler,
RNG, world iteration, wipe behavior or audio eligibility is changed. Existing
identity/fault/pixel controls are documented in the
[key-cache audit](udmf-key-cache-2026-11-10.md).

## Workload and controls

Same Blackwater MAP09 auto00.zds as the
[death/reload capture](blackwater-combat-reload-2026-11-10.md), copied to an
isolated savedir as start.zds. Each process first loads once for warmup without
timing, then runs six console-requested loads with no player input. This uses
the same G_DoLoadGame core as death/reload but does not reproduce the complete
death/menu interaction. It is not a normal level exit or a gameplay demo.

Three fresh processes, three measurements per arm per process: 18 retained
events. Modes are 0,1,1,0,0,1 in processes 1 and 3; process 2 reverses the modes.
Deferred exec files apply each mode at runtime immediately before requesting
the load. Commands are separated by 60 in-level frames. Initial loads are
excluded; OS caches are not cleared. Metal initialization is confirmed in
each log. Intel MacBookAir7,2, 1024×768, cap35, original vsync setting retained,
explicit background activity and snd_asyncprepare=1. Audio preparation's
production default remains off. All recorded endpoints are GS_LEVEL with
no menu or pause. No Instruments recorder, screen recording, identity oracle
or per-key counter runs during this timing experiment.

The first process completed successfully, but the harness then expected the
wrong startup text, "Initializing Metal". Actual logs say "Metal renderer
initialized successfully." The assertion was corrected and the existing
completed run validated; it was not discarded or rerun for a better number.

## Results

Medians, three loads per cell:

| Process | Load core, control → cached | Request through resumed loop, control → cached | End-to-end saving | Largest within-arm end-to-end spread |
|---|---:|---:|---:|---:|
| 1 | 488.580 → 432.349 ms | 1960.373 → 1894.996 ms | 65.378 ms | 24.796 ms |
| 2 | 487.952 → 417.981 ms | 1945.699 → 1881.139 ms | 64.561 ms | 25.087 ms |
| 3 | 490.476 → 427.646 ms | 1964.311 → 1892.418 ms | 71.892 ms | 14.137 ms |

Both predeclared thresholds pass in every process. Core savings are 56–70 ms
(11.5–14.3%), exceeding each process's largest within-arm core spread
(4.6–25.4 ms). Complete-interval savings are 3.3–3.7%, larger than the
within-arm spreads above. These are repeated warm reload results on this host,
not a confidence interval or a cross-platform guarantee.

Across all nine events per mode, median core time is 488.580 → 427.646 ms and
median complete interval is 1955.839 → 1892.418 ms. Complete-interval ranges
are 1925.697–1966.602 ms control and 1879.893–1916.454 ms cached: no overlap.
Use the process comparisons above for repeatability; subtracting pooled
medians is a different statistic and yields 63.420 ms.

The median of per-reload maximum Update gaps is **570.402 → 509.834 ms**.
Ranges are 548.279–579.265 ms control and 497.991–521.575 ms cached. Process
median reductions are 52–67 ms, each larger than its within-arm spread. This
qualifies a shorter observed reload gap, but gaps of roughly half a second
remain. These 18 event maxima do not establish a long-session p99.

Every event contains 44 Update-hook calls, including nested wipe Updates.
Request-to-load-loop-boundary medians are 1871.976 → 1812.551 ms. The remaining
time includes the existing display/wipe sequence; it is not all stalled CPU
execution. PerformWipe freezes game time and updates the screen in its own
loop. This experiment does not separately time every wipe substage and does
not justify removing it or resetting the clock.

The first resumed iteration has a pooled median of 80.395 → 79.840 ms, with
inconsistent per-process differences (+7.705, −0.755, −2.790 ms saved). There
is **no qualified improvement to resumed-loop cost**. Control events execute
17–18 resumed G_Ticker calls; cached events execute 15–16. These are measured
outcomes under the existing elapsed-time scheduler, not skipped ticks or a
deterministic replay comparison at an identical final gametic. No new sound
asset attribution is available from these wall timers.

## Artifacts and final state

`/private/tmp/gzdoom-reload-e2e-20261111` retains initial INIs, copied saves,
arguments, exec files, raw engine/stdio logs, all 18 rows, process/pooled
analysis, source/binary/asset hashes, probe binary, and exact source snapshots
before and during instrumentation. Summary files are `summary.json` and
`analysis.json`. The probe installation/restoration and run/analysis scripts
are temporary and remain outside the repository.

The exact pre-probe source was restored, preserving prior uncommitted work.
Temporary reload timers, counters, CVARs and loop/Update hooks are removed.
The final build and whitespace checks pass. Probe strings are present in the
experiment executable and absent from the final executable; all four restored
source files match their archived pre-probe bytes exactly. Results and final
hashes are recorded in `final-verification.json`. Earlier cache identity and
image checks still apply to the
identical restored implementation; this measurement does not add a simulation
threading or gameplay determinism claim.

## Remaining work, in priority order

1. The [sound follow-up](postload-sound-attribution-2026-11-11.md) now identifies
   the repeatable post-load assets and checks format/budget constraints. A
   temporary precache control prevents repeat decoding. Specify and qualify a
   bounded owner-side retention policy before extending worker admission.
2. Separate remaining map reconstruction, archive decode/hash, snapshot restore
   and synchronous bitmap acquisition costs. Select the next bounded change
   from repeated attribution; do not sum inclusive sample weights.
3. Qualify a survivable sustained-combat segment and a real normal level exit.
   The doorway pass remains scoped to its existing configuration. Match GL,
   cold/warm conditions and longer sessions before broad smoothness claims.
4. Revisit hwrenderer jobs only with measured scene-preparation cost and explicit
   input ownership, private scratch/output and ordered merge contracts. Preserve
   world/thinker/VM/GC ordering. Neither this experiment nor the earlier CPU
   capture proves GPU/driver work innocent.
