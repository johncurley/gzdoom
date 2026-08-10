# Session handoff — Linux bring-up, and the GL black-frame bug

Written 2026-08-10, at the end of the session that brought `metal-audit` to
Linux. `docs/handoff-linux.md` is the task list this session was answering; it
is now marked DONE and should not be re-run.

Read `AGENTS.md` first — the open items there carry the measurements. This file
is the narrative: what was finished, what is still open, and what not to repeat.

---

## Finished, and verified

Both tasks from `docs/handoff-linux.md` pass.

**Task 1 — the merged tree builds and runs on Linux.** The default configuration
compiles clean with **no `gles_*` duplicate- or missing-symbol errors**, so
dropping the `if (HAVE_GLES2)` block during the merge was correct.
`-DHAVE_GLES2=ON` also builds. The binary runs under the native Wayland backend,
and the launcher paints with all four IWADs and the "Add Files..." button.

**Task 2 — `crossbackend.py --backends gl,vulkan` is 11/11 OK.** Every config
reports uniform backend noise, median band mean 0.118–0.296, tone x1.00–1.01,
with the self-check reproducible on both backends for all eleven. The
shared-code changes made for Metal parity (`lineardepth.fp`, `ssaocombine.fp`,
the SSAO uniforms in `hw_postprocess.cpp`) did **not** disturb Vulkan.

Two honest caveats on that number. "Bit-identical" is not what was measured —
there is a uniform ~0.2/255 delta everywhere, which is the noise the tool exists
to separate from structure. And the whole suite runs on AshesHardReset, so it
exercises only maps that render; see the open bug below.

Three fixes were needed to get there, none of them renderer bugs:

| commit | what |
|---|---|
| `8705d4f72` | `HAVE_VULKAN=ON` did not compile — missing forward declarations in `nativevideo.cpp`. This is why the Vulkan path had never run: it was unbuildable, not untested. |
| `8d8d35d36` | Every GL screenshot was black — `glReadPixels` after the buffer swap. Fixed by copying the frame between `Flush()` and `Swap()`. Control pair: mean 0.000 before, 25.318 after, against Vulkan's 25.366. |
| `16f16a835` | The harness was macOS-only in its paths and could not identify the GL backend on Linux. |

Also landed: `-nolauncher` (`00fd509a4`), stock-IWAD scenes plus a
degenerate-frame guard (`515179513`), and the ZWidget Wayland first-paint fix
(`d4fae73da`), which is **pushed to `zwidget/wayland-c-bindings`** and confirmed
working by the maintainer.

---

## Open: OpenGL renders a black frame on some maps

The one substantial thing left. `AGENTS.md` has the full measurement record;
this is the short version.

**Shape.** GL only — Vulkan always renders. Strongly map-biased: DOOM2 MAP01,
02, 03, 04, 06, 07, 09, 10 almost always black; MAP05, 08, 11, 12, 20, 21 always
render. Not absolute: across ~38 MAP01 runs, 3 rendered. So it is a biased race,
roughly 8% the other way, and the failing maps are the small fast-loading ones.

**Not ours, and not new.** Reproduces with `-DGZDOOM_NATIVE_LINUX=OFF` (upstream
SDL path, no ZWidget) and on the upstream `master` mirror with none of this
fork's code. Also present on macOS. So it is neither the native platform layer
nor the Metal-parity changes. As this fork is now the maintained port, it is
ours to fix rather than to report.

**Ruled out by measurement.** Do not re-test these without new evidence:

- the platform layer (SDL build reproduces), the fork's shared-code changes
  (upstream master reproduces), the GPU driver (llvmpipe reproduces exactly:
  MAP01 0.000, MAP12 53.915)
- the `!AppActive` early return in `D_Display` — instrumented, **never taken**;
  `vid_activeinbackground 1` accordingly changes nothing
- frame dropping generally — `D_Display` runs to completion and
  `End2DAndUpdate()` is reached every frame in both the black and the working
  case
- the scene branch — entered in both, `viewactive=1`, `screenvp`/`scenevp`/
  `screen` identical at 640x480
- the 2D command list — `con_notifylines 0` / `con_notifytime 0` black with and
  without, separately and together
- `vid_setmode` (rebuilds the framebuffer), the screen wipe (`wipetype 0`),
  `screenblocks` 10 and 11, `cl_capfps` 0 and 1, window size, and the
  maintainer's own config

