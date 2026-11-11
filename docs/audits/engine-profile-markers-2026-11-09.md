# Native engine timeline markers — 2026-11-09

Purpose: align specific whole-frame stalls and tick invocations with sampled CPU
stacks and scheduler waits. This does not change game simulation or render
execution order. It follows the instrumentation audit's requirement to test the
platform timeline before adding a marker mechanism.

## Platform capability established first

A 3.270207-second `System Trace` recording with an added `Time Profiler`
instrument attached to the paused Blackwater process. Its exported tables contain
7,010 thread-state rows and 217 CPU sample rows, on one trace clock. This is a
capability check, not a gameplay benchmark. Native PointsOfInterest signpost
tables are available, but the existing engine supplies no loop/tick/Update
markers in them.

Artifacts are under `/private/tmp/gzdoom-instrumentation-20261109`, prefixed
`timeline-`. Export and symbolication need access to macOS services outside the
restricted tool sandbox. No claim is made that this paused sample establishes
the cause of an active-gameplay hitch.

## Minimal marker contract

- `vid_profilemarks` is false by default and non-archived. On macOS it enables
  native signposts in subsystem `org.gzdoom.engine`, category `PointsOfInterest`,
  for any renderer. Other platforms compile no native event backend in this
  increment. Disabled paths do not read clocks, initialize logging, increment
  profiler counters, allocate trace storage or format output.
- All marker calls are main-thread-only. Native signposts supply their own
  clock and OS-managed buffering: no additional engine ring or handwritten
  clock-offset conversion is introduced. Payloads contain numeric values and
  fixed event names, never actor/texture pointers or dynamic resource strings.
- `EngineLoop` is an interval around the try-body of one D_DoomLoop iteration.
  It includes coarse timing/reporting work there, but excludes its catch-body
  recovery. RAII closes it on normal exit and stack unwinding. There is one
  active loop at this call site; no nested-loop API usage is supported.
- `EngineTick` is an interval around one G_Ticker invocation. Its ordinal is
  distinct from `gametic`, which is recorded as context. A finished interval
  means the invocation ended, not proof that a tick committed successfully.
  `TryRunTics` waiting, networking/prediction and later consistency processing
  remain outside this interval.
- `EngineUpdate` is a point at the end of DFrameBuffer::Update, next to the
  existing interval tracer. Consecutive points define Update-to-Update wall
  gaps; they are not GPU-submit or display-present events. Wipes/startup can
  generate multiple Updates per outer loop or Updates outside a loop.
- Loop, tick and Update ordinals are separate process-local uint64 counters.
  They advance on eligible emission attempts while markers are enabled; OS
  collection can still drop records. They do not reset on a level load or CVAR
  toggle. A zero parent-loop
  value means no instrumented loop is active. Do not match ordinals across
  processes or compare their values to game tics as if they were one domain.
- Update payloads retain `gametic`, game state, menu state, pause and the native
  app-active query. These support identifying menu/focus/load contamination;
  they do not automatically qualify a manual route or classify every lifecycle
  transition. The native focus query is observer work while enabled.
- Scope destruction attempts to emit its matching end even if the CVAR changed
  during the interval. Capture boundaries, OS event loss or collector changes
  can still produce incomplete intervals. Retain unmatched-event diagnostics
  and inspect recording issues; missing markers must not become invented time.

This is a platform-native implementation of the bounded-event requirement,
using OS-managed event buffers rather than per-frame engine text logging.
It does not repair the separate stalltrace bucket hierarchy/overflow issue.
On/off observer overhead must be qualified before these marks become part of
the performance baseline; they remain opt-in.

## Functional qualification protocol

Prediction before looking: an off run emits zero EngineLoop/EngineTick/
EngineUpdate records while the same collector can see an independent control
signpost. An on run emits all three; loop/tick begin/end pairs match, monotonic
ordinals do not collapse domains, and Updates can be located on the same trace
clock as CPU samples and thread states.

