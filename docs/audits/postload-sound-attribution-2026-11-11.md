# Blackwater post-load sound attribution — 2026-11-11

Status: repeated asset attribution complete; the single-decode hypothesis is
rejected. A temporary precache-declaration control eliminates the repeated
post-load synchronous loads. The subsequent
[bounded retention implementation](sound-retention-2026-11-11.md) now has scoped
warm reload qualification; wider preparation/default acceptance remains pending.

## Question and prediction

The earlier death/reload trace contains a 101 ms Update gap with 29 ticks and
synchronous actor-triggered sound loading. Its precise sound identities were
unknown. The subsequent reload comparison improves parsing/loading but shows
no consistent improvement in the first resumed loop. Identify the remaining
sound assets and separate acquisition, decoder work and OpenAL upload before
expanding worker eligibility.

Predeclared prediction: a repeated non-thunder synchronous decode of at least
10 ms in the first resumed tick batch in at least two of three processes.
**Rejected: zero processes satisfy it.** The actual finding is nine smaller
loads accumulating in one batch. Do not reinterpret the threshold as a sum or
silently substitute the larger enclosing renderer-call duration.

## Diagnostic method

Same copied Blackwater MAP09 auto00 save, assets and Intel Metal 1024×768/cap35
configuration as the warm reload measurements. Explicit background activity,
original vsync setting and snd_asyncprepare=1 retained. Three fresh processes,
each with one initial load and two automated console-requested same-save
reloads. No input, Instruments or screen recording. This identifies repeatable
post-load work; it is not the full normal death/menu sequence or a demo replay.

Temporary owner-thread probes label load epochs and phases: G_DoLoadGame,
its remaining outer loop, the first complete resumed loop, and later gameplay.
Actual G_Ticker calls are counted. The load scope includes local destruction.
Only synchronous sound cache misses are logged with logical name, resolved
resource, size and success. The generic OpenAL loader separately times decoder
opening/metadata, the read/buffer-growth/finalization stage, and AL buffer upload.
No new code executes on the audio worker.

The "decode" stage includes serial PCM container growth/shrink and decoder
close, not just codec arithmetic. Enclosing renderer timing includes the
backend probe's output overhead. This is diagnostic attribution, not a clean
performance baseline. Stage totals below are disjoint calls in the same tick
batch; they are not overlapping inclusive CPU sample weights. Do not add
renderer totals to their nested opening/decode/upload stages.

## Six repeat reloads

Every first resumed loop contains nine synchronous Ogg loads, across 15–16
actual ticks. Exact order/Geiger variant varies, so no deterministic replay
claim is made. Resource paths and PCM sizes are stable for each asset.

| Sound group | Resolved variants observed | Median summed decode-stage time per batch |
|---|---|---:|
| Flesh impacts | fleshdrop1, fleshdrop2, fleshdrop3 | 17.65 ms |
| Monster activity | gasact1, gasact2, gasact3 | 11.75 ms |
| Geiger clicks | geiger4 or geiger5, plus geiger6 | 6.71 ms |
| Rain | weather/rain | 3.41 ms |

Across the six batches, resource reads total 1.83–2.72 ms, decoder opening totals
7.74–8.42 ms, decode/buffer management totals **38.28–41.46 ms**, and OpenAL
upload totals **0.61–0.83 ms**. Enclosing synchronous renderer calls total
49.33–52.68 ms. The largest individual decode stage per batch is 7.46–9.33 ms.
This supports cumulative first-use preparation as the target, rather than
moving OpenAL upload or all sound channel updates onto a worker.

There are ten distinct assets because one Geiger variant differs between
processes. All ten pass the existing Vorbis/header/encoded-size eligibility
checks and have decoded output below 8 MiB. Their combined decoded PCM payload
is **1,500,708 bytes (1.43 MiB)**. This is payload size, not complete driver or
process memory consumption. Header/size eligibility is not a successful job
admission or a thread-safety test.

These new runs reproduce the same class of post-load sound work. They do not
retroactively assign exact assets to the older trace, which lacks those labels.

## Existing coverage and budget constraints

The mod's Sndinfo.txt defines random groups special/fleshdrop, gasbag/active
and special/geigermed, plus weather/rain. Inline A_PlaySound references and
weather configuration also exist. Actor.MarkPrecacheSounds marks standard
sound properties, not arbitrary script/ACS string literals. S_PrecacheLevel
runs during P_SetupLevel, before G_Level's later UnSnapshotLevel restoration;
base-map actors are not necessarily the complete restored actor set.

