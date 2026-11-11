# Engine performance and dependency audit — 2026-11-08

Source baseline: `caacc40847e5637548fc69b85911fa02c08c9fa9`, clean working tree
at inspection. Host: x86_64 macOS; configured build: RelWithDebInfo,
`-O2 -g -DNDEBUG`. Metal is held as the current rendering baseline.

Status: **source dependency audit complete for the entry points below;
workload measurements pending**. No gameplay route, heavy map, or current hitch
save has been supplied. No new runtime profile was captured for this audit.
Consequently there is no measured hotspot ranking, frame-time percentage, or
estimated speedup yet. This is an entry-point/ownership audit, not exhaustive
proof of thread safety for every callee.

## Findings that determine the next experiment

1. A slow `playsim` bucket does not establish slow actor simulation.
   `D_DoomLoop` wraps `TryRunTics()` in that label, but `TryRunTics()` includes
   GC, tick waiting, networking/local command processing, game actions, multiple
   simulation tics, prediction, sound, and client-side logic. Measure CPU running
   time separately from wall time and record tics executed per loop.
2. Current thinker execution is ordered and includes fresh thinkers in the
   same turn. Snapshotting all actors at tick start and committing in list order
   would not preserve what later actors currently observe.
3. Existing collision and sight APIs are not pure worker jobs. Collision can
   invoke gameplay side effects; sight uses shared scratch, visitation markers,
   and named RNG streams. Private output storage alone is insufficient.
4. Scene preparation shares mutable world data with simulation. A copied
   viewpoint or draw-list array does not provide independent ownership.
5. A detached pixel-processing worker boundary already exists on Metal.
   Source bitmap acquisition happens before queueing. Moving more conversion
   to workers cannot remove acquisition/decode time that remains upstream.

## Current dependency classification

The order below follows execution/dependency investigation, **not measured
expense**. `Pending` means unmeasured for the requested workload. Expected
payoff is conditional; none of these rows authorizes implementation.

| Subsystem | % frame time | Hitch contribution | Mutable dependencies | Safe concurrency model | Expected payoff | Risk |
|---|---|---|---|---|---|---|---|
| Tick scheduling, game actions, prediction | Pending | Pending | `gametic`, client commands/sequences, players, `gameaction`, level lifecycle | Serial by semantics for authoritative state; separate waits from computation first | Unknown; scheduling waits are not CPU work | High |
| Actors/thinkers, spawning and destruction | Pending | Pending | Intrusive thinker lists, `NextToThink`, fresh lists, actor fields, GC roots, RNG and callbacks | Serial by semantics; algorithm/data-layout first; only extracted pure kernels are candidates | Unknown; reduce repeated traversal/allocation if measured | Very high |
| ZScript VM/JIT and ACS | Pending | Pending | Actors, globals, native callbacks, GC objects, script state and RNG | Existing script execution stays serial; isolate pure native computations only after call-graph audit | Unknown; dispatch/allocation/native transitions need separate attribution | Very high |
| Collision, blockmap and movement | Pending | Pending | Actor position/height/blockers, block links, line/polyobject visitation, pickup/damage callbacks | Algorithm first; a new side-effect-free candidate kernel could use owned snapshot inputs and private scratch | Unknown; count candidates and repeated queries before choosing | Very high |
| AI sight/perception | Pending | Pending | Live actors/sectors/portals, global `validcount`, static scratch arrays, named RNG | Current API serial; geometric subproblem potentially parallelizable against a snapshot with unchanged observation semantics | Unknown; snapshot, dispatch and revalidation can exceed query cost | High |
| Sector/environment/portal updates | Pending | Pending | Planes, touching actors, linked sectors, portal topology, interpolations and callbacks | Mutation remains serial; immutable geometry calculations require a separate bounded contract | Unknown; identify actual mover/portal stacks first | High |
| Resource acquisition, decode, object/level creation | Pending | Pending | Filesystem/archive readers, texture caches, `FTexture`, palette, class metadata, ImageArena, level/player state | Detached owned-byte decode/conversion potentially backgroundable; publication, actor creation and cache mutation retain owner | Unknown; only overlappable work reduces a critical-path stall | High |
| Existing detached texture conversion | Pending | Pending | Task-owned pixel snapshot/result; completion queue; owner-side metadata/upload | Background/asynchronous already implemented on Metal | Unknown; measure source snapshot, worker, queue delay and completion separately | Moderate within existing contract |
| Render-list/BSP preparation | Pending | Pending | Live actors/sectors, global visitation, portal recursion, borrowed draw objects, mapped backend buffers | Algorithm first; worker collection requires owned scene inputs, private scratch and detached outputs | Unknown; prior source audit does not establish current cost | High |
| Backend encode/submit, presentation and synchronization | Pending | Pending | Frame resources, encoder/command state, in-flight semaphore, recycle mutex, drawable lifetime | Existing backend ownership; any worker encoding needs explicit resource/command ownership contract | Unknown; blocked driver time is not simulation cost | High |
| Allocation/GC and pointer-heavy traversal | Pending | Pending | Global GC roots/state/counters, object lifetime, thinker and world lists | Algorithm/data-layout first; task-local storage only for detached data | Unknown; allocator samples alone do not prove cache misses | High for GC changes |

