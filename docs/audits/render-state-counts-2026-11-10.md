# Metal render-state counts and depth-clip cache — 2026-11-10

Scope: count attempted versus changed render state in the measured Metal CPU
path, then qualify one bounded change. No hwrenderer threading, scene sorting,
world snapshot, simulation, VM/GC, shader or render-graph changes. The user's
performance priority is smooth gameplay: repeatable long-frame frequency and
severity, especially doorway/area-entry and level transitions. Average FPS is
secondary. The isolated optimization below does not establish a hitch reduction.

## Prediction and instrumentation

Before capture: more than 90% of depth-clip commands repeat a value already
applied to the current encoder. Other state ratios are exploratory.

Temporary default-off counters in `MtRenderState` observe Apply, render-pass,
pipeline key changes/binds, material dirty flags and four-field material keys,
stream/matrix writer advancement, push-constant changes and depth-clip attempts.
The four-field material key includes material pointer, translation, clamp mode
and shader override. Encoder creation invalidates the measurement's prior key.
It does not include mutable layer contents, canvas updates, sampler/filter
settings or resource readiness, so an equal key is not sufficient to skip
material preparation. Source `FRenderState::SetMaterial` marks dirty on each
call; dirty count alone is not the count of changed binding inputs.

Writer advancement is observed by index/offset changes around Write. It is a
proxy for advancing records, not a byte-count measurement or a proof of all
buffer writes. These are diagnostic counts with instrumentation active, not
uninstrumented frame-time measurements. Instrumentation can perturb timer-based
stream values. Temporary code and commands are removed before final build.

Three fresh-process paused Blackwater MAP09 repeats count 100 frames after
warmup; a separate unpaused-launch diagnostic counts 350. Explicit background
activity and backend selection are pinned in copied INIs. Same save and mod;
inputs are not demo-controlled. The unpaused diagnostic has no marker-qualified
CPU window or matched route guarantee. The paused counts are repeatability
controls, not gameplay pacing acceptance.

## Counts before optimization

All three paused repetitions have these exact counts except the indicated ranges:

| State | Attempts | Changes/requirements | Fraction |
|---|---:|---:|---:|
| Pipeline key | 50,700 | 2,100 changes and binds | 4.14% |
| Depth clip | 50,700 emitted | 300 encoder/value requirements | 0.59% |
| Material dirty flag | 50,700 | 50,000 dirty | 98.62% |
| Four-field material key | 50,700 | 41,500 changed/encoder invalidated | 81.85% |
| Stream writer | 53,300 | 37,621–37,628 advances | 70.58–70.60% |
| Matrix writer | 53,300 | 8,700 advances | 16.32% |
| Push constants | 50,700 | 39,383–39,393 changed | 77.68–77.70% |

There are 600 successful encoder creations; only encoders that reach depth-clip
application need the tracked command. Stream/matrix attempts exceed Apply calls
because indexed batching can prepare per-draw data outside the full Apply path.
Denominators differ across rows; do not add percentages.

The key-count diagnostic's unpaused launch has 202,413 Apply calls, 1,348
depth-clip requirements, 12,057 pipeline changes, 152,365 material key changes,
217,105 stream/matrix attempts, 135,105 stream advances, 33,099 matrix advances
and 145,910 push changes. The earlier simpler unpaused count had different draw
counts; these are not matched runtime speedup comparisons. They show a similar
pattern: pipeline/matrix paths already filter many repeats, material keys often
change, and depth-clip emission is overwhelmingly repeated.

## Bounded implementation

Metal only. Cache the depth-clamp value last sent to the current encoder, with
an explicit valid bit. Apply on first use or value change. Invalidate after
retiring a render encoder in EndRenderPass; BeginRenderPass always calls that
retirement path before creating a new encoder. This handles encoder-address
reuse. All `mEncoder` assignments and `setDepthClipMode` call sites were checked:
creation is in BeginRenderPass, retirement is in EndRenderPass, and this state
path owns the depth-clip setter. Flush pending geometry before retirement as
before; invalidation follows the flush/end operation. Pipeline selection and
batch ordering stay unchanged. The setter still receives both requested modes.

