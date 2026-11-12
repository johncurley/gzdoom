# Blackwater shared save-load attribution — 2026-11-12

## Disposition

Scoped runtime attribution completed for two automatic same-save reloads on
Intel Metal. No engine behavior, threading or default changed in this tranche.
The original 623.83 ms manual death/reload interval remains unprofiled; these
measurements identify its shared loading path, not that exact interval.

The next bounded investigation is UDMF parser allocation/copy work and repeated
level construction. Residual post-load sound decoding remains a separate
smoothness candidate. This does not justify concurrent VM or actor ticks.

## Workload and capture correction

Continue from `8a12c1ea63`, with the enabled sound-sibling default and the frozen
earlier MAP09 save used for combat acceptance. Retention is zero, audio audit
and frame/stall text logging are off, and native profile markers are on.
Executable, UUID, input/package hashes, image mapping and relevant source
snapshots are retained. Runtime UUID matches the copied symbolication binary:
`597FAB71-24D3-3887-AA56-BC9D397EB663`; ASLR load address `0x10ef1e000`.

Prediction before recording: capture at least one >=300 ms reload-related
Update gap with load ancestry, then distinguish execution from blocked time.
The two qualified shared-path reloads satisfy this event prediction. It is an
attribution contract, not an optimization or performance-gain prediction.

The first attempt requested 80 seconds of System Trace with Time Profiler,
Points of Interest and Filesystem Activity, retaining 100 seconds. The operator
confirmed four normal deaths/reloads without menus. The runner's 150-second
post-readiness wait expired while Instruments was finalizing; its cleanup
terminated the recorder. The resulting 52 KiB bundle has no usable template
and TOC export fails with `Document Missing Template Error`. **Reject this
trace for CPU/wait attribution.** Preserve the failure and operator report.
The runner timeout was too short; no engine defect follows from that failure.

Recovery uses the same warm process and a 25-second recording with 40-second
retention and a 600-second finalization budget. Two scripted `load start.zds`
requests replace further manual input. The source routes `ga_loadgame` and
normal death's `ga_autoloadgame` through `G_DoLoadGame()`; the automatic probe
covers that shared work, not the complete death/reborn/input path. Normal
non-demo operation is used. The test instance was closed cleanly after the
active section and recording time limit; the recorder subsequently saved
successfully. Earlier successful runners also exit their targets. This is an
observed recovery sequence, not proof that every recording requires exit.

## Capture quality and limits

- TOC duration is 25.403685 seconds, end reason `Time limit reached`, retention
  `Windowed (40 seconds)`. CPU samples span 0.014628–25.394632 seconds; marker
  analysis ends at 25.395632 seconds inside the common retained range.
- Main-thread scheduler intervals cover each selected gap completely, without
  overlaps. Update IDs are consecutive across all four selected gaps.
- Marker parsing finds zero duplicate begins and unknown event types. One
  unmatched loop end at 0.365325 seconds crosses recording start; one unmatched
  loop begin at 25.395495 seconds crosses the bounded analysis endpoint.
- The trace issue store contains one `Data stream: Time Mapping` entry at
  relative timestamp zero (`ZTYPE=2`). Retain it. Interior marker continuity
  and scheduler coverage support the selected windows; no blanket loss-free
  collection claim is made.
- The automated run records `active=0`, `paused=0`, `menu=0` at the selected
  boundaries. Background rendering/sound remain enabled by the private config.
  This is background process attribution at cap35, not foreground pacing or a
  matched comparison against the manual run.
- All reloads are warm-process events after prior gameplay/reloads. OS caches
  were not purged. Two events in one process do not establish a backend-wide
  baseline, noise floor, cold-load result or expected optimization payoff.
- Symbols come from the retained matching executable. A sandboxed `atos`
  attempt warned about the symbol service; an unrestricted repeat resolves the
  same 3,689 rows identically. Four engine address rows remain unresolved.
  Use validated engine ancestors, not unexpected/coalesced leaf labels, for
  subsystem attribution.

## Reload windows: execution versus waits

Times below are trace-relative. Scheduler columns partition elapsed time;
sample weights in later tables are a different measurement.

| Window | Trace seconds | Update gap | Running | Blocked | Other scheduler states |
| --- | --- | ---: | ---: | ---: | ---: |
| Reload A | 5.451940–6.326724 | 874.78 ms | 753.12 ms (86.1%) | 88.82 ms | 32.84 ms |
| Reload B | 11.668048–12.257930 | 589.88 ms | 527.87 ms (89.5%) | 39.62 ms | 22.39 ms |

Other states are runnable, interrupted and preempted. Each reload contains one
completed EngineTick scope. Large TryRunTics/playsim elapsed time here is mostly
loading inside a tick, not hundreds of milliseconds of ordinary actor ticks.
Missing CPU samples are not used to estimate blocked time.

## Inclusive CPU sample ranking

Values are nominal milliseconds of main-thread sampled stack weight. Rows
nest/overlap and **must not be summed** or presented as exclusive wall-time
savings. Reload A/B have 756/528 ms total main-thread sample weight.

