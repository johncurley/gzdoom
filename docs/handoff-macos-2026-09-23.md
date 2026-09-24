# Handoff to macOS — Intel Metal framegraph validation — 2026-09-23

This is the next macOS task after the Linux framegraph and renderer-tuning
tranche. The Linux side has established the shared contracts and verified the
OpenGL/Vulkan observers. The macOS side should now validate the same contracts
against the native Metal renderer on the Intel reference machine.

This handoff is intentionally about correctness and observation first. It does
not authorize Metal-specific scheduling, transient aliasing, memoryless
attachments, or other TBDR policy. Apple Silicon has never run this renderer;
those decisions remain gated on real M-series hardware.

## Current state

The framegraph is no longer only a design document:

- `FrameResources` is a shared backend-neutral registry for declared resources,
  dimensions, formats, handles, per-frame reads/writes, and validation.
- `FrameGraph` records pass reads/writes and backend-observed resource uses,
  computes deterministic RAW dependencies, and reports lifetimes/dead-pass
  candidates. It also records backend-neutral upload facts and read
  observations for the covered Metal paths. It does not reorder work, allocate
  resources, emit barriers, or change execution.
- The shared postprocess path records the pass names used by the graph. The
  graph covers the real bloom, SSAO, exposure, tonemap, colormap, lens, and
  FXAA paths when those paths actually run.
- Metal already declares its scene/pipeline targets, AO resources, and bloom
  resources; its scene, postprocess, AO, and bloom code also feeds the graph's
  backend-observation hooks.
- Vulkan and OpenGL now record upload/read observations for their ordinary
  material-texture paths. Vulkan records its staging-copy upload and observes
  material layers when descriptor sets are used; GL records its synchronous
  material upload at bind time. Other upload classes are not yet
  covered, and Vulkan runtime validation remains pending on Linux.
- Metal's material binding path now maps live hardware textures to stable
  per-wrapper graph names and records sampled reads. Private-blit uploads from
  synchronous creation, worker-prepared loads, and the generic hardware-texture
  creation and dynamic staging paths are recorded after commit. Direct
  `replaceRegion` updates and non-material texture classes remain outside this
  upload observation coverage.
- Metal's GCD loader now captures an owned source-pixel snapshot on the render
  thread. The worker processes only snapshot values; texture metadata and
  `ImageArena` updates return to the render thread. GPU creation and upload
  also remain on the render thread. This is not a shared loader and should not
  be made one speculatively.
- Metal shared staging buffers are now retired through the four-slot frame
  recycle ring before becoming reusable. `BeginFrame()` advances the ring only
  after the three-frames-in-flight semaphore wait. This avoids immediate pool
  reuse while a blit may still consume the buffer; cold-load stress and visual
  parity still need validation.

### Corrections from the 2026-09-23 Intel run

The source tree and first runtime check showed that this handoff overstated the
existing upload-observation coverage. The shared graph and Metal hooks are now
implemented for the private-blit paths described above; Vulkan still has no
upload-readiness record. Treat the Metal result as scoped instrumentation, not
as complete cross-backend upload tracking or proof of GPU completion.

### Cross-backend upload observation update (2026-09-24)

The earlier statement above was accurate for the 2026-09-23 check and is
superseded for the material-texture paths below. GL now records uploads from
`FHardwareTexture::BindOrCreate` and observes the corresponding material bind.
A real first-frame MAP07 GL dump reported 38 upload records; all 38 had
`ordered-before-read=yes` and `read-after-upload=observed`, with no upload
validation errors. This demonstrates the ordinary client-memory upload path on
Intel OpenGL; PBO-only, canvas, wipe, and other non-material upload paths are
not claimed as covered.

Vulkan now records `VkHardwareTexture::CreateImage` uploads and observes each
material layer at descriptor use, including cached descriptors. Staging remains
owned through transfer completion, and transfer command buffers precede draw
command buffers on the same queue. This also uncovered and fixed the
non-mipmapped image path: after its buffer-to-image copy it now transitions to
`SHADER_READ_ONLY_OPTIMAL` before sampled use. GL compiled and was exercised
live; the Vulkan translation unit passed a syntax-only compile, but a full
Vulkan configure/runtime test could not run on this Mac because MoltenVK is
absent. Linux Vulkan validation is still required before calling the backend
parity complete.

The Intel run has now exercised a real MAP01 frame with bloom, SSAO, and
exposure active: `r_framegraph` reported 40 passes and 43 edges, and
`r_resource_validate 1` reported no stale-size diagnostics. The only untouched
resources were the screen/save shadow maps, which this run did not use. The
framegraph self-test passed. This does not establish upload ordering or
Metal-versus-OpenGL image parity.

After implementing upload observations and fixing the dynamic private-texture
blit command-buffer lifecycle, `cmake --build build --parallel 4` completed
successfully on Intel. The bundled-app smoke run initialized Metal and
`r_framegraph_selftest` reported `selftest: PASS`; its positive case observes a
read after a committed upload, and its synthetic negative control reports a
false consumer-ordering fact. This does not exercise the dynamic upload path in
a real frame. The required `demo1.lmp` timedemo was unavailable in the
workspace, so no timedemo result is claimed.

