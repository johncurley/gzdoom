# UDMF input-copy investigation — 2026-11-12

Status: **Qualified on Intel macOS Metal/GL; scanner EOF fix and direct UDMF input enabled.**
The initial blocked trial and its baseline restoration are preserved below.
The maintainer approved a bounded scanner fix before further implementation.

## Measured motivation and bounded proposal

The shared-load profile attributes 58/57 ms of memory-move leaf samples to UDMF
parsing. Closer call-site inspection finds input acquisition and temporary
line/side/sector array growth, rather than zero-length-line deletion, as the
main sampled copy sites. These are inclusive samples, not exclusive savings.

The existing input path calls `FScanner::OpenMem(name, map->Read(ML_TEXTMAP))`:
a temporary TArray is read from the map, then copied into an owned FString.
Blackwater MAP09's TEXTMAP is 5,840,412 bytes and ends with newlines without BOM.
The proposed first change reads directly into an owned FString and calls the
existing `FScanner::OpenString`; geometry arrays and scanner normalization stay
unchanged. The input reader checks closed/negative/oversized/failed-seek/short
read cases before scanner publication. This does not introduce worker threads.

Prediction recorded before implementation: eliminate one 5,840,412-byte
intermediate allocation/copy per MAP09 parse and reduce median acquisition time
by at least 10%. The initial trial stopped before timing qualification; subsequent corrected
measurements are recorded below.

## Oracle and failed control

A native input/scanner oracle compares the legacy copy path and direct owned
string path for ten byte sequences in both GetString and GetToken modes.
It compares token bytes/types, numeric values, lines and crossing flags, repeats
candidate scanning from a saved position, closes readers and poisons source
memory before scanning to exercise ownership. Cases include empty input, BOM,
trailing NUL/newline, escaped strings, embedded NUL and unterminated constructs.
Five explicit bad readers exercise rejection. A fault mode appends tokens to
the candidate input; it must fail.

The fault arm fails as intended: 3 comparisons pass, 17 fail; all five bad
readers reject. The normal arm has 19 comparisons pass and one fail, with all
five bad readers rejected. Debugging isolates case 8 in GetToken mode:

~~~text
Input: x="unterminated
Legacy:    token x at line 1, = at line 1, unterminated at line 2
Candidate: token x at line 1, = at line 1, unterminated at line 7
~~~

Token text/type/length match; line counts differ. Candidate rewind reproduces
its own output. This is not a failure caused by the source-poisoning check or
unsupported rewind-after-error behavior. It prevents claiming malformed-input
scanner parity under a changed allocation layout.

## Source-level EOF defect and required contract correction

`src/common/engine/sc_man_scanner.re` defines `YYFILL(n)` as a no-op, assuming
that a trailing newline is sufficient. Its quoted-token rules admit newlines
and NULs in their body and search for a terminating quote. Unterminated quoted
input can therefore drive the generated lexer beyond the logical input end.
The `string_const` action counts newlines up to the lexer cursor, so out-of-input
bytes can affect the reported line. Allocator layout changes expose different
results. Normalizing a final newline is not an adequate EOF guard for this rule.
This is an existing shared-scanner boundary problem surfaced by the new oracle;
it is not safe to ignore the mismatch or narrow the test to the happy path.

Minimal proposal: handle quoted-token EOF with explicit input bounds in the
scanner source, preserving valid escaped/multiline strings and name-constant
behavior, and add a guarded-boundary/unterminated-input regression with a
failing legacy control. Do not simply exclude embedded NUL or change valid
multiline semantics to make the test pass. Revalidate scanner clients before
resuming the UDMF optimization. The maintainer subsequently approved this contract correction under
AGENTS.md's halt-and-flag protocol. The implementation and evidence follow.

## Evidence

`/private/tmp/gzdoom-udmf-input-20261112` preserves the original source/binary,
contract, clean candidate, failed instrumented source/binary/hash/patch,
scanner sources and generated native logs under `scanner-check` and
`scanner-debug`, plus `blocker.json`. The experimental control, oracle command
and timing probes are absent from the restored production source. The restored
baseline build is recorded in `build-restored-baseline.log`.

## Approved bounded scanner correction

The two double-quoted GetToken rules (normal and state mode) now dispatch on
an opening quote and scan explicitly within ScriptEndPtr. They preserve the
original longest finite match: a backslash-prefixed quote can terminate the
token or allow it to continue, while an unescaped quote ends it. Existing token
publication, string decoding, line counting and state transitions remain shared.
Embedded NUL and multiline content stay supported. If no closing quote exists
inside the input, the scanner advances to EOF and reports an unterminated string.
The nonfatal-error mode returns false even when ScriptError returns. The patch
does not claim to repair every possible scanner boundary outside these rules.

A permanent `scanner_quoted_selftest` supplies real buffers ending immediately
before an inaccessible POSIX page. Legacy lexer control exits with SIGBUS
(return code -10) at the first malformed quoted token. Its 28 valid corpus
cases have zero failures and hash `936d9fb9e39868b8`. The fixed lexer preserves
that hash and passes 12 malformed boundary combinations plus 8 valid/edge
boundary cases, covering normal/state and throwing/nonfatal modes. Windows uses
ordinary owned input for this diagnostic (`guarded=0`); physical guard coverage
is qualified on Intel macOS only so far.

