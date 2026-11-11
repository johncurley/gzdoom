# Blackwater warm reload profile with sound retention

Three automated MAP09 reloads on the current Intel Metal build produced Update
gaps of **526, 522 and 559 ms**. The predeclared prediction was three 400–700 ms
gaps with at least 250 ms of main-thread sample weight under `G_DoLoadGame` in
each. All three meet it. UDMF parsing is the largest identified substage.
Reopening the compressed map during validation and setup is a smaller candidate
with a clear boundary for a controlled optimization.

The fixture uses the earlier `auto00.zds` save, a warm process, 1024×768, the
35 fps cap, `snd_asyncprepare 1` and `snd_retainedcache 2048`. The recorder
attached to the game PID with Time Profiler and Points of Interest. Its
25.328-second trace contains a 20.707-second active selection after unpause.
All 662 retained Update markers are consecutive and report active gameplay
with no menu or pause. Deferred console loads issued the three reloads.
All three large gaps contain a single tic advance between their Update
endpoints. The largest later Update gap in the selection is 39.8 ms.

## Attribution

The first row gives elapsed wall time. Every other row gives inclusive
main-thread Time Profiler sample weight in milliseconds, in chronological
reload order. Child rows overlap their parents. Sample weight cannot
establish how much of the remaining wall time the thread spent waiting.

| Path or metric | Reload 1 | Reload 2 | Reload 3 |
|---|---:|---:|---:|
| Update gap, elapsed ms | 525.80 | 522.02 | 558.95 |
| All main-thread sample weight | 461 | 452 | 499 |
| `G_DoLoadGame` | 439 | 432 | 475 |
| `P_SetupLevel` | 349 | 348 | 389 |
| `MapLoader::LoadLevel` | 263 | 263 | 300 |
| UDMF entry point `MapLoader::ParseTextMap` | 116 | 117 | 132 |
| `MapLoader::InitRenderInfo` | 29 | 29 | 36 |
| `P_CheckMapData` | 25 | 25 | 27 |
| Map open during validation | 24 | 25 | 27 |
| Map open during setup | 21 | 21 | 23 |
| `MapData::GetChecksum` | 27 | 25 | 27 |
| `MapLoader::LoadZNodes` | 10 | 9 | 11 |
| `FLevelLocals::UnSnapshotLevel` | 54 | 49 | 48 |
| `SoundEngine::PrepareSound` | 22 | 24 | 23 |
| `D_Display` | 8 | 7 | 12 |
| `FTexture::CreatePixelSnapshot` | 0 | 0 | 0 |
| `HWDrawInfo::RenderBSP` | 0 | 0 | 0 |

The ZIP entry `maps/MAP09.wad` is 1,108,092 compressed bytes and 7,027,217
uncompressed bytes. `G_InitNew` calls `P_CheckMapData`, which opens and
discards a `MapData`. `P_SetupLevel` opens the map again. The stack split
attributes 38–41 ms of `tinfl_decompress` leaf weight to those two map opens
combined and 12–18 ms to synchronous resource reads for sound preparation.
The map checksum contributes another 25–27 ms of inclusive weight. Compare
an owned validated-map handoff against the current two-open path once reader
lifetime, load order, and failure handling have been checked. Validate the
checksum and restored state, then measure the Update gap with repeated
unprofiled reloads.

The texture snapshot zeros describe this warm fixture. The earlier death/reload
profile sampled bitmap acquisition, so cold and first-visit texture work still
needs its own capture before choosing a texture optimization.

## BSP follow-up

In the stationary 19.0–25.2-second gameplay window after the last reload,
`RenderBSP` accounts for 67 ms of main-thread sample weight, with 49 ms under
`RenderBSPNode`. The renderer worker has 60 ms under `HWDrawInfo::WorkerThread`.
`RenderViewpoint` accounts for 537 ms of main-thread sample weight over the
same window. The config enables `gl_multithread` and the worker stack confirms
it ran. Use a moving route for the next renderer capture. The existing
`rendertimes` counters expose BSP traversal, clipping, wall/flat/sprite setup,
and main-thread wait for the worker. `renderstats` supplies geometry and portal
counts. Those measurements can distinguish traversal cost from work produced
by traversal before changing the tree.

## Reproduction

The raw trace, copied save, executable hash and UUID, actual arguments,
ASLR mapping, XML exports, symbolized samples, and `load-attribution.json`
are under `/private/tmp/gzdoom-current-load-20261111-123850`. The launcher and
offline analysis scripts are under `/private/tmp/gzdoom-current-load`. Existing
Update and tick signposts supplied the intervals. The new offline report
groups samples by call path, so this investigation required no engine probes.
The profiled executable hash matches the final sound-retention build. The
build and whitespace checks pass.