### Source evidence and observation boundaries

- **Loop and ticker:** `src/d_main.cpp`, `D_DoomLoop` and `D_Display`;
  `src/d_net.cpp`, `TryRunTics`; `src/g_game.cpp`, `G_Ticker`;
  `src/p_tick.cpp`, `P_Ticker`. `G_Ticker` dispatches loads/saves/map actions
  before ordinary ticking. `P_Ticker` walks all levels, clears actor
  interpolation, runs particles/player thinking, WorldTick callbacks,
  `Level->Tick()`, thinkers, specials, and level clocks. Per-frame and per-tic
  denominators must both be retained; catch-up can run multiple tics.
- **Thinkers:** `src/playsim/dthinker.cpp`, `RunThinkers`, `TickThinkers`,
  `ProfileThinkers`, `CallTick`. Lists run by stat number; fresh lists drain
  until empty. `NextToThink` is shared traversal state. PostBeginPlay precedes
  the timed `CallTick()` in the class profiler, so class totals do not capture
  all spawn/setup work. Dynamic-light recreation/effects follow ticking.
- **Scripts:** `src/common/scripting/vm/vmexec.cpp` declares
  `thread_local VMFrameStack GlobalVMStack`; that isolates a stack, not the
  world, native bindings, or GC. `vmframe.cpp` has shared VM counters and a
  mutating display/reset path. `src/playsim/p_acs.cpp` supplies ACS time and
  instruction-count reports. VM/ACS/collision costs can be nested inside
  thinker costs; adding inclusive totals would double-count.
- **Collision:** `src/playsim/p_map.cpp`, `P_CheckPosition`/`P_TryMove`.
  The former explicitly permits pickups and missile damage, changes blocker
  and temporary actor fields, and handles reentrant calls caused by pickups.
  `src/playsim/p_maputl.cpp`, `FBlockLinesIterator::Next`, writes shared
  linedef/polyobject visitation fields. These are dependencies even when the
  geometry itself is unchanged.
- **Sight:** `src/playsim/p_sight.cpp`, `P_CheckSight`/`SightCheck`.
  File-static `intercepts` and `portals` arrays are scratch; `validcount` and
  line/polyobject markers change. Visibility checks can consume
  `pr_checksight`/`pr_botchecksight`. Some early returns follow
  `SightCycles.Clock()` without reaching its matching `Unclock()`; treat
  `stat sight` as supporting evidence pending timer validation, not a sole
  authority. No timer repair is included in this audit.
- **World changes:** `src/playsim/mapthinkers/a_floor.cpp`,
  `a_ceiling.cpp`, `src/playsim/p_linkedsectors.cpp`, and
  `src/playsim/portal.cpp` are follow-up stack-attribution targets. Do not
  assume `P_UpdateSpecials` measures all world updates: its current body is
  principally the deathmatch time-limit check; movers also run as thinkers.
- **Loading:** `src/g_level.cpp`, `G_DoLoadLevel`; `src/p_setup.cpp`,
  `P_SetupLevel`/`PrecacheLevel`. Setup resets player references and translation
  state and invokes level loading before precache. Separate map construction,
  object/script initialization, source acquisition and backend upload.
- **Detached pixels:** `src/common/textures/texture.cpp`,
  `CreatePixelSnapshot`/`ProcessPixelSnapshot`/`ApplyPixelSnapshotResult`;
  `src/common/rendering/metal/textures/mt_textureloader.cpp` and
  `mt_texture.cpp`. Snapshot creation calls `Get8BitPixels` or
  `GetBgraBitmap` synchronously. Workers process owned values; results return
  via a mutex-protected queue for owner-side metadata application/upload.
  Cancellation removes pending hardware destinations; shutdown joins workers.
  Generic source objects/cache access are not thereby worker-safe.