The effects-on MAP01 case is exercised. The postprocess-off stock MAP06
baseline is now also complete: GL and Metal each reproduced on retry, and
`crossbackend.py --only baseline --scene doom2 --keep` reported `OK`, tone
ratio 1.00, and uniform backend noise (median band mean 0.013). One earlier
Metal self-check sample had an invalid zero-width scene viewport (`0x572`) and
was rejected; the next self-check passed, so that capture was not used.

Two real Metal cold/repeat load runs were exercised in isolated processes,
with all optional postprocess effects off: MAP06 -> MAP01, then MAP01 -> MAP01.
The first MAP01 load produced 56 uploads in run 1 (17 / 2.96 ms, 38 / 4.32 ms,
1 / 0.23 ms) and 57 in run 2 (17 / 3.89 ms, 39 / 4.57 ms, 1 / 0.23 ms).
The repeat produced 39 uploads in both runs (2.88 ms and 3.51 ms). So this
repeat has roughly 30% fewer uploads by count, but is not an upload-free warm
hit. CPU upload time was lower in both repeats, but these two sequential pairs
are not an interleaved timing experiment; no performance claim is made.

Framegraph dumps retained three and six cold-load
`MtHardwareTexture::CreateImage` records respectively, and five repeat records
in each run. Every retained record showed staging retained, transfer committed,
queue ordering asserted, and `read-after-upload=observed`. `r_resources`
reported 16 resources / 28.7 MB, with only the two unused shadow maps
untouched; no stale-size diagnostic appeared. All observed uploads in these
runs were render-thread `CreateImage` uploads—the worker-prepared async and
dynamic staging paths were not identified.

A follow-up same-process cold/re-entry test targeted worker-prepared uploads:
isolated config, `gl_precache 1`, resource validation enabled, and all five
postprocess toggles pinned off; MAP07 -> MAP08 was followed by MAP08 -> MAP08.
The cold transition recorded 3,270 worker-prepared upload events. Early real
framegraph dumps showed two then one sampled reads; all records had
`staging=yes`, `transfer=yes`, and `ordered-before-read=yes`. Re-entering MAP08
added 2,164 pending upload records (the retained-record total rose from 3,268
to 5,432); the following dumps observed six then one more sampled reads. No
upload record had a missing staging/transfer/ordering fact. `r_framegraph`
reported two passes with postprocess disabled, `r_resources` reported 16
resources / 42.1 MB with only the scene/save shadow maps untouched, and no
stale-size diagnostic appeared.

This was not a warm-hit result: the final cold dump held 3,268 records under
2,562 unique stable texture names; the final repeat dump held 5,426 records
under 2,560 names, with 2,559 names shared between the dumps. Thus the repeat
added upload records mostly against wrappers already seen in the cold load.
`TextureUploadCPU` reported one sample per transition (433.696 ms cold,
426.876 ms repeat); that is too little data for a performance comparison.
Inspection explains the likely cause: `MetalRenderDevice::PrecacheMaterial()`
queues each layer unconditionally, and `QueueHardwareTextureLoad()` suppresses
only a still-pending task; `PerformAsyncGPUUpload()` deliberately reuses and
rewrites an existing compatible private texture.

Added the resident-image guard to `QueueHardwareTextureLoad()`, matching the
other backends: GL's `BindOrCreate()` and Vulkan's `GetImage()` only create or
upload when the hardware image is absent. Source invalidation clears the
hardware-texture container (`CleanHardwareData(true)` / `flushtextures`), so
the next precache gets an empty wrapper. A rerun with the same isolated config
and cvars recorded 2,506 worker uploads on the first MAP08 transition, then no
new records on MAP08 re-entry; pending observations only fell from 2,506 to
2,502 as sampled reads were retired. All observed records retained
`staging=yes`, `transfer=yes`, and `ordered-before-read=yes`. The pre-fix
control added 2,164 records on the same re-entry. `TextureUploadCPU` reported
208.675 ms for the first transition and 0.254 ms for the repeat (one sample
each); this is consistent with the upload-record change but is not a stable
timing estimate.

Then issued the real `flushtextures` command and re-entered MAP08. This added
2,165 worker upload records under fresh wrapper names, confirming the resident
guard does not suppress invalidation-driven reloads. All records again had
staging, transfer, and ordering facts set; sampled reads appeared in the early
frame dumps, and resource validation remained clean. The synchronous
first-material-use fallback in `MtRenderState::ApplyMaterial()` was not
changed; this run observed no `CreateImage` fallback records, so that separate
cold-load race remains unproven rather than being claimed fixed.

The invalidation audit also found a distinct pending-task lifetime hazard:
`PendingUpload::hwTex` is a raw pointer, while `CleanHardwareData(true)` /
`flushtextures` can delete the wrapper through `FHardwareTextureContainer::Clean()`.
If that happens before `ProcessAsyncTextureLoads()` drains the worker result,
completion could dereference a destroyed wrapper. The forced-flush check above
ran after several dozen frames, but queue-empty status was not instrumented; it
did not deliberately invalidate a known-in-flight task and does not test this
case. No history entry was found for this specific path.

