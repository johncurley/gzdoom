# UDMF temporary-array investigation — 2026-11-17

Status: maintainer approved the bounds correction; implemented and qualified
on Intel macOS OpenGL and Metal. The initial halt-and-flag finding and
allocation analysis are preserved below. Existing uncommitted scanner and
input-reader work is preserved. Allocation optimization remains separate.

## Input and prediction

The installed AshesBlackwater.pk3 matches the prior handoff's SHA-256:
`af078fab322be816dfc79d50f3ca487f4de27a12384791799be45995add3b2bb`.
Its maps/MAP09.wad TEXTMAP is 5,840,412 bytes, SHA-256
`b1dd53b49928b9372d47cf6a96bfd15965850f86aa826a00ef1f5a5e3dbb1e74`.
Earlier temporary traces and probe binaries are no longer present.

Prediction before counting: the line, side and sector temporary arrays each
require more than ten capacity increases; exact capacity supplied with Grow()
reduces these to one. This predicts allocation requests, not physical copies
or elapsed-time savings. A counting prepass could outweigh any savings.

## Measurements and scope

A diagnostic lexical walk excluding comments and quoted strings counted
top-level blocks. These counts have not yet been checked against live engine
parser counters. A separately compiled C++17 probe uses the repository's actual
TArray header, with small two-integer records and ordinary allocator wrappers.
It pushes that many records, records changes in Max(), and verifies every
record's index/check value after growth. The disabled control omits Grow(n).
The enabled arm checks that Grow(n) leaves Size() zero before pushing.

| Block | Count | Incremental growth events | Final capacity | Exact Grow events | Live records at incremental growth |
| --- | ---: | ---: | ---: | ---: | ---: |
| vertex | 12,789 | 18 | 15,585 | 1 | 31,148 |
| linedef | 16,911 | 19 | 23,377 | 1 | 46,733 |
| sidedef | 28,726 | 20 | 35,065 | 1 | 70,110 |
| sector | 4,259 | 15 | 4,618 | 1 | 9,213 |

The lexical count also finds 800 things. ParsedSideTextures grows separately
with the sidedef count. The last column sums live element counts when capacity
changes; realloc may extend in place, so these are **not measured copies**.
This probe does not use actual geometry record sizes, reproduce allocator
pressure during a load, or establish parser/reload performance. No timing or
RSS improvement is claimed. Repeat runs need not have identical addresses.

TArray::Reserve() increments Count and constructs elements; Grow() changes
capacity without publishing elements. A future capacity optimization must
preserve this distinction. No change to the shared container is proposed.

## Bounds defect found before implementation

In src/maploader/udmf.cpp, ProcessLineDefs() counts non-null line-side
references into sidecount, allocates destination Level->sides with that count,
and sizes siderefs to ParsedSides.Size(). It then tests mapside < sidecount
before indexing both siderefs[mapside] and ParsedSides[mapside].

These are different index domains. Two concrete source-level counterexamples:

- Six source sidedefs, four line-side references, valid source index five:
  the condition rejects a valid source index and clears the reference.
- Sixteen source sidedefs, seventeen line-side references (shared sides make
  this possible), invalid source index sixteen: the condition accepts an
  out-of-bounds source index. It also indexes siderefs out of bounds.

Negative source indices other than the -1 absent-side sentinel also need
explicit rejection. These examples are static proofs of the predicate's
domain error, not claims of reproduced native map crashes. Pointer-encoded
indices and the parser's existing 1 + CheckInt encoding need width review;
do not introduce signed overflow while adjusting the bound.

Minimal proposed contract correction: validate the decoded source index against
ParsedSides.Size(), including its lower bound, while retaining sidecount as
the destination capacity and preserving shared-side expansion/order. Keep the
existing invalid-reference fallback unless explicitly changing its policy.
Require sparse-valid, shared-valid, oversized and negative reference fixtures,
plus a disabled control demonstrating that the original guard fails. Obtain
maintainer confirmation before implementation per AGENTS.md. The maintainer
subsequently approved this correction explicitly; qualification follows.

## Subsequent allocation contract

After correcting the bounds contract, distinguish incremental temporary-array
growth from final allocation/copy. Vertices and sectors copy to final arrays;
lines compact zero-length definitions and relocate references; sidedefs expand
per line-side use and process textures. Their counts cannot be substituted
interchangeably. Do not move parser/world side effects onto workers, estimate
exact counts from raw keyword substrings, or add map-name-only reuse caches.
Any preallocation or ownership-transfer trial needs live parser counts, exact
geometry/save/render parity and interleaved stage/end-to-end timing controls.