- **Scene collection:** `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`,
  `CreateScene` maps vertex/light/bone buffers, traverses BSP, and runs render
  hacks using global visitation. `DrawScene` has static recursion/AO-portal
  counters and restores camera render flags around collection. Existing
  `ProcessAll` and `RenderAll` timers distinguish collection and draw-side
  CPU regions, not pure CPU simulation versus GPU execution. See the
  [render-view ownership audit](audit-render-view-snapshot-linux-2026-09-26.md).
- **Waits:** `src/common/rendering/metal/system/mt_renderdevice.cpp`,
  `BeginFrame`/`AcquireDrawable`/`Update`, includes in-flight semaphore waits,
  `mRecycleMutex`, completion processing, drawable acquisition, presentation,
  and the frame limiter. Preserve completion-handler ownership. Distinguish
  sleeping/blocked stacks from running stacks and GPU execution on the timeline.
- **Lifetime/RNG:** `src/common/objects/dobjgc.cpp` owns global root lists,
  GC state and allocation counters; CheckGC can occur within `TryRunTics` as
  well as display. `src/common/engine/m_random.cpp` owns named RNG lists and
  state serialization. Worker completion order must never reorder legacy RNG
  consumption, object publication/destruction, or callbacks.

## Existing instrumentation and its limits

| Instrument | What it establishes | Limitation |
|---|---|---|
| `vid_frametrace 5` | Update-to-Update wall intervals, p50/p95/p99/max and >100 ms count | Includes present/cap waits; startup, focus and gameplay must be separated |
| `vid_stalltrace 33` | Loop iterations above 33 ms, named inclusive phases | Loop boundary differs from Update boundary; fixed 16-bucket table; not an exclusive call tree |
| `mt_stalltrace 5` | Metal events above its threshold, including compilation/waits | Metal-specific; not a simulation profiler |
| `stat think` | Aggregate thinker time/count and action time | Excludes other ticker work; components overlap |
| `profilethinkers -t 20` | Top 20 classes by timed tick cost | Arms the next RunThinkers report, then resets; not a 20-second capture; profiling/sorting/printing perturbs timing |
| `stat VM`, `stat ACS`, `acsprofile` | VM timing/calls display, ACS timing, ACS instruction rankings | VM display shifts/resets counters; ACS instructions are not milliseconds; JIT/native coverage needs sampling verification |
| `stat gc`, `stat sight` | GC states/allocation trend, sight counters/timing | GC rate is not total allocator churn; sight timer caveat above |
| `stat rendertimes`, `stat gpu` | Collection/draw-side CPU regions; backend GPU timing where supported | Neither alone reports main-thread critical path or GPU occupancy |
| Sampling + thread states + I/O timeline | Native stacks, running versus blocked time, resource activity during the event | Short events may be undersampled; capture/export support must be verified locally |

Two audit findings prevent treating stalltrace output as a percentage table:
`playsim` contains `levelload`, which contains `precache`, but these scopes are
not all marked wrappers. The report sums non-wrapper buckets, so nested work
can be counted multiple times. Also excess distinct phase names are silently
dropped when the fixed table fills. Do not add fine-grained phase names or
derive exclusive percentages by summing this report. Use time-aligned sampled
stacks, or a separately reviewed bounded hierarchical instrument if needed.

## Measurement contract before implementation

Use one selected heavy-map gameplay route, one reproducible area-entry hitch,
and one level transition. Record save/start conditions and exact inputs, mod
load order and hashes, engine revision/binary, resolution, effects, precache,
caps/vsync, JIT configuration, focus, hardware/OS, cache policy and trace times.
A stock MAP06 scene may check capture machinery but cannot substitute for the
user's heavy-map/hitch workload. Do not assume a demo recording exists.

The requested three baselines should be GL and Metal with the same gameplay,
plus **CPU scope timings within that same sequence**, not an invented equivalent
headless run. Disabling rendering changes interpolation, visibility, resource
requests and pacing. A simulation-only harness would require its own input and
state-equivalence contract. Never infer CPU time by subtracting GPU duration
from wall time; their work can overlap.

Capture the normal capped/vsynced configuration first to reproduce symptoms.
Use a separate matched uncapped arm to expose throughput, without combining
the two distributions. Keep effects and observer settings explicit, with a
dedicated config per backend. Retain warmed and fresh-process first-visit
results separately. Fresh process is not proof of cold OS/archive caches;
do not delete the user's caches for an unspecified cold experiment.

Before looking at the workload, record these falsifiable predictions:

- Test the shared-engine hypothesis by asking whether the largest event has
  more than 50% of its **wall span** attributable to upstream non-backend work
  in both GL and Metal. A wait-heavy event or different stacks rejects that
  explanation; matching locations alone does not confirm it.
