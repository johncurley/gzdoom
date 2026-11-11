# Resource preparation regression controls

Run from the repository root with a built engine. Python's standard library is
sufficient; the synthetic Ogg assets are committed, and no mod or IWAD is
redistributed. A working desktop, OpenAL device and caller-supplied DOOM2 IWAD
are required. These are real rendered frames, not a headless startup smoke test.

~~~sh
python3 -m unittest discover -s tools/regression -p 'test_*.py'
python3 -m unittest discover -s tools/profiling -p 'test_*.py'
python3 -u tools/regression/run.py \
  --binary build/gzdoom.app/Contents/MacOS/gzdoom \
  --iwad "/path/to/DOOM2.wad" --backend metal
~~~

Use a native executable path on Linux/Windows. Choose --backend gl or
--backend vulkan there and pass --allow-audio-skip while the platform gate
disables audio preparation. The summary records the skip; it is not audio
worker qualification. On macOS both Metal and GL can exercise the same worker.
Each process has a timeout. A failed run returns nonzero and retains logs.

The five sequential cases are serial controls (all three resource preparation
features disabled), shipping worker defaults, defaults with one upload per
command buffer, defaults with retention disabled, and an identical default
repeat. Retention is explicitly 2048 KiB except in its disabled control; the
engine's shipping retention default remains zero. Own config and save
directories prevent edits to user settings. External autoloads and autoexec files are disabled.

Acceptance predictions are zero pixel differences between all five captures;
six exact PCM comparisons and six detected corruptions per audio self-test;
two ready worker PNGs on Metal versus zero with PNG workers disabled;
identical ordered buffer hashes, format, normalized loops and resident payload
during the first two map precaches; and a retained 12,000-byte played tone
versus zero with retention disabled. The PNGs exercise all five filters,
RGB/RGBA, non-power-of-two dimensions and a paletted fallback. Audio exercises
aliases, random groups, distinct source lumps, a too-large decode and malformed
Ogg input. The two large/small cases must preserve the serial cache result.

The r_texture_snapshot_selftest command checks exact pixels/metadata,
translations and a malformed PNG stream. The snd_preparation_selftest command
checks real serial versus worker decodes, loop metadata, exact-capacity EOF,
too-small output failure, zero-capacity rejection, owner publication,
cancellation, reservation cleanup and shutdown with queued jobs. It creates
no changes to actor ordering or VM execution. Sound reset is also exercised
during the map sequence. The input_axes_selftest command compares the actual
axis mapping against the original full-key-list oracle across 1,984 generated
cases, checking digital fields and deliberately corrupted analog output.
It restores bindings on exit; it does not test physical controller polling.
Audio bytes are hashed only with the opt-in
snd_precache_audit diagnostic, which defaults off and must remain off for
performance measurements.

Metal debug upload counters prove that the same fixture uploads use fewer
command buffers with batching enabled. Debugging is on only during this
correctness test, so its duration is not a performance result.

Outputs default to a temporary directory printed at startup. --output accepts
a **new** directory. Preserve manifest.json, summary.json, each argv.json,
logs and PNGs as the per-machine result. Manifests include binary, IWAD,
generated fixture hashes, backend, platform, commit and working tree state.
No machine-independent timing threshold is applied.

## Repeatable save-load timings

The benchmark stages an empty map, then loads a supplied save twice, keeping
first visits and warm reloads separate. It is a load benchmark; it does not
replay combat or the user's doorway route. No native demo is assumed.
Asset and config versions must match the save.

~~~sh
python3 -u tools/regression/benchmark.py \
  --binary /path/to/gzdoom --iwad /path/to/DOOM2.wad \
  --file /path/to/mod.pk3 --save /path/to/start.zds \
  --map MAP09 --config /path/to/benchmark.ini --backend metal \
  --control sound-precache \
  --prediction "15 first-visit worker hits, peak <=64 MiB, median gap decreases 50 ms"
~~~

Repeat --file in the exact load order. Controls are sound-precache, png
and texture-batching (the latter two require Metal). Interleaved order is
disabled/enabled/enabled/disabled/disabled/enabled, three repetitions per arm.
Missing/duplicate markers, an unexpected map, absent frame timing windows,
worker budget violations, backend fallback, timeout or failed engine exit fail
the run. For sound precache, enabled runs must consume worker results.
Archives/settings remain caller-owned; every run copies the save and config.

The reported gap is the maximum start-to-start Update interval from
vid_frametrace windows, including the load; it is elapsed wall time, not
exclusive CPU execution. Report ranges and median alongside the predeclared
prediction. Verify changes exceed the repeat spread. Do not compare results
with different hashes, display settings or thermal conditions as a controlled
A/B. These runs do not purge OS caches: a new process first visit is not proof
of cold filesystem state. Instrumented traces and gameplay remain separate
qualification steps.

Apple Silicon, Linux and Windows worker/runtime qualification must be recorded
on their actual machines. An Intel macOS pass cannot close those tasks.
