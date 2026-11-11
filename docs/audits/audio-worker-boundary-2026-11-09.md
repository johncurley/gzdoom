# Audio decode worker: boundary and first qualification

Status: installed-library handoff prototype measured on Intel macOS. The
[first engine integration](audio-worker-integration-2026-11-09.md) now exists,
default-off; the measurements below describe the preceding prototype. The existing
streaming thread does not perform the first-use effect decoding implicated in
the Blackwater long frames.

## Ownership boundary

Moving the entire `SoundEngine::LoadSound` onto a worker is unsafe: it resolves
aliases, reads archive resources and mutates the sound table. The OpenAL loader
combines loop parsing, decoder creation, decoding and backend buffer creation.

This build uses `/usr/local/lib/libzmusic.1.3.0.dylib`, not the vendored library.
Vendored source also identifies as 1.3.0; matching version numbers do not prove
identical builds. Relevant findings:

- `source/decoder/sndfile_decoder.cpp` in ZMusic has unsynchronized lazy
  library-loader state in `IsSndFilePresent`. `mpg123_decoder.cpp` has further
  shared initialization state, including `inited`.
- `CreateDecoder(..., true)` borrows encoded bytes via `MemoryReader`; those
  bytes must remain alive through decoder close.
- Installed libsndfile reports version 1.2.2. Its [tagged source](https://github.com/libsndfile/libsndfile/blob/1.2.2/src/sndfile.c)
  retains shared open/error state; the [upstream discussion](https://github.com/libsndfile/libsndfile/issues/279)
  distinguishes this exception from independent handles. The valid-handle
  `sf_readf_float` path uses that handle's state. This supports testing an
  exclusive handoff, not assuming every decoder API is thread-safe.

Initial integration boundary:

1. Owner: resolve sound/variant, acquire owned bytes, parse loop tags, create
   decoder and query metadata. Keep factory work serialized with existing
   owner-side sound/music creation paths.
2. One persistent preparation worker: exclusively read that decoder into owned
   PCM. No actor, RNG, sound-table, filesystem archive, CVAR, console or OpenAL
   access. Do not use the latency-sensitive streaming thread for long jobs.
3. Owner after completion: close decoder, validate generation/identity, select
   backend format, create OpenAL buffer and publish handle. Retain encoded bytes
   through close. Qualify other decoder families/platforms before enabling them.

## Installed-library prototype

Prediction before execution: identical PCM and metadata; at least 90% less
owner-thread CPU during worker decoding. Measure opening separately. No
frame-time improvement is predicted from this standalone probe.

The temporary Python/ctypes harness calls the installed ZMusic API. All opens,
metadata queries and closes stay on the owner thread. Only reads and PCM assembly
run on one persistent worker, with an exclusively owned decoder per trial.
`time.thread_time` measures CPU; `time.perf_counter` measures wall time. Asset
extraction and finished-PCM hashing are outside decode timing. The owner polls
at 1 ms intervals; its reported worker-mode CPU includes dispatch/poll overhead.
This polling is a probe mechanism, not the proposed engine implementation.

Two independent runs each exercised seven inputs with three serial/worker
repetitions: the three actual thunder variants, generated mono silence, empty
stereo WAV, invalid bytes and a truncated header (42 decodes per run). The repeat
explicitly requires accepted, non-empty stereo 48 kHz Int16 thunder output, so
universal decoder failure cannot pass the comparison.

Repeat medians:

| Asset | Serial owner decode CPU | Worker-mode owner CPU | Owner open | PCM bytes |
|---|---:|---:|---:|---:|
| DSAMTHN1.ogg | 147.844 ms | 2.930 ms | 1.067 ms | 4,338,396 |
| DSAMTHN2.ogg | 87.194 ms | 1.639 ms | 0.894 ms | 2,650,844 |
| DSAMTHN3.ogg | 193.730 ms | 3.571 ms | 0.857 ms | 5,571,904 |

Owner CPU reduction is 98.02–98.16%, exceeding the prediction. First-run serial
medians were 148.668/91.763/192.151 ms, versus worker-mode owner medians
2.813/1.827/4.052 ms; this variation is much smaller than the removed owner CPU.
Worker decode wall time still takes approximately 88–195 ms: work moves rather
than disappears. Thunder open measurements range 0.838–3.930 ms, including first
use. These are standalone decoder measurements, not total engine load costs.

Every input has identical accepted/rejected status, output format, byte count
and PCM SHA-256 between modes/repetitions. Three additional paired trials read
different pre-opened decoders concurrently on owner and worker threads; all
signatures match. This supports the tested boundary, not proof of race freedom
for all formats or builds. Music playback, OpenAL, cancellation and engine reset
are not exercised by this probe.

Small silence costs about 0.175 ms serial versus 0.615 ms owner dispatch/poll CPU;
not every short sound benefits from a job. The three thunder PCM buffers occupy
12,561,144 bytes (~11.98 MiB) together, before source bytes, decoder state,
containers, growth temporaries or backend storage.

Artifacts: `/private/tmp/gzdoom-blackwater.Y5o1Yg/audio-handoff-9sjrl169` (first)
and `/private/tmp/gzdoom-blackwater.Y5o1Yg/audio-handoff-4h26lnft` (repeat/overlap).
The repeat retains the exact harness and JSON, including input and library
hashes. No mod assets are added to the repository.

## Bounded engine implementation and acceptance

- First separate owned PCM preparation from buffer publication, checking PCM,
  sample counts, loop points and playback against the current synchronous path.
  Preserve VOC, raw and DMX handling.
- Add one persistent worker, a bounded queue, encoded/PCM memory accounting and
  duplicate-request coalescing. Jobs carry values and generation IDs, never
  borrowed actor or `sfxinfo_t*`/sound-table storage.
- Prepare known expensive sounds ahead of playback. Map `LightningSound` and
  all its random variants are the evidenced first prefetch candidate; current
  `S_PrecacheLevel` does not explicitly mark this field. Enumerating variants
  must not select a random outcome or advance gameplay RNG.
- Preserve existing cache API completion semantics. A dedicated nonblocking
  preparation request must not silently defer an existing synchronous
  `CacheSound` contract.
- At playback consume ready results without waiting on unrelated jobs. If
  preparation is absent, failed, unfinished or over budget, preserve synchronous
  fallback and existing onset semantics. Count fallback reasons. This cannot
  promise stall-free cold misses; do not silently drop/delay sounds or wait on
  the worker queue head.
- On clear/unload/reset or generation changes, discard stale results before
  publication. Cancel queued work; retain running handles/bytes until completion.
  Shutdown joins the worker and closes decoders before destroying owning state.
- Test music plus effects, loop metadata, corrupt/empty inputs, duplicate
  requests, reset with queued/running work and shutdown. Use sanitizers where
  available. Only then qualify disabled/enabled preparation on Blackwater,
  separately recording acquisition/open/decode/upload, ready hits, fallbacks,
  queue/PCM memory and actual doorway frame intervals.

No engine C++ or runtime settings changed in the prototype qualification above.
Integration exposed an additional allocator dependency: `TArray` growth uses
`M_Malloc`/`M_Realloc`, which update non-atomic GC counters. The user approved
owner allocation of a bounded PCM extent before queuing. Workers only write
samples/count/status; owner closes, resizes/frees and publishes. GC/VM code
does not need to change for this boundary.
