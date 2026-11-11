# Reuse the validated map on compressed savegame loads

The [warm reload profile](blackwater-current-load-profile-2026-11-11.md)
found two opens of the same compressed MAP09 WAD. The savegame path in
`G_InitNew` now passes its validated `MapData` to level setup when the
selected map entry is compressed. `MapData` owns its resource file and
destroys the lump readers before their container. The seekable compressed
reader materializes the archive entry into owned memory. The handoff moves
the owner through the load calls, then releases it immediately after
`MapLoader::LoadLevel`, at the previous cleanup point.

Validation still happens at the original point, and setup still computes
the checksum from the map lumps before deserializing the saved state. The
map entry lives in the filesystem initialized at startup. The reader is
held for this load only. Ordinary starts, level transitions and saves with
uncompressed or external maps acquire the setup reader at the usual point.

The predeclared prediction was two map opens with reuse disabled, one
with reuse enabled, and a median reduction of at least 15 ms in both
load-core wall time and the longest Update gap. The comparison uses one
executable with a temporary switch and counters, three fresh Intel Metal
processes, and three measured reloads per mode in each process. The same
copied Blackwater save, 35 fps cap, audio preparation and 2 MiB sound
retention apply to both modes. A warmup reload precedes measurement.

The load core covers `G_DoLoadGame` from entry through scope exit.
The Update metric is the largest interval between frame update hooks
across each reload. The comparison runs with the profiler disabled.
The command manifests, per-event data, fixture copies and scripts live
under `/private/tmp/gzdoom-map-reuse`.

| Warm Metal process | Load core, two opens → one | Longest Update gap, two opens → one | Gap saving | Largest within-mode gap spread |
|---|---:|---:|---:|---:|
| 1 | 439.11 → 413.99 ms | 519.84 → 498.15 ms | 21.69 ms | 14.23 ms |
| 2 | 435.58 → 403.67 ms | 521.91 → 481.86 ms | 40.05 ms | 16.96 ms |
| 3 | 428.51 → 409.82 ms | 509.90 → 487.69 ms | 22.21 ms | 12.68 ms |

All 18 measured reloads reached active gameplay and recorded the predicted
open count. Every process cleared the 15 ms prediction and the largest
within-mode spread for both primary metrics. Request-to-completed-resumed-loop
time fell by 20.1, 45.4 and 13.6 ms in median. The last gain was within its
16.0 ms spread, so the repeatable claim is scoped to the load core and
longest Update gap.

The OpenGL execution control also recorded two opens disabled and one
enabled. A stored ZIP overlay with byte-identical MAP09 WAD contents
recorded two opens in both modes. These fixtures verify path selection.
The altered-checksum save produced the expected rejection. The initial
runner timed out waiting for that error case to exit after termination,
so the final runner has a bounded cleanup for its own child process.

The final build removes the switch, counters and clocks. The clean executable
loaded the original save on both OpenGL and Metal and rejected the changed
checksum. The build and whitespace checks passed. The final baseline and
SSAO GL/Metal comparisons reported OK with four passing framegraph self-tests.
The current images and logs are in `final-matrix`, and
`final-source-verification.json` records the executable and source hashes.
