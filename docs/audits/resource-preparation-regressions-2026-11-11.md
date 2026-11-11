# Durable resource preparation regression controls — 2026-11-11

The previous bounded worker, retention and upload-batching acceptance controls
used temporary scripts and probes. The repository now has
[reusable native controls](../../tools/regression/README.md), synthetic audio
assets and generated texture/map fixtures. A licensed caller-owned DOOM2 IWAD
is still required. No Ashes assets or saves are committed.

## Prediction and Intel Metal results

Before acceptance: zero pixel differences across five cases; six exact PCM
comparisons and six detected corruptions per audio self-test; two ready PNG
workers versus zero with the feature disabled; 12,000 retained bytes versus
zero with retention off; fewer extra command buffers for the same uploads.

The serial, shipping-default, single-upload, no-retention and repeated-default
runs passed. All five captures have identical decoded pixels (1152 × 720 RGB,
SHA-256 b3c688bb35ed4badac0084e87a62d39dfec9e56d879133ca31895d78273d4893).
Both default audio tests report six exact comparisons, zero failures and six
detected one-byte corruptions. The first two map precaches have identical
ordered PCM hashes, format, normalized loops and resident buffer payload.

The large Ogg exceeds the lookahead's 1 MiB PCM cap: one failed preparation is
collected and the serial fallback produces the same resident sound. Enabled
map precaching consumes two worker results, with no not-ready fallback, no
remaining jobs and zero reserved bytes at the endpoint. Default self-test
runs additionally consume six test results; those are not counted as workload
hits. Peak reservation is 2,138,399 bytes, below 64 MiB. Malformed input is
rejected consistently without changing cache output.

Metal reports two prepared PNGs, zero worker failures and zero outstanding
reserved bytes. The paletted image takes the serial fallback. The same four
texture uploads use two extra command buffers with batching and five without
it. This includes synchronous work outside the batch; it is not a claim of
one command buffer for the whole frame. The diagnostic is enabled only in
correctness runs, so these are execution/correctness results, not timings.

The played 0.25-second tone retains 12,000 bytes after the return to the
unmarked staging map with retention enabled, versus zero when disabled.
Sound reset and clean exit complete. Two complete five-case Metal suites
passed; the second added live upload-count qualification. Artifacts:
- /private/tmp/gzdoom-regression-native-3
- /private/tmp/gzdoom-regression-native-4

The final isolated suites also pass on both Metal and OpenGL, with explicit
-noautoexec and -noautoload. All five captures are pixel-identical within
each backend. The GL decoded-pixel SHA-256 is
00da5d735cf0f78a24b6e0d1163b5c4878fb441074ee4c17c817beb8b99f8b09.
This is a same-backend feature control, not a claim that GL and Metal render
byte-identical images. Final artifacts:
- /private/tmp/gzdoom-regression-final-metal
- /private/tmp/gzdoom-regression-final-gl

The seven regression parser/fixture tests and 13 profiling parser tests pass.
The runner rejects enabled-worker output with missing budget fields; the
explicit lazy "no worker" state is valid before any job has been created.

## Native diagnostics

The opt-in snd_precache_audit switch hashes decoded PCM after owner publication
and records resident buffer payload at precache boundaries. Default is off.
The normal serial decode loop is shared with the self-test so it compares
the actual runtime paths. FSoundPreparationState::Wait is also shared with
the production renderer call.

The snd_preparation_selftest command uses private worker state for cancellation,
undersized output and shutdown cases, then the live renderer for admission,
publication and OpenAL metadata. It checks three default loop configurations
per sound. Decoder lifetime and TArray allocation remain on the owner thread.
The normal VM ABI and simulation order are untouched.

The portable benchmark records caller assets/settings in each run's manifest,
retains binary/config/save/mod hashes, separates first and warm loads, and uses
three interleaved repetitions per arm. It rejects missing timing windows,
marker duplication, wrong maps and backend fallback. No generic performance
threshold is added. The measured interval includes loading and blocked time.

## Portable Blackwater timing qualification

The final runner used the caller's MAP09 auto-save, exact mod load order and a
private copied configuration, with autoexec/autoload disabled. Prediction:
15 first-visit worker hits, peak at most 64 MiB, and a 50 ms reduction in the
median first gap. Six interleaved runs completed:

| Precache worker | First median | First range | Warm median | Warm range |
| --- | ---: | ---: | ---: | ---: |
| Disabled | 2,325.58 ms | 2,320.60–2,338.99 ms | 494.73 ms | 488.80–497.71 ms |
| Enabled | 2,261.69 ms | 2,261.35–2,282.23 ms | 493.62 ms | 493.24–498.32 ms |

The median first gap decreases by 63.89 ms (2.7%); the closest arm separation
is 38.37 ms, larger than either arm's internal range. Every enabled first
load consumes 15 preparations with zero failures/not-ready fallbacks. Peak is
55,982,791 bytes, below 64 MiB. Warm ranges overlap; no warm improvement is
established. Post-load intervals around 105–112 ms remain.

An earlier six-run qualification of the same runner recorded first medians
2,337.68 ms disabled and 2,277.28 ms enabled, but a fast 2,201.12 ms control
caused overlap. Preserve that result rather than discard it: absolute timing
drifts between runs. The final result repeats the direction of the previous
sound-precache audit, not a universal fixed saving.

Artifacts and hashes:
- /private/tmp/gzdoom-regression-final-timing
- /private/tmp/gzdoom-regression-timing-1 (earlier overlapping run)

## Limits

These controls cover bounded synthetic cases. They do not establish all mod
sound definitions, large-scale random-group behavior, long sessions, combat
replay or a cold filesystem. Self-tests are diagnostics rather than a
replacement for sanitizer qualification. Linux, Windows and Apple Silicon
runtime qualification remains machine-specific and pending.

The next optimization should address the remaining measured post-load sound
interval or a moving/combat hardware-scene profile. Concurrent actor updates
and VM threading remain unsupported by the current evidence.
