# Session handoff — Linux bring-up, and the GL black-frame bug

Written 2026-08-10, at the end of the session that brought `metal-audit` to
Linux. `docs/handoff-linux.md` is the task list this session was answering; it
is now marked DONE and should not be re-run.

Read `AGENTS.md` first — the open items there carry the measurements. This file
is the narrative: what was finished, what not to repeat, and (updated
2026-08-11) how the one bug it left open was closed.

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

## Solved: OpenGL rendered a black frame on some maps

Fixed 2026-08-11. `AGENTS.md` carries the full record — root cause, the probe
method, the verification table and the retained eliminations. Short version:

**A stale shader-binding cache.** `FShaderManager::SetActiveShader` skips
`glUseProgram` when the requested shader is already the one it believes is
bound. `FShader::Load()` ends with `glUseProgram(0)`, which breaks that belief
without telling the manager. Because shader compilation is incremental and the
start screen draws between compile steps, a shader bound for an early startup
draw could stay cached as "active" while GL's current program was 0 — after
which *no program was ever bound again* for the life of the process. Every
`glUniform` failed, every draw produced nothing, and the frame came out black
including the status bar.

**The fix** is 15 lines in `src/common/rendering/gl/gl_shader.cpp`: clear
`mActiveShader` at the end of `FShaderManager::CompileNextShader()`.

**Everything odd about the bug follows from that.** The cache repairs itself as
soon as any *different* shader is selected. Maps that always worked select a
second shader early (`Stencil`, `No Texture` both observed); maps that were
always black draw only with the already-cached one. `toggleconsole` worked
because console 2D selects `No Texture` — and a benign `echo` did nothing
because it selects no new shader, which is why "any console command" was never
the lever. The ~8% of runs that rendered anyway are runs where startup happened
not to leave a stale bind: **it was never a timing race, though it looked
exactly like one.**

**Verified**: MAP01 0.000 → 26.751–26.768 (4 of 4); the seven other
previously-black maps all render; the six that always worked are unchanged
(MAP12 51.640, identical to the recorded figure); the SDL control build
(`GZDOOM_NATIVE_LINUX=OFF`) is fixed too; `crossbackend.py --backends gl,vulkan`
is 11/11.

**Two things worth carrying forward:**

- **`MESA_DEBUG=1` names the failing GL call**, on stderr, for free. It is what
  cracked this — 17,423 `GL_INVALID_OPERATION in glUniform(program not linked)`
  per run. `gl_debug_level` produced *nothing* here, because the context is not
  a debug context; do not spend time on that cvar on this machine.
- **Readback probes at stage boundaries beat reasoning about the pipeline.**
  Four env-gated `glReadPixels` calls (scene FB, each postprocess pass, after
  `Draw2D`, after present) showed the scene framebuffer already empty at the
  first probe, which eliminated the entire downstream pipeline in one run.

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

**A biased success rate is not evidence of a timing race** (added 2026-08-11,
the fourth retraction). "35 of 38 black, 3 rendered" was recorded here as a race
and it steered the search toward load-time ordering for a whole session. The
cause was fully deterministic per map; the few successes came from startup
variation in a *cache*, not from a window that sometimes closed in time. Before
calling something a race, name the two things you think are racing — if you
cannot, the word is doing no work.

---

## Where things are

- **Branch `metal-audit`**, 14 commits ahead of `origin/metal-audit` as of
  2026-08-11, tree clean, **nothing pushed to origin**.
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

- **Reporting the black-frame fix upstream — there is nowhere to send it.** The
  defect is in inherited code and reproduces on upstream `master` and on macOS,
  so the fix applies unchanged anywhere downstream of GZDoom. But ZDoom/gzdoom is
  frozen (last substantive commits 2025-11; two trivial patches since) and its
  maintainer is not currently active, and UZDoom does not accept contributions
  from this fork. So this stays ours. **Do not spend time preparing a patch for
  either of them** — an earlier revision of this file suggested UZDoom, which was
  wrong.
- **Window focus** — was a live theory for the black frame and is now moot, but
  no window-activation tooling is installed here (no `xdotool`/`wmctrl`), so
  anything else needing a focused window still cannot be tested unattended.
- **`FShaderProgram::Link()` on GL < 4.20** leaves its own program bound without
  restoring the previous one — the same class of bug as the one just fixed. Not
  exercised on this machine (GL 4.6) and not touched.
- **Apple Silicon**, still, and unchanged by any of this.