Implemented the narrow lifetime contract: `Reset()` and wrapper destruction
detach the task mapping while the manager is live; synchronous `CreateImage()`
does the same before its upload; detached worker results remain loader-owned and
are discarded by `ProcessAsyncTextureLoads()` without consulting the wrapper.
Manager shutdown clears all outstanding wrapper task IDs and mappings before
waiting for the loader, so later wrapper destruction sees no manager-owned raw
pointer. `QueueHardwareTextureLoad()` also skips resident images, matching the
other backends; content invalidation still creates fresh wrappers and reloads.
The Metal build passes. A live Metal run with `gl_precache 1` reached MAP08,
ran `flushtextures` on the next scheduled frame, re-entered MAP08, and exited
normally. It did not expose pending-task/cancellation counts, so a deterministic
test that proves a specifically known in-flight task was detached is still
needed; the run establishes transition survival, not that stronger claim.
Existing detailed forced-flush/reload and warm re-entry measurements predate
this cancellation change, so they establish reload/cache behavior but are not
runtime coverage for the new lifetime path.

**Correction, deterministic in-flight test (2026-09-24):** a temporary test
gate held the first GCD worker task for 5 seconds and tracked active task IDs.
With `gl_precache 1`, `+map MAP07 +execafter 1 "flushtextures"`, and tracing
enabled, the render-thread log reported `cancelled-in-flight id=1` while
`flushtextures` destroyed the owning wrapper. After the worker was released,
the same ID reported `late-completion-dropped`; the process exited normally.
This proves the tested worker result outlived wrapper destruction and was
discarded without dereferencing the stale pointer. Delay and tracing hooks were
removed after the run. It does not test arbitrary shutdown timing or every
texture invalidation path.

The direct-run screenshots were 1152x720 in both runs. The two warm-repeat
captures were byte-identical. The cold-load captures differed in 0.4185% of
the analysis region (max channel delta 120), localized to one small feature;
the paired cold-versus-repeat captures had the same localized difference.
Because the cold MAP01 captures did not reproduce, they are not parity
evidence. The postprocess-off MAP06 harness capture is the deterministic
cross-backend check; transition-image parity and source-pixel snapshot output
parity remain open. The worker-prepared snapshot path and render-thread capture
cost have since been exercised and measured below, but upload-observation
coverage is still not exhaustive.

The Ashes save scene was also probed directly (save01, MAP07 -> MAP08). The
transition recorded two `CreateImage` uploads (0.85 ms total) and both showed
`read-after-upload=observed`; it did not produce a worker-prepared record.
`crossbackend.py`'s save-scene self-check did not meet its exact-repeatability
gate: GL's two captures had the same mean but differed on 95 pixels (max
channel delta 8), while Metal differed only at or below the 2-LSB threshold.
Per the harness contract, no cross-backend verdict was taken from that pair.

The later source-pixel snapshot check found and fixed why the GCD completion
path never drained: worker completion was dispatched onto Cocoa's main queue,
but the application runs its long-lived game loop inside a main-queue block.
The worker-produced callbacks therefore remained queued behind the game loop.
Completed tasks now enter a mutex-protected queue directly from GCD; the render
thread drains it and performs texture metadata/ImageArena updates and GPU
uploads. A live precache run exercised 977 completed texture tasks where the
earlier probe had shown `pending=977, worker_done=977, visible=0`. During the
post-fix MAP07-to-MAP08 transition, the renderer's `TexUpload` counter rose to
2,229 (an accumulated transition statistic, not a single-frame count).
Framegraph reset probes saw 73 then 65 sampled reads of newly-uploaded
resources, followed by smaller counts; remaining pending upload records were
textures not consumed at the tested view, not reads that failed ordering.
This confirms that the real worker completion queue now drains and that sampled
consumers are observed after upload recording; it does not prove GPU completion
or every precached texture is used in that frame.

`CreatePixelSnapshot()` accepted every attempt in the exercised save/precache
probe (including 12 representative source dimensions). A temporary bounded
timer around snapshot capture, removed before the clean build, recorded 3,200
accepted calls in the measured interval: cumulative capture cost 1,741.971 ms,
mean 544.366 us, and one 116.565 ms maximum outlier. A second temporary probe
profiled 3,200 calls by source pixel area: <=4kpx (1,677 calls, 223.070 us
mean), <=64kpx (1,151, 769.838 us), <=256kpx (366, 1.401 ms), and >256kpx
(6, 6.884 ms). The new run's largest outlier was 102.210 ms for a 256x256
source; even a 48x46 source reached 38.757 ms. Cost is therefore not
monotonic in source dimensions, so do not optimize solely for large-image
memcpy. These timings cover full source-pixel acquisition/allocation/copy on
the render thread, not just memcpy. Snapshot capture is a visible Intel
cold-precache cost and needs its internal stages profiled before choosing an
optimization or usage limit; no performance win is claimed.

A final temporary split timer on a real MAP07-to-MAP08 precache transition
showed what the outlier contains: a 126.817 ms capture for a 384x128 source
spent 126.764 ms in `GetBgraBitmap()` and 0.052 ms in the snapshot row copy.
Other >10 ms calls were usually dominated by `GetBgraBitmap()` too, but one
cached 370x210 source spent 11.886 ms in the copy itself. So large-image copy
bandwidth is not the general cause of the worst stalls; source bitmap
acquisition is the next investigation target, with copy cost still measurable
in some cases. Do not move `GetBgraBitmap()` to a worker without a separate
source-lifetime/thread-safety contract.