OpenGL and Vulkan are unchanged. GL currently emits glEnable/glDisable in its
SetDepthClamp method; Vulkan includes DepthClamp in the pipeline key. They need
their own measured controls before any analogous optimization. This change does
not move PCM, rendering or simulation work between threads.

## Qualification

Before running: at least 99% fewer commands in the paused fixture, identical
pixels/graph reports and at least 50% shorter isolated repeated-command loops.

A temporary control forces the original unconditional Metal command through the
same helper. Three within-process Blackwater comparisons each count 50 frames
per mode. Control emits 25,350 commands; candidate emits 150, exactly matching
the independent encoder/value tracker. **99.41% fewer API commands**, in all
three repetitions. Counts of Apply calls, pipeline binds and material keys stay
the same. Stream/push counters vary slightly with their timer data.

All three 1024×768 decoded control/candidate image pairs match exactly, as do
complete 386,257-character graph reports. Each process passes the graph self-test.
A transition control requests a value twice, toggles it, repeats, restores it,
and forces invalidation: exactly four commands emit as predicted. This exercises
both clamp/clip values and repeated/invalidated states. The independent live
tracker also checks emission across real encoder boundaries.

The initial 20,000-attempt loops show the expected result but take only fractions
of a millisecond. Increase to 200,000 attempts to improve timing separation.
Eight ABBA loops per process run on a real encoder without intervening draws:

| Process | Unconditional median | Cached median | Reduction |
|---|---:|---:|---:|
| 1 | 2.664 ms | 0.696 ms | 73.9% |
| 2 | 2.984 ms | 0.763 ms | 74.4% |
| 3 | 2.998 ms | 0.725 ms | 75.8% |

Every unconditional loop emits 200,000 commands; every cached loop emits one.
The fastest control is 2.535 ms, slower than the slowest candidate, 0.840 ms.
The timing effect exceeds the repeated-arm spread and the prediction. The
helper is temporarily noinline to prevent repeated no-op loops disappearing;
that annotation is removed from final code. Temporary emission counting and
control checks are included in this diagnostic loop, so these values are not
production per-frame costs, GPU timing, or evidence of a gameplay hitch fix.
The absolute saving for normal frame counts is small.

A further cached unpaused-launch diagnostic counts 177,100 applications over
350 frames and emits 1,050 commands, matching 1,050 tracked requirements. It
checks lifetime/value handling; draw counts differ from earlier launches and
must not be used to calculate a runtime speedup. No lost sound or gameplay
ordering claim is inferred from this rendering diagnostic.

Artifacts: `/private/tmp/gzdoom-state-work-20261110`, including original and
instrumented source/executables, all copied INIs/arguments, raw count logs/JSON,
initial short-loop controls, `long-loop/depthclip-summary.json`, images, graph
reports and the cached diagnostic. Temporary scripts and probes are not added
to the repository or build tree.

## Consequences and next work

This is a small redundant-command fix. It does not explain or resolve the
remaining long-frame tails. Pipeline binds already avoid most repeated keys;
most observed material applications have different keys. Broadly skipping
material work or dispatching Apply on workers is not justified by these counts.

Prioritize representative doorway/transition frame pacing, with absolute long
frames, tail percentiles and counts above fixed thresholds, cold/warm visits
separated. Attribute individual stalls before another optimization campaign.
Within rendering, inspect existing opaque draw ordering and material preparation
for state churn, preserving portal/translucent order, dynamic textures and
resource dependencies. Only extract worker jobs after immutable inputs/private
outputs and ordered merge points are defined. The amount of safely movable
hwrenderer work remains unqualified.

## Final verification

The final probe-free build passes, with the temporary noinline annotation,
counters, commands and control CVARs removed. The 13 profiling-tool tests and
whitespace validation pass. Final MAP06 baseline and SSAO Metal/OpenGL reference
comparisons both report OK; all four engine logs report graph self-test PASS.
Final logs/images are retained under `final-matrix` in the artifact directory.
Runtime qualification is Intel macOS only; Apple Silicon is not tested. No
unprofiled frame-tail improvement or matched doorway result is claimed.
