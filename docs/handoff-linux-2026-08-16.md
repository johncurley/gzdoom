# Linux work — what is open, in the order I would do it

Written 2026-08-16 from macOS, at the end of the AO session. This consolidates
the **Tasks — Linux** section of `AGENTS.md`, corrects one item that is now stale,
and adds what changed this session that reaches Linux.

Read this first, then the numbered items in `AGENTS.md` for the detail — they are
cited per task rather than repeated.

---

## Correction: the Wayland fixes ARE published to the fork

`AGENTS.md` items 10 and 11 say `c3474d697` "exists only on `metal-audit`" and that
publishing is the open work. **That is stale.** Verified 2026-08-16 by fetching both
remotes: `zwidget/wayland-c-bindings` already carries the equivalent commits.

| fix | on the fork (`zwidget/wayland-c-bindings`) | on upstream (`dpjudas/master`) |
|---|---|---|
| Wayland: clamp registry binds + clipboard order | `bebe13394` | **no** |
| Wayland: paint on `xdg_surface` configure | `10b60e035` | **no** |
| X11: raw keyboard through XInput2 | `4c107f4ec` | **no** |
| X11: raw key events on the master keyboard | `f460ad493` | **no** |
| X11: do not focus a non-viewable window | `1c3630540` | **no** |
| Cocoa: `RunModalLoop` + real modal session | `de9963f25` | **no** |

So the publishing step is done; what is open is **upstreaming to dpjudas**, which is
a different task with a different gate (see task 1). Do not redo the cherry-picks.

---

## 1. Reproduce the Wayland first-paint bug, or say it cannot be reproduced — CLOSED 2026-08-16

**`AGENTS.md` Tasks — Linux, item 1.** Does not reproduce under KWin either. Control
build at `d70d1f944` (`d4fae73da^`, pre-fix) launched with no `-iwad` so the
ZWidget bitmap-rendered IWAD picker is under test (not the OpenGL game window,
which never touches this code path). Four independent launches on this KDE
Plasma/kwin_wayland/RX 550 box, including a race loop grabbing the earliest
possible screenshot after process start — painted immediately every time, never
blank. Post-fix HEAD repeated once for symmetry: same result, no observable
difference.

Source reading explains why it's deterministic here: `InitializeToplevel()` does a
*synchronous* `wl_display_roundtrip()` right after `wl_surface_commit()`, inside
the constructor, before it returns — so the initial configure is fully acked
before `WaylandDisplayBackend::CheckNeedsUpdate()` ever gets a chance to consume
the `m_NeedsUpdate` flag on the run loop's first pass. There is no window in this
call sequence where the flag can be lost before the surface is ready. See
`AGENTS.md` Tasks — Linux item 1 for the full writeup.

**Two machines have now tried and neither reproduces it — that is itself the
result** the original instruction asked for: the report came from a compositor
neither machine runs. The fix is still correct (xdg-shell does require a buffer
after every ack, unconditionally) and should go upstream on that protocol-reading
basis, not on a false "confirmed fixed" claim — drop that framing from the PR
description.

**This gated the upstream PR; it no longer does — see task 5, now unblocked.**

## 2. Re-validate the matrix suite's scene choices on this hardware — CLOSED 2026-08-16

**`AGENTS.md` Tasks — Linux, item 2.** Both scene choices hold on this hardware.
8/8 determinism on `doom2`/MAP06 today (the 2026-08-13 single outlier did not
recur), `gl_ssao 3` acts on MAP06 (7.12%, reproduces 2026-08-13 exactly), and a
Linux `baseline.json` is now recorded and holds on a clean re-run. `doom1`/E1M3
passes relations-only, as designed.

