# Resource-read lookup qualification — 2026-11-10

The post-audio Blackwater profile attributes 627 ms of 7,290 ms main-thread
sample weight (8.6%) to `FrameGraph::ObserveResourceRead`, nested within the
1,368 ms material-observation path. These are inclusive CPU sample shares,
not frame-time savings. The earlier material cache removed repeated dependency
searches but retained temporary string creation on every resource-read lookup.

## Scope and ownership

Use the existing `TMap` with case-sensitive content hashing/comparison over
`const char *` lookup keys. On a miss, copy the resource name into a graph-owned
`TArray<FString>`; stored pointers reference those immutable character buffers,
which survive relocation of the array. Clear the map before clearing its owned
names at frame reset; declaration order also destroys the map first. Lookup
arguments are borrowed only for the duration of the call. No caller pointers
are retained. Each read still advances the observation sequence, including
repeated bindings and reads outside graph passes. Alias handling, upload
retirement, and material-cache scope are unchanged.

This adds one owning-string array entry per distinct resource read in a frame.
It does not change simulation, VM/GC ownership, backend execution, or threading.
The separate `mSceneMaterialReads` cache retains its current FString lookup.

## Measurement contract

Prediction stated before running: 256,000 warmed lookup hits create zero
FString allocations in the candidate versus 256,000 in the allocation control,
with at least 20% lower isolated lookup CPU time. Eight measurements per process
alternate in ABBA order, four per mode, against 256 resource names. Names and
map entries are initialized before timing. These synthetic CPU measurements do
not predict a corresponding FPS or frame-time percentage.

The temporary allocation control restores construction/destruction of a
FString lookup key on the same new map. It isolates the removed allocation;
it is not an exact benchmark of the old FString-keyed map layout. A temporary
thread-local counter in `FStringData::Alloc` counts actual string allocation
calls. GC counters would miss these malloc-backed allocations. Both probes
and the control switch must be removed before the final build.

The retained graph self-test covers caller-buffer mutation, growth of owned
storage, case-sensitive identity, empty/null input and existing repeated-read,
late-upload, reset, alias, camera/canvas and attachment dependency controls.
For live parity, pause the copied Blackwater MAP09 save and compare complete
graph dumps and decoded pixels before/after switching the allocation control
within one process. Repeat under Intel Metal and OpenGL.

Artifacts are retained under
`/private/tmp/gzdoom-resource-lookup-20261110`.

## Isolated and live results

| Process | Allocation-control median | Candidate median | Reduction |
|---|---:|---:|---:|
| Intel Metal | 37.411 ms | 12.220 ms | 67.3% |
| Intel OpenGL | 41.123 ms | 12.563 ms | 69.4% |

All eight control measurements allocate exactly 256,000 FStrings; all eight
candidate measurements allocate zero. The corrected Metal ranges are
36.768–38.903 ms versus 11.341–12.300 ms; GL ranges are 39.091–41.883 ms versus
11.398–14.028 ms. The effect exceeds the within-mode spread and the 20%
prediction. An earlier pair of processes reproduced the allocation/time result
and live parity, but their new case-sensitivity test had an incorrect premise:
`ValidateUploads` checks declared ordering unconditionally, even before any read.
That test was corrected to inspect `Dump`'s read-after-upload field directly;
no upload validation policy changed. Initial artifacts remain in `initial-*`.

Both corrected processes pass the graph self-test. Within each process,
1024×768 decoded images are identical, and complete graph dumps match exactly:
386,257 characters on Metal and 10,434 on GL. These are within-backend
allocation-control comparisons, not a claim of cross-backend pixel identity.
`qualification-summary.json` retains all measurements. Probe source and binary
are archived alongside the pre-change source and executable. Temporary counter,
benchmark command and lookup switch are removed from the final source.

## Final build and fresh CPU capture

The final build passes with probes removed. The 13 profiling-tool tests and
whitespace validation pass. MAP06 baseline and SSAO Metal/GL comparisons both
report OK, and all four final engine logs report graph self-test PASS. The
first unattended matrix warmup stopped advancing after map load with background
activity disabled in its isolated config; those test processes were terminated
and the temporary launcher was pinned to the same explicit background settings
used by Blackwater. The rerun completed normally. This changes no engine code.
Linux/Vulkan and Apple Silicon runtime coverage are not available here.

A fresh Time Profiler + Points of Interest recording completed at its 30-second
time limit. Select 0.554–15.554 s after the marker-confirmed unpause: 525
continuous Update events, gametic delta 524, GS_LEVEL, no menu/pause, all focus
markers inactive, with explicit background activity enabled. Main-thread sample
weight is 6,139 ms. Largest Update gap is 41.15 ms; no gap exceeds 100 ms.
Largest complete tick interval is 18.92 ms. RunIssues contains the previously
observed `Data stream: Time Mapping` entry, with no separate explicit loss
entry. This recording does not provide scheduler blocked-time or GPU occupancy.

| Remaining path | Inclusive main-thread sample weight | Share |
|---|---:|---:|
| Scene-material observation, including resource reads | 730 ms | 11.9% |
| Resource-read observation, nested above | 136 ms | 2.2% |
| Thinker iteration including invoked work | 416 ms | 6.8% |
| P_TryMove, overlapping thinker/native work | 395 ms | 6.4% |
| P_CheckPosition, nested movement work | 182 ms | 3.0% |
| DoomLevelAABBTree::Update | 153 ms | 2.5% |
| Scene creation | 143 ms | 2.3% |
| FrameGraph reset | 109 ms | 1.8% |
| Generic synchronous sound-loading ancestry | 40 ms | 0.7% |

Rows overlap and must not be added. Resource-read observation is lower than the
previous 627 ms / 8.6%; its enclosing material observer is lower than the
previous 1,368 ms / 18.8%. This is supporting caller evidence, not a paired
whole-engine speedup measurement: there is one fresh capture, focus differs
from the earlier partly active capture, execution is capped at 35 tics, and
other CPU/scheduling/clock effects are not isolated. The isolated allocation
control above is the acceptance evidence for the bounded change.

The material-observer ancestry still includes allocation/free leaves alongside
strcmp and hashing, consistent with the remaining FString material-cache
lookup. Resource-read ancestry now primarily contains strcmp, hashing and
strlen; occasional misses still allocate owned keys. The next bounded candidate
is the separate material-cache lookup, with its own scope/alias/reset contract;
movement and AABB updates warrant caller-level investigation afterward. This
capture does not justify concurrent actor ticks or GC/VM changes. No new
optimization beyond resource-read lookup is implemented here.

Audio preparation reports three ready jobs at release, three ready hits at the
end, and no failures/not-ready fallbacks. LightningFlash has zero sampled
OpenALSoundRenderer::LoadSound or decoder ancestry; generic SoundEngine lookup
can still appear under lightning and is not itself evidence of synchronous
decoding. The stationary weather workload is not the manual doorway route.

CPU artifacts: `/private/tmp/gzdoom-resource-cpu-20261110-134533`, including
archived final executable, hashes, arguments, image mapping, raw trace, exports,
symbolization and bounded sample JSON. Final matrix logs/images are copied into
the qualification artifact directory. Doorway-route, uncapped throughput and
long-session acceptance remain pending.