The first fixed test run already passed the 12 malformed checks and matched the
valid hash, but its eight valid-boundary assertions assumed End became true
when consuming the last newline. Existing behavior sets End on the following
call. The corrected test exercises both calls; no scanner behavior was changed
to accommodate that test error. Both runs remain in the evidence directory.

## UDMF reader parity and lifetime

With the scanner boundary corrected, the original UDMF oracle passes all 20
comparisons and all five bad-reader rejections; the deliberate appended-token
fault still fails. Both real Blackwater MAP09 byte oracles (Intel Metal/GL)
match all 5,840,412 original TEXTMAP bytes before scanner publication. Scanner
normalization still runs through the existing OpenString entry point.

The reader is borrowed only during acquisition; the returned FString owns its
bytes. The source reader may close and source memory may change before token
consumption, as exercised by the native oracle. Read lengths must be nonnegative
and at most INT_MAX, matching scanner token/offset width. Failed seek, short
read and closed input reject before publication. Previously those malformed
cases could pass uninitialized/truncated data into parsing; their newly explicit
rejection is documented, not claimed as byte-for-byte legacy error equivalence.
No level arrays, parser ordering, FName cache, VM ABI, actor ticks or job ownership
change. One full-size intermediate TArray allocation and copy is removed; this
is requested payload bytes, not a measured process RSS reduction.

## Interleaved input-stage measurement

Both arms include the same scanner EOF fix. Temporary controls select only
legacy OpenMem(map->Read) versus direct FString acquisition. Three independent
processes each exclude their initial load and run off/on/on/off/off/on, with
three samples per arm. There is no Instruments recording, marker stream, PCM
audit or manual input. Sound defaults stay enabled and retention is zero.
These are warm capped Intel Metal loads, not cold-cache or foreground gameplay.

| Process | Legacy input median/range | Direct input median/range | Input reduction | Full parser-body medians legacy/direct |
| --- | --- | --- | ---: | --- |
| 1 | 7.442 ms / 7.038–8.226 | 3.382 ms / 3.378–3.454 | 54.6% | 110.838 / 107.262 ms |
| 2 | 7.482 ms / 7.343–7.965 | 3.452 ms / 3.329–3.462 | 53.9% | 108.691 / 106.951 ms |
| 3 | 7.365 ms / 7.143–7.365 | 3.336 ms / 3.312–3.337 | 54.7% | 112.014 / 105.805 ms |

The declared >=10% acquisition target passes in all three processes, with about
4.03–4.06 ms lower per-process median. All acquisition ranges are separated.
Full parser-body ranges overlap in processes 2/3: retain them and do not claim
a precise full-parser or end-to-end reload saving. The wrapper timer includes
parser construction and input acquisition but excludes wrapper-local parser
cleanup, later world restoration, rendering/wipes and post-load catch-up.
Oracle timings are excluded from this table because verification rereads input.

The temporary switches/timers are qualification tools, not shipping options.
Final production code uses the direct path without new CVARs or a cache.
Array growth/copy inside the parser remains a separate, larger follow-up.

## Corrected qualification artifacts

`/private/tmp/gzdoom-scanner-eof-20261112` retains the scanner contract,
legacy/fixed sources, SIGBUS control, corpus hash, initial/corrected guard runs
and build logs. `/private/tmp/gzdoom-udmf-input-20261112` adds real Metal/GL byte
oracles, three-process timing rows, manifests, qualified probe source/binary and
hashes. Initial failed UDMF evidence and restored-baseline logs remain intact.
Final rendered/native qualification follows below.

## Final shipped-code qualification

Temporary controls, raw-input oracle and timing counters are removed from
production. The final native Metal and OpenGL runs each pass all five resource
modes and four cold sound controls. Both default-mode native scanner commands
preserve the 28-case token hash and pass 12 EOF plus 8 valid-boundary checks;
UDMF normal mode passes 20 comparisons/five rejections while the explicit fault
mode fails. Existing audio PCM/RNG/budget/reset, texture and graph checks pass.

The static UDMF fixture has four byte-identical decoded captures per backend
across legacy/direct/repeat arms, plus two final-build captures per backend
matching the qualified candidate after probes were removed. Native dimensions
are 800x572; nonuniform rendered content and actual KEYPROBE map entry are
required. Cross-backend images have their normal backend-specific differences;
this is within-backend input-path parity. Artifacts are `image-summary.json`,
`final-image-summary.json`, manifests, logs and original PNGs.

Python regression controls: 8 PASS; profiling parser controls: 13 PASS.
Mandatory final build, engine-adjacent package equality and whitespace checks
PASS. The final source/executable/hash snapshot is retained separately from
the measured probe. Linux/Windows/Apple Silicon native runtime qualification
and a full USDF conversation fixture remain open. No demo recording was assumed.
