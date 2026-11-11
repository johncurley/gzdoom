# Material-cache lookup qualification — 2026-11-10

The post-resource-lookup Blackwater sample still attributes 730 ms / 11.9% of
main-thread sample weight to `ObserveSceneMaterialRead`, including 136 ms / 2.2%
inside resource-read tracking. Material-observer ancestry still includes
allocation/free, strcmp and hashing. These inclusive sample shares overlap and
are not whole-frame savings. The next bounded candidate is the remaining
`mSceneMaterialReads.CheckKey(FString(name))` temporary allocation on each bind.

## Scope and lifetime contract

Use the existing case-sensitive content hash/comparison with borrowed lookup
arguments and copied, owned keys in a pass-scoped `TArray<FString>`. Insert the
original resource name, preserving the existing cache identity even when an
alias has a different canonical name. FString character storage survives array
relocation. Clear the map before its owning names at frame reset, backend-pass
entry, and newly declared aliases. Field order also destroys the map first.
Pass reads and observed uses retain their separate graph-owned strings, which
survive clearing this cache. Cache keys never reference caller buffers or pass
record storage. Every binding still records its upload-read sequence before
cache lookup, including cache hits and bindings outside tracked pass types.

No render scheduling, pass declarations, alias semantics, simulation, threading,
VM or GC policy changes. The additional owning-array storage is bounded by the
largest material set observed within one cache scope. The retained repeated-read
self-test now mutates the first caller buffer before repeating equal-content
reads and already covers late upload, scope transition/reentry and frame reset.

## Measurement contract

Prediction stated before measurement: at least 20% lower isolated material
observer CPU time, zero temporary FString allocations on warmed lookup hits,
and identical graph reports and decoded pixels.

A temporary thread-local counter in `FStringData::Alloc` counts actual string
allocations. A temporary control restores FString key construction/destruction
on lookup, on the same new map; it isolates the removed allocation, rather than
reproducing the old map's physical layout. Eight ABBA measurements per process
exercise 256,000 repeated bindings each, with 256 names and a live scene pass:
128 names have earlier producers and 128 are imported inputs. Initial insertion
and graph build/dump are outside timing. Every graph must build and match the
first control's full dump. This is a synthetic CPU measurement, not an FPS
prediction. Remove probes and switch before the final build.

For live parity, use the paused Blackwater MAP09 save, compare within-process
allocation-control/candidate graph reports and 1024×768 decoded screenshots,
and repeat on Intel Metal and OpenGL. Retain raw source, executable, logs,
arguments and analysis under `/private/tmp/gzdoom-material-lookup-20261110`.

## Render-graph direction

Retain the graph: `PPRenderState::ExecutePostprocessGraph` uses its validated
order to replay recorded postprocess draws and falls back to declaration order
if validation fails. The broader frame graph also exposes hazards, imports,
uploads, required outputs and lifetimes. The current measured target is repeated
CPU observation while binding scene materials. Removing the graph would discard
execution/validation behavior instead of isolating that cost.

Keep first-observation dependency work, make repeated bindings inexpensive, then
measure graph build/reset separately if they become dominant. A graph of render
resources is not an immutable simulation snapshot, so it does not by itself
make live scene traversal safe for worker threads. No speculative graph rewrite,
pass culling, memory aliasing or new rendering concurrency is part of this change.

## Qualification results

| Backend | Allocation-control median | Candidate median | Reduction |
|---|---:|---:|---:|
| Intel Metal | 47.840 ms | 21.521 ms | 55.0% |
| Intel OpenGL | 46.700 ms | 21.682 ms | 53.6% |

Each control measurement allocates exactly 256,000 FStrings and every candidate
measurement allocates zero. Metal control range is 47.554–49.129 ms versus
21.178–23.148 ms; GL is 46.370–47.370 ms versus 21.384–22.032 ms. The improvement
exceeds both within-mode spread and the 20% prediction. All 16 synthetic graphs
build and match their process's control dump, and both graph self-tests pass.

