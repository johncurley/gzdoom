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
