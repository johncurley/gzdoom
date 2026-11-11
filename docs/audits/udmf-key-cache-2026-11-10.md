# Bounded UDMF/USDF key-name cache — 2026-11-10

Motivation: the confirmed normal Blackwater reload has 650 ms of load ancestry
in an 863 ms Update gap, including 203 ms in UDMF parsing and 70 ms in FName
lookup across the load subtree. Source inspection finds repeated property-key
interning in `UDMFParserBase::ParseKey`. The separate post-load 29-tick/sound
batch is not changed. See [reload attribution](blackwater-combat-reload-2026-11-10.md).

## Contract

Cache resolved FName values for repeated exact key spellings within a parser
instance. Copy keys out of the scanner's mutable buffer. Heap-backed FString
characters survive owning-TArray growth; declaration order destroys the TMap
before its owning strings. FName indices remain stable for the name manager's
lifetime. No world objects, geometry or parsed map state are cached between
loads, and no name-table operations move to workers.

Case-insensitive identity remains FName's responsibility on a miss. Exact-byte
cache matching deliberately avoids adding new case-folding behavior; differently
cased keys may occupy separate cache entries but resolve to the same existing
name. Cache at most 256 keys, each at most 128 bytes, per parser. Excess/long
keys use the original lookup. The byte bound excludes string terminators and
container/allocator metadata; it is not a claim about total process memory.
Null input retains the original FName lookup's initialization behavior.

The base parser is shared by UDMF maps and USDF dialogue. ParseKey's token,
value, numeric-sign, semicolon and block handling are unchanged; only resolution
of the already-scanned property name is substituted. Existing identities and
first-intern ordering are preserved. No VM bytecode/native binding layout,
actor order, tick scheduling, name-manager implementation or filesystem policy
changes. The cache is ordinary owner-thread TArray/TMap/FString storage.

## Predictions before measurement

At least 95% of actual MAP09 property-key resolutions served from the bounded
cache and at least 10% less full UDMF parser-body wall time in interleaved
controls. Every cached FName must match the original lookup in a separate oracle
run. A deliberately wrong cached value must fail the self-test. Repeatable
static UDMF scene pixels must match within each backend with cache off/on.

The temporary control bypasses only this cache and directly constructs FName
from the same scanner string. The same executable/parser body is used for both
arms. Temporary counters record key calls, hits and original-name calls. An
optional oracle recomputes FName on every hit and aborts on mismatch; do not use
its slower timings as performance evidence. All temporary switches/counters,
commands and the test subclass are archived and removed from final source.

The timer covers construction and the complete UDMFParser::ParseTextMap body,
including its input acquisition, but stops before wrapper-local parser cleanup.
It does not measure complete G_DoLoadGame, subsequent restoration, display/wipe,
post-load tick batches, GPU work or frame tails. Parser cleanup is still part
of the real load and no end-to-end saving is inferred from this isolated timer.

## Correctness controls

The base-parser self-test exercises copied-key lifetime after caller mutation,
case variants, null/empty strings, 300 distinct names against the 256-entry
limit, the 128/129-byte boundary, repeated temporary-string arguments, parsed
negative-float/integer/string values and USDF-style block-key handling. The
fault mode returns NAME_None for cache hits and must fail; the normal mode must
pass. This covers the shared key/parser boundary, not a full USDF conversation
integration fixture or a demo-determinism proof.

The first harness tried to switch the fault via repeated command-line CVARs;
startup CVAR processing applied the final value before queued commands. Both
self-tests therefore ran without the fault, and that attempt was rejected.
The corrected command takes the fault mode as an explicit argument and sets it
inside the self-test. Both Metal and GL logs now show FAIL with fault=1 and PASS
with fault=0. Actual maps run with fault disabled.

Real Blackwater MAP09 oracle loads on Intel Metal and OpenGL each process
259,103 key resolutions: 258,990 cache hits and 113 original FName calls, with
zero identity mismatches. Cache-hit share is **99.956%**. In oracle mode every
hit also executes the reference lookup; the reported original-call counter
counts cache misses, not those extra verification calls.

## Parsing time