One real finding along the way: the 2026-08-13 entry's "`gl_bloom` changes 34%
of MAP06" does not reproduce — six clean, untouched samples all landed on
0.00% differing (matching a 2026-08-12 measurement that said bloom does
nothing on MAP06, not the 34% one). The first sample of that test came back at
93.67% differing, and the window was moved during that capture. **A window
touched mid-run produces one large, misleading outlier against an otherwise
perfectly clean series — not visible drift.** That is the most likely
explanation for the 2026-08-13 MAP06 determinism outlier too, despite that
entry's own "repeated with no interaction" note. Bloom not acting on MAP06
doesn't disqualify the map: bloom is deliberately measured on MAP12 instead,
via the `relations_only` trio, and that relation still holds (34.05%). See
`AGENTS.md` Tasks — Linux item 2 for the full session log.

## 3. Add a Linux CI job that builds Vulkan — CLOSED 2026-08-16

**`AGENTS.md` Tasks — Linux, item 9.** The job existed since 2026-08-12 but only
on an orphaned side branch/worktree that never got merged into `metal-audit` — this
branch's CI workflow had no Vulkan entry at all until today, despite `AGENTS.md`
reading "Done" for four days. Cherry-picked in clean, one file
(`.github/workflows/continuous_integration.yml`), no conflicts.

