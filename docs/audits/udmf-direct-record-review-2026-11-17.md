# UDMF direct-record construction review — 2026-11-17

Status: source review and trial contract only. No implementation, build or
runtime experiment in this tranche. The maintainer reports LLVM compiling on
the reference machine; performance measurements wait until it finishes and
the machine settles. Existing scanner/input and qualified sidedef-bound changes
remain intact.

## Candidate selected for a bounded trial

In UDMFParser::ParseTextMap (src/maploader/udmf.cpp), the linedef, sidedef and
sector branches parse into local geometry records, then Push() copies each
record into its temporary TArray. Instead, reserve one constructed element and
pass its address and original index to the existing parser. For example:

~~~cpp
unsigned index = ParsedLines.Reserve(1);
ParseLinedef(&ParsedLines[index], index);
~~~

Apply the same pattern to ParsedSides and ParsedSectors in one serial
record-construction trial. Keep intmapsidedef_t local and its existing Push()
after ParseSidedef; its three FString members have a separate ownership path.
Leave things, vertices and vertex metadata unchanged. Do not combine this with
growth-policy changes, a counting pass, final-array ownership transfer, workers
or a persistent cache.

This removes one logical local-to-array copy per eligible definition. Based on
the earlier diagnostic MAP09 counts, the structural prediction is 16,911 line
copies + 28,726 side copies + 4,259 sector copies = **49,896 logical record
copies removed per parse**, with identical object counts and array capacities.
First confirm those counts with live parser counters. This is not a prediction
of 49,896 machine memcpy calls: inlining, scalar stores and copy elision can
change generated code. Physical byte traffic and elapsed savings need evidence.

## Copy boundaries distinguished by the review

| Stage | Current behavior | Proposed trial |
| --- | --- | --- |
| Local geometry to temporary array | Parse local record, Push copy | Parse constructed array element |
| Temporary-array growth | Grow/DoResize uses M_Realloc | Same policy and capacities |
| Vertex publication | Allocate final vertices and copy | Same |
| Sector publication | Allocate final sectors, memcpy, attach extsectors | Same |
| Line publication | Remove zero-length lines, relocate vertices, copy final lines | Same |
| Side publication | Expand source sides per line use and process textures | Same |

The previous profile attributed substantial copy samples to array growth. This
candidate does **not** remove those growth events, and the profile does not
separately establish the cost of local-record copies. It earns a trial through
its small scope, not an existing measured speedup. Reject it as a performance
change if its benefit cannot be distinguished from noise.

## Static lifetime and indexing review

- Reserve(1) returns the old Count and increments Size(). Capture its returned
  index once; passing Size() afterward would be an off-by-one error. Grow(1)
  alone does not construct/publish an element and is not interchangeable here.
- ParseLinedef, ParseSidedef and ParseSector initialize their destination with
  memset and their existing defaults. Reserve's default construction must use
  the same record type as the former local. Remove the outer sector memset
  only as part of removing that local; retain ParseSector's initialization.
- These arrays are private to UDMFParser. Searches of their uses find no reads
  of their Size() inside those three parsing functions. User keys and tags use
  the explicit index. The translated-line path operates on the supplied record
  through FLevelLocals::TranslateLineDef; its reviewed implementation does not
  append to ParsedLines or retain the record pointer.
- A pointer to the reserved element is used only during that parse call. No
  additional append to that same geometry array occurs during the call. Later
  growth can move it, as it already does for existing records; no pointer may
  be saved across a later Reserve/Push.
- Side scroll requests retain indices in a separate array, not pointers into
  ParsedSides. Sector scroll/thrust requests likewise retain indices for later
  processing. MapLoader::SetTexture(sector, index, ...) updates the supplied
  sector and missing-texture tracker without publishing that sector pointer.
- The reservation makes one private partial record visible to parser cleanup
  on error. MapLoader::ParseTextMap creates a fresh stack-local UDMFParser for
  every call; the wrapper does not retry parsing on that instance. Verify error
  exits retain the same diagnostic and discard the parser. Do not introduce a
  partial record into final level arrays or a new recovery path.
- The side texture record remains local until successful parsing and is then
  appended as before. Side/texture array sizes must match at each completed
  block and at ProcessLineDefs entry. No change to FString ownership, GC pointer
  handling, record layout, parsing order or name/RNG consumption is proposed.

## Why the other candidates wait

Exact preallocation requires a trustworthy count before parsing, or a resource
identity and invalidation contract for reused counts. A raw keyword scan does
not qualify as a UDMF parser, and another full scan has its own cost. The earlier
capacity probe did not measure that tradeoff.

Moving final vertex/sector storage could remove final copies, but also changes
capacity retention and ownership. FLevelLocals::ClearLevelData calls Clear(),
which retains capacity; current final Alloc() can reuse that storage on later
loads. A transfer trial must account for both temporary slack and discarded
reusable level capacity. It should be measured separately.

Zero-length-line compaction is a different algorithmic candidate. Its ordering,
linemap and reference effects need their own fixtures; earlier attribution did
not identify deletion as the main sampled copy site. Keep it out of this trial.

## Qualification after LLVM finishes

1. Preserve a matching bounds-fixed baseline binary/source/package snapshot.
   The older pre-bounds binary is a correctness control, not this timing arm.
2. Add temporary, narrowly scoped record counters and a selectable old/new
   construction path. Verify the live MAP09 counts and unchanged capacities.
   Keep the same scanner/input/bounds fixes and all other settings in both arms.
3. Compare ordered logical geometry values, tags, user keys, texture properties,
   source-to-expanded-side mapping and scroll/thrust definitions. Normalize
   references to indices; do not hash raw structs containing pointers/padding.
   Include translated and extended namespaces, duplicate properties, zero-length
   lines, shared sides and a truncated-block error. Alter a defined fixture
   property in a diagnostic control and require the comparison to detect it.
4. Repeat within-backend fixed-view image captures on Metal/GL, at least two
   identical captures per arm before cross-arm comparison. Require zero decoded
   pixel differences. Re-run the permanent sidedef and scanner/input controls
   and relevant resource suite, plus the mandatory build and package checks.
5. Predict a **>=1% reduction in warm geometry-block-loop median time** as the
   trial's usefulness target, not an established result. Use three independent
   processes, excluding initial map entry in each, with off/on/on/off/off/on
   ordering. Time the same full block loop in both arms, including candidate
   Reserve and legacy Push. Report ranges and baseline repeat spread. If the
   result overlaps noise, retain that finding and do not claim improvement.
6. Keep temporary geometry-loop timers separate from input acquisition, final
   geometry conversion, parser destruction and whole-load elapsed intervals.
   Repeated MAP09 map entries can qualify the parser stage; save-reload claims
   additionally require an identified, matching frozen save workload. No such
   new save-load run has been established in this review. Remove trial switches
   and counters before final production qualification.

No benchmark, compiler workload or game instance was launched for this review.
Only source reads and documentation edits ran. Prior build/native results in
the array/bounds audit qualify that earlier change, not this unimplemented trial.

The [follow-up source review](udmf-source-review-2026-11-17.md) identifies an
independent sector-scroll mode defect and a smaller allocation trial: skip the
wall-scroller reverse-reference table when no wall-scroller consumer exists.
Prioritize measuring that allocation trial before this direct-record trial;
keep the scrolling behavior correction separate from either performance A/B.
