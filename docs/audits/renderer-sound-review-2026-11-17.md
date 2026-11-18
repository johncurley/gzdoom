# Renderer and sound follow-up review — 2026-11-17

Initial source/history review during the LLVM build. The original candidates
below describe that review; the dated qualification section records the later
implemented dynamic-light trial. Earlier audits retain their own scope.

## Shared hwrenderer candidates

**First small candidate: reuse accepted light geometry within one call.**
In hw_dynlightdata.cpp, GetLight (line 54) calculates PosRelative(group) and
GetRadius(), performs plane rejection, then calls AddLightToList (line 68).
AddLightToList calculates those same two values again at lines 81–82.
PosRelative adds the portal-group displacement; GetRadius checks activation
and scales the radius. Wall, flat and render-hack callers use GetLight.
Model lighting also calls AddLightToList directly and needs to retain its path.

A bounded trial could pass the already computed position/radius to a private
packing helper while keeping the public direct-call wrapper. Retain all
rejection tests, packed float values, shadow/spotlight/color handling and list
ordering. No cross-frame cache or borrowed-world-pointer job is needed.
Structural prediction: two source evaluations become one per accepted GetLight
call; rejected lights and independent AddLightToList callers are unchanged.
The compiler may already remove duplicate evaluation through inlining/CSE.
Inspect optimized code and count actual work before claiming a saving.
Require identical packed light data and same accepted/rejected ordering for
portal groups, inactive/zero-radius lights, plane sidedness, additive,
subtractive, attenuated and spot lights. Follow with within-backend moving
scene image controls and interleaved CPU timing. This is not yet a measured
hotspot or an implementation contract for threading.

**Second candidate: comparator data access in opaque flat sorting.**
HWDrawList::SortFlats calls GetFlatTextureSortKey for each comparison. That
resolves HWFlatEntry, branches on packet status, then reads packet key or
legacy flat texture. Packets already cache their TextureSortKey; avoid
rediscovering that existing optimization. If sorting is measured as material
CPU cost, examine caching the resolved key once per entry/sort rather than
repeating resolution O(N log N) times. Account for the larger entry or scratch
storage and key invalidation. Preserve the exact existing comparator relation
and resulting item order. Do not expand the comparator to arbitrary material
fields or change portal/translucent ordering as part of this trial.

**Do not repeat closed state-cache work.** The render-state audit already
measured pipeline/matrix filtering and fixed repeated Metal depth-clip commands.
It also measured real four-field material-key changes on about 82% of Apply
calls in the paused fixture. A dirty material flag does not prove redundant
work, and equal keys do not prove immutable canvas/layers or resource readiness.
Continue material-preparation/opaque-order attribution only with current
moving/combat traces after LLVM finishes. See
render-state-counts-2026-11-10.md and graph-caller-lookups-2026-11-10.md.

## Sound: implemented worker tranche versus remaining work

The bounded macOS decode preparation, ordered level-precache lookahead and
random-sibling lookahead are implemented and default enabled. Owner code still
acquires resource bytes, creates/closes decoders and publishes OpenAL buffers;
eligible decode work runs in the preparation pool. Simulation/RNG/channel
selection is not dispatched to workers. Source confirms CanPrepareSound returns
false on non-Apple platforms pending their decoder/runtime qualification.
The pool retains the four-job ceiling and 64 MiB budget; retention is a separate
opt-in policy with a shipping zero-KiB default.

The enabled-default Intel Metal doorway/combat/three-minute play observations
are scoped acceptance, not a long soak or all-platform completion. GL/Metal
native resource suites passed previously. Remaining checks include longer
varied play, live sound reset/reload, matched GL gameplay, Apple Silicon and
Linux/Windows qualification. See sound-sibling-preparation-2026-11-12.md and
the current macOS handoff; their later enabled-default disposition supersedes
earlier default-off text.

**Next sound investigation: identify remaining synchronous misses.** The shared
reload audit, with sibling lookahead already enabled, retains 38/36 ms of decode
sample weight in two post-load catch-up windows. These are sampled weights,
not prospective savings or ordinary steady-combat cost. Classify actual assets
and call timing: undiscovered direct sounds, ineligible formats/sizes, not-ready
work, admission rejection, or eviction across reload. Separate diagnostic
identity logging from uninstrumented performance arms.

Start with the existing counters/audit path and source ownership boundaries.
Three ready background preparations historically occupied about 53.7 MB of the
64 MiB pool, so more fixed reservations are not free. Do not increase worker
counts or budgets before identifying which class of miss would benefit.
Generic discovery and measured retention coverage can remove repeated owner
work without expanding the threading model; retention still needs loop/alias,
reset and memory qualification before considering any default change.

**Lower-priority source candidate: loaded-lump lookup.** SoundEngine::LoadSound
scans S_sfx for a loaded canonical same-lump buffer before attempting a prepared
result (s_sound.cpp:967–983). This work can therefore occur even on ready hits.
Measure table length, scan iterations and CPU contribution first. Any index
must preserve the first eligible definition in table order, RAW/rate behavior,
alias/rolloff semantics and invalidation on unload/redefinition/reset. Moving
the prepared-result lookup ahead of sharing changes behavior and is not a safe
shortcut. Existing decoder samples do not establish this scan as a bottleneck.

The useful remaining sound work is coverage, lifetime policy and qualification.
A whole-audio-thread migration, VM threading or wider decoder support is a new
architecture/platform task, not a necessary extension of the completed pool.

## Priority after the competing build

Keep the independent sector-scroll correctness finding separate from timing.
Measure the UDMF unused wall-reference allocation candidate first for reloads;
it has an explicit unused consumer on the reviewed MAP09 source. Then rank
renderer dynamic-light/material preparation and residual sound misses using
the workload the maintainer wants improved. No speedup is asserted by this
review. Documentation whitespace checks pass; prior native results were not
rerun during LLVM compilation.

