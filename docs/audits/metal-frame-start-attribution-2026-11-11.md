# Metal first-visit frame-start attribution

Two fresh Intel Metal processes loaded the same Blackwater MAP09 save from
the KEYPROBE staging map, then performed two warm reloads. Both used the
default PNG worker and upload batching, 1024 × 768, a 35 fps cap, audio
preparation enabled and 2 MiB sound retention. Every retained Update marker
reported level state, no menu, no pause, and an active app.

The prediction was at least 150 ms of inclusive main-thread sample weight
at texture creation and less than 30 ms in buffer recycling. Instruments
Time Profiler and Points of Interest recorded both runs to their 25-second
limits. Existing engine markers correlated the largest Update gap with
main-thread stacks. The executable UUID, hash, load address, input hashes,
and matching source files were retained. Source-line symbolication locates
driver samples at their engine call sites.

| First-visit measurement | Run 1 | Run 2 |
| --- | ---: | ---: |
| Longest Update interval | 2,374.69 ms | 2,262.52 ms |
| BeginFrame sample weight | 356 ms | 338 ms |
| Completed texture drain sample weight | 356 ms | 338 ms |
| Texture creation at the newTexture call | 183 ms | 161 ms |
| Staging buffer acquisition call sites | 35 ms | 49 ms |
| Row copies into staging buffers | 108 ms | 96 ms |
| Completion buffer cleanup | 8 ms | 8 ms |
| S_PrecacheLevel sample weight | 894 ms | 841 ms |
| OpenAL LoadSound sample weight | 835 ms | 788 ms |

Sample weights describe inclusive stacks. The encompassing rows overlap
their children. Row-copy and staging-acquisition values come from the
optimized caller addresses at their source lines. The 413 ms BeginFrame
value from the earlier phase-timer experiment belongs to separate runs.
The residual obtained by subtracting that experiment's upload timer is a
candidate attribution target rather than an exact fixed duration.

At the texture-creation call, the leaf was `mach_msg_trap` for 145 ms and
122 ms respectively. The stacks also include Intel Metal texture and
resource initialization. These samples locate the driver interaction.
CPU execution and blocked-time attribution would require the corresponding
thread-state and scheduler evidence. No large recycling concentration
appeared in either capture. Completion cleanup had 8 ms of sample weight,
including VM deallocation and memory release.

The four warm reload gaps were 479–556 ms. Each had 0–1 ms under BeginFrame
and zero samples under the completed texture drain. The first-visit texture
creation and copy burst was absent from the warm controls.

## The later pause

The two first visits also had separate 116.81 ms and 97.21 ms Update gaps.
Each advanced 35 simulation ticks. OpenAL LoadSound accounted for 48 ms and
37 ms of main-thread sample weight, with decoder work visible in the leaves.
BeginFrame accounted for 1 ms and 0 ms. This confirms that synchronous sound
loading still contributes to the resumed iteration on the current build.

The largest remaining measured preparation stage is level sound precaching.
A bounded follow-up can reuse the independent decoder ownership pattern,
gather a small batch of eligible canonical sounds on the owner thread,
decode on workers, and publish OpenAL buffers on the owner in existing
order. CacheMarkedSounds still needs to finish its required buffers before
returning. The current background sound worker prepares the lightning
candidate after CacheMarkedSounds, so its qualification covers a different
part of the load sequence. Decoder formats, loop settings, aliases, byte
bounds, cancellation and serial fallback need explicit coverage for any
extension.

## Artifacts

The traces, exports, executable, source copies, manifests, symbol maps,
analysis and `frame-start-attribution.json` are in
`/private/tmp/gzdoom-frame-start-20261111-144851`. The offline symbolication
reported a CoreSymbolication service connection warning while resolving the
texture call sites to the matching engine source lines. The capture and
analysis completed. Launcher and source-line attribution scripts are in
`/private/tmp/gzdoom-png-worker`.

This pass used the existing executable and added analysis documentation.
No gameplay or performance change is claimed.
