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

## 1. Reproduce the Wayland first-paint bug, or say it cannot be reproduced

**`AGENTS.md` Tasks — Linux, item 1.** The blank-launcher-until-you-move-the-pointer
fix rests on reading the xdg-shell protocol, **not on a measurement**. It could not
be reproduced on the machine that wrote it. KWin is the compositor whose first
configure is 0x0, so the Linux box is where the bug should live.

Needed, in this order:

1. Check out the **pre-fix** commit and reproduce the blank launcher. *This is the
   control.* Without it the fix proves nothing.
2. Show it painting after the fix.
3. If it will not reproduce under KWin either, **say so** — that is a result, and it
   means the original report came from a compositor neither machine runs.

**This gates the upstream PR.** The dpjudas PR is otherwise ready and would carry
the Wayland, X11 and Cocoa fixes above. Offering an unreproduced protocol fix
upstream is exactly the thing to avoid — and this branch has already published one
feature upstream with a missing half (see the build-the-committed-state trap).

## 2. Re-validate the matrix suite's scene choices on this hardware

**`AGENTS.md` Tasks — Linux, item 2.** `screenblocks 12`, `doom2` = MAP06 and
`doom1` = E1M3 were each chosen from measurements taken on **one machine** (Intel
HD 6000, Metal). The code is platform-neutral; the choices are not — a different GL
driver and different timing can move both properties they were selected for.

Two measurements, both described under the "choosing a map" trap in `AGENTS.md`:

1. **Determinism, eight samples** — `--only baseline --scene doom2`, then
   `--scene doom1`. One identical-looking pair is not enough.
2. **The pass scan** — every pass must visibly act on the chosen map. This is the
   half that was disarmed for months on the old map choice.

If a map that is 8/8 pixel-identical on macOS is not on Linux, **that is information
about the engine, not a broken suite.** Record it either way.

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

## 5. Publish upstream to dpjudas — gated on task 1

Once task 1 has a control run, open the PR to `dpjudas/ZWidget` with the six commits
in the table above. `cocoa-modal-fixes` was verified clean against upstream head on
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
