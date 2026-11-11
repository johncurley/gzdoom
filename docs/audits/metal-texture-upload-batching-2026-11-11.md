# Metal texture upload batching

`mt_batch_texture_uploads 1` groups up to 64 completed texture uploads in a
single Metal blit encoder and command buffer. The render thread submits each
group during the existing `BeginFrame` drain, before any draw can sample its
textures. Each upload retains its staging buffer through the existing frame
recycling ring. Texture readiness and framegraph upload observations are
recorded after submission. Mipmap generation stays in sequence with each
texture's copy. The disabled setting uses batches of one.

## Blackwater MAP09 first visit

The prediction was the same upload count, at least 90% fewer extra command
buffers during the burst, and a median Update gap at least 75 ms shorter.
The fixture starts a fresh process on a staging map and loads a copy of the
same Blackwater save at 1024 × 768 and 35 fps. PNG precaching stays enabled.

The existing `mt_debug` counters identified the burst. The pre-change
diagnostic control recorded 1,965 uploads, 1,966 extra command buffers and
316.44 ms of measured CPU upload work in its largest upload frame. A
disabled/enabled pair on the candidate build recorded the following.

| Batching | Texture uploads in burst frame | Extra command buffers | CPU upload time |
| --- | ---: | ---: | ---: |
| Disabled | 1,965 | 1,966 | 325.88 ms |
| Enabled | 1,965 | 40 | 179.94 ms |

The extra-command-buffer counter includes other blit users, such as
lightmaps. Across the observed visit both arms recorded 1,971 texture
uploads. The burst's extra command buffers dropped by 98.0%, with 145.94 ms
less measured CPU upload work in this diagnostic pair. The latency test
uses separate runs with debug output off.

Six fresh-process latency runs used disabled, enabled, enabled, disabled,
disabled, enabled order. The longest `vid_frametrace` interval between the
load request and the end of the observation window was retained.

| Batching | Longest Update intervals | Median |
| --- | --- | ---: |
| Disabled | 2,606.59 / 2,728.14 / 2,947.55 ms | 2,728.14 ms |
| Enabled | 2,367.86 / 2,386.91 / 2,357.55 ms | 2,367.86 ms |

The median decreased by 360.28 ms (13.2%). The slowest enabled run was
219.68 ms faster than the fastest disabled run. Median peak CPU upload time
was 326.787 ms disabled and 170.766 ms enabled. All six runs uploaded 92 PNG
worker results with four ready first binds and zero PNG first-use fallbacks
or failures. The separate 104–119 ms post-load intervals appeared in both
arms. These measurements cover the first visit on Intel Metal.

The pre-change diagnostic, paired diagnostic, and interleaved timing
artifacts are in `/private/tmp/gzdoom-png-integration-vzurxu6e`,
`/private/tmp/gzdoom-png-integration-z5nm0x_7`, and
`/private/tmp/gzdoom-png-integration-btjeknsy`.

## Rendering and release setting

A static UDMF room placed Blackwater's `WLDLIF6` and `CAMR1` PNG textures
on the two halves of the visible wall. Two disabled and two enabled Intel
Metal runs each uploaded both textures and reported both ready at first
bind. All four 800 × 572 images matched byte for byte. The comparison
detected a one-byte negative control. The framegraph self-test passed
in all four runs. The fixture, logs, and images are in
`/private/tmp/gzdoom-png-frame-vo4ytyd8`.

The Intel macOS build and app package comparison passed. Batching now
defaults on for Metal. Set `mt_batch_texture_uploads 0` for the
single-upload control. The qualification used the existing frame and
upload metrics.
The OpenGL/Metal MAP06 baseline and SSAO image comparisons passed.

The rebuilt default was checked on the same Blackwater save with no
batching override and with explicit `mt_batch_texture_uploads 0`.
Both launches uploaded 1,965 textures in the burst frame. The default
recorded 40 extra command buffers and 166.50 ms of CPU upload work. The
control recorded 1,966 and 322.03 ms. Both had zero PNG first-use fallbacks.
The four two-texture fixture captures repeated exact pixel parity, with
both textures ready at first bind. The default and control artifacts
are in `/private/tmp/gzdoom-png-integration-qphpsgl2` and
`/private/tmp/gzdoom-png-frame-6al7cwn6`.
The rebuilt default also passed the OpenGL/Metal MAP06 baseline and SSAO
image comparisons.
