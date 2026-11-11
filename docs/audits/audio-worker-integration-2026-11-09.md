# Bounded audio preparation worker — 2026-11-09

The 2026-11-11 default update enables the macOS sound preparation worker and
precache overlap. The [precache follow-up](sound-precache-worker-2026-11-11.md)
records the current setting and verification. The qualification and original
default-off disposition below describe this initial integration.


Status: first engine integration, **opt-in**, with Intel macOS controls. Matched
doorway performance qualification remains pending. No frame-time improvement
or stall-free cold-miss guarantee is claimed.

## Implementation

`snd_asyncprepare` defaults false and is non-archived. Initial support is
Ogg/Vorbis on macOS OpenAL. Null sound, other platforms/formats and oversized
requests retain synchronous loading. VOC/raw/DMX branches are unchanged.

After existing synchronous level precaching, request preparation of the level's
`LightningSound`. Follow aliases and enumerate random groups without selecting
RNG outcomes; a visited set bounds cycles. Existing `CacheSound` and
`CacheMarkedSounds` completion semantics remain synchronous.

Owner copies encoded bytes, parses loop tags, opens the decoder, queries format
and allocates an 8 MiB PCM extent using `TArray`. One persistent worker reads
exclusively into that fixed extent and reports byte count/status. It does not
allocate/resize/free engine containers, create/close decoders, access actors,
RNG, sound definitions or archives, log through the engine, or call OpenAL.
Completion is published under a mutex. Owner closes the decoder, publishes the
decoded byte count through OpenAL and frees buffers. Encoded bytes survive close.

`LoadSound` checks ready results after its existing loaded-alias search. Missing,
failed, mismatched or unfinished results take the original synchronous path.
Unfinished requests are cancelled rather than waited on; running reads retain
their independent bytes/decoder until done. The owned job also closes decoders
on zero-PCM returns, addressing the old loader's early-return leak.

Deduplication uses sound ID, lump and explicit loop settings. Generations guard
sound-table/device invalidation. Unload cancels matching jobs; clear/reset
invalidates all. Owner immediately frees unstarted cancellations and retains
running ones until completion. Shutdown signals and joins before freeing jobs.
No detached worker or borrowed actor/`sfxinfo_t*` is queued.

Limits: 8 MiB encoded input, 8 MiB PCM per job, four-job ceiling and 64 MiB total
reservation. Each reservation includes encoded bytes plus twice the PCM extent;
nonempty reservations therefore permit at most three simultaneous full jobs.
Reservation is not total process memory: library internals, acquisition
temporaries and backend storage are separate. An extra sample-frame read
distinguishes exact-capacity EOF from overflow. Overflow is discarded and falls
back synchronously, rather than truncating playback.

Diagnostics: `snd_prepare <name>` requests preparation; `snd_prepare_stats`
reports queued requests, hits, not-ready fallbacks, failures, budget rejections,
cancellations, readiness and reserved bytes. Failure counts include jobs retired
before playback; they are not an exact playback-fallback count.

## Validation

Predictions: zero PCM differences, matching sample/loop metadata, no stale
publication, and ready thunder replacing first-use synchronous loads. Overall
gameplay speedup is a separate acceptance gate.

A temporary C++ harness extracts the actual job/decoder/state implementation,
uses engine `TArray`, and substitutes allocator/decoder wrappers that fail on
off-owner allocation/free/open/close. External libraries remain uninstrumented.

- AddressSanitizer + UndefinedBehaviorSanitizer: PASS for three thunder assets,
  exact-capacity output, undersized rejection, cancellation and shutdown with
  outstanding/queued work. Decoder opens/closes balance 18/18.
- ThreadSanitizer: same cases PASS, no race reported in instrumented code.
  This does not prove race freedom inside installed decoder libraries.
- PCM hashes/counts match the independent serial-library reference exactly:
  4,338,396 / 2,650,844 / 5,571,904 bytes.
- Deliberately resizing on the worker aborts with
  `WORKER_ALLOCATOR_VIOLATION` (exit -6): the guard detects the contract breach.

Harness artifacts: `/private/tmp/gzdoom-blackwater.Y5o1Yg/audio-worker-tests`;
generator: `/private/tmp/gzdoom-blackwater.Y5o1Yg/make-audio-worker-tests.py`.
Temporary test assets/scripts are not committed or put in the build tree.

