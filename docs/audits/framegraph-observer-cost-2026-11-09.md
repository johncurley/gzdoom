# Scene-material observer cost — 2026-11-09

Scope: reduce repeated CPU dependency observation while retaining graph-driven
rendering, upload-order accounting, and all material dependencies. No threading,
resource-ID migration, render-pass removal, or simulation change.

## Controls and predictions

The Blackwater profiles attribute 22.6–28.1% of sampled main-thread weight to
`ObserveSceneMaterialRead`, including nested upload-read accounting. That share
is not a frame-time saving. A temporary switch bypassed only the dependency
observation after `ObserveResourceRead`, retaining per-binding upload tracking.
Prediction before capture: bypass lowers median frame interval by at least 1 ms.

The first paused-save series stayed near 28.6 ms with either setting. Source
inspection identifies `TryRunTics`' no-interpolation tick pacing, which remains
active when paused even with `cl_capfps 0`. This series cannot qualify the
observer's CPU cost. Its named screenshot argument also overwrote one scratch
image; only the last image survives, and it is not a parity comparison.

The corrected series explicitly uses `vid_dontdowait 1`, `vid_maxfps 500`,
`vid_vsync 0`, and separate absolute screenshot paths. The same copied MAP09
save is paused after five in-level frames. Settings and arguments are retained
per run. No profiler or developer logging is active. Results below are weighted
means of complete two-second `vid_frametrace` windows after frame 100:

| Order | Material dependency observation | Mean interval |
|---|---|---:|
| 1 | enabled | 17.269 ms |
| 2 | bypassed | 14.479 ms |
| 3 | bypassed | 14.831 ms |
| 4 | enabled | 14.488 ms |

All four 1024×768 screenshots have identical decoded pixels. The two bypassed
runs differ by 0.352 ms, but the enabled repeats differ by 2.781 ms, and the
last enabled run changes substantially within the run and contains a 217.66 ms
outlier. The >=1 ms median prediction is **not qualified**. Presentation,
scheduling and clock-state effects are not isolated by this test. Do not claim
a gameplay speedup or subtract the first two rows and report it as an effect.

Artifacts:
`/private/tmp/gzdoom-blackwater.Y5o1Yg/scene-observer-control-bedz8bb5` (paced),
`/private/tmp/gzdoom-blackwater.Y5o1Yg/scene-observer-unpaced-oyyqvu6z` (corrected).
The corrected directory retains the control executable and instrumented source.
Control executable SHA-256:
`edbc8bbaf9d9bdc87fdb5c3c83165971f701aaf06eab6fd7ceeab4853377beab`.

## Bounded candidate

Cache completed observations by owned material-name contents within a backend
pass scope, using the existing `TMap`/`FString` containers. A repeat still calls
`ObserveResourceRead` before returning, so a new upload between repeated binds
is observed. The first read retains the original writer search, imported-input
classification, logical read, sampled use, and backend observation. Reset the
cache at frame reset and every `BeginBackendPass`; invalidate it when a new
alias can change canonical identities. Pass descriptions are immutable through
the public API after insertion, except for this observer's additive records.

The retained self-test checks repeated equal-content names, produced and
imported materials, separate pass scopes, pass re-entry, upload between reads,
and frame reset. Existing camera/canvas, alias, hazard, upload and negative
dependency controls remain applicable.

For CPU qualification, a temporary in-engine probe alternates original and
cached paths three times against 256 material names, 128 produced by an earlier
pass, and 100 repeated binding sweeps (25,600 calls per arm). Timing covers the
observer calls only. Build and full graph-dump comparison occur outside timing.
Prediction before running: at least 50% less observer CPU time, successful graph
builds, and byte-identical graph reports. This is a synthetic CPU measurement,
not a Blackwater frame-time prediction. Temporary bypass/cache switches and
probe commands must be removed before the final build.

## Qualification results

The isolated prediction passes. Each row is a fresh engine process, with three
alternating measurements per path inside that process:

| Process | Original median | Cached median | Observer CPU reduction |
|---|---:|---:|---:|
| Metal, original live observer | 151.995 ms | 8.553 ms | 94.37% |
| Metal, cached live observer | 154.828 ms | 8.700 ms | 94.38% |
| OpenGL, cached live observer | 178.816 ms | 9.555 ms | 94.66% |

All 18 probe builds succeed and all 18 full graph dumps match their process's
original-path reference exactly. The original path is the disabled-fix control,
not an observer bypass. Original ranges are 150.602–224.306 ms; cached ranges
are 8.488–15.033 ms. Even the slowest cached measurement is much faster than the
fastest original measurement. This qualifies the repeated-binding CPU saving;
it does not qualify a frame-time percentage or hitch removal.

Existing and extended graph self-tests pass with the cache disabled on Intel
Metal, enabled on Intel Metal, and enabled on OpenGL. Separate Blackwater Metal
launches differed in 2,455 pixels within a small weapon-sprite region, and in
generated texture identities in their graph reports. Those launches therefore
do not establish exact live parity. A subsequent within-process control holds
the paused world fixed, suppresses notification text, captures the original
path, enables the cache, and captures again: **zero differing decoded pixels**
at 1024×768 and **identical complete live graph dumps** (386,257 characters
each). The frame graph remains enabled throughout; upload tracking remains
per binding. No permanent observer-disable switch is introduced.

Probe/source/binary artifacts and per-arm manifests:
`/private/tmp/gzdoom-blackwater.Y5o1Yg/scene-observer-qualification-u7ekry0u`.
Within-process parity artifacts:
`/private/tmp/gzdoom-blackwater.Y5o1Yg/scene-observer-qualification-t1rlcu6u`.
Temporary switches, clock measurements and benchmark commands were removed
before the final build. Only the scoped cache and regression checks remain.

Remaining performance qualification: matched active gameplay with a repeatable
route and CPU samples, including observer allocation/hash costs and first-use
material cost. First observations still execute the original searches. This
change does not address synchronous audio preparation or promise to remove the
doorway hitch.

## Final verification

- Mandatory `cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"`: PASS.
  Known libvpx deployment-target and launcher-overload warnings remain.
- Final graph self-tests: PASS on Intel Metal and OpenGL, including the upload
  assertion strengthened to run before a later pass could mask a skipped read.
- `tools/matrix/run.py --scene doom2 --keep`: ten available configurations;
  all eight effect/identity relations PASS. Optional custom-PP fixtures absent.
  Stored signatures match except SSAO, so the command exits FAIL. The archived
  pre-cache executable reproduces the same SSAO signature (`32bd4cb8` prefix)
  and the same stored-reference failure. Original versus cached baseline and
  SSAO decoded images are both identical. This is a pre-existing reference
  mismatch, not a cache-induced output change; no baseline was updated.
- `tools/matrix/crossbackend.py --scene doom2 --only baseline,ssao --keep`:
  baseline and SSAO both OK under the existing GL/Metal comparison criteria.
  Each final launch also reports graph self-test PASS.
- `git diff --check`: PASS. No temporary control CVAR or benchmark command
  remains in source. No gameplay replay/determinism qualification is claimed.

The matrix used a temporary launcher to pin native 800×600 startup size and
write the selected backend into the matrix-owned configuration before launch;
actual GL/Metal initialization is confirmed in retained logs. Artifacts,
including original-executable comparison images, are retained under the probe
directory's `final-matrix` subdirectory; final GL/Metal logs and images are in
its `final-crossbackend` subdirectory. Vulkan, Windows and Apple Silicon were
not runtime-tested here.
