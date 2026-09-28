# Handoff to Linux — Vulkan framegraph and upload validation — 2026-09-24

> **Status update (2026-09-28):** The Linux validation tranche described
> below is closed; its completed results are appended at the end of this
> document. For current frame-graph status and next steps, see
> [`handoff-framegraph-2026-09-28.md`](handoff-framegraph-2026-09-28.md).

Written at the end of the Intel macOS validation session. The framegraph and
upload-observation work is shared across GL/Vulkan/Metal; the next useful step
is to validate the Linux Vulkan runtime path on the RX 550, then quantify the
shared observer overhead on GL and Vulkan. This handoff does not close the
remaining macOS capture-parity item.

## Before switching machines

The macOS code and handoff updates span shared renderer, backend, and
documentation files. On Linux, verify that the resulting `metal-audit` handoff
commit is checked out before building; do not assume an older local branch
already contains these changes.

Read `CONTRIBUTING.md`, `docs/frame-analysis-vulkan-gl.md`, and the current
state in `AGENTS.md` before changing renderer code. The existing Linux tasks
remain tracked there; this handoff adds a focused renderer-validation tranche,
not a replacement for those tasks.

## What changed or was established on macOS

- The shared framegraph records actual passes, RAW dependencies, resource
  touches, and upload facts; it does not schedule, reorder, allocate, or
  synchronize GPU work.
- GL now records ordinary material-texture uploads and observes the matching
  material reads. A live Intel GL frame reported 38 upload records, all with
  `ordered-before-read=yes` and `read-after-upload=observed`.
- Vulkan now records `VkHardwareTexture::CreateImage` uploads and observes
  material layers when descriptors are used, including cached descriptors.
  The non-mipmapped image path now transitions from transfer destination to
  `SHADER_READ_ONLY_OPTIMAL` before sampled use. This passed a syntax-only
  compile on macOS, but could not receive a full Vulkan build or runtime test
  there because MoltenVK is unavailable.
- Metal's matching private-blit upload and read paths were exercised on Intel.
  This remains scoped coverage: direct `replaceRegion` updates, PBO-only/canvas/
  wipe paths, and other non-material texture classes are not covered by a
  complete cross-backend upload contract. `read-after-upload=observed` is a CPU
  ordering observation, not proof of GPU completion.
- `r_texture_snapshot_selftest BRICK1 M_DOOM` now includes a non-identity
  standard Ice translation for every RGBA conversion mode. The Intel run passed
  18/18 exact comparisons; indexed output correctly remains translation-0 only,
  and the one-byte negative control was detected. This is CPU conversion
  evidence, not GPU upload or rendered-scene parity.
- `cmake --build build --parallel 4` passed on macOS. The former
  `./build/gzdoom -timedemo demo1.lmp -nosound -nogui` command was not run:
  `demo1.lmp` is absent from the checkout. It has since been removed from the
  mandatory gate because no compatible demo input was provided.
- `gl_sort_textures` was image-neutral on the tested MAP07 view on both GL and
  Metal, but showed no Intel Metal speedup. Do not change its default based on
  this result; a CPU-bound Linux scene could behave differently.

## First: build and run Vulkan on real Linux hardware

