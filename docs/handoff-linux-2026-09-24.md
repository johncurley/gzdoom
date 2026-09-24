# Handoff to Linux — Vulkan framegraph and upload validation — 2026-09-24

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
- `cmake --build build --parallel 4` passed on macOS. The repository's
  prescribed `./build/gzdoom -timedemo demo1.lmp -nosound -nogui` gate was not
  runnable there: this build exposes the executable inside the app bundle, and
  `demo1.lmp` is absent from the checkout. Linux should run its normal build and
  timedemo gates with the local demo/IWAD setup.
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
