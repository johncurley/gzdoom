# Blackwater combat candidate and death/reload attribution — 2026-11-10

Status: exploratory combat take plus one confirmed normal death/reload CPU
capture. The user reports smooth fighting before dying. The mixed unprofiled
take is not a qualified steady-combat baseline; the profiler establishes two
separate reload-related events. No engine implementation changes are made.

## Workload and operator observations

Read-only save inspection found Blackwater autosaves on MAP09, MAP11 and MAP12.
The operator did not select one of the initial save choices, so the earlier
mall MAP09 autosave was stated as the default candidate and loaded paused.
The operator then explicitly confirmed enemies were reachable and readiness
for combat. Source: Documents/GZDoom/Savegames/doom.id.doom2.commercial/auto00.zds,
MAP09 Desecrated altars, saved map time 00:36:39. This differs from the later
54-minute doorway save. Original save bytes remain untouched; all writable
saves and INIs are isolated under the capture directories.

Same current build, Blackwater assets and 1024×768 Metal setup as the doorway
checks. Cap35 and explicit background activity remain enabled. Audio preparation
is explicitly enabled for these runs; the engine default is still off. Initial
stats have three ready thunder jobs, no failures or not-ready fallbacks. No new
instrumentation, mod changes, skill changes or invulnerability cheats are used.

The operator initially reported "I did die a few times. Menu opened now." and
then confirmed "Fighting felt smooth." The unprofiled take is therefore labelled
exploratory mixed combat/death/reload/menu, with unknown precise context
boundaries. Do not treat its whole-run percentile or maximum as combat cost.

## Exploratory light capture

Artifacts: `/private/tmp/gzdoom-combat-current-3rt_5hs9`; launch/control scripts
and fixture metadata: `/private/tmp/gzdoom-combat-20261110`.

Existing vid_frametrace 2 is active for 2,625 in-level rendered frames, nominally
75 seconds at cap35. Deferred commands only advance in GS_LEVEL with no game
action, so loading can extend wall time. The complete-window selection contains
38 windows, 2,647 Update intervals and approximately 77.192 seconds from rounded
means/counts. Final partial-window samples are unavailable. The three >100 ms
intervals are 763.45, 100.09 and 602.54 ms. Repeated MAP09 reload banners occur
before the windows containing the 763.45 and 602.54 ms maxima. Text output is
not a precisely aligned CPU timeline; no cause is assigned from this alone.

The frame-summary JSON retains the operator observation. No menu/death-free
segment can be established just from host receipt of the user's message.
The numeric zero->100ms hypothesis fails for the mixed take; that is not a
contradiction of the user's report that the actual fighting felt smooth.
The run is rejected as a whole-run heavy-combat acceptance result. The game was
closed cleanly after automatic capture completion.

## Normal death/reload CPU capture

The operator explicitly chose death/reload profiling and confirmed afterward:
"Yes, normal death and reload." This is a real normal reload, not a console map
warp, but it is distinct from a normal level-exit/next-map transition.

Artifacts: `/private/tmp/gzdoom-reload-cpu-20261110-165906`; control and analysis
scripts: `/private/tmp/gzdoom-reload-20261110`. Retain archived executable,
UUID/hash, save/mod hashes, arguments, actual PID image mapping, raw trace,
exports, complete bounded sample JSON and symbolization. The template is Time
Profiler + Points of Interest; no System Trace, video, filesystem capture or
new engine probes. Direct launch and PID attach avoid app substitution.

Prediction before capture: at least one >=100 ms Update gap contains
G_DoLoadGame sample ancestry. Initial pause/recorder setup is excluded. The
recorder-start notification arms a ten-second paused countdown before unpause;
no exec-file polling remains in the active measurement. A 50-second recording
with a 70-second retention window completes normally at the time limit:
50.341265 seconds. The game exits normally after its deferred stop. RunIssues
contains the common Data stream: Time Mapping entry and no separate explicit
loss entry. This does not provide running/blocked scheduler durations or GPU
occupancy. Missing CPU samples cannot be converted into a particular wait time.

Select 10.546–50.241 seconds, starting 100 ms after marker-confirmed unpause and
ending before the recorder boundary. There are 1,354 continuous Update ordinals,
no menu/pause markers, and 1,341 gametics advanced. Focus is active for 1,145
Updates and inactive for 209; explicit background activity stays enabled. Both
long events below have active endpoints. Main-thread sample weight is 13,298 ms.
The selection deliberately includes loading and cannot be presented as ordinary
steady gameplay. Profiled and unprofiled durations are not a speedup comparison.