A temporary, labeled 100 ms sleep inside an in-level deferred command supplies
the positive control. Its own native interval and a steady-clock measurement
should agree within 1 ms, and its duration should be 100 +/- 10 ms on this host.
The surrounding loop and Update gap should include the delay, without assigning
it to actor execution. The probe is not part of gameplay or a shipping command.
Remove it and rebuild after measurement. Controls qualify marker mechanics,
not representative gameplay performance or arbitrary-mod determinism.

## Functional qualification results

Implemented and live-qualified on Intel macOS, with the temporary delay/wait
commands removed before the final build. Controls used stock DOOM II MAP06,
1280x720, no monsters, `cl_capfps 1`, `screenblocks 12`, isolated config/saves,
and an automatic frame-120 screenshot. These are functional controls, not the
Blackwater workload or an uncapped performance A/B.

| Control | EngineLoop records | EngineTick records | EngineUpdate records | Independent delay records |
|---|---:|---:|---:|---:|
| Intel Metal, marks off | 0 | 0 | 0 | 2 |
| Intel Metal, marks on | 242 (121 pairs) | 254 (127 pairs) | 124 | 2 |
| OpenGL, marks on with off/on toggle | 232 (116 pairs) | 238 (119 pairs) | 119 | 2 |

All three exports have no unmatched/duplicate boundaries or unknown event
types. The GL control toggled marks off after frame 95 and on after frame 100;
interval closure remained balanced and loop/tick/Update ordinals remained
monotonic. Its ~168 ms gap across disabled collection is deliberately **not**
an actual frame-time finding.

Native delay durations were 101.205880 ms (off), 100.117085 ms (Metal on) and
101.019888 ms (GL toggled). Independent steady-clock durations were 101.209600,
100.121948 and 101.024227 ms. Each native interval differs by less than 0.005 ms
from its independent duration, passing the stated 1 ms agreement and 100 +/-
10 ms duration predictions. This establishes duration agreement for the test,
not arbitrary cross-clock absolute timestamp conversion.

The Metal-on delay lies inside one EngineLoop (128.357273 ms) and one observed
Update gap (114.994190 ms), with **zero overlapping EngineTick intervals**.
Clipping thread-state rows from the same recording to the delay shows:
100.030811 ms Blocked, 0.073808 ms Running and 0.012466 ms Runnable. This is
positive evidence that the instrument can distinguish a synthetic main-thread
wait from actor execution. It does not attribute the Blackwater doorway hitch.

The two Metal screenshots decode to identical pixels. Frame-graph self-tests
passed in each control. Ten offline analyzer tests pass, including reference
resolution, interval pairing, incomplete boundaries and Update-gap arithmetic.
The required final build and whitespace checks pass. The installed libvpx
deployment-target warning and existing launcher overload warnings were observed;
no shader or gameplay optimization is claimed.

One attempted Instruments `--launch` control was rejected: Launch Services
substituted `/Applications/gzdoom.app` (UUID
`061D61B1-6139-39C2-ADF9-603A029C1766`) for the requested project executable
(probe-build UUID `4EB2E12F-5910-3B08-9C8A-DABF23AFF1DB`). The successful runs
launch the executable directly and attach its PID, using a temporary pre-loop
barrier released by `--notify-tracing-started`. The barrier/delay were diagnostic
test code only, not shipping commands. Reject traces from the wrong binary even
when their launch arguments look correct.

Artifacts: `native-off`, `native-on`, and `native-gl-toggle` directories under
`/private/tmp/gzdoom-instrumentation-20261109`, including metadata/UUID, logs,
traces, signpost summaries and screenshots. The removed probe and harness are
retained there. `tools/profiling/summarize_signposts.py` is the reusable offline
analyzer; it exports numeric context, matches interval IDs and reports incomplete
data. It never labels missing/disabled marker spans as measured GPU-present time.

Remaining: actual-route qualification, repeated on/off overhead measurements,
matched GL/Metal results and GPU/driver-wait attribution. A capped functional
control cannot establish negligible observer overhead. Linux/Windows compile
guards have not been runtime-tested on this Mac, and no native sink for them is
implemented in this increment. The marker CVAR remains default-off.
