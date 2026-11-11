# Render-graph caller lookup qualification — 2026-11-10

## Caller evidence and rendering scope

The previous active Blackwater window has 6,318 ms main-thread sample weight;
material observation accounts for 448 ms / 7.1%. Re-reading the archived
symbolization with source locations attributes 445 of those sampled ms to
`MtResourceBindingManager::ApplyBindings`. Within material-observer ancestry,
156 ms map to the resource-read call and 116 ms to the separate material-cache
lookup. Of 184 ms of strcmp leaves, 51 ms map to the resource-read call, 47 ms
to the second cache lookup, and the remaining sites include first-observation
read/use/external/observation scans. Source attribution is compiler debug-line
information, not exact instruction-level execution timing; nested rows overlap.
Raw reanalysis is retained in `caller-attribution.json` under the artifact root.

The other source callers are GL hardware-texture binding, Vulkan material layer
observation and Metal render-texture material observation. These callers must
retain upload-read observations even on repeated bindings or when backend
binding state is reused. Removing observation at the caller merely because a
texture is already bound would not preserve the upload/pass contract.

The graph already drives bounded postprocess replay via
`PPRenderState::ExecutePostprocessGraph`, with declaration-order fallback when
validation fails. The full frame graph records broader resource dependencies.
It does not currently allocate/alias resources or automatically cull passes.
Keep that boundary while fixing measured CPU costs. A render-resource graph
also does not freeze live world state for concurrent scene traversal.

## Bounded change and invariants

Store both the most recent upload-read sequence and the completed material
scope in the existing graph-owned resource-read entry. A binding performs one
content lookup, updates sequence unconditionally, then returns on a matching
completed scope. First observations retain eligibility checking, canonical-name
resolution and all writer/read/use/external/backend records. Cache identity
remains the original case-sensitive resource name. This removes the second
per-pass map and its separately copied name array.

Advance a nonzero scope generation on every backend-pass entry and newly
declared alias. On generation wrap, zero all completion markers and restart at
one; sequence values and upload records remain untouched. Frame reset clears
entries before owned keys and resets the scope generation. Resource names stay
owned until frame reset. The resource entry pointer is used only within the
observation; its slow path does not insert into the resource map or invoke other
resource observations, so map growth cannot invalidate it before completion.
No pointer or scope token is exposed to callers.

No simulation, VM/GC, backend binding behavior, pass scheduling, barrier or
renderer-threading changes. The existing repeated-material test covers borrowed
caller-buffer mutation, equal-content keys, an upload between reads, produced
and imported materials, pass changes/reentry and frame reset. Existing alias,
camera/canvas, upload, attachment and graph hazard controls still apply.

## Measurement contract

Prediction before running: at least 20% less isolated repeated-binding CPU time,
with one lookup per warmed binding instead of two, and identical complete graph
reports and decoded pixels.

The temporary control restores per-binding pass-prefix checking and a second
lookup of the same resource entry before checking completion. It isolates the
removed lookup/eligibility work on the same new map; it does not reproduce the
old two-map layout or its first-observation allocations. A temporary call-site
counter verifies the number of explicit lookup calls. Eight ABBA measurements
per process exercise 256,000 repeated bindings over 256 names (128 produced,
128 imported). First insertion and graph build/dump occur outside timing. Every
build must succeed and match its control's report. This synthetic CPU test is
not a whole-frame or FPS prediction. Remove the probe and switch before final
build and CPU profiling.

Within each backend, pause the same Blackwater MAP09 save and compare graph
dumps and decoded 1024×768 screenshots before/after switching the control in one
process. Repeat on Intel Metal and OpenGL. Artifact root:
`/private/tmp/gzdoom-graph-callers-20261110`.

## Isolated and live results

| Backend | Duplicate-lookup control median | One-lookup median | Reduction |
|---|---:|---:|---:|
| Intel Metal | 26.975 ms | 14.295 ms | 47.0% |
| Intel OpenGL | 19.825 ms | 11.203 ms | 43.5% |

All eight control measurements report exactly 512,000 lookup calls and all eight
candidate measurements report 256,000. All 16 graph builds succeed and match
their process's first control dump. Metal control range is 25.670–27.857 ms
versus 13.948–14.681 ms; GL is 19.529–20.740 ms versus 10.545–12.487 ms. Each
backend exceeds the 20% prediction by more than its within-mode spread.
Backend-to-backend CPU differences are not interpreted as GPU effects.

Both graph self-tests pass. Within each paused Blackwater process, decoded
1024×768 pixels are identical and complete graph reports match: 386,257
characters on Metal and 10,434 on GL. This is within-backend control/candidate
parity. Probe and pre-change source/executables are retained with arguments,
logs, images and `qualification-summary.json`. Temporary control, call-site
counter and benchmark command are removed before the final build and capture.

The control isolates an extra lookup and eligibility check on the new unified
map. Its percentage is not an exact comparison against the previous physical
two-map implementation or a whole-frame saving. The final CPU capture provides
separate caller evidence, with the usual sampling/workload limitations.