| Function/family | Reload A | Reload B | Meaning |
| --- | ---: | ---: | --- |
| `G_DoLoadGame` | 725 | 499 | Shared load ancestry |
| `P_SetupLevel` | 590 | 399 | Level setup enclosing several paths below |
| `MapLoader::LoadLevel` | 369 | 325 | Map parse/build pipeline |
| `UDMFParser::ParseTextMap` | 161 | 139 | Text parse and construction, not just token scanning |
| `hw_PrecacheTexture` | 102 | 15 | Variable CPU render-resource preparation |
| `FileSys::DecompressorZ::Read` | 80 | 58 | Cross-cutting decompression, overlaps loads above |
| `FLevelLocals::UnSnapshotLevel` | 61 | 57 | Save/world restoration |
| `FLevelLocals::Serialize` | 44 | 43 | Nested world serialization work |
| `S_PrecacheLevel` | 42 | 33 | Owner-side sound preparation |
| `CreateSections` | 38 | 36 | Render geometry setup |
| `CreateVBO` | 34 | 27 | Geometry buffer preparation |
| `MapData::GetChecksum` | 29 | 27 | Compatibility/save checksum generation |
| `DoomLevelMesh` constructor | 29 | 23 | Additional level geometry construction |

Memory-move leaf samples with UDMF parsing ancestry account for 58/57 ms;
within the broader MapLoader pipeline they account for 93/76 ms. The narrower
UDMF attribution is a repeatable candidate, not an exclusive allocation count
or proof that all those copies can safely be removed.

## Filesystem and resource work

Main-thread logical filesystem Read calls overlap the two reload windows by
47.18/5.41 ms; the longest individual calls are 1.88/0.96 ms. Other filesystem
categories total approximately 1.03/0.88 ms. There are 1,658/1,669 overlapping
main-thread filesystem calls across all categories. Paths in the selected
read records are unresolved, so no per-asset file identity is claimed.
These are syscall intervals, not physical disk latency or exclusive I/O wait.
They overlap scheduler and sampled call stacks and must not be added to them.

The 58–80 ms sampled decompression ancestry demonstrates CPU resource work
outside the syscall totals. Blocked scheduler time still needs caller-specific
attribution; this trace does not establish GPU/presentation innocence.

## Separate post-load simulation catch-up

The loader is followed by presentation/wipe Updates and then longer simulation
intervals. Keep these separate from the core reload windows and from steady
combat. Bounds remain unpaused with no menu, in the background configuration.

| Trace seconds | Gap | Actual ticks | Running | Blocked | SoundEngine load samples | OpenAL load samples | Decode samples |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 7.646576–7.753433 | 106.86 ms | 28 | 94.21 ms | 10.44 ms | 54 ms | 48 ms | 38 ms |
| 13.588510–13.678489 | 89.98 ms | 19 | 87.26 ms | 1.06 ms | 47 ms | 45 ms | 36 ms |

Filesystem Read intervals overlap these windows by 13.12/1.54 ms. VM/thinker
ancestors include the nested synchronous sound loads; their inclusive weight
is not evidence for a VM dispatch bottleneck. `PrepareSoundSibling` also has
4 ms of inclusive samples in the second window: lookahead has owner-thread
acquisition cost and does not make every cold request asynchronous.

These instrumented automatic-reload gaps do not contradict the separate
foreground manual smoothness acceptance. Observer cost, focus, exact world
state and workload differ; no regression or improvement is inferred by
comparing their absolute timings.

## Source boundaries and next intervention

`P_SetupLevel` stops sounds, frees old level data, opens the map, initializes
static handlers, hashes map content and invokes `MapLoader::LoadLevel`.
MapLoader explicitly warns that most ordering is important. UDMF parsing
interns names, resolves textures/translations, fills temporary arrays, copies
vertices/sectors into live level storage, fixes references, and creates
scrollers/thinkers. It is not a pure byte-to-immutable-data worker function.
`MapData` readers also share resource ownership and rewind mutable cursors.

Ranked follow-up:

1. Inspect UDMF allocation/growth/copy boundaries for a bounded serial change
   with exact map/save/runtime parity controls. Preserve parser validation,
   namespace behavior, property ordering, reference fixups and side effects.
2. Audit repeated map acquisition/checksum work before proposing reuse. A cache
   keyed only by map name is insufficient; content/resource identity and
   invalidation must preserve compatibility/save checksums. Do not skip hashing
   or retain live level/thinker pointers across teardown as a shortcut.
3. Independently identify remaining post-load sound misses and readiness.
   The sibling optimization covers only bounded eligible random-group work;
   decoders, owner acquisition, direct sounds and fallback remain meaningful.
4. Revisit geometry and texture preparation using the measured CPU boundaries.
   Texture precache variability is real; it is neither a steady BSP-traversal
   measurement nor a reason to parallelize the entire hardware renderer.

No threading or cache implementation is approved by this audit alone. Before
an optimization, define its contract, predict its effect, measure an interleaved
unprofiled control and preserve exact compatibility evidence. A shorter manual
normal-death trace and matched foreground/backend repeats remain qualification
work; do not claim the complete death path has been profiled successfully.

## Evidence and validation

`/private/tmp/gzdoom-reload-attribution-xsq8sf7g` retains both attempts:
failed `capture.trace` and quality record, successful `capture-short01.trace`,
TOC, profile/marker/thread/filesystem exports, manifests, UUID check and process
sample, ASLR mapping, executable/source snapshots, symbol files, analysis logs,
`short01-attribution.json` and `short01-detail.json`.
Scratch runners are in `/private/tmp/gzdoom-reload-attribution-20261112`.
The test instance has exited. No new runtime code or instrumentation was added.