Use the RX 550 session, not Xvfb, for surface creation and runtime checks. The
Xvfb environment lacks the DRI3/Vulkan surface path used by the existing Linux
tests. Configure/build using the Linux command documented in `AGENTS.md`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPK3_QUIET_ZIPDIR=ON -DHAVE_VULKAN=ON .
cmake --build build --parallel $(nproc)
```

Force each backend explicitly; with `HAVE_VULKAN=ON`, a plain launch may choose
Vulkan by default and make an intended GL control ambiguous. Start with the
platform-keyed matrix baseline, self-checking each backend before trusting a
comparison:

```bash
python3 tools/matrix/crossbackend.py --backends gl,vulkan --only baseline --scene doom2 --selfcheck --keep
```

Then run the same repeatability/comparison gate for `ssao` on `doom2`/MAP06.
Do not use MAP06 to claim a bloom effect; the measured map selection and
relations are documented in `tools/matrix/configs.json`. Do not update a
baseline unless the existing relation guards pass and the result is genuinely
from this Linux platform.

## Verify live graph/resource and upload observations

A CPU self-test or `+quit`-only launch does not prove that a real Vulkan frame
ran. Exercise a short real MAP06/MAP01 session on each backend and capture the
diagnostics after rendering has begun. Check:

1. `r_framegraph_selftest` passes.
2. `r_resources` reports real declared/touched targets; enable
   `r_resource_validate 1` and require no stale-size diagnostics.
3. A live `r_framegraph` reports `ok=true`, actual passes, and no unexplained
   dependency report. Disabled effects should be absent for the expected
   runtime reason, not represented by a static template.
4. For newly uploaded textures that the captured view actually samples, the
   dump reports the upload fact and a later observed read. Untouched precached
   textures are not failures. Do not interpret queue ordering as GPU
   completion.
5. On Vulkan, specifically exercise a non-mipmapped sampled image and verify
   its transfer-to-shader-read layout transition occurs before sampling. Check
   for validation-layer or driver errors as well as image output.

Use an actual play session, a saved view, or a command scheduled after the
first rendered frame for the live dump. Command-line `+` commands are initially
executed before the main frame loop; the synthetic self-test is useful, but it
cannot stand in for a live graph/resource report.

## Then measure observer overhead

Metal's framegraph/resource observer A/B on the Intel Mac showed no measurable
cost: three interleaved MAP07 windows per arm had overlapping distributions
(observer-off average-window median 16.15 ms; on 15.75 ms; p50 about 14.9 ms in
both). The apparent 0.4 ms difference is below run-to-run spread. This says
nothing about GL/Vulkan overhead.

After the Vulkan correctness checks, repeat an interleaved on/off observer A/B
on GL and Vulkan separately, using the shared `vid_frametrace` and a real,
unchanged gameplay route/view. Keep framegraph pass recording and renderer
settings identical between arms; change only the observer gate. Report sample
counts, p50/p95/p99/max, long-frame counts, and the control's run-to-run spread.
Do not call sub-noise differences gains or regressions. If a reproducible
overhead appears, profile it before changing observer semantics.

## Capture-repeatability boundary

Stock MAP06 capture controls reproduced byte-for-byte on Intel macOS and were
reconfirmed on 2026-09-24 (GL mean 20.981; Metal mean 20.989). For the Ashes
save-based MAP08 transition, the bounded test held the final arrival/capture
offset constant (`cl_capfps 1`, screenshot at rendered frame 420):

- Warm same-map re-entry reproduced byte-for-byte on GL and Metal.
- Cold-arrival repeats missed strict byte identity on both backends, but the
  analyzed region differed by at most 1 LSB and had zero pixels over 2 LSB.
- Matched GL/Metal cold and warm captures had only 34 and 33 pixels over 2 LSB
  respectively (max 35), with no broad structured divergence.
- Cold-vs-warm Metal captures differed at 5,095 pixels over 2 LSB (1.743%, max
  42), concentrated near lower-left/player-view details; the cause is not
  isolated.

Therefore the cold arm still fails the project's exact repeatability gate, and
the cold/warm delta is not a cross-backend parity verdict. Keep this as an open
macOS acceptance item; do not convert the near-repeat result into a pass. A
future capture attempt should first pin/freeze the dynamic view and demonstrate
repeatability before comparing cold and warm output.

## Linux exit criteria for this tranche

- GL and Vulkan both build and start on real Linux hardware.
- The `baseline` and `ssao` self-checks reproduce on each backend, and their
  cross-backend comparisons are interpreted only after those controls pass.
- Live Vulkan resources, framegraph, and sampled upload records validate with
  no stale-size or layout errors on exercised paths.
- GL and Vulkan observer overhead is measured separately with repeatable
  controls, or reported as unmeasured if the instrument cannot isolate it.
- Coverage limits and any failed gates are recorded without implying GPU
  completion, exhaustive upload coverage, or macOS/Apple-Silicon validation.

The Linux Tasks in `AGENTS.md` remain independently open where marked there,
including the optional lavapipe CI runtime experiment and the deliberately
reasoned-not-dynamically-verified X11 findings. This tranche does not authorize
expanding into those tasks unless the renderer validation leads directly to a
relevant finding.

## Linux progress — 2026-09-24

The handoff commit was checked out at `31dd07fee` on the RX 550 Linux machine.
The Vulkan-enabled RelWithDebInfo configure and `cmake --build build
--parallel 8` both passed. The prescribed timedemo command initialized the
Vulkan backend and identified the RX 550, but `demo1.lmp` is absent from this
checkout, so no timedemo result is claimed.

The GL/Vulkan matrix baseline and SSAO controls passed before comparison:

| Config | GL self-check mean | Vulkan self-check mean | Comparison |
|---|---:|---:|---|
| baseline | 21.224 | 21.278 | OK, tone x1.00, median band mean 0.113 |
| ssao | 20.481 | 20.537 | OK, tone x1.00, median band mean 0.123 |

Both comparisons had one sparse high-delta pixel below the 100-pixel coverage
floor; neither produced a structured band difference. Captures are retained
under `/tmp/gzdoom-matrix`.

Real RX 550 sessions exercised both backends. With bloom, SSAO, exposure,
tonemap, lens, and FXAA enabled, each produced 45 passes and 46 edges. GL
reported 28 registry resources / 2.8 MB, with only `PipelineDepthStencil`
untouched; Vulkan reported 29 / 4.8 MB, with only `ShadowMap` and
`PipelineDepthStencil` untouched. Neither report contained a stale-size
diagnostic. `r_framegraph_selftest` passed. Early-frame dumps caught pending
uploads before the following graph reset retired them: GL showed two sampled
uploads and Vulkan showed 18; every displayed record had staging, transfer,
ordering, and later-read facts set to `yes`/`observed`. `read-after-upload`
remains a CPU observation, not GPU-completion evidence.

A short Vulkan MAP06 run with
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` completed without validation
messages in its output. The upload dump does not identify mip status per
resource, so a specifically identified non-mipmapped sampled image has not yet
been isolated as an acceptance case.

