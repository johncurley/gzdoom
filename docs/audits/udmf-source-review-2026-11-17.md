# UDMF source review and small optimization candidates — 2026-11-17

Scope: UDMF record parsing, side expansion, scroller bookkeeping and nearby
texture lookup. Source inspection only while LLVM is compiling. No engine
implementation, build, game launch or timing experiment ran in this tranche.
Existing working-tree changes are preserved.

## Correctness finding — P2: textual sector-scroll modes are overwritten

src/maploader/udmf.cpp:2236–2239 and 2266–2269 parse the textual ceiling and
floor scrolling modes with this control flow:

~~~cpp
if (both) mode = SCROLL_All;
else if (visual) mode = SCROLL_Textures;
if (physical) mode = SCROLL_All & ~SCROLL_Textures;
else mode = 0;
~~~

The second if/else always executes. For "both" and "visual", it replaces the
just-selected mode with zero. Subsequent sector-scroller collection/creation
therefore receives no enabled flags for those values. The numeric
scrollceilingmode/scrollfloormode properties use a separate path.

Source-level result table, using a_scroll.h's current constants:

| Text | Existing result | Result implied by named constants |
| --- | ---: | ---: |
| both | 0 | 15 |
| visual | 0 | 1 |
| physical | 14 | 14 |
| unknown | 0 | 0 |

Minimal proposed correction: make the physical comparison an else-if in both
switch cases, retaining unknown-value fallback and case-insensitive matching.
This is a behavior fix, separate from parser optimization. Per AGENTS.md's
halt-and-flag requirement, do not implement it without confirmation of this
contract correction. The earlier approval covered sidedef bounds, not this.

History check: git blame and the introducing patch both identify commit
16378df77c62dd00b79d144956edae285455317a, dated 2023-10-29, titled
"implemented DSDA's sector scrolling properties." Both faulty branches were
added there. This predates the current modernization work; our recent parser
changes did not introduce it. This is a local-history conclusion, not a claim
about the current state of a separately fetched upstream branch.

Qualification needs both floor and ceiling fixtures with nonzero speeds, all
three textual modes and unknown text, mixed case, numeric aliases and repeated
properties in both orders. Observe live texture movement and physical carry
separately, and prove the original path fails both/visual while physical and
unknown controls retain their behavior. The table above is static reasoning,
not a native test result or a new speedup claim.

## First optimization trial: omit unused wall-scroller reverse references

ProcessLineDefs unconditionally sizes siderefs to ParsedSides.Size() (line
2544), then appends each expanded destination-side index into its source-side
list (line 2566). Its only reader is the wall-scroller loop at lines 2792–2797.
By ProcessLineDefs entry, all sidedef blocks and UDMFWallScrollers have already
been parsed. No wall-scroller entries are added during line processing.

Candidate: compute whether UDMFWallScrollers is nonempty, and guard both
siderefs.Resize and siderefs[mapside].Push with that condition. Preserve the
existing full lookup path when wall scrolling exists. Keep source bounds,
destination side expansion, textures, line linking and scroller ordering intact.

This avoids constructing/destroying an outer array of empty TArrays and their
per-source allocation work when no consumer exists. A first Push into each
empty source list currently requests capacity for 16 ints via TArray::Grow.
Shared source sides reuse their list; do not equate destination references with
the number of such initial allocations.

The retained MAP09 TEXTMAP has no case-insensitive xscroll/yscroll occurrences.
Those keys are the only assignments to ParseSidedef's zero-initialized scroll
vectors; UDMFWallScrollers is populated only for nonzero vectors. This supports
MAP09 eligibility from source inspection. It still needs live counters before
claiming an allocation reduction. Earlier lexical source counts are provisional.

Structural prediction for MAP09: siderefs entries fall from the provisional
28,726 to zero; per-source list allocations fall to zero, saving one initial
allocation for each distinct referenced source side (at most 28,726), plus
subsequent growth of shared-side lists where present. Allocation counts and
requested payload are separate from process RSS and elapsed time.

Qualify an ordinary no-scroll map, MAP09, a map with one wall scroller, shared
source sides and per-part/global wall scroll combinations. Require exact
geometry/texture/source-index mapping and matching scrolling behavior on the
populated path. Compare live allocation counts and interleaved unprofiled
parser-conversion timings after LLVM finishes. Do not mix the new sector-scroll
behavior correction into the timing A/B. This candidate can precede the larger
direct-record trial because it removes whole allocations on an eligible path.

## Smaller candidate: duplicate sector initialization

ParseTextMap clears local sector_t at line 2738, then ParseSector clears the
same record at line 1794 before reading any of its fields. The outer clear is
redundant at source level. Retain ParseSector's complete initialization and
defaults. Its removal would eliminate 4,259 logical clears at the provisional
MAP09 count, but compiler optimization may already remove redundant stores.
This is already subsumed by the proposed direct-record sector branch; do not
claim two independent savings or start a separate benchmark for it first.

## Candidates not promoted

- The previously reviewed direct-record construction trial remains useful,
  but its 49,896 logical-copy prediction does not establish physical byte
  traffic. See udmf-direct-record-review-2026-11-17.md for its own contract.
- Changing ParsedSideTextures.Push(st) to an rvalue could use FString's move
  constructor, but these ordinary string copies normally share buffers already.
  Moving resets the source to NullString and updates that reference count.
  Do not describe this as eliminating three string-buffer copies or all
  reference-count traffic per side. No measured benefit currently justifies it.
- Missing-texture diagnostics scan all lines to enumerate side references in
  MapLoader::SetTexture. The scan is gated by lookup failure and a per-name
  warning limit. It is not established as a MAP09 hot path. Replacing it with
  a single backlink needs a separate audit of binary-map/shared-side callers
  and preservation of diagnostic coverage; defer without relevant evidence.

Review result: one source-proven behavior defect, one higher-priority allocation
trial and one redundant-initialization observation. No new native correctness
or performance result is claimed. Whitespace validation applies only to these
documentation edits; earlier build/native passes qualify the earlier fixes.

## Approved implementation, awaiting verification

The maintainer subsequently requested adding the fixes while LLVM compiles.
The two scroll branches and the conditional wall-scroller reverse-reference
lookup are now edited, uncommitted and not rebuilt. This supersedes the
implementation hold for the scroll correction; it does not qualify either
change. See [pending validation](udmf-pending-validation-2026-11-17.md) for
separate patches, scoped rollback and the exact deferred gates.
