# Engine instrumentation audit and implementation order — 2026-11-09

Scope: source inspection of existing timing/counter paths and their suitability
for the [Blackwater doorway investigation](blackwater-doorway-profile-2026-11-09.md).
The initial audit is source-only. The first measured instrumentation correction
is recorded below; no tracing framework is implemented. Existing source-
dependency findings stand.

## Verdict

Retain the current tools, but do not treat their outputs as an exclusive,
time-aligned engine profile. The largest immediate gap is aligning a specific
long frame with CPU stacks and scheduling/wait activity. Fix reporting semantics
before adding more per-function timers. Keep frame, loop, tick, backend-submit
and presented-frame identities distinct: they are not one-to-one.

## Inventory and confirmed source limitations

| Instrument | Source and boundary | Finding | Disposition |
|---|---|---|---|
| `cycle_t` | `src/common/engine/stats.h` | On this Intel Mac it uses calibrated TSC differences, not per-thread CPU accounting; elapsed scopes include descheduling/waits. Clock/Unclock are arithmetic accumulation without nesting/exit validation. | Keep, label duration honestly; validate each scope's exits before trusting it |
| `glcycle_t` / render timers | Same header; `hw_clock.cpp`, `checkBenchActive` | Clock reads are enabled by `stat rendertimes` or `bench`; the display refreshes about once per second. Several totals are subtraction-based and not an exclusive call tree. | Diagnostic arm only; record gating and refresh cadence |
| `vid_frametrace` | `v_framebuffer.cpp`, `TraceFrameInterval` | Measures Update-to-Update wall time and percentiles, including present/cap waits. Stores samples in a vector, copies/sorts and prints at report boundaries. No per-frame IDs, retained event timestamps or profiler-clock mapping. | Keep for distributions; compare observer on/off; not a CPU-utilization measure |
| `vid_stalltrace` | Same file; `VLoopPhase`/`V_LoopTraceBoundary` | Coarse inclusive wall durations. `playsim` wraps TryRunTics including GC/waits/loading/sound. Nested playsim/load/precache can double-count in the non-wrapper sum. Only 16 distinct name pointers fit; overflow drops silently. | Do not sum into percentages; do not widen with fine-grained scopes |
| Metal graph-name ring | Same file; `RecordFrameGraphSnapshot` | Bounded 128 snapshots, 1024-byte text per snapshot. Formatted names are collected only on Metal while stalltrace is enabled. Overwrites/truncation are reported, but text formatting and reporting are observer work. | Useful context, not a zero-cost or matched GL observer |
| `mt_frametrace`, metrics and `mt_stalltrace` | `mt_debug.cpp`, Metal render-device boundaries | Renderer bracket differs from whole-frame interval. Stall lines contain local frame index and duration but lack an explicit shared timestamp with CPU sampling. Totals and per-frame metrics have different windows. | Keep backend context; do not equate its index or duration with engine tick/presentation |
| `stat think` | `dthinker.cpp`, `RunThinkers` | Resets around collection execution; count is ticked thinkers, not resident actors. Includes non-actor thinkers and downstream effects/light work; latest collection can differ from all levels/tics per frame. | Keep; collect per-tick/all-level denominators separately |
| `profilethinkers -t 20` | Same file; `ProfileThinkers` | Profiles one collection turn, sorts/prints then disarms. PostBeginPlay precedes the class timer. Per-call timing, class map and print/sort overhead differ from normal execution. | Separate diagnostic run; top 20 classes, not 20 seconds |
| `stat VM` | `vmframe.cpp`, `ADD_STAT(VM)`; `stats.cpp`, `PrintStat` | History shifts/resets in GetStats, called while displaying stats. Label says “last 10 tics,” but entries are driven by overlay reads, not a tick-boundary sampler. Native transitions pause/resume VMCycles, so it is not all time attributable to scripts. | Correct cadence/meaning before using for trends; retain sampled native callers |
| VM/JIT call counter | `vmexec.h`, `jit.cpp::IncrementVMCalls` | Interpreter and JIT increment VMCalls; timer and call-count coverage are not identical. FirstScriptCall may compile lazily. | Distinguish first-call compilation, dispatch, script body and native work |
| `stat ACS`, `acsprofile` | `p_acs.cpp` | ACS elapsed timing and instruction-count rankings measure different quantities. ACS may be nested within thinker/script work. | Keep units/windows explicit; no instruction-to-ms conversion |
| `stat sight` | `p_sight.cpp::P_CheckSight` | Null/visibility-blocked early returns bypass Unclock after Clock. The arithmetic timer has no guard to repair the imbalance. Counters reset at tick-related sites. | Timer correctness defect; do not rely on reported cost until repaired/controlled |
| `stat gc` | `dobjgc.cpp` | GC state/steps and allocation-rate history; not a complete allocator profile or directly per-render-frame data. | Pair with sampled GC/allocator stacks; Allocations is a separate observer arm |
| Time Profiler export analyzer | `tools/profiling` | Resolves references, windows and optional engine symbol mappings. Percentages are inclusive sample-weight shares. No automatic mapping discovery, frame correlation or blocked-time reconstruction. | Retain; validate UUID/image mapping and event window for every process |

