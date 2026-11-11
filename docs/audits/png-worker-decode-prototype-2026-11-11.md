# Owned PNG decode prototype

Two worker threads decoded the fixture set in about **51 ms**, down from
**83 ms** for one thread. Every raw RGB or RGBA byte matched the serial IDAT
decoder on 192 Blackwater assets. This is the isolated decode experiment
selected by the [first-visit profile](blackwater-first-visit-textures-2026-11-11.md).

The fixtures are 96 RGB and 96 RGBA images sampled across eight size bands
per format from the mod archive. The admission script validates chunk lengths
and CRCs, and selects 8-bit, non-interlaced images without a `tRNS` chunk.
The manifest records every selected archive path, dimensions and input hash.
The 192 decoded images total 12,841,911 bytes. These fixtures sample the
archive inventory. A live precache capture is needed to measure coverage
among textures MAP09 actually queues.

The prototype prepares owned IDAT bytes and pixel buffers before starting the
workers. Each worker uses a private miniz inflater and the engine's row
unfilter routine. It checks completion, compressed bytes consumed, exact
decoded length and filter values before accepting the output. The reference
uses the actual `M_ReadIDAT` and row routines extracted from `m_png.cpp`, with
an in-memory reader and scratch container in the standalone harness. Workers
join before the harness compares results in fixture order. The two bounded
batches peak at **15,583,474 bytes** of job storage and inflater state against
a **16 MiB** budget. Test fixture and reference storage is outside that budget.

The prediction recorded before measurement required exact bytes, detection of
a deliberate pixel change, rejection of bad streams, and at least 15% less
median decode time with two threads in each of two fresh processes.
The negative controls change a pixel, truncate compressed input, use an
invalid filter, and compress shorter and longer row streams than declared.
All controls behave as expected.

| Run | One thread | Two workers | Median reduction | Largest within-mode spread |
|---|---:|---:|---:|---:|
| 1 | 83.307 ms | 50.913 ms | 38.9% | 3.352 ms |
| 2 | 83.074 ms | 51.169 ms | 38.4% | 2.886 ms |

Each arm has seven measurements, alternating order with the opposite
starting order in the second process. Each measurement sums the two batches'
decode times, including thread startup and join. Input parsing, allocation of
job buffers, and reference comparisons happen outside the timer. These
results establish raw decode parity and a speedup for prepared inputs.
The next parity check covers engine BGRA conversion, transparency, and the
final texture result.

The next engine test should construct an eligible PNG job at Metal's texture
queue and feed its result through the existing pixel snapshot and upload path.
Test byte parity at the final texture boundary and count jobs admitted, ready
at first use, and superseded by synchronous first use. Keep the byte budget
through completion, count upload commands, and measure the first MAP09 Update
gap. The renderer can record the upload in the framegraph after publication.

For sound, `S_PrecacheLevel` currently calls `CacheMarkedSounds` synchronously
before requesting lightning preparation. A parallel precache experiment can
keep that completion guarantee. Resolve sound IDs and open decoders on the
owner thread, give separate decoder instances and PCM buffers to workers,
then publish OpenAL buffers on the owner thread in the existing order. The
current selected-effects worker provides the decode/publication boundary.
The earlier Vorbis handoff controls matched PCM and format signatures with
two independent decoder reads in flight. Qualify the actual precache asset
set and measure sound and texture batches together before choosing worker
counts on this host.

The sources, generator, manifests, raw timings and sanitizer logs are under
`/private/tmp/gzdoom-png-worker`. AddressSanitizer, UndefinedBehaviorSanitizer
and ThreadSanitizer passed with the harness and miniz instrumented. The engine
build and whitespace checks passed.