CacheMarkedSounds unloads unmarked, non-linked buffers. A sound loaded during
play can therefore need decoding again after a same-save reload when precache
discovery does not mark it. None of these assets is currently requested by the
additional preparation hook, which only requests Level->LightningSound.

Every diagnostic stats sample has three ready thunder jobs, reserving
53,741,839 bytes against the 67,108,864-byte budget. The remaining 13,367,025
bytes cannot admit another job: the fixed reservation requires encoded bytes
plus twice the 8 MiB PCM extent, already more than 16 MiB. This is conservative
reservation accounting, not measured RSS. The four-job ceiling is an additional
constraint. Do not simply widen requests or raise the budget and assume these
short sounds will be prepared.

## Existing precache mechanism: functional control

Prediction recorded before this control: zero target synchronous loads in both
post-load batches in each of three fresh processes, all ten assets successfully
loaded during initial setup, and existing thunder preparation preserved.

A temporary additional PK3 supplies only this declaration:

```text
GameInfo
{
    PrecacheSounds = "special/fleshdrop", "gasbag/active",
                     "special/geigermed", "weather/rain"
}
```

The original mod and user saves are untouched. Existing CacheRandomSound
enumerates variants without selecting an RNG outcome. The initial load in
each process records all ten assets successfully loaded. Neither subsequent
reload has any further target synchronous load, during loading or afterward.
All six first resumed batches have **zero synchronous sound loads**, and the
three ready thunder jobs/reservation remain present. The control prediction
passes in all three processes; the baseline's six batches each have nine loads.

This establishes that existing precache hints preserve the relevant loaded
buffers across these reloads. It is stronger than moving the same decode into
each load. Initial cold preparation still has a cost, and this experiment does
not measure total cold/warm latency or qualify a hitch-free result. No audible
output parity, final tick-state equivalence or end-to-end speedup is inferred
from the absence of load records.

## Next bounded engine work

The [retention follow-up](sound-retention-2026-11-11.md) implements and measures
the bounded policy below, including a user-approved cross-definition loop guard.
The original requirements remain recorded here for context.

Prefer a bounded owner-thread retention policy for recently used small effects
over hardcoding Blackwater names or expanding the worker queue indiscriminately.
The temporary metadata fixture demonstrates the opportunity; it is not a
shipped engine fix. Before implementation, specify:

- An explicit entry/payload budget and eviction order, accounting for loaded
  aliases without double counting shared buffers. Backend payload bytes are
  not a claim about total driver memory.
- Sound-table/resource identity and loop metadata validity. Explicit unload,
  local sound-definition changes, table/device reset and shutdown must still
  invalidate/release retained data on the owner. Existing synchronous cache and
  playback completion semantics stay intact.
- No extra actor/script precache callbacks, speculative script execution,
  RNG selection, VM ABI changes or tick scheduling changes. Re-running a
  virtual mod callback is not automatically side-effect-free.
- A disabled-policy control that reproduces repeated loads, plus repeated
  whole-reload/resumed-loop timings and payload-budget checks. Retention cannot
  eliminate a first-ever cold decode; wider background preparation remains a
  separate ownership/admission task.

After retention is qualified, revisit small-job sizing/admission and explicit
preparation hints for cold first visits. Keep bounded buffers, overflow fallback,
decoder ownership and reset/cancellation guarantees; do not borrow live actors
or move archive/name-table/OpenAL operations onto the worker.

## Artifacts and final state

`/private/tmp/gzdoom-postload-sound-20261111` retains six process manifests/INIs,
copied saves, raw logs, all per-load stage rows, `analysis.json`, group costs,
mod references, `precache-control-analysis.json`, the temporary metadata PK3,
source snapshots, probe executable and asset/source/binary hashes. Initial ZIP
lookup assumed engine-lowercased paths were archive spellings; analysis was
corrected to require a unique case-insensitive match. The same retained captures
were analyzed; no measurement was discarded or repeated for that correction.

Temporary timers, phase hooks, counters and CVARs are removed by restoring the
exact archived pre-probe source, preserving all previous changes. The final
build and whitespace checks pass. All four restored source files match their
pre-probe bytes; diagnostic strings exist in the probe binary and are absent
from the final binary. Results/hashes are in `final-verification.json`.
The metadata override is not installed in user configuration or the mod. No
production sound/VM/threading behavior is changed in this investigation.
