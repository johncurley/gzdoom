# Agent notes — current state

Working state for this fork. Read this first, then `CONTRIBUTING.md` for how
work is verified here (that part is not optional — it is the house standard and
it is unusual).

- **Historical log:** `docs/history/agent-log.md` (~6,200 lines, 2026-06 to
  2026-08). An archive, not a guide. Its value is that it records what was
  **disproved**. Grep it before chasing anything in the Metal renderer.
- **Durable roadmap:** `docs/engine-modernization.md`
- **Metal field guide:** `.github/copilot-instructions.md` and
  `src/common/rendering/metal/README_METAL_RENDERER.md`
- **GPU capture runbook:** `docs/gpu-capture-protocol.md`
- **Linux session handoff:** `docs/handoff-linux.md` — two validation tasks that
  only Linux hardware can perform, both pending.

---

## What this fork is

GZDoom with a **native Metal renderer** for macOS (no MoltenVK), plus a native
POSIX platform layer for Linux/BSD (Wayland/X11 + libinput via ZWidget, no
SDL2). Both lines were merged on 2026-08-09; this tree carries both.

Development machine is an **Intel MacBookAir7,2, HD 6000, Metal 2.0, macOS
12.7.6** — GPU-bound, non-Retina. Many defaults here are tuned for it and are
runtime-gated on `MtGPUArchitecture::Intel` rather than compiled in.

Hardware available for verification: **macOS (Intel)** and **Linux**. A Windows
10 machine exists but the port has not been run there yet. Apple Silicon is
covered by CI compilation only — nothing has ever *run* on an M-series part.
Treat any claim about Windows or Apple Silicon as untested unless it cites a
measurement.

---

## Current state

### Metal renderer

Verified against OpenGL at the aobug viewpoint (AshesHardReset MAP01,
`save01.zds`):

| Area | State |
|---|---|
| Scene normal G-buffer | matches OpenGL within 1/255 |
| SceneFog | matches within 1 LSB, with a sensitivity control |
| Model vertex normals | signed packing; mean delta vs GL 0.626 (was 31.216) |
| Palette-tonemap LUT | invalidates correctly on restart / CVAR change |
| Linear depth | mean |diff| 0.18 |
| SSAO attenuation | **~0.047 in occlusion units still diverges — open, see below** |

### Launcher

macOS now uses the same ZWidget launcher as every other platform, plus an
"Add Files..." browse button on the play page (all platforms — no platform had
one before). Required three ZWidget Cocoa fixes; see
`ZWidget/cocoa: fix modal windows under a host that owns NSApp`.

### Tooling

- `tools/matrix/run.py` — golden-image regression over 11 postprocess configs.
  Runs with its **own config file**, a pinned window and a warmup launch, so it
  cannot contaminate your `gzdoom.ini` or drift with the display. Currently PASS.
- `tools/matrix/crossbackend.py` — Metal-vs-OpenGL oracle, per-band shape
  analysis. Currently 11/11 OK on macOS (gl,metal) and 11/11 OK on Linux
  (gl,vulkan, 2026-08-10). Runs on both platforms now; paths come from
  `launch_<platform>` blocks in `configs.json`, and `GZDOOM_MATRIX_BINARY`
  points it at an alternate build directory.
- `tools/pngdiff.py`, `localize.py`, `cluster.py` — stdlib-only PNG analysis
  (this machine has neither PIL nor ImageMagick).

---

## Open items

**SSAO residual.** Raw AO attenuation differs from OpenGL by ~0.047 in occlusion
units (12.52/255 on the debug view). Reaches the shipped frame as mean
**0.392/255**, max 11, on under 1% of pixels at maximum AO strength — measured
by isolating AO's contribution per backend, which cancels unrelated backend
differences. Bounded, not explained.

Ruled out by measurement, **do not re-test without new evidence**: screen-space
Y inversion (rendering `TexCoord.y` and `gl_FragCoord` gives identical values on
both backends), linear depth, scene normals, every SSAO uniform (dumped from
shared code — bit-identical), AO scene size, Metal's fast-math default, the
random texture's index/format/snorm decode/sign/wrap, and fragment-coordinate
phase. The divergence scales with sampling radius, which points at how
far-reaching samples are handled — the `sampleUV` clamp in
`ComputeSampleHorizon` and `LinearDepthTexture`'s default sampler state.

**OpenGL captures black on Ashes2063 + capspot.zds.** Scene-specific: the same
build captures AshesHardReset normally. Blocks nothing today because
`crossbackend.py` has its own scene override, and `run.py` pins Metal. Three
readback-level fixes were tried in 2026-08-08 and all failed; the capture
probably has to move before the buffer swap.