### Event 1: map reconstruction and preparation

Update 1893 -> 1894, trace time 37.383224134–38.246392573:
**863.168439 ms**. The endpoints are GS_LEVEL, no menu/pause, app active.
One complete EngineTick interval lasts 672.675922 ms. Main sample weight within
the Update gap is 782 ms; the following inclusive rows overlap:

| Stack | Sample weight |
|---|---:|
| G_DoLoadGame | 650 ms |
| G_InitNew | 643 ms |
| G_DoLoadLevel / FLevelLocals::DoLoadLevel | 604 ms each |
| P_SetupLevel | 546 ms |
| MapLoader::LoadLevel | 357 ms |
| MapLoader::ParseTextMap | 203 ms |
| UDMFParser::ParseTextMap | 198 ms |
| D_Display | 118 ms |

Within load ancestry, further inclusive candidates are archive decompression
89 ms, texture precaching 83 ms, FName lookup 70 ms, CreatePixelSnapshot/queued
texture acquisition 64 ms, P_OpenMapData 61 ms and UnSnapshotLevel 57 ms. These
are overlapping caller paths, not independently additive costs. Representative
leaves are tinfl_decompress 81 ms, FName lookup 56 ms, memmove 52 ms, bzero 29 ms,
scanner string processing 28 ms and MD5Transform 27 ms. No precise filesystem
latency or GPU wait duration is inferred.

This event satisfies the predeclared >=100 ms load-ancestry prediction. CPU
samples identify substantial main-thread map reconstruction/preparation; the
whole 863 ms is not assigned to one function or exclusively to CPU execution.

### Event 2: post-load tick batch and first-use sound work

Update 1936 -> 1937, trace time 39.559970951–39.661083001:
**101.112050 ms**, again active GS_LEVEL endpoints with no menu/pause.
There are **29 complete EngineTick intervals**, summing to 70.651932 ms;
maximum individual tick is 17.456601 ms. Gametic advances 1896 -> 1925 across
the gap. This is a multi-tick batch, consistent with elapsed-time catch-up;
it is not evidence of one actor tick taking 101 ms.

The gap has 101 ms main-thread sample weight. Inclusive paths include
TryRunTics 82 ms, RunThinkers/P_Ticker 54 ms, SoundEngine::LoadSound 50 ms and
OpenALSoundRenderer::LoadSound 47 ms. Actor/VM-dispatched sound starts and Vorbis
MDCT/decode leaves are present. This is synchronous non-qualified first-use
sound work, not proof that all audio is now asynchronous. The resolved sound
asset is not known from these stacks, and this event must not be relabelled as
thunder merely because the earlier hitch was thunder-related.

Source context: G_DoLoadGame rebuilds the base level via G_InitNew before
restoring snapshots and game state; it invokes Net_SetWaiting and starts a GC
collection. UDMF ParseKey interns scanned property strings as FNames. TryRunTics
computes available ticks from clocks/command buffers and runs G_Ticker in a
loop. These findings identify ownership/scheduling dependencies; they do not
authorize worker parsing against the global name table or dropping catch-up
ticks. Changing tick scheduling requires a separate compatibility contract.

## Disposition and next bounded work

Doorway smoothness remains a separate three-run pass. Fighting is subjectively
smooth here, but a controlled heavy-combat fixture still needs a survivable
segment or a better save. Normal level-exit qualification also remains open.

For reload latency, repeat this event before claiming a precise cost or gain.
Audit immutable map bytes/decompression/hash work versus mutable UDMF/level setup
and snapshot restoration. The 64 ms queued-texture acquisition path still does
work synchronously before workers can consume it. Separately identify the actual
post-load sound assets and how preparation is invalidated on reload. Tick count
and wipe/input context must accompany any post-load pacing experiment.

Keep native simulation order, world/VM/GC ownership and the renderer baseline.
No new actor threading, scheduling reset, parser cache, asset prefetch or sound
change is implemented in this measurement step. This is stronger event-local
attribution, not a general claim that the renderer/GPU is innocent or that
ordinary simulation saturates the main core. The current build gate and
profiling-tool checks pass; temporary scripts stay outside the repository.
