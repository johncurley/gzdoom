# Engine CPU profiling tools

Start with the [source dependency audit](../../docs/audits/engine-performance-dependencies-2026-11-08.md)
and [replay fixture procedure](../../docs/performance-replay-fixtures.md).
The [first Blackwater capture](../../docs/audits/blackwater-doorway-profile-2026-11-09.md)
is preliminary evidence, not a qualified comparative baseline.
The [instrumentation audit](../../docs/audits/engine-instrumentation-2026-11-09.md)
records timer correctness/cadence limits and the ordered plan for minimal new
frame/tick correlation and targeted events.
The first repair balances the two sight-query early returns and relabels VM
history as display samples. Its live failing-control/passing-candidate evidence
is recorded at the end of that audit; timeline correlation is still pending.

## Export and summarize an existing trace

The analyzer uses Python's standard library. It changes no engine behavior and
adds no runtime observer. For the current capture:

```sh
xcrun xctrace export \
  --input /private/tmp/gzdoom-blackwater.Y5o1Yg/doorway-metal-02.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' \
  --output /private/tmp/gzdoom-blackwater.Y5o1Yg/doorway-metal-02-profile.xml

python3 tools/profiling/summarize_xctrace.py \
  /private/tmp/gzdoom-blackwater.Y5o1Yg/doorway-metal-02-profile.xml \
  --json /private/tmp/gzdoom-blackwater.Y5o1Yg/summary.json
```

This resolves XML references, uses the schema's column names, reports thread
sample weights, and ranks the selected thread's leaf labels. The default thread
prefix is `Main Thread`; change it with `--thread`. `--start` is inclusive and
`--end` exclusive, in trace-relative seconds. No automatic gameplay segmentation
is claimed: identify the event window before selecting it. The tool rejects an
empty selection, malformed references and incompatible table schemas.

Inclusive engine stacks require optional address/symbol files:

```sh
python3 tools/profiling/summarize_xctrace.py \
  /private/tmp/gzdoom-blackwater.Y5o1Yg/doorway-metal-02-profile.xml \
  --addresses /private/tmp/gzdoom-blackwater.Y5o1Yg/engine-addresses.txt \
  --symbols /private/tmp/gzdoom-blackwater.Y5o1Yg/engine-symbols-resolved.txt \
  --json /private/tmp/gzdoom-blackwater.Y5o1Yg/summary-symbolized.json
```

These particular files belong to trace 02. For a new process, obtain its image
mapping from Instruments or a process sample, verify executable UUID and image
range, collect unique engine addresses from the exported backtrace fragments,
and resolve those addresses with `atos -o EXECUTABLE -arch ARCH -l LOAD_ADDRESS
-f ADDRESS_FILE`. Retain one output line per address. Do not reuse ASLR load
addresses across processes, or silently symbolize against a rebuilt binary.
Non-engine frames remain represented by the exported leaf labels; this tool
does not fully symbolize system-library ancestors. Automatic image-map
discovery/symbolication is not implemented. Preserve the original trace and
symbols alongside derived summaries.

All percentages use selected-thread **sample weight**, not elapsed wall time,
frame time, or utilization. Inclusive rows overlap and cannot be summed; each
symbol counts once per stack even if recursion/inlining repeats it. A counter
such as `RunThinkers` includes native/script calls and any synchronous loading
they invoke. No duration is assigned to missing samples. A Time Profiler export
with waiting-thread recording off cannot exonerate GPU, driver or I/O waits.

## Measurement tools to use before adding engine timers

### Native frame/tick correlation on macOS

The [marker contract and control measurements](../../docs/audits/engine-profile-markers-2026-11-09.md)
describe `vid_profilemarks 1` (default off, non-archived). In a rebuilt macOS
process this adds `EngineLoop`, `EngineTick` and `EngineUpdate` signposts on
Instruments' native timeline. It works with GL and Metal; it does not add a
native event backend on Linux/Windows. Scope IDs are profiler-local, not demo
or game-state identifiers. Marker overhead is not yet qualified for performance
baselines.