**Apple Silicon is untested.** Nothing here has ever run on an M-series part.
The Intel gates are runtime checks, so Apple Silicon takes the **compute** AO
and bloom paths by default — paths that have never executed on that hardware,
along with the Tier 2 argument-buffer paths. Expect the first run to be a
bug-finding exercise, not a benchmark. `run.py --update-baseline` is the first
useful command there; it refuses to record if any pass is broken.

**RESOLVED 2026-08-10 — the merged tree builds and runs on Linux.** Both
`docs/handoff-linux.md` tasks were carried out on the Linux box (Arch/CachyOS,
KDE Plasma Wayland, AMD RX 550 polaris12, Mesa 26.1.6, GL 4.6, Vulkan 1.4).

Task 1: `HAVE_VULKAN=OFF HAVE_GLES2=OFF` (the default) compiles clean with **no
`gles_*` duplicate- or missing-symbol errors**, so dropping the
`if (HAVE_GLES2)` block was the right call. The binary runs under the native
Wayland backend and the ZWidget launcher paints, listing all four IWADs and the
new "Add Files..." button. `-DHAVE_GLES2=ON` also builds.

Task 2: `crossbackend.py --backends gl,vulkan` is **11/11 OK** — every config
"uniform backend noise", median band mean 0.118–0.296, tone x1.00–1.01, no
structural divergence. The self-check passes for both backends on all 11
configs. So the shared-code changes made for Metal parity
(`lineardepth.fp`, `ssaocombine.fp`, the SSAO uniforms in `hw_postprocess.cpp`)
did **not** disturb Vulkan. Note "bit-identical" is not literally what was
measured: there is a uniform ~0.2/255 delta everywhere, which is the expected
backend noise the tool exists to distinguish from structure. `colormap` reports
the same sparse under-the-coverage-floor note as macOS, so that note is not
Metal-specific and is benign on Vulkan too.

Three things had to be fixed to get there; none was a renderer bug.

**`HAVE_VULKAN=ON` did not compile at all.** `nativevideo.cpp` calls
`I_GetVulkanPlatformExtensions` and `I_CreateVulkanSurface` above their
definitions at the bottom of the same file, and no POSIX header declares them
the way `win32vulkanvideo.h` does for Windows. Fixed with forward declarations.
This is why the Vulkan path had never been executed — it could not be built.

**Every OpenGL screenshot on Linux was solid black.** `GetScreenshotBuffer()`
runs after the buffer swap (`M_TickDeferredScreenShot` is called after
`D_Display`), and the window back buffer is undefined after a swap — EGL
defaults to `EGL_SWAP_BEHAVIOR = EGL_BUFFER_DESTROYED` and GLX promises nothing.
Reproduced on Wayland/EGL and X11/GLX, and on llvmpipe under Xvfb with no
compositor, so it is not a driver or compositor effect. Fixed by
`DFrameBuffer::ArmScreenshotCapture()`: the deferred-shot countdown arms the
backend, `OpenGLFrameBuffer::Update()` copies the frame out between `Flush()`
and `Swap()`, and the shot is taken one frame later off that copy. Vulkan and
Metal are unaffected — they re-present into their own image rather than reading
the swapchain — so it is a no-op there and the unarmed path is unchanged.

Control pair, same scene and harness: GL mean **0.000** before, **25.318**
after, against Vulkan's **25.366** on the same frame.

**Do not test this on a window that is not painting.** Two failed fix attempts
were caused by testing on DOOM2 MAP01 runs whose *window* was black for an
unrelated reason (see the open item below) and on llvmpipe, which also renders
nothing. In both, FB 0 and the offscreen pipeline texture read entirely zero, so
the capture path looked broken when it was being handed a black frame. Use the
matrix scene (AshesHardReset + `save01.zds`, with the harness warmup) — it is
the only bed here known to render reliably.

**The harness was macOS-only.** `configs.json` hardcoded
`build/gzdoom.app/Contents/MacOS/gzdoom` and `~/Documents/GZDoom/*.pk3`, and
`run.py` hardcoded the macOS screenshot directory (`M_GetScreenshotsPath()` is
`$HOME/.config/gzdoom/screenshots` on Unix). Now merged from optional
`launch_<platform>` / `crossbackend_launch_<platform>` blocks, with the macOS
values kept as the defaults so nothing about that machine changes.
`GZDOOM_MATRIX_BINARY` overrides the binary path for a second build directory.
`tools/pptest/make.py` shelled out to `zip`, which is not installed here; it now
uses stdlib `zipfile`.

