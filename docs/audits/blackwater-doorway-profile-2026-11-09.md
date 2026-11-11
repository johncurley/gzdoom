# Blackwater mall doorway — first CPU capture — 2026-11-09

Status: one usable Intel Metal Time Profiler capture, user-confirmed hitch during
the crossing. **Hitch cause and comparative baseline are not established.** No
engine changes were made. Source-dependency findings remain unchanged.

## Workload and capture provenance

- MacBookAir7,2, Intel Core i5-5350U / HD Graphics 6000, macOS 12.7.6.
- Engine `caacc40847e5637548fc69b85911fa02c08c9fa9`, documentation-dirty build,
  RelWithDebInfo. Executable UUID `F918E038-5553-3243-9F86-DC54DC10C9CC` matches
  the running process and trace symbol archive.
- DOOM2.wad and AshesBlackwater.pk3, MAP09, Desecrated altars. Launch log
  confirms Metal at **1024x768**, not an assumed 1280x720 baseline.
- Starting save is the copied `save02.zds`, named `start.zds`; gameplay is a
  manual saved route, not a native demo. The user reports area-entry hitches
  in the original video, the second doorway video, and this profiler run.
- Dedicated copied config and saves live under
  `/private/tmp/gzdoom-blackwater.Y5o1Yg`. Config was copied from the player's
  settings; Metal selection was corrected in that copy before launch. No
  deliberate cache clearing or observer-off control was performed.
- `doorway-metal-01.trace` is **not a qualifying route capture**: duration
  2.873478 seconds, end reason `Target app exited`.
- `doorway-metal-02.trace`: process 66708, 2026-11-09 11:06:18.676–11:06:58.848
  NZDT; duration **40.171498 s**, end reason **Time limit reached**. Time Profiler
  14.1, nominal 1 ms sampling, waiting-thread recording disabled.

The second video, `~/Documents/Screen Recording 2026-11-09 at 10.37.32 AM.mov`,
is 19.8667 seconds, 1023x796 including the window chrome, nominal 59.598 fps.
A contact sheet identifies the corridor/doorway traversal and the menu around
13 seconds. Sparse stills do not measure hitch duration. This video and the
CPU trace are separate runs and cannot be time-aligned as one event.

The trace contains a `G_DoLoadGame` burst at 2.530–3.050 seconds. That is save
reload work, not evidence of the mall-doorway cause. Menu/focus/gameplay windows
are not yet precisely marked. The values below describe the whole capture,
not an isolated gameplay baseline. The launch log also reports missing
autoloaded ACS library `FIRECHECK`; asset warnings were not repaired or silently
changed during the experiment.

## Export and symbol resolution

`xctrace export` succeeds **outside the sandbox** on this host. This is useful
new capability evidence; the historical export failure does not apply to this
successful run. Exported `time-profile` rows contain backtrace address fragments
and leaf labels. IDs/references were resolved, retaining per-sample timestamps
and weights; repeated frames within one stack count once for inclusive totals.

A separate one-second `sample` of the still-running process supplied its image
mapping: executable base `0x102a2b000`, end `0x1038c6fef`. It was used only for
address resolution, **not gameplay timing**. `atos` resolved 4,479 unique engine
addresses against the matching executable. Both symbolication and Instruments
require access to macOS services outside the restricted tool sandbox.

There are 20,876 main-thread samples with nominal 1 ms weight. This is sampled
execution attribution, not a continuous running/blocked timeline; leaf labels
include syscall boundaries. Waiting-thread recording is off. Do not interpret
missing samples as a measured duration for a particular wait, or infer GPU
occupancy or filesystem activity from this capture.

## Preliminary inclusive stack ranking

Denominator: **20,876 ms of main-thread sample weight over the whole capture**.
These are overlapping inclusive stacks, not exclusive categories or percentages
of wall/frame time. For example, material observation is inside draw submission;
sound loading can be inside thinker execution. Do not sum rows.

| Stack | Sample weight | Share of main-thread sample weight |
|---|---:|---:|
| `D_Display` | 12,518 ms | 60.0% |
| `TryRunTics` | 6,233 ms | 29.9% |
| `FrameGraph::ObserveSceneMaterialRead` | 5,857 ms | 28.1% |
| `I_WaitForTic` | 2,265 ms | 10.8% |
| `P_Ticker` | 1,918 ms | 9.2% |
| `FThinkerCollection::RunThinkers` | 1,714 ms | 8.2% |
| `FrameGraph::ObserveResourceRead` | 1,194 ms | 5.7% |
| `OpenALSoundRenderer::LoadSound` | 647 ms | 3.1% |
| `G_DoLoadGame` | 513 ms | 2.5% |
| `HWDrawInfo::CreateScene` | 496 ms | 2.4% |