Source findings are proven by inspection, not live fault-injection tests. For
example, the missing sight timer exit is a concrete correctness defect, but no
claim is made about its contribution to the player's hitch. Render graph
dependency bookkeeping is distinct from diagnostic overlays: turning overlays
off does not remove the measured scene-material observer path.

## Minimum additions, in order

1. **Capture provenance and window qualification.** Preserve exact input/save,
   assets/load order, binary UUID, live backend/resolution, settings and cache
   history. Record capture end reason and actual duration. Separate reload,
   menu/focus, warm gameplay and transition windows. Complete native replay
   qualification if a suitable new-game route can be recorded; otherwise label
   the manual route and report its variability. Avoid converting an existing
   save into a new-game demo by assumption.
2. **Validate the platform timeline before adding engine code.** Capture the
   same crossing with System Trace and, where supported, CPU stacks/GPU activity
   in a common recording. First verify export/readability. Preserve separate run
   IDs if those data cannot be captured together. A CPU-only sample table with
   waiting threads disabled cannot clear GPU or filesystem waits.
3. **Repair only instrumentation we will use.** Give sight timing balanced exits
   without changing query results/RNG order. Correct the VM history label/cadence
   contract rather than calling overlay reads tics. Expose stall-bucket overflow
   and distinguish inclusive scopes/remainder from a false exclusive total.
   Each is a separate bounded increment with a failing control; no game-loop or
   VM execution refactor is implied.
4. **If platform data lack engine boundaries, add a minimal opt-in marker.**
   Record fixed-size numeric events for loop/frame boundaries, actual executed
   ticks and lifecycle/focus context, linked to one verified monotonic clock.
   Export after the measured window, with dropped-event counts. Provide a
   measured Instruments clock correlation or a supported marker track; never
   assume steady_clock, TSC, media time and trace-relative seconds are identical.
   Disabled mode must not allocate/read clocks/format output in the hot path.
   Use existing containers/ownership idioms; no borrowed actor pointers.
5. **Add one targeted event family only after alignment works.** Likely first
   candidates are sound acquisition/decode/publication and scene-material
   observer call/scan counts. Numeric stable asset/resource IDs and bounded
   counters should replace per-call strings/logging. A sound load event should
   distinguish cache hit/miss, format/bytes and preparation stage; resolving
   names belongs outside the capture. Counts establish scale but do not alone
   establish cost or safe worker ownership.

Items 3–5 are proposed implementation increments, not implemented features.
An event-buffer capacity and overwrite policy, frame/tick identity/reset rules,
clock-correlation mechanism and cross-platform interfaces must be settled in a
narrow contract before writing the first engine-marker patch. This is local
instrumentation design, not authorization for a tracing framework rewrite.

## Measurement acceptance gates

- State numeric predictions before each control. A labeled 100 ms development
  delay must be detected in the intended interval; predict duration within
  10 ms and profiler-clock alignment within 1 ms, then measure whether the host
  actually meets those bounds. Do not use the injected run as gameplay baseline.
- For timer repairs, exercise all affected early exits and repeated calls;
  the original unbalanced version must fail the accounting check. For nesting
  and overflow reports, deliberately exceed capacity and nest a known interval.
  Reports must flag missing data and never silently invent exclusive time.
- Demonstrate that marker/timer changes preserve gameplay outputs and RNG
  consumption. Use build gates and relevant deterministic checks; no native
  demo or exhaustive arbitrary-mod state hash is assumed to exist.
- Compare instrumentation off/on under repeated matched routes. A >5% median
  or p99 shift flags contamination; a smaller result still needs its spread
  reported. Avoid attributing within-noise changes. Check video-on separately.
- Retain absolute ms/tick, ms/frame, tics/frame, whole-frame p95/p99, event
  counts and trace sample denominators separately. No addition of inclusive
  percentages. Track count must distinguish ticked thinkers, resident actors,
  visible sprites and light interactions if those quantities are collected.

## Optimization and modernization sequence