- Initially flag frames/loops above 33 ms and report >100 ms separately.
  If the ordinary heavy-map baseline exceeds 33 ms, retain that result and
  additionally flag events above twice its median. Report absolute milliseconds.
- Target at least three matched repetitions per backend and cache condition,
  with 60 seconds of active normal gameplay and explicit transition windows.
  Interleave backends and allow thermal settling. Report spread rather than
  claiming a precise improvement below it.
- Probe overhead in matched instrument-off/on runs; a >5% median or p99 change
  flags the instrumented distribution as contaminated, not engine baseline.
  Validate event detection using a separate labeled control with a known
  100 ms pause before trusting attribution. Do not inject the pause into
  baseline measurements or claim it validates GPU/I/O attribution.

Use optimized symbols and Time Profiler for first attribution. Acquire thread
states, GPU activity and filesystem events over the same hitch interval where
supported. If separate captures are necessary, retain their separate run IDs
and do not present them as one correlated event. The prior Intel-host handoff
records an `xctrace export` failure; a trace file alone is not an analyzed
profile. Validate readable stacks/export before a long capture. Do not use a
single-frame GPU capture to stand in for a gameplay hitch timeline.

Start with sampling and coarse existing observers. Run class/VM/ACS diagnostics
separately to avoid their UI/printing and timer overhead contaminating the
primary baseline. Allocation profiling is a separate observer arm; report
allocations/bytes per tic or frame and top allocation stacks. Pointer-heavy
source and allocator activity suggest locality candidates but do not establish
cache-miss cost without suitable hardware counters or a measured algorithm A/B.

## Ranking and decision gate

After capture, replace Pending with per-workload results and sort by measured
critical-path contribution, retaining normal gameplay and transition rankings
separately. Each row needs CPU ms/tic, CPU ms/frame, wall time, sampled running
CPU share (with denominator), inclusive/exclusive status, slow-event excess,
run IDs, repeat spread and confidence. Do not add nested subsystem percentages.
During a load with no rendered frames, report load wall/CPU milliseconds rather
than a fictitious frame percentage.

Bound expected payoff by removable measured serial time. Include snapshot/copy,
dispatch, join, invalidation, publication and memory costs. A job whose result
is immediately awaited may only move the work. A tick-start snapshot is not
equivalent to a query after an earlier actor's mutation: preserve the original
observation point or prove a narrower invariant. Ordered commit alone is not
that proof.

Choose one intervention only after the table identifies a substantial,
repeatable cost and its ownership contract. Verify with the same route and a
disabled-fix control, plus appropriate visual and simulation-state/RNG checks.
Existing network consistency checks are supporting evidence, not exhaustive
proof of arbitrary mod compatibility. No generic concurrent actor ticks,
off-thread GC, borrowed scene snapshots, or global loader changes are approved
by this source audit.

## Prior evidence retained, not promoted to new findings

- [Modernization roadmap](../engine-modernization.md), Track C, already requires
  sampling first and preservation of ordered legacy simulation.
- [Mac resource investigation](../handoff-macos-2026-09-23.md) found source PNG
  work and an idle-scene observer candidate; it explicitly did not capture the
  rare hitch. These are historical candidates, not this audit's ranking.
- [Linux transition measurements](../handoff-linux-2026-09-29.md) separate source
  and upload stages and show validation-layer contamination. Their hardware,
  backend, aggregate totals and workload cannot establish current Mac costs.
- `AGENTS.md` closes the older Ashes freeze report as of 2026-10-01 without
  isolating its fix. Do not treat its old drawable wait as the current cause.

Remaining input for runtime completion: chosen mod/map, local save or exact
repeatable route, transition trigger, and normal player settings. No threading
or engine behavior has changed.

The [replay fixture protocol](../performance-replay-fixtures.md) defines the
native-demo/video/save bundle and same-backend qualification gate using existing
facilities. It records save-start and initial-precache caveats found in source;
fixture creation and runtime repeatability remain pending.

## Verification of this audit increment

Runtime follow-up: the [first Blackwater doorway CPU capture](blackwater-doorway-profile-2026-11-09.md)
records one complete user-confirmed-hitch Metal run. It identifies preliminary
sound-decoding and material-observation candidates; matched baseline and exact
hitch attribution remain pending. It does not alter the source findings above.

`cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"` completed successfully
(macOS equivalent of the required processor-count build; `nproc` is unavailable).
CMake emitted dependency deprecation/policy warnings and the linker warned that
the installed libvpx targets macOS 12.0 while the deployment target is 10.15.
All four relative report links resolve, and whitespace checks pass. Changes are
documentation only; no visual regression, determinism test, or runtime
performance result is claimed by this build.