`confirm_backend()` could not identify OpenGL on Linux: it looks for
"Initializing OpenGL backend", which the native POSIX backend never prints, so
every gl row came back `launch failed (log says None)` even when GL had started
correctly. It now falls back to the `GL_VERSION:` line, checking for an ES
context first so GLES is not reported as desktop GL.

**A savegame had to be made.** `capspot.zds` and `save01.zds` are macOS-local
data. The Linux `save01.zds` is AshesHardReset MAP01 ("Night School") at the map
start, **not** the macOS aobug viewpoint. Fine for crossbackend, which is a
within-machine comparison, but the two machines are not comparing the same frame
and `baseline.json` must not be shared between them.

**The self-check passes on an all-black frame.** The control run above reported
`REPRODUCIBLE (mean 0.000)` and "Self-check passed" for a capture that was
entirely black — two identical broken frames satisfy a reproducibility gate.
Worth a degenerate-frame check (near-zero mean, one distinct value) before the
gate is trusted again.

**Wayland windows do not paint until an unrelated event arrives.** Reported by
the maintainer: the launcher comes up blank on first run and only paints once
the pointer moves over it. `xdg_surface_handle_configure` acked the configure
and nothing else, and the toplevel-configure path sets `m_NeedsUpdate` **only**
when it is given a non-zero size — but a compositor's first configure is
normally 0x0, which is how it tells the client to choose its own size (KWin does
this). `m_NeedsUpdate` starts true, but the run loop consumes it on its first
pass, which can come before the handshake completes; `DrawSurface()` then
returns early because no buffer exists yet and the flag has already been
cleared. Nothing repaints until some later event sets it again.

Fixed by setting `m_NeedsUpdate` at the ack point, which is where xdg-shell
requires a buffer to be attached and committed. **UNVERIFIED** — the blank
launcher could not be reproduced on this machine (it painted on every attempt,
before and after the change), so there is no control run and the fix rests on
reading the protocol, not on a measurement. Needs confirming by someone who can
reproduce it. This is a ZWidget subtree change and, if it holds up, an upstream
bug affecting every ZWidget Wayland application — it belongs in the dpjudas PR
alongside the Cocoa fixes.

**OpenGL renders some maps as a black frame. Not the platform layer.** Still
open. Separate from both the capture bug and the Wayland paint bug — it survives
the fixes for both, and it is GL-only: Vulkan renders every case below
correctly.

It is **map-selective**, measured 2026-08-10, same binary, only `+map` changing:

| DOOM2 map, GL | native backend | SDL backend |
|---|---|---|
| MAP01 | 0.000 | 0.000 |
| MAP02 | 0.000 | 0.000 |
| MAP07 | 0.000 | 0.000 |
| MAP12 | 51.640 | 51.816 |

DOOM1 `E1M1` renders (37.235). AshesHardReset renders on both `+map MAP01` and
`-loadgame save01.zds` (24.119 / 24.513) — that is a *different* MAP01, since
Ashes is a total conversion that replaces it, which is why an earlier reading of
this as "bare IWAD versus mod" was wrong. It was always the map.

**The SDL column is the important one.** Built with `-DGZDOOM_NATIVE_LINUX=OFF`,
so upstream's SDL platform layer, no ZWidget, none of the fork's windowing —
verified by `libSDL2` being linked and `Native Linux backend initialized`
appearing zero times in the log. It reproduces identically. **The native
platform work did not cause this**, and no amount of work on the Wayland/X11
backend will fix it.

That it is map-selective rules out the window system on its own: a surface or
compositor fault cannot depend on which map is loaded.

Almost certainly the same defect as the older macOS item, "OpenGL captures black
on Ashes2063 + capspot.zds, scene-specific" — same signature, GL-only and
scene-selective, on Cocoa windowing, which shares nothing with the Linux
backends. One difference not yet reconciled: the macOS note says the *capture*
was black, whereas on Linux the **window** is black too (confirmed visually,
correct title bar over a black client area). Both FB 0 and the offscreen
pipeline texture read entirely zero, so the renderer is drawing nothing rather
than failing to present.

One pristine run did render DOOM2 MAP01 correctly and has never been reproduced;
every other attempt (4 samples over 32s in one run, plus five later runs across
two builds) was black. Treat that as unexplained, not as intermittency.

Why it matters beyond the bug: **the whole matrix suite runs on AshesHardReset**,
which is a map that works, so none of this is visible to `run.py` or
`crossbackend.py`. The 11/11 cross-backend result is real but exercises the
working path only. A config on a black-frame map would compare black to black
and pass.