| Stage | Work | Exit gate |
|---|---|---|
| A — trust the data | Instrumentation fixes/alignment, qualified save/manual or native-demo route, current GL/Metal settings frozen | Event-local frame/CPU/wait attribution with observer overhead known |
| B — qualify the workload | Repeat heavy gameplay, doorway and level-transition fixtures; at least three matched runs per backend/cache condition | Separate gameplay and hitch rankings with spread; exact event alignment |
| C — one measured optimization | Choose sound-cache/preparation, graph lookup/scanning, actor/query/VM or another demonstrated cost | Fix-disabled control, improvement beyond noise, unchanged required behavior |
| D — serial algorithm and ownership work | Remove measured redundant traversal/lookups/allocation; define owned inputs/results only where a consumer exists | Lower critical-path time with compatibility checks; cost of copies/dispatch known |
| E — bounded asynchronous services | Move only genuinely independent decode/math/query kernels; keep authoritative tick observations and mutations ordered | Defined cancellation/lifetime/publication, deterministic results, net wall-time benefit |

Current evidence gives sound loading and material observation **candidate**
status. It does not decide stage C's winner, prove that every actor is expensive,
or clear the GPU. Investigate actor count using class/tick time plus visible
sprite/light/scene cost; do not use destructive actor removal as a matched
baseline. Keep existing detached pixel workers, backend parity tests, frame
graph dependencies and legacy VM/thinker semantics. Renderer correctness work
continues as needed; new speculative rendering optimization campaigns and
generic concurrent actor ticking remain outside this tranche.

## Verification of this document

Entries above were checked against the named source paths. The initial audit
added no timers or engine changes. The follow-up below qualifies the first
accounting correction; the remaining proposed instrumentation and the actual
doorway cause are still pending.

## First implementation increment — sight accounting and VM label

The inventory above records the pre-fix source findings. The first bounded
implementation adds the missing `SightCycles.Unclock()` before the null-actor
and visibility-blocked returns in `P_CheckSight`. It also changes the VM display
label from “last 10 tics” to “last 10 display samples”; history collection and
VM execution are unchanged. Nested stall accounting, overflow reporting and
clock/frame correlation remain pending.

Prediction stated before the control: the unbalanced exits leave negative time;
after correction each trivial query reports a non-negative duration below 10 ms
with the same result. A temporary in-engine console probe ran on stock DOOM II
MAP01, Intel Metal, 1280x720, `-nomonsters`, with an isolated config/save folder.
It invoked null/null, null/source, null/target, visibility-blocked and ordinary
self-sight queries, followed by 100 consecutive null queries. The valid query
used ignore-visibility/water flags, avoiding the randomized visibility branch;
the pawn's flags and saved timing accumulator were restored by the probe.

| Case | Control accounting | Corrected accounting | Query result in both builds |
|---|---|---|---|
| null/null | -13,012,057.263 ms, FAIL | 0.001208 ms, PASS | false |
| null source | -13,012,057.622 ms, FAIL | 0.000170 ms, PASS | false |
| null target | -13,012,057.924 ms, FAIL | 0.000407 ms, PASS | false |
| visibility blocked | -13,012,058.204 ms, FAIL | 0.000106 ms, PASS | false |
| ordinary self sight | 0.019338 ms, PASS | 0.042844 ms, PASS | true |
| 100 null calls | -1,301,205,930.205 ms, FAIL | 0.001617 ms, PASS | zero true results |

These values establish balanced accounting and unchanged outcomes for these
paths, not a speed comparison. Differences among the small positive durations
are not performance findings. VM label output was checked in both builds;
`r_framegraph_selftest` passed in both. The first bundle launch did not reach
the probe and was excluded; direct executable launches produced the evidence.

The temporary probe was removed before the final rebuild. It is not a shipping
command or VM binding. Verification logs and the removed probe are retained
outside the repository in `/private/tmp/gzdoom-instrumentation-20261109` as
`before.log`, `after.log` and `probe.txt`. No broad replay determinism, actor
scaling, GPU-wait exclusion or hitch improvement is claimed by these tests.
Rebuilds change the on-disk executable; older Instruments traces must continue
to use their recorded UUID/symbol mapping rather than the newly built binary.

## Timeline increment

The [native marker contract and measurements](engine-profile-markers-2026-11-09.md)
now qualify the combined System Trace/CPU-sampling capability and opt-in macOS
loop/tick/Update markers. That supplies an engine-to-profiler timeline without
modifying tick order. Actual Blackwater event attribution and observer overhead
remain pending; the inventory above describes the pre-marker reporting limits.
