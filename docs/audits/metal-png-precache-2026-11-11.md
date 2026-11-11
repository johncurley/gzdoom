# Metal PNG precache worker

Metal enables `mt_async_png` by default. It moves eligible PNG decoding
into the texture precache queue. Set `mt_async_png 0` to disable worker
decode for the session. The render thread still reads the archive entry and
copies the IDAT chunks. A serial GCD queue inflates and unfilters the stream,
converts RGB or RGBA to BGRA, and runs the existing snapshot postprocessing.
The render thread drains completions at `BeginFrame`, applies texture metadata,
and submits the blits through the existing Metal command queue.

Eligible textures are direct, untranslated, non-upscaled 8-bit RGB or RGBA PNG
images without interlacing or `tRNS`. Shared bitmap precache entries use the
existing path. Admission checks a 16 MiB reservation against the worst-case
job buffers and holds it until the completion is uploaded or discarded.
Unsupported and over-budget requests use the existing capture path. If a
material needs a pending texture, the render thread cancels publication and
uses synchronous first-use loading. A failed decode leaves the texture
available for the same fallback.

`mt_png_precache_stats` reports accepted jobs, submitted uploads, failures,
first material binds with a worker-prepared texture, first-use fallbacks for
pending jobs, and current and peak reserved bytes. The reservation measures
job buffers. GPU allocations, process-wide memory, and OS file-cache usage
are outside that figure.

## Qualification

The prediction was exact output and metadata parity, a working one-byte
negative control, a peak reservation within 16 MiB, and at least 100 ms off
the longest Update-to-Update gap on the first Blackwater MAP09 visit.

The native snapshot self-test made 28 exact PNG comparisons on 14 Blackwater
RGB and RGBA assets using both ordinary and expanded output. It compared
decoded BGRA, final pixels, dimensions, masking, translucency, and hole
areas. The malformed zlib stream was rejected. The existing snapshot
comparison passed 126 cases and detected its one-byte negative control.
The framegraph self-test also passed.

Each timing run started a fresh Intel Metal process in the staging map,
then loaded the same isolated MAP09 save at 1024 × 768 with a 35 fps cap.
The six runs used disabled, enabled, enabled, disabled, disabled, enabled
order, with the same audio settings and active-in-background policy.
`vid_frametrace` recorded the longest Update interval after the load
request. The OS file cache was allowed to warm across runs.

| Metal PNG decode | Longest Update interval per run | Median |
| --- | --- | --- |
| Disabled | 2,733.63 / 2,711.63 / 2,711.66 ms | 2,711.66 ms |
| Enabled | 2,682.32 / 2,678.92 / 2,701.76 ms | 2,682.32 ms |

The enabled median was 29.34 ms lower, about 1.1%. This missed the
100 ms prediction. Each enabled visit queued and submitted 92 PNG uploads.
Four of those textures reached first bind in the observation window, all
after the worker-prepared upload had been submitted. There were zero
first-use fallbacks or failures. Peak reserved job storage was 16,763,612
bytes, with zero outstanding at the end of each run. The original
qualification used an opt-in switch. The maintainer enabled the Metal
default after reviewing the measured gain.

Timing and native self-test artifacts are in
`/private/tmp/gzdoom-png-integration-m8kmd8dd`.

## Rendered output

A static UDMF room used Blackwater’s `WLDLIF6` PNG on its walls.
Precache was enabled in both arms. The enabled counter increased by one
submitted upload and one ready first bind when the room loaded, proving the
visible wall used the worker path. Two disabled and two enabled 800 × 572
captures matched byte for byte after PNG decoding. Flipping one output byte
was detected. Frames and logs are in
`/private/tmp/gzdoom-png-frame-wzjxteus`.

## Upload timing

A supplemental disabled/enabled pair sampled `mt_metrics` before the
upload burst aged out of its rolling window. The largest per-frame
`Upload TexUp` value was 345.587 ms disabled and 311.279 ms enabled.
This CPU metric covers staging copies, blit encoding, and submission.
The corresponding Update gaps were 2,775.89 and 2,662.59 ms. This upload
comparison used one pair. The six-run comparison above provides the
primary Update-gap estimate. Artifacts are in
`/private/tmp/gzdoom-png-integration-inwedwb4`.

The Intel macOS build and whitespace check passed. The OpenGL/Metal
cross-backend MAP06 matrix passed both baseline and SSAO comparisons.

## Default-on verification

The rebuilt Intel macOS binary was tested in two fresh Metal processes
without a CVar override and two with `mt_async_png 0`. Both default runs
queued and uploaded the fixture wall and reported one additional ready
first bind when the room loaded. Both disabled controls queued zero
PNG jobs. All four rendered frames matched byte for byte, and the
one-byte negative control was detected. Logs, arguments and captures are
in `/private/tmp/gzdoom-png-frame-gozp6y8t`. The build, app package
comparison and whitespace checks passed.
