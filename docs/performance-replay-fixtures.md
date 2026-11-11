# Performance replay fixtures

Immediate milestone: reproduce a player-observed hitch with existing demo/save
facilities. No engine changes or new instrumentation are required to create
these fixtures. Runtime qualification is pending selection and recording of the
actual mod/map/route; this document does not claim recordings were created.

## Facilities verified in source

`recordmap <filename> <map name>` starts a **new game** on that map and records
inputs. `recordmap <filename> *` also starts anew on the current map; it does not
record the current world state. `-record <filename>` is the startup alternative.
`G_RecordDemo` adds `.lmp` when omitted. The format is GZDoom FORM/ZDEM, not a
vanilla Doom demo. Headers include map, RNG seed, user info and selected CVARs;
they do not package mods, the executable, or every rendering/player setting.

`stop` finalizes the recording; in current single-player code it returns to the
full console. Wait for `Demo ... recorded` and verify the file exists before
quitting. Do not terminate the process to stop a recording.

`playdemo <filename>` or startup `-playdemo <filename>` performs ordinary replay.
Use ordinary replay first. `timedemo` changes pacing and offers `-nodraw` and
`-noblit`; it is not the primary reproduction of a visible gameplay stutter or
an automatically equivalent CPU-only baseline.

Source: `src/g_level.cpp`, `recordmap`; `src/d_main.cpp`, startup argument
handling; `src/g_game.cpp`, `G_RecordDemo`, `G_BeginRecording`,
`G_WriteDemoTiccmd`, `G_DoPlayDemo`, `G_ProcessIFFDemo`, `G_TimeDemo`.

### Save-start and playback caveats

Do not prescribe `-loadgame ... -record ...` as a verified save-start replay
recipe. Startup passes `NULL` to `G_BeginRecording`, which substitutes the
current map name. Playback initializes a new map for a nonempty header name;
only an empty header map name selects the existing saved world. That inspected
path does not establish a valid save/demo pair. Do not patch headers or change
the engine to bypass this. Use a new-game route, or retain the save and manual
route as the fallback.

`G_DoPlayDemo` sets `precache = false` around initial map initialization and
restores it afterwards. Thus recording and playback can differ in initial
resource preparation. Qualify the in-play event itself, and retain the
save/manual route if replay changes or suppresses it. This caveat also prevents
using initial demo map load as proof of ordinary level-transition behavior.

## Record three independent fixtures

| Fixture | Content | Required event evidence |
|---|---|---|
| Heavy gameplay | 60–90 seconds of representative active combat in the chosen demanding map | Actor/script/light activity and route landmarks |
| Area entry | Approach the troublesome location, enter, continue 10–20 seconds | Exact trigger landmark and gameplay position/time |
| Level transition | Play through the actual exit/transition and into the next map | Source/destination maps, transition trigger and post-entry activity |

For a new-game native recording, use console commands such as:

```text
recordmap "/private/tmp/gzdoom-perf/area-entry.lmp" MAPXX
```

Create the destination directory first and replace MAPXX with the real map.
Keep any necessary approach from map start in the demo; define a shorter
analysis window around the event rather than pretending a save was embedded.
After completing the route, open the console and type `stop`. Repeat separately
for the other fixtures. Use a supported ordinary save made through the save
menu near the troublesome area for the fallback, with an exact written route.

Capture a screen recording of the original event and each qualification replay
needed to assess visible divergence. Use macOS's screen-recording facility,
record the game window/region, and keep gameplay focused. Mark capture start,
route landmarks and event time. Video records visible presentation, not CPU
attribution; recording overhead can change pacing. Later performance baselines
need a separate video-off arm. Do not assume screen-recording frame rate can
resolve individual engine frames.

## Preserve a manifest with each fixture

Store outside the source/build tree while qualifying. Keep the native demo,
video, optional fallback save, route notes and a manifest together. Include:

- Engine commit, dirty state, executable hash and required runtime PK3 hashes.
- IWAD and every mod's version, SHA-256 hash, exact path and load order.
- Map, skill, compatibility/gameplay settings, player setup and JIT settings.
- Exact launch arguments, dedicated config copy, backend verified from log,
  resolution, effects, precache, vsync/caps and audio configuration.
- OS/hardware, recording tool/settings, focus and cache history. Distinguish
  fresh process/first visit from repeat visit; neither proves cold disk caches.
- Demo/save/video hashes, duration, landmarks and event windows. Preserve
  prerequisites without redistributing game/mod assets unnecessarily.

## Qualify before benchmarking

Replay twice on the recording backend with the same executable, assets, load
order and settings. Example console command:

```text
playdemo "/private/tmp/gzdoom-perf/area-entry.lmp"
```

Both runs must reach the same map, trigger landmark and meaningful gameplay
sequence with comparable health, inventory, kills and actor/world behavior.
Record checkpoint observations and any demo errors; visual agreement is useful
but is not a complete deterministic world-state proof. Check the trigger under
matched first-visit and warmed conditions separately. A hitch absent in both
replays fails as a hitch fixture even if the gameplay inputs replay correctly.
Unexplained state divergence fails qualification. A valid heavy-gameplay fixture
may have no hitch; label that distinction explicitly.

If replay fails, retain the original video and save/manual route, record the
failure, and use that fallback for reproduction. Do not change engine behavior
to manufacture a qualifying demo. After same-backend qualification, check the
other backend for replay divergence before collecting comparative profiles.

The two-replay check establishes fixture suitability, not a performance result.
Only then execute the repeated, interleaved GL/Metal captures and CPU running
versus blocked-time attribution specified in the
[performance audit](audits/engine-performance-dependencies-2026-11-08.md).
Keep normal gameplay and transitions separately ranked. Future builds must
requalify each fixture; cross-build compatibility is not assumed.

The existing `build/framegraphdemo.lmp` is a stock DOOM II MAP01 route, with a
historical successful Metal replay in `AGENTS.md`. It can exercise demo playback
but is not evidence of the reported mod-specific hitch and must not silently
replace these fixtures.