Launch the intended executable directly and attach to its **PID**, then collect:

```sh
xcrun xctrace record --template 'System Trace' --instrument 'Time Profiler' \
  --attach PID --time-limit 40s --window 60s --output /private/tmp/doorway-marked.trace

xcrun xctrace export --input /private/tmp/doorway-marked.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="os-signpost" and @category="PointsOfInterest"]' \
  --output /private/tmp/doorway-signposts.xml

python3 tools/profiling/summarize_signposts.py /private/tmp/doorway-signposts.xml \
  --end END_SECONDS \
  --json /private/tmp/doorway-markers.json
```

Replace PID and use fresh output paths. On this host, Instruments `--launch`
resolved the requested project executable to `/Applications/gzdoom.app` with a
different UUID. That run was rejected. A direct executable launch followed by
PID attachment avoided the substitution. A process left running before a
rebuild still has its old code; relaunch before using a new marker CVAR.

Explicit retention matters: the first marked Blackwater command requested 40
seconds but the template's default window retained only 10 seconds and reported
event loss. A 12-second paused check with `--window 20s` retained 12.330 seconds.
Always verify exported duration, table ranges and loss diagnostics; a time-limit
message alone does not establish full capture. See the
[marked-frame analysis](../../docs/audits/blackwater-marked-frame-2026-11-09.md).

The marker summary retains intervals, context-bearing Update points and observed
Update gaps, and reports unmatched/duplicate boundaries and unknown event types.
Numeric context normalizes the recorded host's integer grouping commas; raw
messages remain available. Archived atos image names and line numbers are
stripped before inclusive CPU aggregation, so one function is not split by
symbol-file naming.
Replace END_SECONDS with the verified shared recording endpoint from the TOC
and CPU/scheduler table ranges. The corrected Blackwater export included
signpost records beyond its 40.391389-second CPU window; failing to bound the
summary would rank those unpaired tail events as captured hitches. `--start` is
inclusive and `--end` exclusive; clipped interval boundaries remain visible as
unmatched records. Keep the unbounded export for quality inspection.
Use a qualified interval's trace seconds with `summarize_xctrace.py --start ...
--end ...`. Export `thread-state` from the **same recording** to distinguish
blocked and running time. A long Update gap across a reload, menu/focus change,
disabled markers or lost events is not automatically a gameplay hitch. The
helper does not classify those contexts or prove loss-free OS collection.

| Question | Existing tool/data | Interpretation and follow-up |
|---|---|---|
| Which frames hitch? | `vid_frametrace 5`, `vid_stalltrace 33` | Whole-frame distribution and coarse loop attribution; phases are inclusive, fixed-capacity and not an exclusive call tree |
| What executes on the main thread? | Time Profiler + exported window summaries | Native engine, VM/JIT, audio/decode and CPU render work; retain identical input/settings and repeated runs |
| Is time blocked rather than executing? | System Trace with thread scheduling/wait states | Validate available tables/readability first; capture together with CPU stacks where supported, otherwise keep separate run IDs |
| Is the GPU/presentation involved? | Metal System Trace, existing Metal stall events | GPU activity and waits; low sampled CPU cost alone does not establish GPU innocence |
| What resources are being acquired? | File Activity plus sampled decode/copy stacks | Attribute reads separately from decompression/conversion; file reads may be cached |
| Are actors expensive? | `stat think`, separately `profilethinkers -t 20` | Thinker time/count, then the next-turn top 20 class ticks; includes non-actor thinkers, excludes some initialization, and is not a 20-second profile |
| Are scripts expensive? | `stat VM`, `stat ACS`, `acsprofile`, sampled callers | VM/ACS counters overlap thinker costs; ACS instruction counts are not CPU time |
| Does visible content raise CPU render cost? | `stat rendertimes`, `stat renderstats`, `stat lightstats` | Collection/submission regions plus sprites, walls, flats and light interactions; displayed counts are not total simulated actors |
| Is allocation/GC material? | `stat gc`, Allocations in a separate observer arm | Allocation stacks/bytes versus GC pauses; profiler overhead must be measured |

Use overlays/class profiling in separate diagnostic runs; UI/printing and
per-call timers can alter the baseline. Existing render-time displays refresh
at different rates and derive some totals by subtraction. They supplement
sampling rather than establish a precise exclusive breakdown. Enabling all
tools together is not the initial experiment.

## Smallest next instrumentation gap

Native macOS loop/tick/Update correlation is now available as an opt-in path;
the combined System Trace/CPU sampling capture and marker controls are tested.
The next gap is qualification on the actual Blackwater route, including observer
overhead, repeated results and GPU/wait context. No per-frame text logging is
added. Do not widen the fixed 16-name stall bucket table with arbitrary nested
timers. Other-platform event sinks, actor-count time series and focused sound/
observer metadata remain separate future increments.

An actor hypothesis needs total ticked thinkers, class distribution, visible
sprite/light workload and tick time together. The same actor can cost time in
both simulation and scene construction. Do not delete actors or disable AI as
an unqualified baseline: that changes interactions and RNG consumption. Select
an algorithm or concurrency experiment only after attribution and a separate
compatibility contract.

The first comparison should retain the captured sound-loading and material-
observation candidates. CPU hardware-renderer work includes traversal,
collection, materials and command encoding, but those paths may also block on
drivers/GPU resources. `hwrenderer` as a directory is not a CPU-only workload.

## Validation

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools/profiling -p 'test_*.py' -v
```