**Upstream `master` does the same — this is inherited, not ours.** Built the
`master` mirror (`092b9c051`) in a worktree with its own defaults: no
`GZDOOM_NATIVE_LINUX` option at all, SDL2 linked, none of this fork's code.
DOOM2 MAP01 comes up as a black window with a correct "Entryway" title bar;
MAP12 on the same binary renders "The Factory" normally. Same map-selective
split as both fork builds.

The backend is confirmed by inference rather than by a log line, because
upstream prints nothing to stdout here: Vulkan renders DOOM2 MAP01 correctly
(measured 26.812), so a black MAP01 could not have been a silent Vulkan
fallback — `+vid_preferbackend 0` did select GL.

So the fork's shared-code changes for Metal parity did **not** cause it, and
neither did the native platform work. It predates all of it. Since this fork is
now the maintained port, that makes it ours to fix rather than ours to report.

**Best lead so far: opening the console makes the frame render.** Same launch,
same map, `+execafter 60 toggleconsole` — mean 27.199, max 255, against 0.000
max 0 without it. So the 2D path, the present and the capture all work; whatever
fails is upstream of them and is disturbed by whatever opening the console
changes (pause state, a forced full redraw, the `gamestate` the draw path sees).

Ruled out: `wipetype 0` (still black, so not the screen wipe), `screenblocks`
10 and 11 (both black, so not the inset scene viewport). Unexplained: at
`shotafter 400` MAP01 rendered once (27.493) while 120 and 900 were black —
settle-dependent in a way that is not monotonic and not yet understood. Note the
per-map results themselves are solid: MAP01 0.000 on 4 of 4 runs (plus ~8
earlier), MAP12 51.637-51.640 on 4 of 4.

The `master` worktree is left in place for bisecting; it is the cheapest way to
find which upstream commit introduced this.

**X11 has two loose ends, neither chased down.** The Wayland first-paint fix is
in `xdg_surface_handle_configure`, which is xdg-shell — X11 has no equivalent
path and was untouched, so it is *not* covered by that fix.

- `BadMatch` (opcode 42, `X_SetInputFocus`) is printed on every X11 backend
  start. Non-fatal — Xlib's default handler prints and continues — but it is a
  real protocol error, and the usual cause is `XSetInputFocus` on a window that
  is not viewable yet, i.e. a map/focus race.
- The launcher **exited on its own under bare Xvfb**, between 12s and 25s, with
  nothing in the log. Under XWayland with a real window manager it stayed up,
  so this may be a no-window-manager artifact rather than a bug; not confirmed
  either way. Note CI's smoke test runs under Xvfb but passes `-iwad`, so it
  never opens the launcher and would not catch this.

**ZWidget Cocoa fixes are not upstream yet.** `libraries/ZWidget` is a subtree.
The three Cocoa fixes sit directly on dpjudas's code (this fork has never
touched `src/window/cocoa/`), so they cherry-pick cleanly onto a branch off
`master` for a PR (verified: applies clean, 3 files). The Wayland work does not
— it is entangled with the waylandpp replacement.

Note for that PR: the commit directly beneath it on `master` is *"standardized
asynchronous Update() method to replace Repaint()"*, so the `dispatch_async` the
Cocoa fix removes is a deliberate, recent upstream design decision rather than
an oversight. The fix is still correct — a serial main queue starves it when the
host owns that queue — but it argues against a standing convention and the PR
should say so. Likewise the manual event pump is a pragmatic idiom, not the
canonical API: `-[NSApplication runModalForWindow:]` handles the modal-session
bookkeeping properly, but needs to know *which* window is modal, which
`RunLoop()` at backend level does not. Upstream may prefer that API shape.

**State the pump's limitation explicitly in the PR.** It does not disable other
windows and does not maintain a modal session, so it is sufficient for a
single-window host and **insufficient for a multi-window one** — an editor with
a document window behind a modal dialog would keep accepting clicks on the
document. GZDoom never notices because only one window exists at launcher time;
ZWidget is a general-purpose toolkit and its other consumers may not be so
lucky. dpjudas knows that consumer list and we do not, so give him the fact
rather than the reassurance.

If he wants it done properly, the change is additive and smaller than it looks:

- `virtual void RunModalLoop(DisplayWindow* modal) { RunLoop(); }` on
  `DisplayBackend` — defaulted, so **only Cocoa overrides it** and Win32, X11,
  Wayland, SDL2/3 and Haiku compile unchanged.
- Four call sites pass their window: `dialog.cpp`, plus GZDoom's
  `launcherwindow.cpp`, `errorwindow.cpp`, `netstartwindow.cpp`.
- Cocoa implements it with `runModalForWindow:`, and `ExitLoop` grows a third
  case — modal session, manual pump, or owns the loop.

