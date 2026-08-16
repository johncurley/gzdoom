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

## 4. There is no smoothness instrument on Linux

**New, and the most transferable lesson of the macOS session.** On 2026-08-16 a
renderer configuration that **froze constantly in gameplay** was reported healthy by
the benchmark harness — avg 5.5ms, max 90.6ms, 4 stalls, indistinguishable from the
good path. A settled viewpoint never exercises what a renderer does badly while the
camera moves.

The fix on macOS was `mt_frametrace <seconds>` (`b15f25f9d`): frame-interval
percentiles to stderr during actual play — p50/p95/p99/max plus >33ms and >100ms
counts, because p50 stays healthy while p99 blows out and a mean hides exactly the
frames the player feels.

**It is Metal-only.** It lives in `mt_debug.cpp` and hooks the Metal frame-interval
recorder, so GL and Vulkan on Linux have no equivalent. Either lift it to a
backend-agnostic place (`v_video.cpp` sees every frame) or write the GL counterpart.
Until then, no Linux renderer change can be judged for smoothness by anything but a
person's impression — which is how the macOS defect escaped for a whole session.

## 5. Publish upstream to dpjudas — task 1 is closed, this is now unblocked

Open the PR to `dpjudas/ZWidget` with the six commits in the table above. Describe
the first-paint fix as a protocol-correctness fix (xdg-shell requires a buffer
after every ack, unconditionally), not as a confirmed repro — two machines tried
and neither reproduced the blank-launcher symptom (task 1). `cocoa-modal-fixes` was verified clean against upstream head on
2026-08-11 (12 warnings before and after, all pre-existing OpenGL deprecations).

**Use the cherry-pick procedure in `CLAUDE.md`, never `git subtree push`** — measured
2026-08-09, a subtree split synthesises 22,906 commits of which 2 touch the files
being published, and `git merge-base --is-ancestor` calls that a clean fast-forward.

## 6. Engine: the Vulkan/GL side of the frame analysis — the next engine step

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

**Then the graph work returns to macOS**: interface design informed by both analyses,
then `Pass2` (tonemap → colormap → lens → fxaa) as the first migration — four pure
`current → next` passes already covered by the suite's relations — with AO last, for
the reasons in frame-analysis.md §4.

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