A follow-up profile split `FImageSource::GetCachedBitmap()`'s pixel
construction from cached-bitmap copying during matched MAP07-to-MAP08 Metal
precaching. The earlier 25 calls above 10 ms were all on uncached-decode or
precache-fill paths; pixel construction accounted for essentially all their
delay, while cached bitmap-copy time was zero in every slow case. A temporary
`CopyPixels()` probe on two further matched transitions recorded the same six
slow sources each time: `textures/frame`, `forest11`, `conclarg`, `cnhuge11`,
`citybak1.png`, and `citybak4.png`. All six are PNG data (including the
extensionless lumps). Their ≥5 ms calls summed to 40.4 ms and 40.8 ms in the
two runs; per-source times repeated within about 0.6 ms, with `cnhuge11` the
slowest at 9.7–10.0 ms. The earlier 70–127 ms individual outliers did not
reproduce in these matched runs, so those remain unexplained and should not be
treated as steady-state costs.

A deeper temporary split inside `FPNGTexture::CopyPixels()` showed
`M_ReadIDAT()` accounts for nearly all of those PNG calls; subsequent
pixel-format conversion was about 0.2–0.4 ms. In two matched inner profiles,
IDAT totals were stable for the largest examples: `frame` 5.89–6.14 ms,
`forest11` 5.85–6.10 ms, and `cnhuge11` 8.35–8.45 ms. Timing `inflate()`
separately attributed about 2.14–2.21 ms to zlib for `frame`, 2.93–3.04 ms
for `forest11`, and 3.75–3.78 ms for `cnhuge11`.

A final low-level probe split `inflate()`, `UnfilterRow()`, file reads, and
residual IDAT work in another matched transition. For `frame`, the split was
2.322 ms inflate / 4.141 ms unfilter; for `forest11`, 3.018 / 2.966 ms; for
`conclarg`, 2.059 / 2.127 ms; for `cnhuge11`, 3.825 / 4.763 ms; for
`citybak1.png`, 3.487 / 1.056 ms; and for `citybak4.png`, 3.566 / 1.036 ms.
File reads were 0.005–0.045 ms and residual setup/loop work 0.04–0.10 ms.
This one inner run agrees with the repeated inflate totals: zlib and row
unfiltering are both material, with the balance varying by image. The costs
are therefore in the generic PNG decoder rather than cache copying or GPU
upload. A fast path would need preserve PNG filter behavior and likely address
both zlib streaming and row reconstruction; profiling alone has not established
a low-risk change or justified enabling global `gl_precache` by default. All
temporary timing code was removed and the clean build passed.

A separate per-filter probe confirmed why some PNGs pay so much more in
`UnfilterRow()`: the expensive examples are Paeth-filtered. One 512x512 RGBA
input spent 3.926 ms of its 6.178 ms IDAT time in 511 Paeth rows (inflate
2.172 ms); a second 512x512 RGB input spent 3.471 ms of 7.297 ms in 502 Paeth
rows (inflate 3.702 ms). A 384x384 RGBA Paeth image spent 2.652 ms of 5.676 ms
there. By contrast, a 512x605 indexed PNG using only the None filter spent
0.040 ms unfiltering and 2.193 ms inflating (2.333 ms total). Thus PNG is not
uniformly slow: zlib is consistently material, and Paeth adds several more
milliseconds for these true-color assets; the simpler filters are much
cheaper. The probe was temporary and removed, followed by a clean build.

The rare 70-127 ms `GetBgraBitmap()` outliers are still not attributed. The
existing stage probes localize them to pixel construction/source acquisition,
not the snapshot row copy, while repeated matched PNG decodes of the six slow
MAP08 assets stay in the 5-10 ms range. A Time Profiler capture taken after
MAP08 had loaded did not overlap a stall, so its idle-scene stacks must not be
used to explain those outliers. Continue by capturing a cold transition with
`mt_frametrace`/`mt_stalltrace` enabled and correlating its slow frames with
per-texture stages; do not infer that the ordinary Paeth cost explains the
much larger spikes.

A 3-second system sample of steady MAP08 rendering did show a distinct
framegraph CPU cost candidate: Metal resource binding repeatedly reaches
`FrameGraph::ObserveResourceRead()`, where it constructs temporary `FString`s
and performs `TMap` lookups (including allocator activity). This sample was
after precache, not during a hitch; it supports a possible per-frame CPU tax,
not a cause for the rare long stalls. Quantify it with an observer-on/off A/B
before changing the tracking path. The Time Profiler trace for this idle
interval was saved under `/private/tmp`, but `xctrace export` crashes on this
macOS 12 host; the readable `sample` report is the evidence available here.

### PNG decoder A/B (isolated Intel CPU, 2026-09-23)

The bundled `stb_image` candidate was tested in-engine on the six recurring
MAP08 true-color PNGs. All six decoded pixels matched the existing path exactly.
The whole-call comparison changed direction when decoder order changed, because
the second path benefited from archive/cache warmth; those timings are not a
valid speed claim. A same-input in-memory decode comparison was added as a
separate control and did not establish a consistent stb win, so no route change
was kept.