This capture does not support a claim that ordinary actor simulation dominates
the main thread. Nor does its display-heavy aggregate identify the hitch.

### Candidate A: synchronous lightning sound preparation

Three main-thread bursts enter `OpenALSoundRenderer::LoadSound` underneath
`DLightningThinker::LightningFlash`, with Vorbis/libsndfile decoding leaves:

| Trace-relative interval | First-to-last sample span | Sample weight in sound-loading stack |
|---|---:|---:|
| 8.412–8.504 s | ~92 ms | 92 ms |
| 14.728–14.932 s | ~204 ms | 204 ms |
| 29.530–29.676 s | ~146 ms | 147 ms |

Intervals group sound-loading samples separated by no more than 20 ms. Spans
and sample weights are different quantities; they do not measure an exact
frame duration or the sound function's complete call latency.

Representative chain:

`TryRunTics → G_Ticker → P_Ticker → RunThinkers → LightningFlash → S_Sound →
SoundEngine::StartSound → SoundEngine::LoadSound → OpenALSoundRenderer::LoadSound
→ Vorbis/libsndfile decode`.

`src/playsim/mapthinkers/a_lightning.cpp` invokes `S_Sound` for the level or
temporary lightning sound. `src/common/audio/sound/oalsound.cpp` implements
`LoadSound`. The trace identifies synchronous decoding there; it does not yet
identify the exact sound asset, why multiple flashes incur decode work, cache
lifetime behavior, or whether any burst coincides with the doorway hitch.
These are investigation questions, not grounds to disable weather/sound or
move existing audio calls to workers.

### Candidate B: scene-material observation cost

Of 3,617 samples with leaf `_platform_strcmp`, 3,529 have
`FrameGraph::ObserveSceneMaterialRead` in their resolved engine ancestry.
This is direct caller evidence, not an inference from a hot library function.
The path is `MtResourceBindingManager::ApplyBindings` into graph observation
while drawing flats, walls, decals and 2D content.

`src/common/rendering/hwrenderer/frame/hw_framegraph.cpp` scans prior writes,
pass reads, resource uses and observed uses, comparing canonical names;
`ObserveResourceRead` also performs FString-keyed bookkeeping. The existing
source supports an algorithm/data-structure candidate, not a measured complexity
or speedup claim. `ObserveResourceRead` is nested within scene observation and
its 1,194 ms must not be added to the 5,857 ms as independent cost.

The size of this sampled footprint warrants a bounded measurement of the
bookkeeping path. It does not authorize disabling dependency tracking or
changing renderer output. There is no observer-on/off control, repeated
measurement or frame-aligned event data yet.

## Next experiment

1. Keep the save, binary, assets, settings and manual route fixed. Verify that
   the missing-library warning also belongs to the player's intended setup.
2. Capture one more crossing with the existing `vid_frametrace` and
   `vid_stalltrace` observers, or an appropriate time-aligned system timeline,
   so actual frame spikes can be aligned to sampled sound/observation stacks.
   First verify timer output timestamps and avoid the audit's nested-phase
   double-counting trap. Do not claim an exact alignment from human timing alone.
3. Test the specific prediction: a spike aligned with the 90–200 ms lightning
   decode stack should have a substantial main-thread sound-loading component;
   a doorway spike without that stack rejects sound as that event's cause.
   Track scene-observer cost independently of this event-local test.
4. Repeat matched runs and then GL. Retain video-off timing and separate
   first-visit/warm conditions. This single capture is not the three-repeat
   GL/Metal qualification required by the audit.

The first intervention remains on hold. No worker or playsim redesign follows
from this capture. The next task is event attribution, not another architecture
review.

Artifacts: trace, TOC, profile XML, symbolized sample JSON, symbol maps, video
manifests and analysis scripts are under the temporary benchmark directory;
video originals remain in Documents. The temporary directory is not a durable
benchmark archive. Preserve a chosen final fixture outside it before cleanup.

Follow-up: the [marked long-frame analysis](blackwater-marked-frame-2026-11-09.md)
aligns one 129.6 ms Update gap with a 106.1 ms tick dominated by synchronous
lightning sound loading. That capture has retention/event-loss limitations and
is not yet a complete matched-route baseline.