Three fresh Metal processes each load the same copied save once for warmup,
then six times with cache modes 0,1,1,0,0,1. Three measurements per mode per
process, with deferred exec files applying the mode at runtime before each load.
The log verifies the actual mode, call counts and identities; an argv value is
not assumed to prove runtime selection. Source/mod/configuration are fixed;
manual input is absent. Loading and other stages outside the timed parser are
not included. No native profile or per-key oracle is active during timing.

| Process | Original median | Cached median | Reduction |
|---|---:|---:|---:|
| 1 | 174.732 ms | 113.611 ms | 35.0% |
| 2 | 168.958 ms | 110.089 ms | 34.8% |
| 3 | 170.514 ms | 115.031 ms | 32.5% |

Across the nine measured loads per mode, original range is 167.839–175.180 ms;
cached range is 108.460–117.098 ms. The fastest original remains slower than the
slowest candidate by about 50.7 ms, exceeding within-mode spread and the 10%
threshold. Every measured candidate load has the same 258,990 hits / 113 misses.
This qualifies a **roughly 55–61 ms reduction in this parser-body stage**, not
a 33% reduction in reload latency or a gameplay smoothness improvement.

## Static image oracle

An independent, temporary UDMF room fixture exercises repeated geometry/property
keys with static textures, no player sprites and paused gameplay. Capture twice
per cache mode on each backend and compare decoded pixels. The fixture is not a
performance workload. It is stored only under the temporary artifact directory.

Initial attempts exposed sizing assumptions: a minimal new INI selected an
1152×720 drawable; the established matrix launcher pins an 800×600 outer window,
whose actual drawable is 800×572 on this host. Existing matrix captures confirm
that size. Initial attempts are retained and excluded; the final image oracle
uses the actual fixed drawable dimensions, with a discarded warmup per backend.
No renderer sizing code or golden baseline is changed.

Artifacts: `/private/tmp/gzdoom-udmf-keys-20261110`, including pre-change source
and executable, oracle/self-test logs, rejected initial harness attempts, raw
per-load timings, script arguments/INIs, `probe-summary.json`, static fixture
and image comparisons. Scripts/probes/fixtures are not added to the repository
or build tree. The parser remains serial; this is algorithmic/locality work,
not a new threading claim.

## Final verification (completed 2026-11-11)

All four static-fixture images per backend (two control and two candidate)
are byte-identical after decoding. Metal pixel SHA-256 is
`378d17ad80bc7a208916ed79f7baedca91cf43b3da7320d529ffe9645fbce269`;
OpenGL is `f5848d70d46b53a42b3e2d4fe6b1b563e9194ba99bc8901f7a10d7156124fb0c`.
These are within-backend cache comparisons, not cross-backend pixel identity.
The image is nonuniform, the fixture map is confirmed loaded, and the actual
UDMF path is exercised. It is not a screenshot of the startup screen.

The final build passes with all temporary parser switches, counters, timer,
self-test/fault commands and test subclass removed. Two final captures on each
backend match that backend's qualified candidate pixel hash exactly. Final
MAP06 baseline/SSAO cross-backend comparisons report OK; graph self-tests in all
four logs pass. The 13 profiling-tool tests and whitespace checks pass. Final
binary/source/control/probe/save/mod hashes are retained in
`final-artifact-hashes.json`; final regression logs/images are copied into the
artifact directory. Intel macOS is tested; Linux/Vulkan, Apple Silicon, full
USDF conversation integration and cross-build demo determinism are not claimed.

The cache is retained. It reduces repeated serial name resolution while keeping
bounded per-parser ownership and the original fallback/identity rules. The
[matched end-to-end follow-up](blackwater-reload-end-to-end-2026-11-11.md)
qualifies 65–72 ms less request-to-completed-resumed-loop latency across three
warm Metal process comparisons. The median per-reload maximum Update gap falls
from 570 to 510 ms; substantial loading gaps remain. The resumed-loop cost has
no consistent improvement. The subsequent
[sound attribution](postload-sound-attribution-2026-11-11.md) identifies repeated
small effects and validates a temporary precache control; generic retention/
preparation and stable heavy-combat/normal level-exit fixtures remain separate
work. No worker jobs or tick policy changes are made.