**The one lever that works.** `+execafter 60 toggleconsole` renders the frame —
and the capture shows the *scene*, not just console pixels: Entryway geometry,
weapon sprite, and the 2D status bar. A benign `echo` does nothing. So the
renderer, textures, shaders, 2D layer and present path all work for a map that
is otherwise black, and the only variable that differs between the two runs is
`ConsoleState`.

**Where that leaves the search.** Below `D_Display`, between `RenderView` and
the final composite. The probe points used were: the `!AppActive` early return,
`D_Display` entry, the `gamestate == GS_LEVEL` scene branch, and
`End2DAndUpdate` — all env-gated on `GZDOOM_TRACE_DISPLAY` and reverted before
committing, so re-adding them is a few minutes' work.

**Suggested next step:** a GPU frame capture, or probes inside `RenderView` /
`FGLRenderer::Flush`, comparing console-open against console-closed to find
where the pipeline texture stops receiving the scene. Not more cvar roulette —
that avenue is exhausted, see the eliminations above.

**Reproduce in one command** (roughly 40s; expect `mean 0.000`):

```bash
GZDOOM_MATRIX_BINARY=build-vkonly/gzdoom \
  ./build-vkonly/gzdoom -iwad ~/.config/gzdoom/DOOM2.WAD -config /tmp/probe.ini \
  +vid_preferbackend 0 +vid_fullscreen 0 +cl_capfps 1 +map MAP01 \
  +shotafter 120 quit
python3 -c "import sys;sys.path.insert(0,'tools');from pngdiff import read_png,stats;\
import glob;print(stats(*read_png(sorted(glob.glob('$HOME/.config/gzdoom/screenshots/*.png'))[0]))['mean'])"
```

---

## Traps this session paid for

Each of these produced a wrong conclusion that had to be retracted. They are
here so the next person does not buy them again.

**One run proves nothing here.** At an ~8% flake rate, single samples produced
two confident, committed, wrong findings: "bare IWAD renders black and loading a
mod fixes it" (it was the map — Ashes replaces MAP01 with a different one), and
a config bisect that fingered `hud_vertical`, a key that **is not a cvar in the
source at all**, only a stale entry in a hand-grown ini. Repeat everything.

**The engine rewrites the config file on exit.** A `-config` copy stops being
the file you copied after its first launch. The `hud_vertical` bisect ran every
comparison against an engine-normalised file rather than the intended one. Copy
the config fresh for every launch.

**Do not test a capture path against a window that is not rendering.** Two
attempts at the GL screenshot fix were judged failures because they were
measured on DOOM2 MAP01 (this bug) and on llvmpipe under Xvfb. In both, FB 0 and
the offscreen pipeline texture read entirely zero, so a working fix looked
broken. Use the matrix scene, or a map known to render.

**`vid_preferbackend` is worth confirming from the log, not the flag.** Upstream
prints nothing to stdout, so on that build the backend had to be established by
inference instead (Vulkan renders MAP01, therefore a black MAP01 was not a
silent Vulkan fallback).

---

## Where things are

- **Branch `metal-audit`**, 13 commits ahead of `origin/metal-audit`, tree clean,
  **nothing pushed to origin**.
- **`zwidget/wayland-c-bindings`** is pushed and at `8e0db078a`; the subtree copy
  and the fork are byte-identical.
- **Build directories** (~4 GB total, all gitignored): `build` defaults,
  `build-vkonly` Vulkan+GL and the one the harness was driven against,
  `build-vk` Vulkan+GLES2 (superseded), `build-sdl`
  `-DGZDOOM_NATIVE_LINUX=OFF` control. Only `build-vkonly` and `build-sdl` are
  worth keeping.
- The `master` worktree used for the upstream control has been removed; recreate
  with `git worktree add --detach <path> master`.
- **This machine:** Arch/CachyOS, KDE Plasma Wayland, AMD RX 550 (polaris12),
  Mesa 26.1.6, GL 4.6, Vulkan 1.4. IWADs and mod pk3s in `~/.config/gzdoom/`.
  No `ccache`, no `zip`, no `xdotool`/`wmctrl`. `spectacle -b -n -f -o out.png`
  is how to screenshot a Wayland window; X11 root grabs come back black for
  Wayland-native windows.

## Still untested

- **Window focus.** The unattended runs never focus the window and interactive
  play always does, which would fit the bug. Against it: MAP12 rendered under
  Xvfb with no window manager at all. No window-activation tooling is installed
  here, so this was never settled.
- **Whether the black-frame bug is known upstream.** A search found no report of
  this shape, though "open the console" already circulates as a folk workaround
  for assorted GZDoom black screens, which may be this bug seen from outside.
- **Apple Silicon**, still, and unchanged by any of this.