## Dynamic-light qualification — 2026-11-18

Retained the first candidate: a private scalar packing helper reuses position
and radius already calculated by GetLight. The public AddLightToList wrapper
still computes them for direct/model callers. No cross-frame cache, threading,
light ordering, shader, or graph execution change. Optimized assembly confirms
the original duplicate geometry evaluation survived compilation. A vector-
reference helper added stack traffic; the retained scalar version avoids it.

Predictions preceded measurements: zero packed-data/image differences, one
failure from deliberate data corruption, at least 5% accepted-call improvement,
and no repeatable direct-call regression over 3%. Two interleaved long runs
(three million calls/sample, six samples/arm/case) show accepted plain/spot
calls 5.66–14.99% faster, roughly 2–3 ns per call in the first run. Direct/model
medians vary within 2.48%; identical-code controls establish timing noise.
Rejected portal-group calls vary by fractions of a nanosecond and support no
separate improvement claim. Earlier short runs are preserved, including noisy
model results. These are isolated CPU-call timings, not frame-rate evidence.

The byte oracle passes 49,152 checks/run covering classification, portal offsets,
sidedness, radius boundaries, alpha, light styles, shadows and forced model
attenuation; deliberate corruption produces exactly one failure. Six native
GL/Metal cases verify moving-light packed data with zero mismatches, then exact
original/candidate pixels at two held poses. Disabling lights changes over 95%
of pixels, proving the image control is sensitive. Graph dumps match within
each process, with effects off/on. Metal shadows execute; this Mac's GL 4.1
lacks SSBO support and correctly disables shadows, so GL shadow execution is
not qualified.

All temporary C++ probes are removed. Default and static-development builds
pass. Four final uninstrumented native runs against the preserved packaged
baseline give exact pixels at both poses on both backends, with effects enabled.
Metal graphs match literally (51 passes/113 edges); GL graphs (48/105) match
under a bijective renaming of anonymous GL.Texture creation ordinals. The first
literal cross-process GL comparison rejected those IDs; original logs and the
explicit mapping are retained. Earlier harness corrections likewise retain
both evidence and explanations rather than discarding failed controls.

The Metal graph reports the earlier MetalRenderDevice scene.target as a dead-
pass candidate before HWDrawInfo::Clear3DViewport. Source tracing shows target
selection is deferred state setup, not proof of a separate GPU clear. Audit
that graph declaration against actual encoder work before any pass-culling or
clear-removal proposal; no GPU saving is established. Linux/Windows Vulkan/GL
and Apple Silicon runtime qualification remain open. The frozen Mac test ZIP
predates this renderer change and has not been replaced.

Evidence: `/private/tmp/gzdoom-dynlight-x02lorsp`, especially `bench-long-*`,
`bench-null-metal`, six `images-*` cases and `final-production/summary.json`.
Temporary runners, assembly, manifests, baseline source and failed harness
controls remain outside the repository. Nothing is committed or published.

## Metal target-selection audit — 2026-11-18

Prediction: zero scene encoders between scene-target selection and the deferred
clear; the first actual scene encoder clears color/depth/stencil. Temporary
logging at SetRenderTarget, Clear and successful BeginRenderPass creation
confirms this across 90 real frames with effects off and 90 with bloom/SSAO/
tonemap/lens/FXAA on, using the shadow-enabled light fixture. With AO enabled,
the subsequent scene encoder loads all three attachments. Graph self-tests
pass with no build errors or stale-size diagnostics. An offline validator
control inserting an early encoder rejects both recorded example sequences;
this is a trace-parser control, not a deliberately modified GPU command stream.

The earlier MetalRenderDevice scene.target graph entry therefore represents
state selection, not a separate scene draw/clear in this workload. Removing
that graph declaration would correct diagnostic accounting; removing the actual
target selection or Clear3DViewport would be a different, unsafe inference.
No runtime optimization or graph implementation change was made. The comparable
GL/Vulkan target-selection entry points now defer graph writes to other scopes;
historical documentation describing their older conservative boundary is not
evidence of current code. Multisample, stereo and camera routes still need
review before any graph-declaration edit.

Temporary render-state probes are removed and the source restored exactly.
Artifacts: `/var/folders/hg/d3wtgjl97hl8zcy1pct4zt4r0000gn/T/gzdoom-graph-target-k3j1qe8e` (pointer:
`/private/tmp/gzdoom-graph-target-active-path.txt`); runner:
`/private/tmp/gzdoom_graph_target.py`. Build logs are
`/private/tmp/gzdoom-graph-target-final-{default,static}.log`.

## Metal clear and resize accounting

The clear now records the active Metal attachment names, including thumbnail
resources and the screen MSAA resolve target. Target selection no longer
records a pass or a shadow read. Paired Intel Metal and GL runs matched all
24 sampled images and seven save thumbnails across eight cases. The old mono
graph reports nine passes and the corrected graph reports eight. The two-eye
render loop drops from 99 to 97 and retains both clears.

The combined camera and fullscreen transition exposed an intermittent stale
registry reference. The old build reported five size discrepancies when the
scene moved from 640×480 to 1440×900. The registry now starts its frame with
the same viewport dimensions used to allocate the screen attachments.
Save buffers retain their own fixed size. Both production builds passed
after removing the temporary thumbnail observation.

One original fullscreen capture differed from a repeat of the same binary.
The first corrected capture matched the original repeat exactly. The strict
image runner retains its failure against the first original capture. Raw
captures, logs and manifests are in /private/tmp/gzdoom-framegraph-clear-emzgd4tf.