For the library candidates, libspng 0.7.4 and libdeflate were fetched and built
under `/private/tmp` only; GZDoom's build/dependency configuration was not
modified. A standalone optimized harness consumed the actual six PNG entries
from the Ashes pk3 through stdin, excluded archive I/O, and took the median of
31 decodes per process in three independent sweeps. stb and libspng produced
byte-identical RGBA output for all six. Summed per-file medians were 31.832 ms
for stb and 22.165 ms for libspng, about 30% lower for libspng. Four inputs
improved materially (roughly 32-44%); the two 1024x256 images were 3-5% slower.
This makes libspng the strongest full-decoder candidate so far, but the result
is a decoder-only harness measurement, not a GZDoom transition result.

The same harness joined each PNG's IDAT chunks and compared system zlib with
libdeflate's whole-buffer zlib API on the exact unfiltered scanline bytes. The
output matched byte-for-byte. Summed medians were 14.173 ms for zlib and 6.352
ms for libdeflate (about 2.2x faster); per-image savings ranged from about
0.65 to 2.39 ms. This excludes chunk collection and the existing Paeth
unfiltering, so it is an upper bound on the whole-decoder improvement. A real
integration test must account for buffering compressed IDAT data, temporary
raw-output memory, interlacing/bit-depth fallbacks, and engine output parity.
The temporary harness and library builds remain in `/private/tmp`; no probe
code remains in `pngtexture.cpp`.

### Local-only libspng engine prototype (2026-09-24)

A disposable GZDoom build routed only 8-bit, non-interlaced RGB/RGBA PNGs
through libspng 0.7.4; all other formats and libspng errors fell back to the
existing `M_ReadIDAT()` path. No dependency or build-system change is retained.
The local-only runtime switch was used in matched `gl_precache 1` MAP07 → MAP08
Metal transitions. All six timed runs plus the verification run reached MAP08
without a load error.

During one verification transition, every successful libspng raw decode was
compared byte-for-byte against `M_ReadIDAT()` before the unchanged
`FBitmap` conversion. There were zero mismatch reports; the log recorded 52
eligible libspng decodes taking at least 1 ms (faster eligible decodes were
also checked). Indexed PNGs stayed on the existing path.

Across three control/candidate pairs, the summed decoder times for the six
recurring MAP08 PNGs were 42.290 / 28.114 ms, 35.126 / 32.138 ms, and
34.830 / 24.691 ms (internal / libspng). The median of the three summed
readings was 35.126 ms versus 28.114 ms, about 20% lower. Per-file medians
sum to 35.427 ms versus 27.836 ms (~21% lower): five assets improved, while
`citybak1.png` was about 10% slower. The pairwise win varied from 8.5% to
33.5%, and the control's first run was notably slower than the other two.
These are timed decoder-stage totals, not full map-transition wall times.

This was an exploratory diagnostic rather than a predeclared acceptance A/B:
no numeric engine-level prediction was recorded before the first measurement,
so the result is evidence to justify a more controlled follow-up, not enough
to adopt or ship a new decoder. A production decision still needs a
predeclared end-to-end transition metric and repeatable noise-floor control,
plus cross-platform dependency/build review. All temporary integration,
profiling, and parity-check code was removed afterward; libspng source/build
remain under `/private/tmp` only.

### Preregistered six-texture libspng transition A/B (2026-09-24)

Follow-up prediction: the six MAP07/MAP08 recurring PNG decode-stage median
would remain near the earlier 35.4 ms internal versus 28 ms libspng, with
byte-identical raw output; aggregate PNG time and `G_DoLoadLevel()` wall time
would determine whether that local win matters to a transition. Three fresh
control and three candidate launches used identical `gl_precache 1` Metal
configuration, loading MAP07 and then MAP08 at the same frame delays. The
candidate was deliberately restricted to the six preselected assets; all
other PNGs stayed on `M_ReadIDAT()`.

All six target calls succeeded through libspng with zero fallback. Summing
the per-asset medians gives 33.847 ms internal versus 24.629 ms libspng
(27.2% lower). Across full level loads, median PNG decode totals were
234.458 vs 228.778 ms for MAP07 (2.4% lower) and 195.582 vs 193.317 ms for
MAP08 (1.2% lower). Median `G_DoLoadLevel()` time was 1082.662 vs 1083.344
ms for MAP07 and 1541.771 vs 1533.299 ms for MAP08: effectively unchanged,
within observed run-to-run spread. The six-file decode win therefore did
not produce a measurable end-to-end transition improvement in this test.

One initial candidate run routed every eligible 8-bit RGB/RGBA PNG through
libspng before the preregistered scope was enforced. That broad-path run
raised the MAP07/MAP08 summed PNG decode total to 254.349/244.973 ms versus
233.695/195.582 ms in its control; it is not adoption evidence for the
six-file proposal, but it is a clear reason not to switch the general PNG
path on the basis of the isolated decoder benchmark. Raw-output parity was
already checked byte-for-byte in the preceding prototype verification. No
engine integration or dependency change is recommended from these results:
the targeted microbenchmark win is real, but the user-visible transition
metric is a wash. All temporary hooks, CMake wiring, and the local switch
were removed; libspng sources/builds remain only under `/private/tmp`.