## Checks and artifacts

The mandatory build succeeds using getconf _NPROCESSORS_ONLN as the macOS
parallelism equivalent. Adjacent gzdoom.pk3 equality passes. Eight regression
Python tests and thirteen profiling tests pass; git diff --check passes.
Existing pngdiff ResourceWarnings and a libvpx deployment-target linker warning
remain. No native rendered suite or new performance comparison ran; these
checks do not qualify an engine optimization.

Scratch input, lexical counter, counts and native probe are in
/private/tmp/gzdoom-udmf-growth-20261117. Inputs remain caller-owned and are not
added to the repository. The initial probe compile requested C++14 and failed
because TArray requires C++17; the corrected C++17 probe compiled and ran.

## Approved bounds correction and native qualification

ProcessLineDefs now decodes its temporary source-side index in uintptr_t and
compares against ParsedSides.Size(). Negative indices convert to large unsigned
values and reject, while the null (-1) sentinel keeps its existing fast path.
Both sidefront and sideback encoders perform the +1 operation in uintptr_t,
avoiding signed int overflow for INT_MAX, including on 32-bit targets. This is
source-level width reasoning; native 32-bit/Windows/Linux/Apple Silicon runs
are still open. The destination allocation, shared-side expansion, texture
processing, ordering and invalid-reference null fallback are unchanged.

The permanent tools/regression/udmf_sides.py runner generates seven small maps
and inspects their loaded geometry with a ZScript WorldLoaded handler. It
requires actual requested IWAD/backend and map entry, successful engine exit,
five front sides, no backs, five expected texture offsets and five correct
side-to-line links. Cases cover sparse source indices, shared source sides,
out-of-range and negative backs, INT_MIN, INT_MAX and explicit -1 absence.
Each fixture keeps valid front walls so invalid backs exercise the existing
null fallback without requiring a change to invalid-map policy.

Prediction before native runs: all seven fixed cases have those exact geometry
counts; the original binary fails sparse and at least one invalid-index case.
Observed on Intel HD Graphics 6000/macOS Monterey:

| Binary/backend | Cases | Outcome |
| --- | --- | --- |
| Original/OpenGL | shared | PASS, expected geometry |
| Original/OpenGL | sparse | FAIL, exit -11 after dropping valid fronts |
| Original/OpenGL | oversized, negative | FAIL, exit -6 each |
| Fixed/OpenGL | all seven | 7 PASS, exact expected geometry |
| Fixed/Metal | all seven | 7 PASS, exact expected geometry |

The original binary was copied before the bounds edit; both arms include the
earlier scanner/input-reader changes. Manifests preserve binary/package/IWAD
hashes and argv/logs. Initial OpenGL fixtures used ZIP timestamps from creation
time. The final runner fixes ZIP timestamps; the contained map/script entries
are the same across the old/new timestamp forms. No pixel parity, gameplay
smoothness, timing or long-session result is inferred from this geometry test.

Two initial harness attempts are rejected: the archived executable initially
lacked game_support.pk3 and could not select the IWAD; after staging the package,
immediate +quit exited before map loading. The runner now delays quit with
execafter and requires the WorldLoaded geometry marker. These attempts did not
qualify either engine arm. The final runner also owns its save directory and
disables autosaves. No user configuration is edited.

Evidence: /private/tmp/gzdoom-udmf-sides-20261117 contains legacy/gzdoom,
harness-check[2,3], legacy-controls, fixed-gl and final-metal directories.
Summary JSON records each case; failure logs remain alongside successful runs.
Mandatory build, adjacent-package comparison, eight regression Python tests,
thirteen profiling tests and whitespace checks pass after the bounds edit.
The change does not address sector-index validation or other map-loader bounds.

## Source-review continuation during the LLVM build

The [direct-record review](udmf-direct-record-review-2026-11-17.md) selects a
bounded serial construction trial: parse line/side/sector records directly into
their temporary array slots. It distinguishes the predicted 49,896 logical
MAP09 record-copy removals from unchanged realloc growth and final geometry
copies. Implementation and measurement are deferred until the competing LLVM
build finishes; no additional engine or runtime change was made in that review.