The analyzer reproduces the independent temporary parser's trace-02 totals:
20,876 main-thread samples, 5,857 inclusive scene-material observer samples and
1,714 thinker samples, all nominally weighted at 1 ms. Synthetic controls check
XML reference resolution, schema order, recursive-frame deduplication, exact
time-window boundaries, and rejection of broken references/empty selections.
These validate analysis mechanics, not hitch attribution, symbol accuracy for a
different binary, or runtime performance improvements.


## Native regression controls and load timing

Reusable generated fixtures, audio/texture/graph self-tests and an interleaved
save-load runner now live in [tools/regression](../regression/README.md).
They preserve asset hashes and distinguish first-visit and warm intervals.
Use them for feature controls before adding another timer or changing worker
ownership. They do not replay combat or replace an Instruments trace.


### Reload capture finalization and identity follow-up

The [2026-11-12 reload audit](../../docs/audits/blackwater-reload-attribution-2026-11-12.md)
retains a failed 80-second combined recording: the wrapper stopped it after
150 seconds while saving, leaving a bundle with `Document Missing Template
Error`. A 25-second capture with 40-second retention and a longer finalization
budget exported successfully after clean target exit. Earlier successful runners
also scheduled target exit. Prefer short focused captures and allow completion
with periodic progress checks; a time-limit message is not a saved trace.
Keep failed bundles and reject them rather than interpreting partial metadata.

On this host `vmmap` supplies load addresses, but does not print the executable
UUID. A short paused `sample` provides a Binary Images UUID to compare against
the retained binary's `dwarfdump --uuid`; it is an identity check outside the
measured workload. Resolve sandboxed symbol-service failures with an authorized
symbolication run, and retain the mapping/output comparison.

Check native `active`, `paused` and `menu` context. Enabled background execution
does not make an `active=0` automated probe a foreground gameplay benchmark.
Retain trace issue records (including Time Mapping), check continuity and
scheduler coverage of each selected interior window, and bound markers by the
shared CPU/scheduler endpoint. Full interval coverage is useful evidence, not
proof that every OS stream is loss-free. Automatic same-save loads can profile
the shared loader while complete death/reborn validation remains separate.