Follow-up probe on 2026-09-24: a temporary diagnostic printed the mip choice
inside `VkHardwareTexture::CreateTexture` and the resource names read by
`VkMaterial::ObserveTextureReads`. The stable baseline scene sampled only
mipmapped textures. Repeating with the MAP06 status bar visible exercised more
material reads, but those creations were also all mipmapped. Thus these runtime
captures do not reach the `mipmap == false` branch or its explicit
`TRANSFER_DST_OPTIMAL` to `SHADER_READ_ONLY_OPTIMAL` barrier in
`vk_hwtexture.cpp`; source order shows the barrier after
`copyBufferToImage`, but dynamic validation of that exact branch remains open.
The temporary probe was removed. A purpose-built indexed texture draw or other
route that actually selects `CTF_Indexed` is needed before claiming closure.
An attempted temporary ZScript `RenderOverlay` draw of `TITLEPIC` with
`DTA_Indexed` did not emit its runtime sentinel or any indexed-upload/read
diagnostics, despite loading the package and rendering the map. It is not
evidence for the barrier path; the next probe needs a confirmed active draw
route before collecting Vulkan validation output.

## Observer overhead A/B — 2026-09-24

A temporary `r_framegraph_observe` control disabled the full active GL/Vulkan
observer path: FrameGraph records and backend-use observations, resource
registry touches, upload/read records, plus caller-side postprocess pass-name
resolution and `PassDesc` construction. The gate was not retained in the
renderer. Both arms used the same MAP06 route, 640x480 mode, frame cap, and
full postprocess settings (bloom, SSAO, exposure, tonemap, lens, FXAA); the
on-arm live graph reported 45 passes / 46 edges, while the off-arm reported
zero. Three interleaved launches per backend and arm produced two 8-second
`vid_frametrace` windows each. All captured MAP06 images reproduced byte for
byte within each backend and between observer on/off.

| Backend | Observer | Samples | Weighted avg | Median window p50 | Median window p95 | Median window p99 | Max | >33ms | >100ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| GL | off | 1,683 | 28.572 ms | 28.595 ms | 30.255 ms | 31.045 ms | 35.09 ms | 3 | 0 |
| GL | on | 1,683 | 28.572 ms | 28.680 ms | 30.065 ms | 30.330 ms | 35.27 ms | 2 | 0 |
| Vulkan | off | 1,683 | 28.570 ms | 28.590 ms | 30.220 ms | 30.380 ms | 33.24 ms | 0 | 0 |
| Vulkan | on | 1,682 | 28.572 ms | 28.665 ms | 30.230 ms | 30.530 ms | 38.47 ms | 1 | 0 |

The percentile columns are medians of the six per-window percentile reports;
they are not recomputed from raw frame samples. All means were between 28.570
and 28.572 ms, and the p95/p99 windows overlap. `vid_frametrace` p50 includes
the display wait and sits at this session's 35 fps pacing floor, so that value
is not renderer cost. This A/B found no measurable observer overhead on this
RX 550 under this route and configuration. It does not establish zero cost on
other scenes, GPUs, or Apple Silicon.

The first full-effects attempt used MAP01. Its live graph checks passed, but
Vulkan screenshots varied even between observer-identical runs, so those
timings were excluded from the conclusion. MAP06 then reproduced exactly and
was used for the reported A/B. MAP06 is a measurement route here, not evidence
that bloom visibly affects that map.

## Tranche disposition — closed 2026-09-24

The Linux framegraph/upload and observer-overhead tranche is closed at the
user's direction. The build, live GL/Vulkan framegraph/resource checks,
baseline/SSAO comparisons, sampled-upload observations, and interleaved
observer A/B are recorded above. This closure does **not** claim dynamic
validation of Vulkan's indexed non-mip sampled-image barrier: the attempted
test did not reach an active indexed draw. Keep that as an explicit follow-up
if a confirmed `DTF_Indexed` 2D draw route becomes available. The earlier
`demo1.lmp` timedemo command was not run because its input was absent; it is no
longer part of the mandatory gate.