Then verified past what the CI job itself can ever prove, since this machine has
real Vulkan 1.4 hardware and CI does not: `-DHAVE_VULKAN=ON` compiled clean under
gcc 16.2.1 (a second compiler, not CI's pinned gcc-12), `+vid_preferbackend 1`
launched against the real GPU (RX 550, RADV POLARIS12) and rendered a correct
frame, and `crossbackend.py --backends gl,vulkan --scene doom2` came back **12/12
OK** — Vulkan agrees with GL within the tool's own noise floor on every pass,
including the two runtime-only defects (`a60ea956d`, `e1f47ce5b`) already fixed
on this branch that the compile-only job could never have caught either way.

Found the same problem a second time in the same spot: `run.py`'s degenerate-frame
guard (`AGENTS.md` Tasks — Linux item 4) had the identical fate — fixed
2026-08-12, only on that same orphaned branch, never merged. Cherry-picked that
too (`f528bfa24`) since it was sitting right there; re-verified clean afterward.

**Still open:** CI itself still has no GPU, so a third runtime-only Vulkan defect
of the same shape as the two above would still slip past it. Mesa's **lavapipe**
(a CPU Vulkan ICD) could let the CI job actually launch the backend under Xvfb
instead of only linking it — not yet tried, see `AGENTS.md` item 9 for the cost
and the trap to avoid (do not fold it into the compile-only job; `HAVE_VULKAN`
flips `DEFAULT_RENDER_BACKEND` to 1).

## 4. There is no smoothness instrument on Linux — CLOSED 2026-08-16, lifted backend-agnostic

**New, and the most transferable lesson of the macOS session.** On 2026-08-16 a
renderer configuration that **froze constantly in gameplay** was reported healthy by
the benchmark harness — avg 5.5ms, max 90.6ms, 4 stalls, indistinguishable from the
good path. A settled viewpoint never exercises what a renderer does badly while the
camera moves.

The fix on macOS was `mt_frametrace <seconds>` (`b15f25f9d`): frame-interval
percentiles to stderr during actual play — p50/p95/p99/max plus >33ms and >100ms
counts, because p50 stays healthy while p99 blows out and a mean hides exactly the
frames the player feels.

**Took the first option, not the second: lifted rather than duplicated.**
`mt_frametrace` hooks Metal's own frame-interval recorder in `mt_debug.cpp`, which
GL and Vulkan have no equivalent of and nothing to hook. Rather than write a second,
GL-specific copy (a third if Vulkan ever needed its own), the same instrument now
lives in `DFrameBuffer::Update()` (`v_framebuffer.cpp`), which every backend's own
`Update()` override already chains to via `Super::Update()` — confirmed by reading
all three (`gl_framebuffer.cpp`, `vk_renderdevice.cpp`, `mt_renderdevice.cpp`) before
touching anything, all three call it. One implementation now covers GL, Vulkan *and*
Metal, at the cost of self-measuring the interval with `steady_clock` instead of
being handed a frame time (the base class isn't given one).

New cvar `vid_frametrace` (deliberately not `mt_frametrace`, so the name doesn't
imply Metal-only). Metal's own `mt_frametrace` is untouched — did not risk touching
`mt_debug.cpp` while a concurrent macOS session was actively pushing commits to the
same file's neighbourhood.

**Verified on both Linux backends, real gameplay, MAP06:**

- GL (default): first two 1s windows show the startup stalls (`n=2 avg=568ms`,
  `n=4 avg=294ms p99=1114ms`), then settles to `n=36 avg=28ms p50=28 p99=31,
  >33ms=0` for several windows, with one window catching a real blip
  (`p99=38.33, >33ms=1`) that avg alone would have buried — exactly the shape the
  instrument exists to show.
- Vulkan (`+vid_preferbackend 1`): same shape, same order of magnitude
  (`n=2 avg=567ms` startup, then `n=35-37 avg=28ms p99=31-33ms`) — confirms the
  hook fires identically on both backends without a single backend-specific line.
- Default off (`vid_frametrace 0`, the default): zero `vid_frametrace` lines in the
  log — silent unless armed, same proof-of-execution discipline `CLAUDE.md` already
  asks for elsewhere.

Linux renderer changes can now be judged for smoothness the same way macOS ones can.

## 5. Publish upstream to dpjudas — branch pushed 2026-08-16, PR not yet opened

**Scope grew from "six commits" to the full `wayland-c-bindings` branch (12
commits), deliberately — see the correction below before assuming the six-commit
table above is still the plan.**

Attempting to cherry-pick just the five Wayland/X11 fixes (leaving out the
waylandpp→generated-C-bindings replacement, `8621e9767`/`1932ebf9c`, as
apparently-unrelated) onto a fresh branch off `zwidget/master` produced real
conflicts, not context drift: `zwidget/master` is still on the old
waylandpp-based `wayland_display_window.cpp`, and `1932ebf9c`/`8621e9767` turn
out to touch X11 files too (`x11_dynamic.h`, `x11_remap.h` — the dlopen loader
and macro-remap shim both backends use). The Wayland and X11 fixes are not
separable from that replacement; one resolved conflict (`OnClientMessage`)
would have silently dropped the `XSetWMProtocols` registration the WM_TAKE_FOCUS
handler needs to ever fire, had it not been checked against the fork's actual
final file state rather than resolved from the conflict markers alone.

Given that, the decision (2026-08-16) was to widen the PR to the whole branch
rather than fight the entanglement: `zwidget-wayland-c-bindings-clean` was fast-
forwarded to exactly `zwidget/wayland-c-bindings` (12 commits, ~19,700
insertions / 3,400 deletions across 59 files) and **pushed to the fork** —
https://github.com/johncurley/ZWidget/tree/zwidget-wayland-c-bindings-clean.
Confirmed before pushing: `zwidget/master`'s HEAD SHA (`4cf65e59c`) is byte-
identical to `dpjudas/ZWidget`'s current `master` via the GitHub API, so this
branch sits directly on current upstream — no rebase needed, ready to PR as
pushed.

**Built standalone and verified, both sides:** this branch compiles clean
(exit 0) against a bare CMake configure, and comparing warning counts against
a `zwidget/master`-only build shows **51 warnings on master, 40 on this
branch** — net fewer, mostly from removing the deleted waylandpp helper files.
It is not warning-free relative to master, though: four **new** warning types
appear (three `%p`-format mismatches against `wl_display*`/`wl_registry*`/
`xkb_context*` in debug trace logging, one member-initialization-order warning
in `WaylandDisplayWindow`) that were not on master. Not yet fixed — small and
mechanical (cast to `void*`, reorder two initializers) whenever this is picked
back up.

**Deliberately not opened as a PR yet — inspect the branch first, open it
yourself when ready.** `cocoa-modal-fixes`'s two commits (verified clean
against upstream head on 2026-08-11, 12 pre-existing warnings before and
after) are content-identical to the two Cocoa commits already inside this
branch (`de9963f25`/`51a25ffa5` vs `7374fb476`/`877c585ff` — same diff hunks,
different SHAs from being cherry-picked onto different bases) — no separate
Cocoa PR is needed once this one exists.

**Use the cherry-pick procedure in `CLAUDE.md`, never `git subtree push`** — measured
2026-08-09, a subtree split synthesises 22,906 commits of which 2 touch the files
being published, and `git merge-base --is-ancestor` calls that a clean fast-forward.
Moot for this particular branch (it was fast-forwarded from an already-published
branch, not freshly split), but still the rule for anything published from here on.

## 6. Engine: the Vulkan/GL side of the frame analysis — CLOSED 2026-08-16

`docs/frame-analysis.md` maps the renderer's passes, resources and couplings, and is
explicitly **Metal-only**. The frame graph it feeds (Track A items 3-4) is meant to be
backend-neutral, and designing that interface from one backend is how you get an
abstraction shaped like Metal with holes where the others differ. This task closes
that gap; the graph interface is designed afterwards, against all three.

**Produce `docs/frame-analysis-vulkan-gl.md`**, same structure as the Metal one, so
the two can be read side by side.

**Extract mechanically, do not read prose.** The Metal pass table came from grepping
the `SetInput*`/`SetOutput*` calls, because prose and code have already disagreed on
this branch. The `hw_postprocess` half is *shared*, so that table should transfer
unchanged — if it does not, that discrepancy is itself the finding.

Four questions, each cheap, each chosen because the Metal answer was load-bearing:

1. **What drives PP render state?** Metal's `MtPPRenderState` drives the *main*
   render state, which is why a PP pass could silently change shader-variant
   selection for every later scene draw (frame-analysis.md §3.1 — 134 of 135 wall
   draws compiled with no normal output). Vulkan's `VkPPRenderState` is a separate
   object, so that coupling should be absent. **Confirm it, and check GL**: GL brackets
   with `FGLPostProcessState` (saves/restores `GL_CURRENT_PROGRAM`), which is a third
   discipline again. A graph has to accommodate all three, or force one.
2. **Who writes `SceneNormal`, and how is the G-buffer variant selected?** Metal uses
   `DrawBufferCount > 1 ? GBUFFER_PASS : NORMAL_PASS`; the identical logic is at
   `vk_renderpass.cpp:251`. If Vulkan selects the same way but is not vulnerable,
   the difference is *where the state lives*, which is exactly what the graph should
   make explicit.
3. **Are there alternate implementations of a node?** Metal has compute AO and compute
   bloom that bypass the PP chain entirely and are chosen by conditionals at the call
   site. GL has no compute path at all (GL 4.1 on macOS cannot; the Linux GL is 4.6
   but nothing uses it). If Vulkan has none either, then "a node may have
   direct-compute / raster / disabled implementations" is a *Metal-only* requirement
   today — worth knowing before it shapes the interface.
4. **Who owns resource lifetimes?** Metal has three tiers, the third being module-private
   textures invisible to shared code (nine in AO, five in bloom). Establish whether
   Vulkan/GL have a tier 3 at all. If they do not, the registry can stay simpler than
   the Metal side suggests.

**This also decides where the resource registry lives.** It is currently
`metal/renderer/mt_resources.{h,cpp}`, phase 1, deliberately Metal-side because a
shared header with one user is worse than a local one. Nothing in the interface is
Metal-specific except an int pixel format, so lifting it to `hwrenderer/` is
mechanical — but only worth doing once a second backend has something to register.
Answer question 4 and the decision makes itself.

`crossbackend.py --backends gl,vulkan` is available for checking claims now that
`HAVE_VULKAN=ON` builds in CI (task 3, closed).

**Done. `docs/frame-analysis-vulkan-gl.md` written**, all four questions answered:

1. **PP render state** — not the "three disciplines to accommodate" the framing
   predicted. Metal's pipeline selection reads a raw, PP-writable field on the
   *shared* render-target struct (`mRenderTarget.DrawBuffers`,
   `mt_renderstate.cpp:769`) — that's the bug. Vulkan uses the identically-named
   field for scene draws (`vk_renderstate.cpp:517,538`) but its PP draws
   (`vk_pprenderstate.cpp:53`) never touch it — architecturally isolated, not just a
   separate object. GL's shader selection reads a decoupled logical field
   (`mPassType`, `gl_renderstate.cpp:100`) that PP draws never touch either, so GL
   was never at risk of *this* bug — its `EnableDrawBuffers` restore
   (`gl_framebuffer.cpp:448-456`) protects the raw GL binding, a different failure
   mode. Vulkan's immunity checked against a real result, not just reading: the
   `ssao` config in this session's `crossbackend.py` run came back clean.
2. **`SceneNormal`/G-buffer selection** — shared logic, `hw_entrypoint.cpp` +
   `hw_drawinfo.cpp`, identical on all three backends; not what varies.
3. **Alternate implementations** — confirmed Metal-only. Zero compute-shader calls
   anywhere in the Vulkan or GL renderer source.
4. **Resource lifetimes / tier 3** — Vulkan and GL have none; tier 3 is a direct
   consequence of Metal's compute AO/bloom (question 3), so the registry doesn't
   need to model it for the other two. Vulkan does carry one extra system neither
   other backend has: a render-pass cache keyed on format/samples that a resize
   must invalidate (`vk_renderbuffers.cpp:69-70`).

**The graph work returns to macOS now**: interface design informed by both
analyses, then `Pass2` (tonemap → colormap → lens → fxaa) as the first migration —
four pure `current → next` passes already covered by the suite's relations — with AO
last, for the reasons in frame-analysis.md §4 (strengthened, not just retained: §3.1
is now traced to its exact field on all three backends, so AO can be migrated
correctly rather than cautiously).

## Not tasks — closed, do not reopen

- **X11 raw input via XInput2** (item 6) — done and published. `XIAllMasterDevices`
  is in the tree at `x11_connection.cpp:76`; the earlier "published inert" state is
  fixed and on the fork as `f460ad493`.
- **Remove the input diagnostics** (item 7) — `in_keytrace` is gone;
  `ZWIDGET_TRACE_REPEAT` is not in the tree.
- **`FShaderProgram::Link()`'s old-GL path** (item 8) — the premise was wrong, on
  three independent grounds. **Leave it alone**; do not add a save/restore inside
  `Link()`. If you test that branch anyway, use `-glversion 3.3` and note the
  confirming log line is `Emulating OpenGL v 3.3`, not `GL_VERSION:`.
- **The X11 `BadMatch`** (item 5) — fixed `89d79bcbe`, published `dd88a86b7`.

## From this session, for context

Platform-neutral changes that reach Linux, all pushed:

- `743237fea` — `crossbackend.py` band-outlier ratio now has an absolute floor
  (`BAND_MIN_OUTLIER_MEAN = 0.5`), so a near-zero median stops manufacturing
  outliers. Applies to the `gl,vulkan` pair too.
- `1421b98db` — `bloom_compute` in `configs.json` now passes
  `+mt_compute_bloom_intel 1`. Metal-only cvar, inert on Linux, but the *lesson*
  is not: that config was testing the reference path while claiming to test
  compute, and only the `bloom path in use` label revealed it.
- `fe1b9c0ab` — Vulkan auto-detect is non-Apple only. See task 3.
- `b15f25f9d` — `mt_compute_ao` now defaults **false** on every platform, and
  `mt_frametrace` was added. Metal-only; see task 4.