Within-process paused Blackwater decoded pixels match exactly at 1024×768 on
each backend. Complete graph dumps match too: 386,257 characters on Metal and
10,434 on OpenGL. This is control/candidate parity within each backend; it is
not a claim that Metal and GL pixels are identical to one another.

Raw measurements are in `qualification-summary.json`; control and probe source
and executables are archived with the run arguments, logs and screenshots.
Temporary allocation counter, benchmark command and control CVAR are removed
before the final build. These results qualify isolated repeated-binding cost,
not a whole-frame percentage or a doorway hitch fix.

## Final verification and CPU follow-up

The final build succeeds after removing all temporary probes. The 13 profiling
tool tests and whitespace check pass. MAP06 baseline and SSAO cross-backend
comparisons both report OK; all four final Metal/GL logs report graph self-test
PASS. Final logs and images are copied into the qualification artifact directory.
Linux/Vulkan and Apple Silicon runtime coverage are unavailable here.

A fresh Time Profiler + Points of Interest recording completed normally at
30.307 s. Select 0.494–15.494 s after the marker-confirmed unpause: 525
continuous Update events, gametic delta 524, GS_LEVEL, no menu/pause, all focus
markers active. Main-thread sample weight is 6,318 ms. The largest Update gap is
47.58 ms, none exceed 100 ms, and the largest complete tick interval is 14.73 ms.
RunIssues contains the previously observed `Data stream: Time Mapping` entry,
with no separate explicit loss entry. Scheduler blocked duration and GPU
occupancy were not captured.

| Path | Inclusive main-thread sample weight | Share |
|---|---:|---:|
| Material observation including resource-read tracking | 448 ms | 7.1% |
| Resource-read tracking, nested above | 152 ms | 2.4% |
| Thinker iteration including invoked work | 544 ms | 8.6% |
| P_TryMove, overlapping thinker/native work | 446 ms | 7.1% |
| P_CheckPosition, nested movement work | 239 ms | 3.8% |
| Scene creation | 231 ms | 3.7% |
| FrameGraph reset | 124 ms | 2.0% |
| DoomLevelAABBTree::Update | 126 ms | 2.0% |
| Generic synchronous OpenAL loading | 112 ms | 1.8% |

Do not add these overlapping rows. Material observation was 730 ms / 11.9% in
the previous capture. Its current leaves include strcmp 184 ms, hashing 76 ms,
and strlen 33 ms. Allocation still occurs on misses and first observations;
zero allocation applies to the warmed repeat loop, not the entire graph frame.
This supports the bounded allocation result, but focus differs from the earlier
all-inactive window, inputs are not demo-controlled, and there is only one fresh
capture. Main-thread sample totals are not a measure of throughput. No overall
CPU/FPS improvement or doorway-hitch elimination is claimed from this pair.

LightningFlash has zero sampled OpenAL load ancestry. Initial audio stats show
three ready jobs; final stats show three ready hits, no failures/not-ready
fallbacks, six jobs queued in total, one cancellation, and two ready jobs still
queued. These counters are endpoint snapshots around the full run, not events
restricted to the selected 15-second CPU window.

CPU artifacts: `/private/tmp/gzdoom-material-cpu-20261110-142211`, with the
archived final executable, hashes, arguments, raw trace, exports, image mapping,
symbolization, bounded sample JSON and `targeted-ranking.json`.

The next investigation can focus on movement/spatial-query callers alongside
the remaining string-heavy graph observation. A registered resource identity
could eventually avoid repeated string hashing/comparison, but requires its
own lifetime, alias and generation contract. No identity migration or further
optimization is implemented here. The present evidence supports retaining the
graph and lowering its bookkeeping cost; it does not establish a need for
concurrent simulation. Manual doorway, uncapped and long-session acceptance
remain open.