## Final verification and remaining hotspots

The final build succeeds with probes removed. The 13 profiling-tool tests and
whitespace check pass. MAP06 baseline and SSAO Metal/GL comparisons report OK,
and all four final engine logs report graph self-test PASS. Logs and images are
copied into the qualification artifact directory. Linux/Vulkan and Apple Silicon
runtime validation are unavailable here.

A fresh Time Profiler + Points of Interest capture completed at its 30.329 s
time limit. Select 0.498–15.498 s after marker-confirmed unpause: 525 continuous
Updates, gametic delta 524, GS_LEVEL, no menu/pause. Focus is inactive for 216
Updates and active for 309, with explicit background activity enabled. Main
sample weight is 6,319 ms. Maximum Update gap is 48.41 ms, none exceed 100 ms;
largest complete tick interval is 21.05 ms. RunIssues contains the usual
`Data stream: Time Mapping` entry and no separate explicit loss entry. This
capture does not measure scheduler blocked duration or GPU occupancy.

| Path | Inclusive main-thread sample weight | Share |
|---|---:|---:|
| Metal state application, selecting samples without binding-manager ancestry | 823 ms | 13.0% |
| Thinker iteration, including invoked work | 602 ms | 9.5% |
| P_TryMove, overlapping thinker/native work | 467 ms | 7.4% |
| Material observation, including resource-read lookup | 341 ms | 5.4% |
| Shared hwrenderer scene creation | 262 ms | 4.1% |
| Metal binding manager, selecting samples without material-observer ancestry | 238 ms | 3.8% |
| P_CheckPosition, nested movement work | 226 ms | 3.6% |
| Resource-read entry observation, including other resource reads | 203 ms | 3.2% |
| FrameGraph reset | 133 ms | 2.1% |
| Generic synchronous OpenAL loading | 122 ms | 1.9% |
| DoomLevelAABBTree::Update | 115 ms | 1.8% |

Rows overlap; do not add them. The two explicit exclusions are evaluated on
actual sample ancestries, not by subtracting broad aggregate percentages.
Broad parents remain substantial: `DrawScene` 33.7%, `RenderScene` 25.6%, and
`MtRenderState::Apply` 20.7%, including their children. Their percentages are not
exclusive costs or independent optimization opportunities.

Material observation is now 341 ms / 5.4%, compared with the previous 448 ms /
7.1%. All 341 ms have the Metal binding caller. Source-line attribution shows
206 ms at the shared resource-entry call. Of 154 ms strcmp leaves in material
ancestry, 78 ms are at that call, and 76 ms are at first-observation sites
(read/use/external/backend-observation scans and earlier-writer search). The
second cache-lookup site is gone. Thus the repeated caller work is reduced, but
string comparison is not eliminated: resource identity remains content-based,
and first-observation dependency work still runs. This does not qualify an FPS
or whole-engine CPU improvement: there is one fresh capture, focus differs,
inputs are not demo-controlled, and sampling/clock/workload effects remain.

The next CPU rendering investigation should split `MtRenderState::Apply` and
its shared hwrenderer callers. Outside binding-manager ancestry, sampled children
include `ApplyMaterial` 169 ms, `ApplyRenderPass` 137 ms and `ApplyStreamData`
75 ms. Driver state/constant work and memory operations also appear as leaves;
these are host-side stacks, not measured GPU execution time. Scene creation
runs through BSP traversal: AddLines 131 ms and AddLine 83 ms inclusive. Movement
is independently worth investigating: P_TryMove ancestry includes block-line
iteration (109 ms leaf), point-side tests (64 ms) and bounding-box checks (35 ms).
These source-backed candidates are narrower than a generic hwrenderer rewrite
or concurrent actor ticking. No further optimization is implemented here.

The next render-state experiment should count attempted versus changed material,
pipeline/render-pass and stream state before changing caches. Keep graph-driven
replay and dependency checks. Advanced pass scheduling, culling and resource
reuse need their own correctness contracts and GPU/memory evidence; the current
CPU sample cannot justify them. Eliminating the remaining resource-name lookup
would require stable registered identities with lifetime/generation/alias rules,
not caller pointer identity or skipping observations of already-bound textures.

Audio has three ready hits, no not-ready/failure/cancellation counters, and no
sampled LightningFlash -> OpenAL synchronous load ancestry. This remains a
stationary saved-world workload, not the manually controlled doorway route.
Doorway-route, uncapped and long-session acceptance stay open.

CPU artifacts: `/private/tmp/gzdoom-graph-callers-cpu-20261110-145334`, including
archived final executable, hashes, arguments, raw trace/exports, image map,
symbols, bounded samples, `targeted-ranking.json` and render-state/scene-creation
breakdowns. `caller-attribution-final.json` and its reanalysis script are in the
qualification artifact directory alongside the earlier caller attribution.
