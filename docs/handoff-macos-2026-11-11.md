# macOS handoff — resource preparation and smoothness — 2026-11-11

## Start here

The source baseline is commit **c809931cb5** on
[engine-performance-regressions](https://github.com/johncurley/gzdoom/tree/engine-performance-regressions),
pushed to origin and verified. The committed checkout builds with a clean
working tree. Read AGENTS.md, CONTRIBUTING.md and the
[regression qualification](audits/resource-preparation-regressions-2026-11-11.md).
This handoff sets the next CPU/resource investigation priority; the older
[Metal correctness handoff](handoff-macos-2026-10-04.md) remains historical
evidence for camera, shadow and rendering controls.

Runtime qualification is Intel MacBookAir7,2, macOS Monterey 12.7.6,
i5-5350U, Intel HD Graphics 6000, Metal and OpenGL. This build has
HAVE_VULKAN=OFF. Apple Silicon, native Linux/Windows decoder behavior and
long-session acceptance remain open. A macOS compile is not Apple Silicon
runtime qualification.

## What is enabled and who owns the work

| Control | Shipping default | Boundary |
| --- | --- | --- |
| snd_asyncprepare | 1 | macOS-gated Vorbis preparation; owner opens/closes decoders and publishes OpenAL buffers |
| snd_precache_async | 1 | One eligible canonical lookahead, 1 MiB PCM cap, ordered owner consumption |
| mt_async_png | 1 | Metal RGB/RGBA PNG preparation; owned snapshots, 16 MiB reservation budget, serial fallback |
| mt_batch_texture_uploads | 1 | Metal completed uploads batched up to 64; readiness published after submission |
| snd_retainedcache | 0 KiB | Optional idle canonical buffers; active/required buffers are outside this payload budget |
| snd_precache_audit | 0 | Expensive opt-in PCM publication hashes; correctness diagnostics only |

Background sound preparation and precache lookahead share a 64 MiB pool and
four-job ceiling. The existing background candidate uses an 8 MiB PCM extent;
lookahead uses 1 MiB. Oversized/rejected/late results fall back to the serial
loader. These preparation workers do not perform archive/name-table/OpenAL
operations or actor/VM updates. Preserve tick ordering, loop/alias semantics, cancellation and reset.

The frame graph remains dependency/readiness evidence. It does not schedule
simulation jobs or make live world pointers into independent snapshots.

## Completed measurements

- Five native modes pass on each of Metal and GL: serial, defaults,
  single-upload, no-retention and default repeat. Pixels match exactly
  within each backend; ordered PCM hashes/format/loops and resident payload
  match. Audio self-tests each report six exact comparisons and six detected
  corruptions. Overflow fallback, cancellation, reset and budgets pass.
- The same four fixture uploads use two extra command buffers with batching
  versus five without. The real Blackwater burst previously used 1,966 versus
  40 extra command buffers for 1,965 uploads; its median first gap decreased
  from about 2.73 to 2.37 seconds.
- The final six interleaved Blackwater sound-precache runs measured first-gap
  medians of 2,325.58 ms disabled and 2,261.69 ms enabled: 63.89 ms shorter.
  Enabled first loads consume 15 worker results, peak 55,982,791 bytes,
  zero failures/not-ready fallbacks. Warm ranges overlap; no warm gain
  established. An earlier six-run comparison also has overlapping first
  ranges; retain that noise evidence.
- Seven regression tests plus 13 profiling tests pass. MAP06 baseline/SSAO
  GL-versus-Metal comparisons pass. This does not claim the entire golden
  image matrix passed.

The synthetic native suite explicitly sets retention to 2048 KiB except its
disabled arm. The final Blackwater timing runner supplies no retention
override: it uses the shipping zero default. Do not attribute those timings
to 2 MiB retention. Earlier retention and frame-start audits use different
explicit settings; keep those experiments separate.

## Reproduce the native controls

Run from a clean checkout of the branch. Use caller-owned DOOM2; assets and
saves are not redistributed. Build and verify the adjacent package:

~~~sh
cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"
cmp build/gzdoom.pk3 build/gzdoom.app/Contents/MacOS/gzdoom.pk3
python3 -m unittest discover -s tools/regression -p 'test_*.py'
python3 -m unittest discover -s tools/profiling -p 'test_*.py'

gzdoom_bin="$PWD/build/gzdoom.app/Contents/MacOS/gzdoom"
doom2_iwad="/absolute/path/to/DOOM2.wad"
python3 -u tools/regression/run.py --binary "$gzdoom_bin" \
  --iwad "$doom2_iwad" --backend metal
python3 -u tools/regression/run.py --binary "$gzdoom_bin" \
  --iwad "$doom2_iwad" --backend gl
~~~

The runner disables external autoexec/autoload and owns its config/save
directories. It prints retained artifact paths; --output requires a new
directory. Before running, predict zero pixel/PCM differences, two ready
Metal PNGs versus zero in the serial control, six audio comparisons per
self-test, and 12,000 retained bytes versus zero. Require actual reduced
upload command-buffer counts. See [runner details](../tools/regression/README.md).
Do not weaken a failed control or update image baselines to accept a mismatch.

## Reproduce the load benchmark

Freeze the original MAP09 auto-save and config before more gameplay can
overwrite them. This workload stages REGSTART, then loads that save twice
per process; it does not replay the mall-doorway route or combat. Repeat
--file in the original mod order. These variables are examples to replace:

~~~sh
bw_mod="/absolute/path/to/AshesBlackwater.pk3"
bw_save="/absolute/path/to/frozen-MAP09.zds"
bw_config="/absolute/path/to/frozen-metal.ini"
python3 -u tools/regression/benchmark.py --binary "$gzdoom_bin" \
  --iwad "$doom2_iwad" --file "$bw_mod" --save "$bw_save" \
  --map MAP09 --config "$bw_config" --backend metal \
  --control sound-precache \
  --prediction "15 first-visit hits; peak <=64 MiB; median first gap decreases 50 ms"
~~~

The runner performs three repetitions per arm in off/on/on/off/off/on order.
It requests 1024 × 768/windowed, cap 35, and preserves the supplied config's
vsync/render settings; the reference config has vsync enabled. Keep first
visits and warm reloads separate. New processes do not prove cold OS caches.
Preserve settings, manifests, argv, binary/package hashes and raw logs.
Use elapsed Update intervals as end-to-end gaps, not exclusive CPU time.

Reference input SHA-256 identities:

| Input | SHA-256 |
| --- | --- |
| DOOM2.wad | 10d67824b11025ddd9198e8cfc87ca335ee6e2d3e63af4180fa9b8a471893255 |
| AshesBlackwater.pk3 | af078fab322be816dfc79d50f3ca487f4de27a12384791799be45995add3b2bb |
| MAP09 auto00.zds | 800d4d07e90b5da72acdc0dd635cf670e24f6df76212386ec5ae00c91ab48003 |
| Source metal.ini | 7864f505a2b8a057d7e9b89c3ff82456e6e6a4feae67f60408c3181c896f8607 |

## Next work, in order

1. Reattribute the remaining 105–112 ms post-load interval on the current
   baseline. Capture actual tick counts, main-thread stacks and thread states,
   plus filesystem activity where available. Earlier profiles found cumulative
   synchronous sound loads; they do not identify the exact current assets.
   Distinguish required loading, catch-up ticks, decode work and waits before
   choosing one bounded change. Compare explicit retention 0/2048 separately
   if investigating repeat loads; the load runner has no retention sweep.
2. Preserve an uninstrumented timing arm. Use existing vid_profilemarks,
   vid_frametrace, vid_stalltrace and sound statistics first; keep PCM audit
   and Metal debug logging off in performance arms. Follow the
   [profiling runbook](../tools/profiling/README.md): launch the exact app
   executable, attach Instruments to its PID, verify UUID, capture range and
   event loss. Earlier xctrace --launch resolved a different installed app.
3. After attribution, make one evidence-backed change with disabled/negative
   controls and repeat the relevant native suites. The prior
   [sound attribution](audits/postload-sound-attribution-2026-11-11.md) and
   [frame-start profile](audits/metal-frame-start-attribution-2026-11-11.md)
   explain historical constraints, not a new implementation contract.
4. Then profile moving/combat hwrenderer work: BSP traversal, clipping, scene
   setup, sorting, material preparation and worker waits. Separate simulation,
   render preparation, backend submission and presentation. Existing samples
   do not justify concurrent actor ticks, VM threading or assuming GPU/driver
   waits are absent.
5. On Apple Silicon, build natively, repeat the Metal controls and capture the
   same workload with per-machine baselines. Check decoded output, loops,
   worker admission, budgets, reset and upload ordering before tuning.
   Intel timings do not qualify ARM64, unified-memory behavior or TBDR policy.

## Retained evidence and caveats

Local artifacts are temporary, may disappear and are not prerequisites for the
committed runners. Preserve externally before cleanup:

- /private/tmp/gzdoom-regression-final-metal and -final-gl: five-case suites.
- /private/tmp/gzdoom-regression-final-timing: final six-run load benchmark.
- /private/tmp/gzdoom-regression-timing-1: earlier overlapping timing result.
- /private/tmp/gzdoom-frame-start-20261111-144851: earlier Instruments traces,
  matching executable, symbols and source snapshots.

The measurements predate the commit: their manifests identify caacc40847 plus
working-tree changes. Use the archived executable hash/UUID, not just git HEAD,
to interpret those captures. The final timing executable SHA-256 is
81c0e715aa492339a02506a02f57336794fdfa9ad91f1bb6338d0b518f20b548.
The clean committed build was verified afterward; a rebuilt binary is not a
symbolication substitute for an archived one.

No native deterministic Blackwater demo is established. User-confirmed smooth
doorway/combat runs, load timing, generated fixtures and Instruments attribution
are distinct evidence. Preserve legacy simulation/VM semantics; choose the
next change for smooth gameplay from measured critical-path cost.


## Post-load continuation — 2026-11-12

The [post-load/input audit](audits/postload-input-axes-2026-11-12.md) now
reattributes the baseline on matching CPU/scheduler/filesystem timelines:
six resumed intervals are predominantly running, with 43–61 ms of sampled
OpenAL loading and a separate 8–15 ms in repeated axis binding scans. Most
file-call time is below 0.4 ms; one first resume includes a longer file read.
Tick catch-up is measured, not inferred from the enclosing playsim label.

A direct per-axis binding lookup removes the full-table scans without a
cache or new worker. Its legacy-oracle input test passes 1,984 comparisons.
Matching candidate traces have zero axis lookup samples. Three unprofiled
runs per arm show roughly 10 ms lower median first resumed maxima and 6 ms
lower warm maxima; retain the audit's ranges and small-sample limits.

Temporary global precache hints also remove deferred decoded-buffer
publications in nine load windows; disabled controls reproduce 9–10 each.
Those hints preload the staging map and are not an engine fix or a cold-load
speedup measurement. Retention remains default zero. Next investigate generic
sound discovery/retention coverage, preserving decoder/OpenAL ownership and
simulation ordering. Physical controller and native Linux/Windows validation
of the shared input path remain open.


## Optional random-sound sibling lookahead — 2026-11-12

Continue with the [sibling trial audit](audits/sound-sibling-preparation-2026-11-12.md)
and expanded `tools/regression/README.md`. Following explicit maintainer
approval on 2026-11-12, `snd_asyncsiblings` defaults on;
`snd_asyncprepare` and `snd_precache_async` retain their existing defaults.
One unselected eligible sibling can use the existing decode pool after normal
sound selection. Bounds are one acquisition attempt, 128 KiB encoded payload,
1 MiB PCM extent, 64 graph nodes and the unchanged four-job/64 MiB pool.
Resource acquisition and OpenAL publication stay on the owner thread.

Do not change the normal sound RNG to satisfy tests. Ordinary random groups
reach the base class's C `rand()`, not the separately named derived
`RandSound` method. The paired diagnostic requires the external macOS RNG
shim built by the regression runner, checks eight choices/draws plus ordered
PCM outputs, and detects an extra draw. Independent cold readiness runs and
Blackwater timings do not load the shim. Failed earlier validation artifacts
are retained in the audit. Wider gameplay, Apple Silicon and Linux/Windows
qualification remain open despite the approved default enablement.

Intel Metal and OpenGL each pass all five resource modes and four cold playback
controls, including corrected paired RNG and ordered PCM/loop checks. Six final
Metal Blackwater save-load runs measure a first resumed maximum median of
103.71 ms off versus 97.99 ms on; first useful worker hits rise from 15 to
17–18, with zero late/failure results and unchanged 55,982,791-byte peak.
Warm ranges overlap. The disabled first-window range is 103.58–118.93 ms,
enabled 96.76–98.25 ms; retain that spread when assessing the 5.72 ms median
effect. This is a window maximum around loading, not a combat replay result.
Enabled-default Intel Metal manual acceptance now passes scoped doorway,
combat/audio observations and three minutes of varied sustained play. Doorway:
1,340 intervals, max 56.93 ms, zero >100 ms; sustained play: 6,284 intervals,
max 53.64 ms, zero >100 ms, operator confirms normal sound and no deaths/menu
use or perceived frame lag. All use cap35 and complete timing windows; these
are acceptance runs without disabled-fix comparisons. The mixed combat/death/
reload run retains one 623.83 ms reload-adjacent interval. Audio counters add
no late/failure results or waits in any section. See the audit for evidence.
Next: longer soak, live sound reset/reload, matched GL gameplay and separate
reload attribution. Use `snd_asyncsiblings 0` for matched controls.


## Shared reload attribution — 2026-11-12

See [the reload audit](audits/blackwater-reload-attribution-2026-11-12.md).
The 80-second manual death trace was rejected after the runner's saving timeout;
it cannot attribute the original 623.83 ms interval. A 25.403685-second recovery
trace with two automated warm loads is usable with explicit quality/focus limits.
Reloads spend 753/528 ms running in 875/590 ms gaps. UDMF parsing accounts for
161/139 ms of inclusive sample weight, with 58/57 ms of memory-move leaf samples
under that ancestry. Map construction, world restore and resource preparation
are separate candidates; rows overlap. Post-load catch-up has 28/19 ticks with
38/36 ms of synchronous decode samples. This is not ordinary actor/VM CPU cost.

Next bounded investigation: UDMF array allocation/copy, then content-identified
map acquisition/checksum reuse and remaining sound misses. Do not move the
current parser or level setup onto a worker wholesale. No engine code changed.
The test game has exited. Before another System Trace run, follow the audit's
short capture, target-exit/finalization, PID/UUID, table coverage and active=0
caveats; actual foreground normal-death/backend repeats remain open.
