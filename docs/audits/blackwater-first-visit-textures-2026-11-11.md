# Blackwater first-visit texture acquisition

The first MAP09 load from a small staging map produced a **2.745–2.875 s**
Update gap in two fresh Intel Metal processes. PNG decode is the largest
identified stage in texture acquisition. Sound precaching follows the
texture queue and provides an interval for worker decode. The subsequent
Metal upload burst is another substantial part of the gap.

Each process started on the `KEYPROBE` fixture, loaded the copied Blackwater
`auto00.zds` once and issued two warm reloads. Both used 1024×768, the 35 fps
cap, audio preparation and 2 MiB sound retention. First visit refers to MAP09
texture residency in a fresh process. The operating system file cache was
left in its normal state. Time Profiler attached to the launched PID and
Points of Interest provided Update intervals. Each recording ran to its
25-second limit and captured all three load events. The first process was
unfocused throughout the selected interval with explicit background
activity enabled. The second was focused throughout. Treat the pair as
attribution in two conditions rather than a matched timing comparison.

## Measured paths

The prediction recorded before capture was at least 40 ms of main-thread
sample weight under `CreatePixelSnapshot` in the first gap and at most
10 ms per warm gap. Both first visits and all four warm controls meet it.

| First MAP09 Update gap | Process 1 | Process 2 |
|---|---:|---:|
| Elapsed wall time | 2,874.53 ms | 2,744.85 ms |
| All main-thread sample weight | 2,622 ms | 2,516 ms |
| `G_DoLoadGame` | 1,944 ms | 1,848 ms |
| `hw_PrecacheTexture` | 521 ms | 511 ms |
| `FTexture::CreatePixelSnapshot` | 494 ms | 489 ms |
| `FPNGTexture::CopyPixels` | 475 ms | 473 ms |
| `M_ReadIDAT` | 356 ms | 369 ms |
| Archive decompressor inside texture snapshot | 47 ms | 41 ms |
| `S_PrecacheLevel` | 938 ms | 881 ms |
| `OpenALSoundRenderer::LoadSound` | 880 ms | 830 ms |
| `MtTextureManager::PerformAsyncGPUUpload` | 611 ms | 603 ms |

All rows below wall time are inclusive main-thread sample weights. Nested
rows overlap. In the upload path, `mach_msg_trap` accounts for 304 and
282 ms of leaf sample weight, and `memmove` accounts for 119 and 128 ms.
The trace lacks thread-state and GPU timing data to assign driver wait or
GPU durations. The existing texture worker has 106–116 ms under
`ProcessPixelSnapshot` during the first gap.

The warm loading gaps measure 506–515 ms with zero samples under
`CreatePixelSnapshot`. Both first visits also have a separate 113 ms gap
after the load and wipe, with 35 tics advanced and 46–54 ms of main-thread
weight under OpenAL's synchronous sound loader.

## Candidate for an isolated worker test

Start with direct, untranslated PNG images encountered during material
precache. Give a worker owned encoded bytes, validated dimensions and
private decode scratch. `M_ReadIDAT` currently allocates its scratch with
`TArray`, which updates shared GC allocation accounting. The worker version
needs caller-supplied scratch or another worker-safe allocation contract.
`FImageSource::GetCachedBitmap` also changes shared precache lists and
reference counts. Keep resource lookup, image-cache publication and
texture metadata updates on the owner thread.

The last main-thread texture snapshot sample precedes the first upload sample
by about 1.0–1.1 seconds in both captures. Sound precaching accounts for most
of the sampled work between them. This provides a place to test overlap. The
timestamps bound the observed phase sequence, while actual task readiness
depends on the worker and the workload.

Use a bounded batch and byte budget for the prototype. Compare decoded pixels,
transparency and dimensions against the serial PNG path before measuring
latency. Count admitted jobs, results ready at first use, synchronous
fallbacks and peak bytes in flight. Keep the existing owner-thread completion
handoff at `BeginFrame`, with the backend upload recorded before the first
sampling pass. A synchronous first-use request currently cancels the pending
publication for its hardware wrapper. Include that case in the test to catch
duplicate decode and stale uploads.

Measure the longest first-visit Update gap and the upload phase as well as
decode. The current upload path submits a blit command buffer for each
completed texture. Capture upload command counts and staging bytes before
deciding whether to batch those submissions.

## Artifacts

The two traces, manifests, executable, image maps, exports and
`texture-attribution.json` are under
`/private/tmp/gzdoom-first-visit-20261111-130441`. Launcher and analysis
scripts are under `/private/tmp/gzdoom-first-visit`. Existing markers and
sample attribution supplied the measurements. The build and whitespace
checks passed.