The saved-MAP07 screenshot check compared two async captures and two
synchronous (`gl_precache 0`) captures at the same paused viewpoint. The async
pair was byte-identical. Async capture 1 and synchronous capture 1 were also
byte-identical. However, the synchronous repeat differed by 84.47% of analyzed
pixels over 2 LSB (mean luminance 17.011 vs 10.983), despite the same pause and
capture commands. That failed control repeatability gate makes source-pixel
snapshot output parity inconclusive; do not treat the one exact async/sync pair
as a parity result. The earlier MAP07-to-MAP08 screenshot pair also varied in
its control arm. This invalidates the manual saved-MAP07 comparison; the stock
MAP06 harness control established below is a separate, repeatable setup.

That control has since been established for the stock cross-backend harness:
`python3 tools/matrix/crossbackend.py --only baseline --scene doom2
--selfcheck --keep` reported byte-reproducible MAP06 captures for both GL
(mean 20.981) and Metal (20.989). The subsequent baseline comparison was
`OK`, with uniform backend noise (median band mean 0.013). Repeating the same
sequence for `ssao` also passed both backend self-checks (GL mean 20.744,
Metal 20.244) and the cross-backend comparison (`OK`, uniform noise, median
band mean 0.609). Prediction was byte repeatability within each backend and no
structured cross-backend divergence; both checks matched. This establishes a
repeatable capture control for these stock MAP06 configurations, not for the
saved-MAP07 manual capture or for cold/warm texture-load behavior. Keep those
as distinct acceptance cases.

Bounded fixed-view transition capture (2026-09-24): from the Ashes `save01`
MAP07 save, captured MAP08 after one arrival at rendered frame 300 (cold arm),
and after arrivals at frames 120 and 300 (warm/re-entry arm); both shots were
taken at frame 420 with `cl_capfps 1`. GL and Metal were confirmed from their
logs. The cold-arm repeat was not byte-identical, but both backends stayed
within max channel delta 1 and mean luminance delta 0.0075 in the analyzed
region (zero pixels over 2 LSB). The warm-arm repeat was byte-identical on both
backends. Matched GL/Metal comparisons were sparse: cold had 34 pixels over 2
LSB (max 35), warm had 33 (max 35), with no broad or structured divergence.
However, cold-vs-warm Metal differed at 5,095 pixels over 2 LSB (1.743%, max
42); the localized changes cluster around dynamic scene/player content. The
strict exact-repeat gate therefore still fails for the cold arm, and the
cold-vs-warm difference is not a renderer-parity verdict. This is useful
bounded evidence, not closure of the transition parity acceptance case.

To separate conversion parity from scene-capture noise, added the bounded
`r_texture_snapshot_selftest <texture> [texture ...]` diagnostic. Prediction:
zero output-byte and dimension mismatches between synchronous
`CreateTexBuffer()` and snapshot capture/processing for every tested flag set;
the one-byte negative control must be detected. Intel run on `BRICK1` and
`M_DOOM` passed 10/10 exact comparisons (plain, expand, upscale,
expand+upscale, indexed) and detected the corrupted-byte control. The command
uses `CheckForTexture()` so unknown fixture names fail instead of silently
falling back to the default texture. A follow-up Intel run passed 18/18 exact
comparisons, covering translation 0 and the active non-identity standard Ice
remap for each RGBA flag set; indexed output remains correctly translation-0
only. The one-byte negative control was still detected. This validates
converted CPU pixels for these two fixtures, not Metal GPU upload output or
scene parity. Its reference conversion can update
mask/translucency/hole metadata, so an RAII guard restores that per-texture
state after testing and avoids perturbing a live renderer diagnostic session.

After removing all temporary timing probes, the clean Intel build was exercised
through a fresh MAP07-to-MAP08 Metal transition with `gl_precache 1` and resource
validation enabled. `r_framegraph_selftest` and the texture snapshot self-test
passed. Three early graph dumps each reported 41 passes / 44 edges: the first
landed before MAP08 had rendered (0 observed upload reads), and the next two
reported 4 then 2 later sampled reads before those consumed upload records were
retired on subsequent resets. `r_resources` reported 38 resources / 49.0 MB;
only `save.ShadowMap` was untouched, and no stale-size diagnostic appeared.
Thousands of other precache upload records remained `not-observed` because this
map/view did not sample those cached textures. This is expected for broad
precache, not a claim that every cached texture was consumed. Dumping only
after many later resets misses the short interval where consumed upload records
are visible, so transition checks should capture the first few frames.

The Metal reverse-Z lineardepth shader patch had also gone stale. The old
expression matcher expected `: 1.0);`, while the current shader has ternaries
ending in `: 1.0;`. The matcher now targets that syntax and requires the two
expected branches. A live Intel run showed no missed-patch warning. Image
parity for this shader correction remains part of the capture comparison below.

