# Bounded sound precache decoding

The `snd_precache_async` setting overlaps one eligible Vorbis decode with
the next owner-thread cache operation. It uses the existing sound
preparation worker. Both `snd_asyncprepare` and `snd_precache_async` now
default on. The worker's platform gate enables the qualified macOS path.
Set `snd_precache_async 0` to disable precache overlap, or
`snd_asyncprepare 0` to disable all worker sound preparation.

CacheMarkedSounds retains its original ascending traversal. It looks ahead
to the next marked entry and queues it when it is a direct, unloaded,
canonical sound. Random headers, aliases, tentative entries and raw input
continue through the existing cache calls. This preserves the ability of a
cache call to mark later entries during traversal. The owner waits when a
prepared entry reaches its normal turn, then the existing loader resolves
loaded-lump sharing before it consumes the result. Unused preparations are
cancelled after that turn.

Resource lookup, decoder creation, engine allocation and decoder cleanup
run on the owner thread. The worker reads an independent decoder into its
preallocated PCM buffer. OpenAL publication stays on the owner in the
original cache order. Required buffers finish before CacheMarkedSounds
returns. Rejected, failed and oversized preparations use the serial loader.

Precache jobs have a 1 MiB PCM capacity. The existing 8 MiB background jobs
keep their original capacity. Reservations use each job's actual capacity
and share the existing 64 MiB budget. This allows a small precache job
alongside three ready thunder jobs. `snd_prepare_stats` now reports the
peak reservation, byte budget, and precache waits.

The optional `snd_retainedcache` remains the existing idle-retention policy.
Required precached sounds are resident OpenAL buffers. Temporary decode
storage belongs to the preparation budget.

## Correctness evidence

A temporary native probe compared every consumed worker result with a fresh
serial decode using memcmp and checked sample format, channel count, rate
and loop metadata. It also changed a byte in a comparison copy and verified
that the comparison detected it. The probes were removed before timing and
the final build.

The Blackwater control and worker launches produced identical ordered
manifests of PCM hashes and publication metadata during each precache pass.
Resident payload also matched exactly. At the end of the first MAP09
precache, both had 179 canonical buffers totaling 30,251,592 bytes. After
the warm reload both had 188 totaling 31,614,996 bytes. These are buffer
payload totals queried through OpenAL.

The native edge fixture exercised aliases, random groups, conflicting loop
definitions, raw audio, malformed input and a sound larger than the 1 MiB
worker capacity. The two arms had identical ordered PCM manifests and
resident payload. The oversized preparation failed and the serial fallback
provided the same output. The fixture's marked payload was 9,180,799 bytes
in 45 canonical buffers in both arms.

The detached worker harness, including the new wait operation, passed
AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer checks for
bounded output, cancellation and shutdown. The owner-allocation guard
detected a deliberately forbidden worker allocation. All three thunder
PCM outputs matched the previously qualified reference.

## Measurement contract

The prediction was at least 20 first-visit worker hits, an existing 64 MiB
reservation bound, exact output and metadata, and at least 100 ms off the
median longest first-visit Update interval. The Blackwater qualification
prepared 15 sounds during the first MAP09 visit. This misses the coverage
prediction. Its 22 native serial comparisons include seven earlier staging
map preparations. Its peak reservation was 55,982,791 bytes, about 2.14 MiB
above the control's ready thunder jobs. The loop/alias fixture's peak was
57,053,898 bytes.

Native Blackwater PCM controls are in
`/private/tmp/gzdoom-sound-precache-cwolewg5`. The initial edge fixture used
incorrect resource references. The corrected fixture and its controls are
in `/private/tmp/gzdoom-sound-precache-gw2y_om7`. Test generators, detached
harnesses, sanitizer logs and the final build log are in
`/private/tmp/gzdoom-sound-precache-worker`.

## Final timing

The final binary ran six fresh Metal processes in disabled, enabled,
enabled, disabled, disabled, enabled order. Each loaded the isolated MAP09
save from KEYPROBE, then performed one warm reload. Both audio arms used
`snd_asyncprepare 1`, `snd_retainedcache 2048`, and the current Metal
defaults. PCM verification and per-buffer probe output were absent.

| Precache worker | Longest first-visit Update gaps | Median | Warm median |
| --- | --- | ---: | ---: |
| Disabled | 2,328.77 / 2,330.10 / 2,327.17 ms | 2,328.77 ms | 500.26 ms |
| Enabled | 2,270.28 / 2,278.15 / 2,216.21 ms | 2,270.28 ms | 497.29 ms |

The first-visit median decreased by 58.49 ms, about 2.5%. The slowest enabled
run was 49.02 ms faster than the fastest control. The 100 ms prediction was
missed. Warm ranges overlap, so the warm comparison establishes no measured
improvement. Every enabled first visit consumed 15 prepared sounds, with
zero failures or not-ready fallbacks. The 106–114 ms post-load intervals
still appeared in both arms.

A separate OpenGL disabled/enabled pair recorded first-visit gaps of
2,252.17 and 2,190.54 ms. The enabled run also consumed 15 prepared sounds
with zero failures or not-ready fallbacks. This is one pair on macOS.
Native Linux and Windows decoder qualification remains pending.

Metal timing artifacts are in
`/private/tmp/gzdoom-sound-precache-3fhsi8r7`. The OpenGL pair is in
`/private/tmp/gzdoom-sound-precache-1zu7o_2h`. The final Intel macOS build,
app package comparison and whitespace check passed.
The OpenGL/Metal MAP06 baseline and SSAO image comparisons passed with
both sound preparation settings enabled.

The default update rebuilt the Intel macOS app and repeated one Blackwater
control/default pair. The default launch supplied neither sound switch on
the command line. It consumed 15 prepared sounds with zero failures and
not-ready fallbacks, staying at the same 55,982,791-byte peak reservation.
The explicit `snd_precache_async 0` control consumed zero preparations
during precache. Its first-visit gap was 2,340.15 ms, compared with
2,280.56 ms for the default. This confirms default selection and repeats
the direction of the earlier six-run result. Arguments and results are
in `/private/tmp/gzdoom-sound-precache-b1z1alsg`.