Real Blackwater save controls use original assets, explicit matched background
settings, 525 in-level frames, and developer logging. They are identity/lifecycle
diagnostics, not timing baselines or deterministic route replays:

| Order | Mode | Lightning events | In-play thunder loads | Ready hits | Not-ready / failures |
|---|---|---:|---|---:|---|
| 1 | disabled | 3 | thunder2, thunder1 | 0 | n/a |
| 2 | enabled | 3 | none | 2 | 0 / 0 |
| 3 | enabled | 3 | none | 2 | 0 / 0 |
| 4 | disabled | 3 | thunder1, thunder2 | 0 | n/a |

Both enabled arms initially have three ready jobs (53,741,839 reserved bytes).
The third flash reuses an already-loaded variant. Variant order differs; do not
infer deterministic equivalence or frame-time gains. Artifacts:
`/private/tmp/gzdoom-blackwater.Y5o1Yg/audio-worker-controls-k2oz87ow`.

An extra isolated SNDINFO fixture gives new test names to existing thunder data.
Temporary commands read actual OpenAL metadata, then are removed from source:

- Serial and worker explicit-loop output both report 4,338,396 bytes, 48 kHz,
  16-bit stereo, 1,084,599 frames, 22,595 ms, loop [123, 5000], AL error 0.
- Out-of-range loops both clamp to [0, 662711], with 2,650,844 bytes,
  662,711 frames, 13,806 ms, AL error 0.
- Repeated requests do not add duplicate decode jobs. Extra distinct requests
  hit the budget: queued stays 3, rejected becomes 4.
- Immediate playback forces fallback: notready=1; outstanding jobs subsequently
  clean up to jobs=0, reserved=0.
- Device reset immediately after queuing succeeds; the fresh backend reports no
  worker, then a subsequent request again prepares three ready buffers. No stale
  publication or exit crash occurs.

Corrected edge artifacts:
`/private/tmp/gzdoom-blackwater.Y5o1Yg/audio-worker-edges-ufp7qzrf`.
The earlier `audio-worker-edges-07lj_2ku` harness is rejected for chained checks:
`execafter` calls `C_DoCommand` without splitting semicolons, so several commands
never executed. The corrected harness uses newline-separated exec files.
The original mod and user configuration were not edited.

Final build/whitespace checks pass. The final OpenGL Blackwater run also records
three lightning events, two ready hits, zero in-play thunder loads and zero
not-ready/failure counts (`audio-worker-gl-tz1ozshx`). Final GL/Metal baseline
and SSAO cross-backend comparisons both report OK; graph self-tests in the
launches report PASS. Temporary publication-probe commands are absent from the
final source. These checks do not qualify the actual doorway pacing improvement.

## Remaining acceptance

The [metrics follow-up](blackwater-audio-metrics-2026-11-09.md) now qualifies
stationary saved-world hitch reduction: three runs per setting give seven
>100 ms intervals disabled and zero enabled in equal approximately 18-second
selections. Separate CPU captures show zero sampled main-thread thunder decode
ancestry enabled versus 447 ms disabled. Actual doorway-route, uncapped ceiling,
long-session and fresh scheduler-wait qualification remain pending below.

Keep default off pending matched active-gameplay/doorway captures, CPU execution
versus waits, acquisition/open/upload attribution and long-session checks.
Measure allocation/GC pressure and ready-cache memory from fixed buffers.
Native demo/determinism, multiplayer, Apple Silicon, Linux and Windows runtime
qualification are not claimed. Wider actor prefetch/formats need evidence.

## Future GC/VM work

Allocator accounting is a bounded project: choose atomic reporting or per-worker
accounting merged at defined owner safe points; audit all readers/writers and
measure overhead and collection cadence. That alone does not permit concurrent
object allocation or world mutation.

A concurrent collector/VM needs thread root registration, safepoints/stop
coordination, correct read/write barriers, finalizer/destruction ownership and
an audit of shared allocator/string/type/reflection state. `GlobalVMStack` is
already thread-local; native bindings and live world state still need explicit
ownership. Define immutable inputs and ordered commits before moving scripts.

Preserve bytecode/native bindings and saves where possible; explicitly update
bindings/reflection if interfaces change. Acceptance needs forced-GC stress,
sanitizers, VM/JIT/native-call parity, save/load and mod coverage, plus repeatable
tick-state comparisons with controlled RNG/order. Do not assume an available
demo or deterministic observable behavior merely from atomic counters. Audio
does not require this redesign: its allocation and GC accounting remain on owner.