The Intel SSAO check then caught a separate Metal format-mapping gap. Shared
`PPAmbientOcclusion` now allocates `LinearDepthTexture` as `PixelFormat::R16f`,
but Metal's `GetMetalPPTextureFormat()` had no `R16f` case and silently fell
back to BGRA8 (4 bytes/pixel). It now maps to `MTL::PixelFormatR16Float`
(2 bytes/pixel); Metal's resource-format decoder also reports `R16F`.
`r_resources` on a real Intel frame reports `AO.LinearDepth` as `576x360 R16F`
and touched for both read and write. `r_framegraph` reported the live seven-pass
SSAO chain with nine edges, and the self-test passed. No stale-size diagnostics
appeared.

Measurement used `crossbackend.py --only ssao --scene doom2 --selfcheck
--keep` before and after the fix, with both backends byte-reproducible each
time. The fixed-build cross-backend run reported `ssao OK`, uniform backend
noise (median band mean 0.609), below the sparse-pixel coverage floor. Against
the reproducible pre-fix Metal capture, the fixed capture's analysis-region
mean luminance fell by 0.676, with 51.795% of pixels darker and none brighter
(max channel delta 10). That is the predicted direction for restored linear
depth beyond 1.0 contributing to AO. Captures remain an Intel MAP06 harness
check, not the outstanding fixed-viewpoint Metal-vs-OpenGL parity suite.

Follow-up repeatability check (2026-09-24): the stock MAP06 baseline again
reproduced exactly on GL (mean 20.981) and Metal (20.989); the SSAO arm also
reproduced exactly on GL (20.744) and Metal (20.244). A fresh cross-backend
SSAO comparison reported `OK` and uniform noise, but its largest localized
signal was 29 on one pixel, below the 100-pixel coverage floor. Treat this as
repeatable overall parity with no detected effect-shaped divergence, not as a
pixel-level AO-specific parity proof.

Observer overhead and texture-sort checks (2026-09-24): a temporary runtime
gate enabled/disabled Metal's framegraph/resource observers while leaving pass
recording enabled. Three interleaved real MAP07 runs per arm showed no
reproducible frame-time cost: observer-off average-window median 16.15 ms,
observer-on 15.75 ms, with essentially identical p50 (~14.9 ms) and overlapping
tails. The apparent 0.4 ms advantage with observers enabled is below the
run-to-run spread and is not evidence of a speedup. This measurement covers
Metal only; observer overhead on GL/Vulkan remains unquantified.

The existing `gl_sort_textures` option was then tested off/on with postprocess
disabled. Repeated GL and Metal captures were byte-identical within each
backend (zero changed pixels); the cross-backend comparison also remained
within its established uniform-noise floor. Three interleaved Intel Metal
MAP07 timing windows per arm had overlapping frame-time distributions (sorted
medians about 15.7 ms, unsorted about 15.5 ms; p50 about 14.9 ms for both).
On this GPU/present-bound setup texture sorting has no demonstrated performance
gain, so this evidence does not justify changing its default. It does establish
that enabling it preserves the tested scene's pixels. The timing result is
specific to this static scene and Intel Mac; a CPU-bound scene could behave
differently.

The important boundary is:

```text
CPU decode/processing -> backend GPU upload -> first material/postprocess read
        worker              Metal render thread              framegraph consumer
```

The graph should describe that relationship. It should not own `FTexture`,
`DObject`, or backend texture lifetimes.

## First macOS objective

Validate that Intel Metal produces the same truthful graph and resource report
as the existing Linux backends, without changing the renderer's execution
policy.

Use a fixed viewpoint and a fixed configuration for capture comparisons. Test
at least:

1. Stock scene rendering with postprocess disabled.
2. Bloom and SSAO enabled, so the full postprocess resource set is exercised.
3. A cold material/texture precache, followed by a warm repeat.
4. A level transition or new-area traversal, where asynchronous texture work
   can complete while normal rendering continues.

For each case, collect:

- `r_resources` after a real rendered frame;
- `r_resource_validate 1` output, checking for stale-size diagnostics;
- `r_framegraph` after a real rendered frame, checking `ok=true` and an empty
  report;
- `mt_metrics` and, for cold-load runs, `mt_frametrace`/`mt_stalltrace` as
  appropriate;
- a deterministic screenshot or harness capture against the existing Metal
  baseline.

Do not use `+quit` as proof that a real framegraph ran. Command-line `+`
commands execute before the main frame loop. Use an actual short play session,
an existing save, or a temporary in-game console command after the first frame.

## Metal upload observation contract — validation in progress

The backend-neutral `FrameGraphUploadDesc` records existing work; it does not
submit, wait for, or schedule GPU work. Its fields intentionally separate the
following facts:

The contract should keep these facts distinct:

- **Preparation:** identifies where source pixels were prepared (GCD worker or
  render thread). The Metal GPU upload itself remains on the render thread.
- **Staging retained:** true only while the staging buffer cannot be reused
  before the blit has consumed it. Metal now delays pool reuse through its
  frame-retirement ring; stress this during cold loads and transitions.
- **Transfer recorded:** true only after the blit encoder has ended and its
  command buffer has been committed.