Roughly half a day, nearly all of it testing the three exit paths (OK, Cancel,
window close) across the launcher, error and net-start windows. **It replaces
only one of the three fixes**: the use-after-free and the repaint starvation are
independent of which loop API is used, since the view still outlives its C++
owner and `dispatch_async` still cannot drain inside a main-queue block.

---

## Traps that have cost real sessions

Each of these produced a wrong conclusion that survived until something
measured it. They are listed because re-learning them is expensive.

### Build and cache

**The engine loads the pk3 next to the executable.** Rebuild
`build/gzdoom.app/Contents/MacOS/gzdoom.pk3`, not `build/gzdoom.pk3`. Updating
the wrong one leaves the old shader running with no error and no visible sign —
a correct AO fix was measured as a failure this way.

**A shader edit is not live until `zipdir` runs — including a revert.** Reverting
a diagnostic in the working tree while the pk3 still holds it produced a
measurement of 195.28 that meant nothing. Hit twice in one session. Re-zip
immediately after any shader change *or* revert, and confirm what actually ran
by grepping the translated MSL in
`~/Library/Application Support/zdoom/cache/*.msl`.

**Clear the PSO archive as well as the MSL cache.** `mt_pipelines.bin` serves
previously compiled pipelines and will silently mask a shader or compile-option
change. A fast-math A/B came back byte-identical until it was deleted.

**Discard the first launch after a build.** Cold shader/PSO compilation perturbs
the settle. GL is worse than Metal here, so you cannot infer it from one backend
looking stable.

### CVARs

**Archived CVARs leak between runs and into your game.** `vid_preferbackend` is
`CVAR_ARCHIVE | CVAR_GLOBALCONFIG`, so one OpenGL launch silently turns every
later "Metal" run into a GL run — caught only because a comparison returned a
suspiciously perfect 0.00. The same mechanism left `gl_bloom` disabled and
`gl_ssao` lowered in the developer's own config, which was then mistaken for a
performance win from unrelated code changes. **Pass every CVAR that matters on
the command line, use `-config` for throwaway runs, and confirm the backend from
the log** (`GL_RENDERER` present means OpenGL ran).

**A CVAR set in the console does not survive a restart.** Verify a pass is off by
its **missing** `mt_debug` label, not by having typed the command.

**`CUSTOM_CVAR` callbacks do not fire for command-line `+set`.** Anything whose
effect depends on the callback (e.g. `gl_paltonemap_powtable` invalidating the
palette LUT) must be changed at runtime via `+execafter` to be exercised.

### Measurement

**`Frame ... avg=` is the cost metric. `FrameGPU` is not.** It reports spans that
cannot coexist with the measured frame interval (~220ms against 7ms frames). The
cause is unresolved; the obvious fix is a no-op.

**Turn off vsync and the fps cap for any performance A/B.** With
`vid_vsync=true` and `vid_maxfps=60`, every config that clears 60fps reports
identically. Use `+vid_vsync 0 +vid_maxfps 0 +cl_capfps 0`.

**Run the effect's own on/off control before quoting a comparison.** In every
scene tested, the Metal-vs-OpenGL *background* difference was larger than the
entire effect of SSAO — so a shipping-path AO comparison proves nothing on its
own. Isolate the effect's contribution per backend and compare those.

**Clamped or thresholded statistics amplify tiny differences.** A diagnostic that
wrote a signed value through a path clamping at zero turned a sub-LSB shift into
an apparent 11% difference. Ask what the metric does to values near its limits
before believing it.

**State a numeric prediction before looking.** Every real defect found on this
branch was caught that way; several confident readings of the code were not.

### macOS specifics

**Carbon pollutes `Point`, `Size` and `Rect`.** Including ZWidget headers in a
`.mm` fails with "redefinition of 'Point'", and no include ordering fixes it.
Use the rename-across-the-include idiom from ZWidget's own
`src/window/cocoa/AppKitWrapper.h`. This is the macOS counterpart of the X11
`GC`/`None` pollution.

**GZDoom owns `NSApp` and runs `DoMain` inside a main-queue block.** Three
consequences, all of which bit at once in the launcher work: a nested
`[NSApp run]` does not deliver events; anything queued with
`dispatch_async(dispatch_get_main_queue(), ...)` from inside `DoMain` **never
runs**, because the main queue is serial; and gzdoom's `processEvents:` timer
drains the entire event queue, so it must stand down while another modal pump is
active.

**An NSView outlives the C++ object that owns it.** AppKit retains it through the
window and tracking area, and `if (impl && ...)` guards only catch null, not
dangling. Sever back-pointers in the destructor.
