# Stability and smoothness priority audit — 2026-11-17

Decision audit of the current working tree, regression tooling, earlier measured
workloads and recent source reviews. This is not a fresh runtime certification
or an exhaustive engine audit. LLVM is compiling on the reference machine;
no engine builds, launches or benchmarks ran for this audit. No engine code
changed. Existing uncommitted work is preserved.

## Immediate decision

Prioritize correctness, repeatable compatibility checks and player-visible
stalls over new functionality. Finish the current changes before growing the
optimization patch set. Additional worker threads and broad cleanup are not
the next milestone.

| Order | Work | Evidence and reason | Completion gate |
| --- | --- | --- | --- |
| 1 | Validate and consolidate pending parser changes | Working source includes previously qualified scanner/input/bounds changes plus two uncompiled scroll/lookup edits; the on-disk executable does not match the latest source | Required build, adjacent packages, floor/ceiling scroll controls, wall-scroll/shared-side coverage, native parser/resource checks; prepare separately reviewable verified commits when authorized |
| 2 | Protect fixes with automated regression gates | Inspected continuous_integration.yml runs Linux startup/resource smoke checks, but does not invoke the new quoted-scanner/input commands or the native sidedef runner | Add appropriate pure parser commands and exact result checks to a qualified CI path; add live-map geometry/scroll coverage separately; establish positive and failing controls |
| 3 | Extend audio lifetime/compatibility qualification | Enabled-default Intel Metal doorway/combat/three-minute observations pass; long soak and broader platform/backend acceptance remain incomplete | Longer varied-play test with loads, resets and looping sounds; normal sound, correct loops/choices, bounded reservations, clean completed cancellation/shutdown, matched GL checks |
| 4 | Reduce remaining post-load and reload stalls | Shared-load traces retain synchronous decoding in resumed ticks and substantial parser/map-construction cost | Identify sound misses; qualify the existing unused side-reference allocation trial with a separate control; remeasure the target workload before further changes |
| 5 | Attribute moving/combat renderer preparation | Dynamic-light duplicate calculations and comparator data access are source candidates, not established runtime bottlenecks | Current CPU/wait attribution, generated-code check, exact packed data/order/image controls and measured benefit for one bounded change |
| 6 | Focused upstream compatibility triage and tester intake | The fork has limited independent mod/hardware coverage; source-only cleanup cannot replace that | A small reproducible issue shortlist and test-build manifest/report format; verify fixes exist in this fork and are not already solved by later upstream work |

These are execution priorities, not newly assigned defect severities. A new
reproducible crash or compatibility regression can take precedence.

## What the sound evidence establishes

The stationary weather control recorded seven >100 ms intervals with decode
preparation disabled and zero enabled over comparable selected windows, while
median frame times remained near the same 35 fps cap. This is useful hitch
reduction independent of average-FPS improvement. It does not establish that
every sound-loading stall, every mod or every platform is solved. See
[audio metrics](blackwater-audio-metrics-2026-11-09.md).

The later enabled-default checks confirm scoped doorway/combat acceptance and
6,284 complete-window intervals over three minutes of varied play, with no
>100 ms interval in that section. A separate mixed death/reload take retains
623.83 ms; do not erase it or classify it as an audio failure without evidence.
See [sibling qualification](sound-sibling-preparation-2026-11-12.md).

Remaining sound work is primarily discovery, eligibility/readiness, lifetime
and platform qualification. The existing admission contract has four jobs and
64 MiB, with owner-side archive access, decoder setup/close and OpenAL
publication. Historical ready jobs already occupied about 53.7 MB. Increasing
threads or reservation budgets is not justified by current miss attribution.
Ready retained jobs are not automatically leaks; require cleanup at completed
reset/cancellation/shutdown boundaries, not zero reservations at every normal
gameplay checkpoint. Optional decoded-buffer retention still defaults to zero.

The shared reload trace retains 38/36 ms of synchronous decode sample weight
in two resumed catch-up windows after sibling lookahead was enabled. Identify
actual asset/source identity and reason for fallback before proposing another
audio change. These samples overlap broader call stacks and are not exclusive
wall-time savings. See [reload attribution](blackwater-reload-attribution-2026-11-12.md).

## Regression infrastructure finding

The inspected workflow's Linux smoke command starts with Freedoom, disables
sound, requests resource validation and immediately quits. It checks startup,
GL identification and stale resource sizes. That has value, but it cannot
exercise a loaded map or scrolling behavior. The new pure scanner/input
selftests could be candidates for that startup path; each needs a CI-qualified
result assertion. Neither a printed FAIL with exit zero nor a missing command
must be accepted as success. Native geometry/scroll fixtures need delayed exit
and confirmed map entry on a separate path.

The existing Python tests validate tooling, not C++ parser behavior. The
standalone tools/regression/udmf_sides.py runner currently has no invocation
in the inspected CI workflow. Audio/native image suites need their actual
device/backend requirements respected; do not silently skip them and call
the result audio qualification. The current runner contains synthetic reset,
cancel and teardown controls already; the gap is broader lifecycle exposure
and automation, not the total absence of reset tests.

For wider compatibility, include USDF/shared scanner consumers and representative
existing ZScript/ACS mods before changing scripting interfaces. Apple Silicon,
Windows and Linux require native qualification; an Intel macOS pass cannot
close their behavior or performance gates. Preserve legacy simulation ordering.

## Narrow UZDoom source comparison

The inspected public trunk sound files show a background thread for stream
processing in OpenALSoundRenderer::CreateStream, and synchronous decoder reads
and buffer publication inside OpenALSoundRenderer::LoadSound. SoundEngine's
demand path invokes that loader inline. This supports distinguishing existing
streaming threads from cold sound-effect preparation; it does not establish
that UZDoom has done no audio threading work or reveal team priorities/hardware.
This was a web source inspection of two files, not a complete repository/history
audit or matched benchmark. Branch contents and cached views may change.

Primary sources inspected:

- https://github.com/UZDoom/UZDoom/blob/trunk/src/common/audio/sound/s_sound.cpp
- https://github.com/UZDoom/UZDoom/blob/trunk/src/common/audio/sound/oalsound.cpp

The fork's UX benefit should stand on its own controlled measurements and
operator observations. No cross-engine performance ranking follows here.

## Work during and after LLVM compilation

While the machine is busy, prepare fixture expectations, review source/history,
triage a few reproducible upstream bugs and keep the pending validation notes
current. Do not add more unverified engine edits solely to stay busy.

After the machine settles, execute
[pending validation](udmf-pending-validation-2026-11-17.md) first. Keep the
scroll correction in both timing arms of the allocation trial. Use identical
compiler/settings/assets per A/B; building LLVM does not justify silently
changing the GZDoom compiler between arms. Require two identical baseline
captures and estimate timing noise before interpreting a change.

The existing [renderer/sound source review](renderer-sound-review-2026-11-17.md)
and [direct-record trial contract](udmf-direct-record-review-2026-11-17.md)
remain a candidate queue. No new renderer campaign, scripting feature or
general simulation-threading redesign is scheduled by this audit.

Checks completed: local source/history/workflow inspection and git diff --check.
All runtime and compile claims above refer explicitly to earlier qualified
work; this audit ran no new tests and provides no new performance result.