- **Ordered before consumers:** report queue ordering, not GPU completion.
  Metal upload buffers and later render buffers are created from the same
  command queue, and the upload is committed during `BeginFrame()` before the
  frame's later draw submission. [Apple's command-buffer documentation](https://developer.apple.com/documentation/metal/mtlcommandbuffer)
  says a queue preserves the order in which its command buffers are enqueued.
  Do not label this as “completed” or infer that `commit()` means GPU
  completion.

Each `MtHardwareTexture` wrapper receives a unique graph name that remains
stable when its underlying `MTL::Texture` is recreated. The manager maps only
currently-live Metal texture handles to these names and removes the mapping
when a handle enters the recycle pool. `MtResourceBindingManager` records a
read at the actual material-texture binding path. This gives the graph an
identity/read observation without importing engine texture ownership or Metal
handles into the shared graph.

Upload observations are pending across `FrameGraph::Reset()` until a sampled
read of the same stable name occurs. Passes and reads remain frame-scoped; the
upload event is retained for the first frame that can observe its consumer, then
retired by the following reset. This covers uploads made during startup or
precache before the first framegraph reset.

The current implementation records private-texture blit uploads from the
synchronous `CreateImage` and `CreateTexture` paths, the dynamic staging path,
and the worker-prepared async load path. The dynamic path now commits its
separate blit command buffer in normal gameplay as well as startup; the
startup/early-frame wait remains conditional. Direct `replaceRegion` updates
and non-material texture classes do not yet emit upload events; this is
deliberately not evidence of full-backend upload coverage.
`read-after-upload=observed` means the CPU recorded a read observation after
the commit observation in this frame. It does not mean the GPU completed the
upload, nor does it prove parity for every texture path.

This has now been observed live on Intel, not only in the synthetic self-test.
An isolated MAP06-to-MAP01 transition produced a cold-load upload burst (the
renderer logged 17 uploads, then 38, then 1 across consecutive frames). A
framegraph dump after the transition retained three
`MtHardwareTexture::CreateImage` records across frame resets; each showed
staging retained, transfer committed, queue ordering asserted, and a later
material read observed. The first dump after the map command landed between
frames and had no passes, so it was not used as evidence. The startup-only
MAP06 probe also showed no upload records; the map transition was necessary to
exercise this path. This verifies reset-persistence and read observation for
those three real consumers, not GPU completion or coverage of every upload in
the burst.

The synthetic negative control now sets the consumer-ordering fact false and
asserts that validation reports it; the positive case also checks that the dump
shows a later read observation. Keep these tests synthetic; do not break a real
frame to prove the observer works.

Once the observation contract is established, measure before and after any
upload change:

- cold first-use latency;
- number and total CPU time of texture uploads;
- warm-run frame time;
- whether the first material read occurs only after the recorded upload;
- visual parity and absence of partially uploaded textures.

An upload record that merely makes `r_framegraph` look complete is not enough;
the control run must demonstrate that the instrument detects an intentionally
missing ordering flag or equivalent invalid state.

## What is explicitly deferred

Do not implement these on the Intel-only pass:

- transient resource aliasing or memoryless Metal attachments;
- pass reordering or graph-owned scheduling;
- tile-memory load/store policy;
- replacing Metal's command-buffer ownership with a graph allocator;
- moving GPU texture creation or Metal API calls to worker threads;
- a common loader that captures raw engine texture objects without a lifetime
  contract;
- changing default draw sorting or material order as part of this handoff.

Stable wall/flat sorting is shared scene code and should mechanically apply to
Metal, but it remains an independently measured opt-in experiment until Metal
captures confirm the same correctness and transition behavior.

## Acceptance criteria for this handoff

The macOS tranche is complete when all of the following are recorded with
commands, configuration, and output in the next handoff or commit:

1. Intel Metal builds cleanly from the current branch.
2. `r_framegraph_selftest` passes.
3. A real Metal frame reports declared and touched scene/postprocess resources
   without stale-size errors.
4. The graph reports the passes that actually ran, rather than a static
   template; disabled effects are absent for the expected reason.
5. Cold and warm texture-load runs show whether the currently-covered Metal
   private-blit upload records are correctly ordered; do not claim full upload
   coverage until direct updates and other texture classes are audited.
6. Source-pixel snapshot conversion matches `CreateTexBuffer()` exactly for
   representative fixtures/flags, with a negative control that proves the
   comparator detects a mismatch. This is a CPU conversion gate, not a
   substitute for a rendered capture.
7. Existing Metal-vs-OpenGL capture parity remains within the established
   baseline for the tested scene/configuration, with a repeatable capture
   control.
8. Any Metal-only change includes a control result and does not claim Apple
   Silicon behavior.

After this acceptance pass, Linux work can continue independently on Vulkan and
OpenGL tuning. Apple Silicon remains the gate only for the policy layer that
exploits TBDR behavior.

## Useful references

- `docs/frame-graph-resources.md` — registry vocabulary and validation policy.
- `docs/frame-analysis.md` — Metal pass/resource inventory.
- `docs/frame-analysis-vulkan-gl.md` — Linux backend counterpart.
- `docs/renderer-methodology.md` — measurement and falsification rules.
- `docs/gpu-capture-protocol.md` — capture procedure.
- `docs/handoff-framegraph-2026-09-21.md` — Linux implementation history.
- `src/common/rendering/metal/textures/mt_textureloader.{h,cpp}` — current
  worker-side texture processing contract.
- `src/common/rendering/metal/textures/mt_texture.cpp` — current render-thread
  GPU upload path.
