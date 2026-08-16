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

## 3. Add a Linux CI job that builds Vulkan

**`AGENTS.md` Tasks — Linux, item 9**, and this session made it more relevant.
`42052f77a` added an unconditional `find_package(Vulkan QUIET)`; on 2026-08-16 that
was narrowed to **non-Apple** (`fe1b9c0ab`), because Vulkan-on-Apple means MoltenVK
and this fork exists to replace it. Consequence: Linux is now the **only** platform
where the Vulkan backend can be auto-detected and built, and **no CI job builds it
at all**, so `HAVE_VULKAN=ON` compiles nowhere in CI.

That matters beyond tidiness: `CONTRIBUTING.md` asks that shared-code changes leave
GL and Vulkan bit-identical, and `crossbackend.py --backends gl,vulkan` is the
tiebreaker that says whether a Metal divergence is a Metal bug or a deferred-backend
convention. Both are unavailable if Vulkan never builds.

The item notes the dependency cost is low — both loaders `dlopen` `libvulkan.so.1`.

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
