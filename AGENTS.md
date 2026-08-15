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
  only Linux hardware could perform. **Both done, 2026-08-10; do not re-run.**
  What is still open on that machine is the **Tasks — Linux** section below.
- **Current handoff:** `docs/handoff-gl-blackframe.md` — the GL black-frame bug,
  **fixed 2026-08-11** (a stale shader-binding cache; see the section below for
  the root cause and the verification table), plus the traps that produced four
  retracted conclusions along the way. Start there.

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
  analysis. Currently **12/12 OK on macOS** (gl,metal, 2026-08-15, all configs
  including the scene-keyed `baseline_bloom`) and 11/11 OK on Linux
  (gl,vulkan, 2026-08-10). Runs on both platforms now; paths come from
  `launch_<platform>` blocks in `configs.json`, and `GZDOOM_MATRIX_BINARY`
  points it at an alternate build directory.
- `tools/pngdiff.py`, `localize.py`, `cluster.py` — stdlib-only PNG analysis
  (this machine has neither PIL nor ImageMagick).

---

## Tasks — Linux

Work that **needs the Linux box** (Arch/CachyOS, KDE Plasma Wayland, AMD RX 550,
Mesa 26.1.6, GL 4.6, Vulkan 1.4) because macOS cannot perform or cannot check
it. `docs/handoff-linux.md` is finished — its two tasks passed on 2026-08-10 and
must not be re-run. This is what came out of that session and the two since,
roughly in order of value. Everything below is either an unverified fix, an
unmeasured assumption, or a tool gap; none of it is speculative work.

An outside audit of this subsystem was run on 2026-08-12 against
`docs/audits/audit-contract-linux.md`; its report is
`docs/audits/findings-linux-2026-08-12.md` and items 10 and 11 came out of it.
It read a **pruned export** of the tree with `AGENTS.md`, `CLAUDE.md` and
`docs/` deleted, so §8 blindness was structural rather than requested — worth
repeating that way, because the previous round could not honestly claim a blind
read.

Ordinary build there:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPK3_QUIET_ZIPDIR=ON -DHAVE_VULKAN=ON .
cmake --build build --parallel $(nproc)
```

### 1. Confirm the Wayland first-paint fix — it has no control run

The blank-launcher-until-you-move-the-pointer fix (`m_NeedsUpdate` set at the
`xdg_surface_handle_configure` ack point, ZWidget subtree) **rests on reading
the xdg-shell protocol, not on a measurement**. It could not be reproduced on
the machine that wrote it — the launcher painted on every attempt, before and
after. KWin is the compositor whose first configure is 0x0, so this box is where
the bug lives.

Needed: reproduce the blank launcher on the pre-fix commit (that is the control;
without it the fix proves nothing), then show it painting after. If it will not
reproduce here either, say so — that is a result, and it means the report came
from a compositor neither machine runs.

This one gates something: it is an upstream ZWidget bug affecting every ZWidget
Wayland application, and it belongs in the dpjudas PR alongside the Cocoa fixes.
That PR is otherwise ready (`zwidget/cocoa-modal-fixes`, two commits directly on
upstream head, verified 2026-08-11).

### 2. Re-validate the matrix suite's scene choices on this hardware

`screenblocks 12`, `doom2` = MAP06 and `doom1` = E1M3 were each **chosen from
measurements taken on one machine** (Intel HD 6000, Metal). The code is
platform-neutral; the choices are not. A different GL driver and different
timing can move both properties they were selected for.

Two measurements, both from the "choosing a map" trap entry below:

1. **Determinism, eight samples** — `--only baseline --scene doom2`, then
   `--scene doom1`. A single identical-looking pair is not enough: MAP01 and
   MAP02 both read 0.00 on one pair and then produced 4 and 5 distinct states
   over eight launches.
2. **Every pass visibly acts** — the `%px differing` scan for `gl_ssao 3` and
   `gl_bloom`, by the suite's own `>2 levels` metric.

If a map that is 8/8 pixel-identical on macOS is not here, **that is information
about the engine, not a broken suite** — record the states, do not just swap the
map. The quoted 0.00% noise floor is what that GPU produced, not a guarantee.

Only after both pass: record a Linux `baseline.json`. It **must not be shared
between machines** — the Linux `save01.zds` is AshesHardReset MAP01 at the map
start, not the macOS aobug viewpoint, so the two boxes are not comparing the
same frame even on the mod scene.

**Linux result, 2026-08-13:** E1M3 passed the determinism control (8/8 samples
were identical) and the pass-action scan: `gl_ssao 3` changed 45.46% of pixels,
reference bloom changed 32.66%, and compute bloom changed 32.66%. MAP06 passed
the pass-action scan (`gl_ssao 3` 7.12%, reference bloom 34.11%, compute bloom
34.05%) but failed determinism: eight unattended samples produced seven
`4b46f461` states (mean 21.856) and one `81668f46` state (mean 21.917). The
MAP06 result was repeated with no interaction during the run, so do not blame
the pointer or substitute another map based on a single pair. No Linux
baseline was recorded because the two chosen scenes did not both pass.

### 3. `crossbackend.py` drops a config's own map — and that disarms bloom

`crossbackend.py:158` calls `matrix.launch(forced, spec, verbose)` with **no
`scene` argument**, so `effective_map()` resolves a scene-keyed map dict
(`{"doom2": "MAP12"}`) against `"default"` and gets nothing. The three bloom
configs — `baseline_bloom`, `bloom_ref`, `bloom_compute` — therefore run on the
scene's own map under `--scene doom2`, MAP06, instead of the map they were given
for a measured reason.

**Fixed 2026-08-13** — `launch_backend()`, `selfcheck()` and all three call
sites now thread `scene`; the savegame-override path deliberately keeps
`scene=None`, which was previously true by accident. Verified on Linux with
`--backends gl,vulkan` on both stock scenes: 11/11 OK for `doom2` and 11/11 OK
for `doom1`. The stock scene definitions also now use Linux's case-sensitive
`DOOM.WAD`/`DOOM2.WAD` names.

**How bad it was depends on which of two documents you believe, and they
disagree — see the note under the map-choice trap below.** `configs.json`'s
`_map_choice_note` records MAP06 bloom at **6.9%** of pixels against MAP12's
**50.9%**, making this a ~7x loss of signal. The table in *this* file records
MAP06 bloom as **dead**, which would instead make it the same "reports ok on a
dead pass" failure the map audit exists to prevent. Both are dated 2026-08-12.
Until that is settled, claim the signal loss — it is the weaker statement and it
is true under either reading.

One consequence of the fix worth watching on the re-run: with the bloom trio
back on MAP12, that scene's launch-to-launch torch variation (0.004% of pixels,
which bloom amplifies) now enters those rows. That is ~12px of the ~292,320px
compared region, under the 100px coverage floor — reasoned, not observed.

**Why this lands here rather than on macOS:** crossbackend compares two
*independent implementations*, which is a stronger invariant than one
implementation agreeing with its own past, and needs no baseline file and no
determinism across time. This is the only machine with two non-Metal backends
that both run.

### 4. `run.py` has no degenerate-frame guard — `crossbackend.py` does

The 2026-08-10 finding ("the self-check passes on an all-black frame") **was
acted on, in `crossbackend.py` only**: `DEGENERATE_MEAN = 1.0` at line 144,
enforced in `selfcheck()` at line 321, set above a blank frame and below the
darkest real scene at 13–52. Do not re-do that.

`run.py` has no equivalent — read 2026-08-12, there is no mean-based check
anywhere in it. So nothing there separates "identical because the pass is a
no-op" from "identical because both captures are blank". `must_differ_from`
happens to catch a fully-black suite (black equals black, so it fails), but
`must_match` is satisfied by it, and that is the relation with no independent
proof behind it — its own docstring says it is "only meaningful when some
sibling config carries a `must_differ_from`".

Small, and the reason it belongs on Linux is that an all-black capture was a
live condition here twice (readback-after-swap, and the stale shader binding),
where on the Metal machine it has never occurred.

**Done 2026-08-12.** `DEGENERATE_MEAN` now lives in `run.py` as the single
definition (`crossbackend.py` imports it), and `run.py`'s relation loop fails
either relation kind when either capture's `mean_lum` is at or below it.
Control: the darkest real pair in the recorded baseline, 13.25 vs 21.63, is not
flagged.

### 5. Two X11 loose ends — the `BadMatch` is now traced to two sites

The Wayland first-paint fix is in xdg-shell and **does not cover X11**.

**`BadMatch` (opcode 42, `X_SetInputFocus`) on every X11 backend start.**
~~Non-fatal — Xlib's default handler prints and continues~~ — **wrong, and
measured wrong on 2026-08-14: it is fatal.** Xlib's default error handler
prints and then calls `exit(1)`. See the bare-Xvfb result below. Read 2026-08-12 (code only, nothing run), and **two independent
readings landed on the same site**, one of them an outside audit that had not
seen this file.

`XSetInputFocus` is reachable from exactly two places, both in the ZWidget
subtree, both through the `dlopen` shim in `x11_remap.h`:

| | site | reached when |
|---|---|---|
| A | `x11_display_window.cpp:327`, in `Activate()` | `_NET_ACTIVE_WINDOW` does **not** exist — i.e. no EWMH window manager |
| B | `x11_display_window.cpp:737`, in the `WM_TAKE_FOCUS` handler | a WM sends the ICCCM message |

`SetInputFocus` returns `BadMatch` for one reason only: the focus window is not
viewable when the server processes the request. A stale timestamp makes the
server *ignore* the request, not fault it, so `RevertToParent`/`CurrentTime` are
not suspects at either site. Neither site checks viewability, and nothing in
this backend tracks it — `OnEvent` selects `StructureNotifyMask` but has **no
`MapNotify` case at all**, so the one event that reports viewability is received
and dropped.

**The correction worth keeping: at site A this is not a race, it is a fixed
ordering.** `LauncherWindow`'s constructor ends with `PlayGame->SetFocus()`,
whose `OnSetFocus` focuses the games list first, so `Activate()` runs **twice**
before `ExecModal` ever calls `Show()` — the X window exists, `XMapRaised` has
not been called. That predicts exactly 2 messages per launcher start, every
start, which is what "on every start" already suggested. The earlier
"map/focus race" reading in this file was wrong in a useful way.

**Linux result, 2026-08-13:** Under XWayland on this KDE session,
`ZWIDGET_DISPLAY_BACKEND=X11 DISPLAY=:0` with `-iwad DOOM2.WAD` produced no
`BadMatch` or other X protocol error, and the launcher path without `-iwad`
also produced none during a 20-second run. The game control bypassed the
launcher `Activate()` path and exited cleanly after a capture; the launcher
remained alive until the timeout. This environment therefore did not
reproduce either suspected site. The discriminator result is still useful:
it does not justify an X11 fix or claim the bare-Xvfb observation is solved.

**Bare-Xvfb result, 2026-08-14 — site A confirmed, and the error is fatal.**
Ran the discriminator above under `Xvfb :77 -screen 0 1280x1024x24`, which has
no EWMH window manager, so `_NET_ACTIVE_WINDOW` is absent and site A's branch is
live:

| launch | `BadMatch` | exit | elapsed |
|---|---|---|---|
| `-iwad DOOM2.WAD +quit` (skips launcher) | 0 | 0 | 1.7s |
| no `-iwad` (launcher path) | **1** | **1** | **0.25s** |

Site A (`x11_display_window.cpp:327`, `Activate()`) is therefore the site, and
site B is not implicated. The prediction of *exactly 2* was wrong for a reason
that matters more than the count: **Xlib's default error handler calls
`exit(1)`**, so the process dies on the first one and a second can never be
observed. The stdout that appears after the error in a combined log is buffered
early-startup text flushed at exit, not execution continuing.

Consequence, and it is bigger than "a noisy protocol error": **the launcher
cannot start at all on an X server without an EWMH window manager.** It dies in
a quarter of a second. This also supersedes the "launcher exits on its own
between 12s and 25s" note below — under this build it is immediate, and the
cause is the error handler, not a timeout.

CI does not catch it precisely as predicted: the smoke test passes `-iwad`, the
row that scores 0.

**FIXED 2026-08-14, `89d79bcbe`, published upstream as `dd88a86b7`.** Both
halves of the fix direction turned out to be needed, and the class already had
the state to do it — `isMapped`, which this file wrongly said was untracked:

- `Activate()` defers when `!isMapped` and `Show()` discharges the pending
  activation. That addresses the root cause at both sites, including the
  *silent* EWMH variant noted below — a `_NET_ACTIVE_WINDOW` ClientMessage for
  an unmapped window was simply thrown away, so the launcher's initial focus
  intent was being lost under a window manager too, with no error to notice.
- The fallback `XSetInputFocus` and the `WM_TAKE_FOCUS` handler both check
  viewability first, via a shared `IsWindowViewable()`. `XGetWindowAttributes`
  is a round trip, so it doubles as the synchronisation that guarantees a
  preceding map has been processed. `WM_TAKE_FOCUS` keeps its WM-supplied
  timestamp.

Measured, bare Xvfb, launcher path: **1 `BadMatch` / exit 1 / 0.25s → 0 / exit
124 / ran the full 25s**. Controls unchanged — `-iwad` under Xvfb 0 and exit 0,
launcher and game under XWayland+KWin 0 X errors, native Wayland unaffected.

This also resolves the "launcher exits on its own under bare Xvfb between 12s
and 25s" note below: that was this, and it now stays up.

**Site B is exercised here after all, and the guard does not block it.**
Instrumented the `WM_TAKE_FOCUS` handler and ran under XWayland+KWin: the
launcher receives **2** messages and the game **1**, all three with
`viewable=1`, so focus is taken normally. KWin supports `_NET_ACTIVE_WINDOW`,
so site A's fallback is dead there — but the window advertises
`WM_TAKE_FOCUS` in `WM_PROTOCOLS`, so site B runs on every start under a WM.
That was the regression risk in guarding it, and it is now measured rather
than reasoned.

Still untested, and not reachable on this box: `WM_TAKE_FOCUS` arriving while
the window is *not* viewable — the case the guard exists for. That needs a
non-EWMH window manager that focuses early, and **no window manager at all is
installed here** (`twm`, `mwm`, `fvwm`, `blackbox`, `cwm`, `jwm`, `icewm`,
`openbox`, `matchbox`, `dwm`, `wmaker` all absent). `twm` is the right one to
install for it — ICCCM-era, no `_NET_ACTIVE_WINDOW`, uses `WM_TAKE_FOCUS`.

The two sites are mutually exclusive on the axis nobody recorded: whether the
observing session had an EWMH window manager. Under bare Xvfb, site A is
certain and site B unreachable; under KWin (including XWayland), site A's branch
is dead code and only B remains. The `BadMatch` was seen alongside the Xvfb
launcher-exit note below, which points at A — but that was not written down at
the time.

**Cheapest discriminator, one launch: run with `-iwad`.** That skips the
launcher entirely, and nothing on the game-window path calls `Activate()`. Error
still present ⇒ site B. Absent, with exactly 2 on a launcher start ⇒ site A. Do
this before reaching for an error handler and `XSync`, which costs a subtree
rebuild.

Fix direction at either site: bail unless the window is viewable
(`XGetWindowAttributes` is already loaded as `p_GetWindowAttributes`), or handle
`MapNotify` properly and discharge a pending activation on it. Do **not** drop
`WM_TAKE_FOCUS` from `WM_PROTOCOLS` — that changes the input model. This is
stock upstream ZWidget code, not fork-specific, so it belongs in the dpjudas PR.

Related, and silent rather than noisy: at site A's *other* branch the
`_NET_ACTIVE_WINDOW` ClientMessage is sent for a window that is not yet mapped,
so a WM has nothing to activate and the launcher's initial focus intent is
probably lost on X11 under a WM too — with no protocol error to notice.
- **The launcher exits on its own under bare Xvfb**, between 12s and 25s, with
  nothing in the log. Under XWayland with a window manager it stayed up, so this
may be a no-WM artifact rather than a bug; not confirmed either way. CI would
not catch it — the smoke test passes `-iwad`, so it never opens the launcher.

**X11 sizing result, 2026-08-13:** The fullscreen window initially displayed
the renderer's 640x480 image in the bottom-left. X11 `ConfigureNotify` reported
the correct 1920x1080 client size, but the EGL path had not initialized the
optional Xlib geometry function table, so `SystemGLFrameBuffer` silently fell
back to 640x480. The framebuffer also needed to recalculate its viewport after
the asynchronous resize. The accessors now use ZWidget's X11 pixel geometry,
and framebuffer initialization/resize reconciles the size and viewport. The
rebuilt fullscreen X11 run was visually confirmed correct.

**Addendum, 2026-08-14 — that diagnosis was right but incomplete, and the
reconciliation half was crashing Vulkan.** Two follow-ups, both committed:

- `a60ea956d`. The accessor fix above landed on `SystemGLFrameBuffer` only.
  `SystemBaseFrameBuffer::GetClientWidth/Height` — which `VulkanRenderDevice`
  inherits and never overrides — was still `return 640;`/`return 480;`, a stub
  present since `fbc831511` (2026-05-04). So the *same symptom* existed on
  Vulkan under both Wayland and X11, and had nothing to do with the Xlib
  function table. Measured: the present blit was going to a 640x360 rect at
  (0,60) of a correctly-sized 1920x1080 swapchain, and `AcquireImage`'s resize
  test (`GetClientWidth() != CurrentWidth`) compared 640 against 640, so the
  swapchain was never rebuilt on resize. The accessors now live on the base
  class; the GL subclass keeps its direct-X11 query only as a fallback for when
  there is no ZWidget window. Every other platform backend (win32, cocoa, SDL)
  already implemented these on the base class — the native port is the only one
  that stubbed them.
- `e1f47ce5b`. The "framebuffer initialization reconciles the size" half was
  `screen->Update()` in `IVideo::SetResolution`, a **virtual** call. On Vulkan
  it dispatches to `VulkanRenderDevice::Update()`, the frame-end present path,
  before any frame has begun — dereferencing a `VkRenderBuffers::PipelineImage`
  that only exists after `BeginFrame()`. Startup SIGSEGV, between the Vulkan
  device banner and `W_Init`. It is now `screen->DFrameBuffer::Update()`: the
  base implementation is the whole of the resize reconciliation, the overrides
  are the present path.

Verification used `vid_showcurrentscaling` (prints `GetClientWidth/Height` as
"Real resolution") plus `vid_setsize` for a runtime resize, one backend per
launch. Note `vid_setmode` does not exist in this build. Still untested: an
actual mouse-drag resize under a compositor-initiated Wayland configure, which
is a different entry point from `vid_setsize`.

### 6. X11 raw input via XInput2

The standing feature gap: `in_rawkeyboard` works on Wayland and X11 has no
equivalent path. Whatever lands must respect the `I_StartTic()` /
`ResetButtonTriggers()` trap in `CLAUDE.md` — without it a single tap latches a
button on for every later tic, and every input trace still shows balanced
press/release pairs.

**Verified 2026-08-13:** X11 selects `XI_RawKeyPress` and
`XI_RawKeyRelease`, enables delivery from `LockKeyboard()`, and forwards the
focused window's events as `OnWindowRawKey` using the X11 keycode minus 8
(evdev/`RawKeycode` numbering). The first interactive run exposed that the
root subscription was using `MasterPointerID`, which silently excluded raw
keyboard events; selecting `XIAllMasterDevices` fixed it. The rebuilt Linux
binary then produced matching raw press/release events for forward, left,
back, and right, with the held-button set returning to `(none)` after every
release.

### 7. Remove the input diagnostics

The temporary `in_keytrace` diagnostic was removed after the X11 trace passed.
`ZWIDGET_TRACE_REPEAT` is not present in the current tree.

### 8. `FShaderProgram::Link()`'s old-GL path — the premise was wrong

**Downgraded 2026-08-12.** This file previously called the `glslversion < 4.20`
branch of `gl_shaderprogram.cpp` "the same class of bug as the GL black-frame
defect". Code reading says it is not, for three independent reasons, any one
sufficient:

1. **`mActiveShader` does not track this class of object.** It caches `FShader*`
   — material shaders. `FShaderProgram` is a different class (a
   `PPShaderBackend`) and nothing ever assigns one to `mActiveShader`, so
   `Link()` cannot make that cache disagree with itself.
2. **Every reachable call sits inside an `FGLPostProcessState` bracket**, whose
   constructor saves `GL_CURRENT_PROGRAM` and whose destructor restores it. All
   four paths were checked (`GLPPRenderState::GetGLShader` via `Draw()`,
   `FPresentShader::Bind` via `CopyToBackbuffer` and via `PresentStereo`,
   `FShadowMapShader::Bind` via `UpdateShadowMap`). That is exactly what the
   black-frame bug lacked: `FShader::Load()`'s `glUseProgram(0)` ran during
   incremental compilation interleaved with drawing, outside any bracket.
3. **`FShaderProgram::Bind()` is an unguarded `glUseProgram` on every GL
   version**, on the same paths, shortly after `Link()`. If leaving a program
   bound were the defect, GL 4.6 would be broken too.

The `glUseProgram` at the head of that branch is also *required* — the
`glUniform1i` calls after it are the non-DSA form and act on the current
program. Worst case if it did desync is one screen-quad draw with a valid but
wrong fragment shader, self-correcting at the next `Bind()`; the black-frame
bug's severity came entirely from the stranded program being **0**. Leave it
alone; at most add a comment recording that the bracket is what makes it safe.
Do not add a save/restore inside `Link()`.

**The test recipe recorded here was also wrong, and would have produced a clean
run that meant nothing.** `MESA_GL_VERSION_OVERRIDE=4.1` alone does *not* reach
this branch: `glslversion` derives from `GL_SHADING_LANGUAGE_VERSION`, a
different string, which still reports 4.60. `MESA_GLSL_VERSION_OVERRIDE=410` is
the variable that does the work — but on its own it leaves `gl_version` at 4.6,
which keeps `RFL_SHADER_STORAGE_BUFFER` set, and the shadow-map shader is then
emitted as `#version 410` while still carrying `layout(std430, binding = 4)`,
invalid before GLSL 4.30 → `I_FatalError`. **Set both, or neither.**

Cheaper still, and the engine's own switch: **`-glversion 3.3`** forces
`gl_version = 3.31`, which trips the force-down that sets `glslversion` to 3.31,
taking the branch on any driver with no environment variables and clearing the
SSBO flag so the shadow-map hazard cannot fire. Caveat, and it is the kind that
wastes a session: the confirming log line is **`Emulating OpenGL v 3.3`**, not
`GL_VERSION:` — `gl_PrintStartupLog` prints the driver's real 4.6/4.60 strings
regardless, so the startup log is actively misleading here.

Verified as a side effect: on GL 4.6 `PatchShader` promotes `maxGlslVersion` to
420 for every postprocess shader, so `RemoveSamplerBindings` never runs and
`samplerstobind` is always empty. Both halves of the old-GL path are dead on
this hardware, which is what "untested" meant.

### 9. No CI job builds Vulkan

`HAVE_VULKAN` appears nowhere in `.github/workflows/continuous_integration.yml`.
`HAVE_VULKAN=ON` **did not compile at all** until 2026-08-10 — `nativevideo.cpp`
called `I_GetVulkanPlatformExtensions` and `I_CreateVulkanSurface` above their
definitions with no POSIX header declaring them — and that had gone unnoticed
because nothing ever configured it. A single Linux job with the flag on would
have caught it at zero cost. Compile only; CI has no GPU and the smoke test
cannot exercise the backend.

**Done 2026-08-12**, as a separate "Linux GCC 12 Vulkan" matrix entry with a
`compile_only` flag that skips the smoke test and both packaging steps. It is
deliberately *not* folded into an existing job: `DEFAULT_RENDER_BACKEND` becomes
1 when `HAVE_VULKAN` is defined, so an existing job would pass its smoke test
only via the OpenGL fallback and would upload a Vulkan-default package. **No
extra apt packages are needed** — checked, not assumed: ZVulkan vendors the
headers, `find_package(Vulkan)` is inside the Apple branch, volk and ZWidget
both `dlopen` `libvulkan.so.1`, and the only new system header is
`<X11/Xlib.h>`, already covered by `libx11-dev`. The job has never run.

**2026-08-14 — compile-only is demonstrably not enough.** Two Vulkan-only
defects were found by hand in one session, and **neither would have been caught
by this job**, because both are runtime:

- `a60ea956d`. `SystemBaseFrameBuffer::GetClientWidth/Height` returned a
  hardcoded 640x480 and `VulkanRenderDevice` inherits them. Compiles perfectly.
  Live since `fbc831511` (2026-05-04) — roughly three months in the tree.
- `e1f47ce5b`. A virtual `screen->Update()` during `IVideo::SetResolution`
  reaching `VkPostprocess::SetActiveRenderTarget()` before any frame had begun.
  Compiles perfectly; segfaults on startup.

The reason GL never had either is that GL is what gets exercised — by hand and
by the Xvfb smoke test. Vulkan is compiled and never launched, so the entire
class of "builds fine, dies at runtime" is invisible on that backend.

Worth investigating: Mesa ships **lavapipe**, a CPU Vulkan ICD, so a CI job
*could* actually launch the Vulkan backend under Xvfb rather than only linking
it. That would have caught `e1f47ce5b` outright — it dies before `W_Init`, the
same assertion the existing smoke test already makes — and plausibly
`a60ea956d` too if the check compares `GetClientWidth()` against the Xvfb
screen size. Not yet tried; the cost is one apt package
(`mesa-vulkan-drivers`) and `VK_ICD_FILENAMES`, and the risk is the usual
software-rasteriser flakiness. Do not fold it into the compile-only job —
`DEFAULT_RENDER_BACKEND` becomes 1 under `HAVE_VULKAN`, which is the same trap
recorded above.

### 10 and 11. Wayland bind versions and clipboard init order — FIXED, not published

From the outside audit, 2026-08-12 (`docs/audits/findings-linux-2026-08-12.md`,
findings 1 and 2). Both were **fixed the next day in `c3474d697`**, "ZWidget:
clamp Wayland binds and initialize clipboard in either order".

These two items previously stood here as open work, describing the bugs in the
present tense long after they were fixed. Re-verified against the tree on
2026-08-14; the description below is what the code now does.

- **Finding 1 (bind versions).** `registry_handle_global` ignored its `version`
  argument and bound every interface at a hardcoded constant, so a compositor
  legally advertising a lower version could raise a protocol error and
  disconnect the client before a window existed. Every bind is now
  `std::min(version, N)`.
- **Finding 2 (clipboard).** The data device was created only in the
  `wl_data_device_manager` branch and only `if (backend->m_waylandSeat)`, with
  no retry from the `wl_seat` branch — so the reverse announcement order left
  the clipboard permanently dead, silently. There is now an idempotent
  `EnsureDataDevice()` called from both branches.

The second half of finding 1's fix direction — "gate listener entries and
requests on the version actually obtained" — was checked and needs nothing:

- Clamping is downward only, so a listener struct always has at least as many
  entries as the bound version can deliver. Extra entries are never called,
  because the compositor does not send events above the bound version.
- The one listener with a genuinely version-dependent entry,
  `wl_pointer_listener`, already guards it with
  `#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION`.
- No version-gated *request* is used anywhere in this backend — `grep -rn
  '_RELEASE\b'` over `src/window/wayland/*.cpp` is empty, so there is no
  `wl_seat.release`/`wl_output.release` class of problem to gate.

**What is actually still open: publishing.** `c3474d697` exists only on
`metal-audit`. It is not on `zwidget/wayland-c-bindings` or any other
`zwidget/*` ref. This is stock upstream ZWidget code, so the fix belongs in the
dpjudas PR alongside the Cocoa fixes, and it goes through the
cherry-pick-and-publish procedure in `CLAUDE.md` — **not** `git subtree push`.

Neither defect can be reproduced on this machine, which is why they were fixed
on reading alone. Measured 2026-08-14 under this KDE session, requested versus
what KWin advertises:

| interface | requested | KWin advertises |
|---|---|---|
| `wl_compositor` | 4 | 6 |
| `wl_seat` | 8 | 10 |
| `xdg_wm_base` | 4 | 6 |
| `wl_output` | 3 | 4 |
| `wl_data_device_manager` | 3 | 3 |
| `zxdg_output_manager_v1` | 3 | 3 |

KWin also announces `wl_seat` (10th) before `wl_data_device_manager` (14th), so
finding 2's ordering never occurs here either. Two interfaces have zero
headroom, which is worth knowing but is not a defect now that the clamp exists.
A real reproduction needs a nested compositor advertising lower versions, or a
`libwayland` proxy that rewrites registry advertisements; **nothing suitable is
installed on this box** (`weston`, `sway`, `cage`, `mutter`, `labwc` all
absent), and building that harness costs more than the fixes did. Do not treat
it as a prerequisite.

Note what the audit method bought: both came from a model that had never seen
this project's notes, reading a pruned export with `AGENTS.md` and `docs/`
removed, so it could not have been repeating anything.

---

## Tasks — macOS

Written 2026-08-14 at the end of a Linux session, as the entry point for the
next macOS one — the mirror of **Tasks — Linux** above. Start with
`git pull` on `metal-audit`: that branch is the integration branch and already
contains everything, `native-platform-expansion` is 229 behind it with nothing
of its own, and `master` is the upstream mirror. No merge is needed.

Ordered by value.

### 1. Re-run `crossbackend.py` — shared code changed under Metal — DONE 2026-08-15

**Result: 12/12 OK**, scene `doom2` (MAP06), `gl,metal`, on the reference
MacBookAir7,2. Self-check first: both backends REPRODUCIBLE (`baseline` gl mean
20.981 / metal 20.989; `ssao` gl 20.744 / metal 20.245). No regression from
`e1f47ce5b` — its two Metal-reaching changes are viewport-shaped, and a viewport
or extent fault shows as a half-frame or a displaced band, which is precisely
the shape this tool resolves. Nothing of that shape appeared; passing median
band means ran 0.013–0.609.

One row needed judgement rather than being clean: `colormap` reported SUSPECT at
*band 0 mean 0.035, 69.3x the median band (0.001)* — the same palette
quantisation recorded for it on 2026-08-09, magnified into a large ratio by a
near-zero denominator. That was a hole in the tool, now closed by
`BAND_MIN_OUTLIER_MEAN`; see below. `colormap` reads OK with the faint outlier
still printed.

Worth a note for whoever runs the suite next: AGENTS.md records that roughly
**half** of full-suite runs used to report *some* config SUSPECT with means of
15–70 and a wandering victim, cause unmeasured. This run showed nothing of the
kind. The status-bar-face nondeterminism found on 2026-08-12 and now pinned by
`screenblocks 12` is the obvious candidate, but **one clean run is not evidence**
— it would take several full-suite runs to claim that, and nobody has.

#### The ratio test had no absolute floor (fixed, `crossbackend.py`)

`BAND_OUTLIER_RATIO` divided by the median band mean with only a `1e-9`
divide-by-zero guard. When the other bands agree almost perfectly, any dust
divides into a large ratio: 0.035 of 255, on 0.01% of pixels, with exactly one
pixel over `BAND_MAX_DELTA_FLAG`, reported as a 69x outlier.

Fixed with `BAND_MIN_OUTLIER_MEAN = 0.5`, applied as an AND-gate — the same
shape as `BAND_MIN_HOT_PIXELS` on the max-delta path, and for the same reason.
A gate can only suppress, so it cannot hide a defect the ratio would otherwise
have had to invent; retuning `BAND_OUTLIER_RATIO` instead would have stopped the
band being checked at all. The value is not delicate: real SUSPECTs on this
suite run per-band means of **15–70**, the false positive is **0.035**, so 0.5
sits ~14x above the noise and ~30x below the smallest recorded real signal.
Gated bands are still **printed** (`[faint outlier: ...]`), not dropped.

Why it was run, kept for context:

`e1f47ce5b` altered `v_video.cpp` and `v_framebuffer.cpp`, which every backend
renders through, Metal included. Two behaviour changes reach Metal:
`SetViewportRects(nullptr)` now runs whenever the reconciliation resizes, and
`IVideo::SetResolution` no longer renders a full frame during startup.

Metal will not crash the way Vulkan did — `MetalRenderDevice::Update()` opens
with `if (!mInFrame) { BeginFrame(); ... }` and Vulkan has no such guard, which
is exactly why Vulkan was the one that segfaulted — but "will not crash" is not
"is correct". This is the first Metal exposure to that change.

Use the harness as it now stands: `1aa3e0c63` fixed `crossbackend.py` dropping
a config's own map (item 3), and that fix is platform-independent. A run from
before that commit tested the bloom trio on the wrong map.

### 2. Vulkan on Apple — DECIDED 2026-08-15: auto-detect off Apple, opt-in on it

**Decision (user's, 2026-08-15):** Vulkan-through-MoltenVK does not make sense
as a shipping backend on Apple hardware — it is a reference platform there, and
having it on by default defeats the point of the Metal backend. Auto-detect
stays for the native platforms that want it; Apple requires an explicit
`-DHAVE_VULKAN=ON`, and is OFF otherwise.

Implemented by guarding both the `find_package(Vulkan QUIET)` and the
auto-enable with `NOT APPLE`. The opt-in path still works and still resolves
through `libraries/ZVulkan`, which does its own `find_package` — so nothing
depended on the root detect having run.

**This also fixed a live breakage nobody had noticed.** The committed state does
not configure from scratch on this machine at all: a fresh default `cmake -B`
found Vulkan 1.3.268, set `HAVE_VULKAN`, entered `libraries/ZVulkan`, and died
on `Could NOT find Vulkan (missing: MoltenVK)`. Verified 2026-08-15 by stashing
the fix and configuring a clean directory — rc=1. It was invisible here because
the checked-in `build/` has `HAVE_VULKAN:BOOL=OFF` cached from before
`42052f77a`, so incremental builds never re-ran the detect. An Apple contributor
cloning the repo would have hit it immediately.

Note this is the **build-the-committed-state trap** in a new costume: not a
half-committed change, but a stale *cache* concealing that the committed state
was broken. `git stash push -u && cmake --build` does not catch it; only
configuring a fresh directory does.

The original question, for the record:

`42052f77a` added an unconditional `find_package(Vulkan QUIET)` that sets
`HAVE_VULKAN` whenever Vulkan is found. On the macOS CI runner it is found, so
**Apple builds now compile the Vulkan backend for the first time** — which
immediately broke the macOS build, because it pulled in an `#ifdef HAVE_VULKAN`
block in `cocoa/i_video.mm` that had never been compiled and carried an ARC bug
dating to `2fc29251f` (2026-01-11). Fixed in `f733aee14`; CI green.

The open question is intent, not correctness. This fork exists partly to
*replace* MoltenVK with a native Metal backend, and Vulkan-on-Apple means
Vulkan-through-MoltenVK. `DEFAULT_RENDER_BACKEND` is still 3 on Apple because
`HAVE_METAL` is tested first, so the default does not change — but the backend
is now built, linked and offered in the menu and launcher. Either narrow the
auto-detect to non-Apple platforms or decide it is wanted; do not leave it
accidental.

### 3. Metal on Apple Silicon — still never run

The standing item from `CLAUDE.md`. The renderer was developed entirely on a
macOS 12.7 Intel Mac. TBDR versus IMR differences — memoryless storage, store
actions, `didModifyRange:` — mean Intel-correct code can be wrong on M-series.
Needs hardware, so it may block on availability rather than effort.

### 4. SSAO attenuation residual

The last open row of the Metal-versus-OpenGL parity table: ~0.047 in occlusion
units. Detail under **Open items** below.

### Checked already, do not redo

`buttonMap.ResetButtonTriggers()` is **present** in `cocoa/i_input.mm`, as it is
in `native/` and `win32/`. Only `posix/sdl/` is missing it, matching the note in
`CLAUDE.md` that upstream SDL has the same omission. Verified 2026-08-14.

`in_rawkeyboard` is **not needed on macOS** and does not exist there. Cocoa's
gameplay path is already raw: `[NSEvent keyCode]` is a hardware positional
virtual keycode, layout- and modifier-independent, mapped straight through
`KEYCODE_TO_DIK[]` to a `DIK_*` scancode at `i_input.mm:550-551`, with no
keysym or text translation anywhere in it. Linux needs the CVAR because its
cooked path is the only one producing text and both must coexist; Cocoa splits
the same way structurally, so there is nothing to switch. `grep -rn
in_rawkeyboard src/` returns two hits, both in `native/i_input.cpp`.

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

*Also ruled out, 2026-08-15: the compute AO kernel is not involved, and on this
machine it never was.* `mt_postprocess.cpp:531` forces `useComputeAO = false` on
Intel unless `mt_compute_ao_intel` is set, and that defaults **false**. So Metal's
default AO on the reference machine has always been the same reference PP path
(`hw_postprocess.ssao`) that OpenGL runs — the residual was never a
compute-versus-raster comparison. Measured by sweeping `+mt_compute_ao 1` against
`+mt_compute_ao 0` on Metal: **bit-identical**, md5 and all, because both arms ran
the same code. Do not re-test this; the cvar cannot do anything here on its own.

*The compute kernel IS a separate divergence, and item 3 should expect it.*
Forcing it on with `+mt_compute_ao_intel 1` changes the frame by mean
**0.656/255, max 10, on 3.46% of pixels** against the reference PP path, Metal on
both arms so no cross-backend convention is involved. Path proved per arm by
`mt_metrics`: *"AO path in use: Metal compute (MtAOModule)"* with a `ComputeCPU AO`
line, versus *"reference PP (hw_postprocess.ssao) -- compute AO off"*. This
matters on Apple Silicon, where the Intel guard does not apply and the compute
path is therefore the **default** — AO there will differ from every figure in this
table before anyone changes a line. Do not read that as a regression.

*Narrowed target.* The residual is GL's `ssao.fp` against the **translated**
`ssao.fp` — same algorithm, same uniforms (already proven bit-identical),
different emitted arithmetic. The cached MSL
(`~/Library/Application Support/zdoom/cache/mt_shadersppssaofp_*.msl`) shows
SPIRV-Cross emitting `fast::` intrinsics where the GLSL has IEEE ones:
`fast::clamp` on the very `sampleUV` line above, `fast::max/min` through the
horizon math, and `fast::normalize` in `FetchNormal`.

Note this is **not** the "Metal fast-math default" already ruled out above — that
was the compiler flag; this is SPIRV-Cross selecting lower-precision *function
variants* in the emitted source, and it is a defensible mapping because GLSL
leaves `min`/`max`/`clamp` undefined on NaN.

Most of those emissions are probably inert: on finite inputs `fast::max`,
`fast::min` and `fast::clamp` return exactly the IEEE result, differing only on
NaN/Inf. **`fast::normalize` is the exception** — it is backed by a fast
reciprocal-sqrt and differs by a few ULP on ordinary finite input. It is
therefore the only emission in that shader that can move a pixel without a NaN
being involved, and it feeds `dot(viewNormal, viewDelta)`, where the largest
deltas come from the most distant samples — consistent with the recorded
radius-scaling.

**Tested 2026-08-15, and the answer is no — `fast::` accounts for none of it.**
The caveat above (a few ULP is very small for 12.52/255) was the right instinct.
`mt_msl_precise_math 1` strips the qualifier from every translated shader before
the Metal compiler sees it; `ssao.fp_frag` gave up **12** of them, including the
`fast::normalize` in `FetchNormal`, plus `lineardepth.fp` and `ssaocombine.fp`.
The frame is **byte-identical** to the stock arm, md5 and all, Metal on both
arms with 98 shaders patched in one and 0 in the other.

That kills a second hypothesis at the same time, for free: IEEE and `fast::`
min/max/clamp differ **only** on NaN, so an identical frame proves **no NaN is
reaching those guards** either. Do not re-test the NaN theory; this was its test.

So the residual is not the compute kernel, not `fast::` precision, and not a NaN.
What remains from the original lead is `LinearDepthTexture`'s **sampler state**
— the one item on that list never actually measured — and the possibility that
the ~0.047 figure is an artefact of the AO-isolation method that produced it
rather than of the renderer. Whoever picks this up should consider re-deriving
the measurement before hunting further; three specific suspects have now died
and that raises the prior on the measurement itself.

**The "~2x on Intel" premise for the compute AO guard does not reproduce.**
Measured 2026-08-15. `mt_postprocess.cpp:528` justifies disabling compute AO on
Intel with "compute AO measured at ~2x its cost there"; that predates the
AlchemyAO work, which changed the kernel and its pass structure, and had never
been re-checked. Metal on both arms, `gl_ssao 3`, path proved per launch by its
`mt_metrics` label, arms interleaved A,B,A,B so thermal drift is shared, and
**`cl_capfps 0` + `vid_vsync 0`** — with the harness default `+cl_capfps 1` both
arms just report the cap.

| | compute AO | reference PP | delta |
|---|---|---|---|
| 800x600, mean of `Frame avg` | 4.971ms | 5.189ms | −0.218ms (0.96x) |
| 1600x776, mean of `Frame avg` | 5.139ms | 5.164ms | −0.025ms (1.00x) |
| 1600x776, best `Frame min` | 1.512ms | 1.244ms | +0.268ms (1.22x) |

Per-arm repeat spread was 0.083–0.264ms, and CLAUDE.md puts the machine's noise
floor at ~0.4ms. Every delta above is inside that, and the sign is not even
consistent between `avg` and `min`. **The honest reading is that the two paths
cost the same at frame level, not that compute is faster.** What is definite is
the negative: a 2x on a ~5ms frame would be ~5ms, more than ten times the noise
floor, and nothing of the sort is present at either resolution.

Note the resolution arm was verified rather than assumed — the captures are
1600x776 (the display clips the requested 1200), ~2.6x the pixels of the 800x600
arm, and the frame time did not move. That says this scene is not fill-bound at
either size, which is also the limit of the measurement: at ~200fps the frame is
dominated by work that is not AO, so a real per-pass difference could hide inside
it. **The definitive test is per-encoder timing from an Xcode GPU capture**, which
CLAUDE.md already names as the only trustworthy per-pass figure on this hardware.
The frame-level result is enough to retire the 2x claim, not enough to prove the
paths are equal in isolation.

The guard is deliberately left in place — flipping a default is a judgement call,
not a measurement — but its stated reason is now known to be stale, and the
comment at `mt_postprocess.cpp:528` should not be quoted as if it still held.

#### GPU captures: the pass structure, read without Xcode

Taken 2026-08-15 following `docs/gpu-capture-protocol.md` §0–§2, adapted from the
AO-composite question to a timing one. `mt_capture` was armed with `+execafter 60`
rather than typed, so the arms cannot differ by operator timing and the console
(which stalls the countdown) is never opened. PSO archive cleared before each.

**A `.gputrace` is a bundle, and the encoder labels are greppable inside it.** No
Xcode is needed to answer structural questions:

```bash
grep -ao "PP [a-z]*: [a-z0-9_/.]*" frame-<stamp>.gputrace/capture | sort | uniq -c
```

| arm | labeled PP encoders in the trace |
|---|---|
| reference PP | `lineardepth.fp`, `ssao.fp`, `depthblur.fp` **x2**, `ssaocombine.fp` |
| compute AO | **none** |

So the reference path is a **five-encoder render chain** and the compute path
replaces all of it with its own unlabeled compute encoders — as
`gpu-capture-protocol.md` §3 warns, the native compute paths carry no `PP` label.

**The absence was verified, not assumed.** Zero labels could equally mean the
format did not serialise them in that arm, which would make the comparison
worthless. Positive control: a third capture, compute AO **plus** `gl_fxaa 1`,
returns `2 PP fxaa: shaders/pp/fxaa.fp`. The compute-arm trace format does carry
labels, so the missing AO labels are real.

**RESOLVED 2026-08-15 by differencing, and the AO paths cost the same.** With
per-encoder timing unavailable (below), the pass cost was recovered arithmetically:
`Frame avg(AO on) - Frame avg(gl_ssao 0)`, three reps per arm, interleaved
N,C,R,N,C,R so drift is shared, at 1600x776.

| arm | mean `Frame avg` | AO cost |
|---|---|---|
| no AO (`gl_ssao 0`) | 4.681ms | — |
| compute AO | 5.385ms | **+0.704ms** |
| reference PP | 5.399ms | **+0.717ms** |

Difference between the paths: **−0.013ms (0.98x)**, worst within-arm spread 0.422ms.

This is the measurement the frame-level comparison could not give, because the
subtraction removes the ~4.7ms of non-AO work the AO pass was hiding inside. Both
AO costs (~0.71ms) clear the floor, so the cost of the pass is resolved; the
difference between paths is 30x below it. **A 2x would mean compute costing
~1.43ms, i.e. +0.7ms — well above the floor and impossible to miss.** The claim at
`mt_postprocess.cpp:528` is therefore not merely unreproduced, it is excluded: the
method can detect a difference of ~0.42ms and sees 0.013ms.

Worth stating the bound rather than "they are equal": this excludes ratios above
roughly **1.6x**, not every conceivable difference.

*Incidental confirmation of an archived-CVAR leak.* The `N_noao` arm does not set
`mt_compute_ao*`, and its `mt_metrics` path label came back as compute on rep 1 and
reference on reps 2-3 — inherited from whichever launch wrote `matrix.ini` last.
Harmless here (with `gl_ssao 0` no AO runs at all, and the label reports the
configured path, not an executed one) but it is the archived-cvar contamination trap
happening live, and it is why every arm above passes its cvars explicitly.

**Per-encoder GPU timing is NOT available on this machine, and CLAUDE.md was wrong
about it — now fixed.** The operator checked Xcode's counters on the captured frame
on 2026-08-15: **no data**. So the conflict below is resolved against CLAUDE.md,
whose claim was never verified. Kept here because the reasoning is worth preserving: `mt_caps` on the HD 6000 reports *"Stage counter sampling:
no  <- per-pass GPU timing gate"* while CLAUDE.md's "Measuring a rendering change"
calls an Xcode capture's per-encoder timing "currently the only trustworthy
per-pass figure on this hardware". Those cannot both be right, and the hardware
gate is the more credible of the two — and it was. Xcode 14.2 on this GPU shows
**no counter data** for a captured frame, so there is no per-encoder duration to
read at any price. CLAUDE.md has been corrected accordingly; use the differencing
method above for pass cost, and captures for *what ran*, never for what it cost.

Traces are no longer needed and can be deleted:
`~/Documents/GZDoom/gputrace/frame-20260815-234713` (compute), `-234725`
(reference), `-235136` (compute + fxaa control), ~317MB each. The greppable
label result above is recorded, so nothing is lost with them.

`mt_msl_precise_math` is kept as a permanent diagnostic (default off, **not**
archived, `mt_shader.cpp`). It applies in `CompileMSLToLibrary`, deliberately
*after* the `.msl` disk cache, so a cached translation cannot serve the other
arm's source. Its stderr proof line is not optional and must not be quietened:
two of the three attempts at this experiment produced a *correct-looking*
PIXEL-IDENTICAL verdict while the patch was not running at all — first because
the line was gated behind `mt_debug`, then because `Printf(PRINT_LOG, ...)` at
shader-compile time is emitted before the console exists and reaches no log.

**OpenGL captures black on Ashes2063 + capspot.zds.** Scene-specific: the same
build captures AshesHardReset normally. Blocks nothing today because
`crossbackend.py` has its own scene override, and `run.py` pins Metal. Three
readback-level fixes were tried in 2026-08-08 and all failed; the capture
probably has to move before the buffer swap.

**Why the Metal backend exists, with the 2026-08-11 evidence.** Worth recording
because it gets asked (Graf asked it directly) and because the landscape moved.

*ZVulkan has diverged from the vendored subtree, and the gap is macOS-shaped.*
Upstream `dpjudas/ZVulkan` (head 2026-01-23) **removed surface creation entirely**
(commit "Remove surface creation from ZVulkan", 2025-02-09). Measured against the
copy in `libraries/ZVulkan`:

| | vendored subtree | upstream today |
|---|---|---|
| `VK_MVK_MACOS_SURFACE` | required | **gone** |
| `VK_KHR_WIN32_SURFACE` / `VK_KHR_XLIB_SURFACE` | required | **gone** |
| `VK_KHR_PORTABILITY_ENUMERATION` | required | **gone** |

Adopting current ZVulkan therefore means GZDoom must itself request portability
enumeration and its instance flag (without which MoltenVK will not even
enumerate a device), create a `CAMetalLayer`, and drive `VK_EXT_metal_surface`
directly, since `VK_MVK_macos_surface` is deprecated. That is the concrete
"substantial changes for macOS" cost. Upstream also added MoltenVK layer
detection setting `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` via
`VK_EXT_layer_settings` (2024-07-03) — **not present in the vendored copy**;
argument buffers are the bindless mechanism and Tier 2 is what bindless needs.

Do **not** overstate this: upstream ZVulkan still degrades gracefully on
*features*. Ray query, descriptor indexing, buffer device address and graphics
pipeline libraries are all still `OptionalExtension`, and it still tries
1.2 → 1.1 → 1.0. The cost is integration and capability tier, not a feature wall.

*KosmicKrisp does not rescue old Macs.* LunarG's Vulkan-on-Metal Mesa driver
(merged in Mesa 26.0, Vulkan 1.3 CTS conformant) is per Mesa's own docs "a Vulkan
conformant implementation for macOS on **Apple Silicon** hardware", requiring
**macOS 26+** and **Metal 4**. The reference machine (Intel HD 6000, macOS
12.7.6, Metal 2.0) fails all three, and Intel Macs are not TBDR anyway. Two
consequences, and the second is the honest one:

- it *strengthens* the case for a native Metal backend on old Intel Macs — the
  modern Vulkan-on-Metal story has moved to hardware that excludes them
- it *weakens* the blanket claim that Vulkan on macOS is bad. On Apple Silicon
  with macOS 26 it is about to be good. Keep the specific argument (no
  translation layer, old hardware, our bugs to fix, Xcode captures work
  properly); retire the general one.

**Shader strategy — what is actually native today, and what should be.**
Measured 2026-08-11, because the naming invites the wrong assumption:

- `shaders/native/mt_ao.metal` and `mt_bloom.metal` are **hand-written MSL**,
  built into `native_shaders.metallib` and loaded first at runtime. Two files —
  the compute AO and bloom kernels — plus inline C++ raw-string fallbacks.
- **Everything else is translated at runtime**: GLSL → SPIR-V → MSL via glslang
  and SPIRV-Cross, cached as `.msl`. That is roughly fifty programs (15
  `defaultshaders` with non-alphatest variants for the first seven, 5
  `effectshaders`, two pass types). `mt_shader.cpp`'s own comment says
  "Metal shader compilation is slow (GLSL->SPIRV->MSL->Lib)".
- So it is **not** true that only mod shaders compile dynamically. The engine's
  own set does too.

Recommended order, if the native path is expanded:

1. **Translate the engine's ~50 known programs at build time** into the metallib.
   The permutation set is known; only mod shaders genuinely need the runtime
   path (custom GLSL from PK3s cannot be precompiled, so the translation
   pipeline never goes away). This costs no hand-written MSL, removes the
   startup cost, and eliminates the whole compile-while-drawing hazard class.
2. **Hand-write MSL only for the postprocess and compute passes.** Imageblocks,
   tile shaders and programmable blending have *no GLSL expression*, so they can
   never come out of the translation pipeline — native MSL is a prerequisite for
   the TBDR work, not a preference. Small closed set, already started.
3. **Do not hand-write the material shaders.** Not because fifty is many, but
   because they are *composed* from interchangeable fragments (a texel function
   glued to a material model plus defines). Hand-writing means reimplementing
   `hw_shaderpatcher` for MSL, for shaders that need no Metal-only feature.

Before expanding native MSL at all, **kill the inline C++ fallback-string
duplication** by generating it from the `.metal` at build time. Two files already
required a bespoke parity script after `mt_ao.cpp`/`mt_ao.metal` drifted twice;
ten would be a standing tax. One source of truth makes
`tools/check_shader_parity.py` unnecessary rather than busier.

**Metal is NOT vulnerable to the GL stale-shader-binding bug** (read 2026-08-11,
code reading rather than a runtime test). `MtRenderState::ApplyPipeline` has the
same *shape* of cache — `if (pipelineKey != mPipelineKey || !mPipelineBound)` —
but `mPipelineBound = false` is cleared at the render-command-encoder creation
site itself (`mt_renderstate.cpp`, where `renderCommandEncoder()` is called), and
again on frame begin and encoder-state reset. That is precisely the invalidation
the GL path was missing. A fresh `MTLRenderCommandEncoder` carries no pipeline
state by definition, so the invariant holds structurally.

**Apple Silicon is untested.** Nothing here has ever run on an M-series part.
The Intel gates are runtime checks, so Apple Silicon takes the **compute** AO
and bloom paths by default — paths that have never executed on that hardware,
along with the Tier 2 argument-buffer paths. Expect the first run to be a
bug-finding exercise, not a benchmark. `run.py --update-baseline` is the first
useful command there; it refuses to record if any pass is broken.

**RESOLVED 2026-08-10 — the merged tree builds and runs on Linux.** Both
`docs/handoff-linux.md` tasks were carried out on the Linux box (Arch/CachyOS,
KDE Plasma Wayland, AMD RX 550 polaris12, Mesa 26.1.6, GL 4.6, Vulkan 1.4).

Task 1: the default configuration compiles clean, and the binary runs under the
native Wayland backend with the ZWidget launcher painting all four IWADs and the
new "Add Files..." button. (The original wording of this task concerned whether
the merge's `if (HAVE_GLES2)` resolution was correct. That question is moot — the
GLES backend was removed on 2026-08-12, see below.)

**The GLES backend has been REMOVED, 2026-08-12.** Gone from the tree: 23 source
files (6,027 lines), 30 GLES shaders, the `HAVE_GLES2` build option, the branches
in four platform video layers, the menu entry and the launcher radio button.
`-DHAVE_GLES2=ON` is now an unused-variable warning, not a build option.

Why, recorded so the decision is not relitigated from scratch:

- **It was measurably broken.** In a properly configured `HAVE_GLES2=ON` build it
  rendered entirely black — menu included — on MAP12 (twice) and MAP01, where GL
  and Vulkan both render MAP12 at ~51.6. The cause was never investigated,
  because there was nothing to investigate it *for*.
- **It was reachable in builds that never asked for it.** The 2026-08-09 merge
  resolution listed the sources unconditionally, and the `V_GetBackend() == 2`
  branch in `nativevideo.cpp` carried no `#ifdef` — unlike the Vulkan branch
  beside it. So `+vid_preferbackend 2` constructed GLES on a default Linux build
  while the startup line naming the backend was compiled out: the engine ran a
  backend it did not admit to. Gated 2026-08-11, removed the day after.
- **Gating made the rot faster, not slower.** Once it no longer compiled by
  default, refactors would break it silently. Gated-off broken code is an
  unstable state — commit to it or delete it.
- **No hardware, no users, no test.** Nothing here can run an ES driver, so it
  could never be verified. Shipping an unverifiable renderer is worse than not
  shipping one.
- **The merge-cost argument had evaporated.** Keeping inherited code cheap to
  re-merge mattered while upstream was alive. GZDoom is frozen and UZDoom does
  not take contributions from this fork, so divergence is nearly free now.

**Backend enum value `2` is deliberately vacant.** Do *not* renumber Metal from 3
down to 2 to close the gap — every existing `gzdoom.ini` carrying
`vid_preferbackend 3` would silently come to mean something else.
`V_GetBackend()` maps 2 to OpenGL and says so in a comment.

The embedded case this served — Raspberry Pi 3 and older, pre-2018 Android SoCs —
is real but unreachable from here; Pi 4/5 have Vulkan via V3DV. If it is ever
wanted back, `git revert` the removal and fix it *with the hardware in hand*,
which is the only way it could be done properly. A branch would have been worse
than deletion: it looks maintained, diverges silently, and charges a rebase for
nothing.

**What replaced it in the menu.** `OptionValue PreferBackend` in `menudef.txt`
now wraps each entry in `IfOption(Vulkan|Metal)`, OpenGL unconditional. That
needed `IfOption` support inside `OptionValue` blocks, which did not exist —
`ParseOptionValue` was a flat `value, "text"` loop and is now
`ParseOptionValueBody`, recursing through the same `CheckSkipGameBlock` /
`CheckSkipOptionBlock` helpers option *menu* bodies already used. So the syntax
is identical in both places and `ifgame`/`ifnotgame` work there too. Verified by
capture on the Linux Vulkan+GL build: `vid_preferbackend 0` shows "OpenGL", `1`
shows "Vulkan", `3` shows **"Unknown"** — Metal is genuinely absent from the
list, where it previously read "Metal".

**`V_GetBackend()` also resolves against what is compiled in.** It used to send
an unavailable Metal (3) to GLES (2), and had no fallback at all for Vulkan.
Everything unavailable now falls back to OpenGL. It deliberately no longer writes
the corrected value back to the CVAR: that existed so the menu would show
something valid, and the menu now offers only what the build has. The visible
consequence is "Unknown" rather than a silent rewrite when an ini arrives from
another platform — easy to reverse if the rewrite is preferred.


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

**OpenGL rendered some maps as a black frame — SOLVED 2026-08-11.** Root cause,
fix and verification below. The long investigation that preceded it is kept
after the fix, because its eliminations are what made the answer findable and
because two of its readings were wrong in instructive ways.

**Root cause: a stale shader-binding cache.** `FShaderManager::SetActiveShader`
skips `glUseProgram` when the shader it is asked for is already the one it
believes is active:

```cpp
if (mActiveShader != sh) { glUseProgram(...); mActiveShader = sh; }
```

`FShader::Load()` ends with `glUseProgram(0)` (`gl_shader.cpp`, end of `Load`),
which invalidates that belief without telling the manager. Shader compilation is
**incremental** — `CompileNextShader()` is called repeatedly and the start screen
draws between calls — so the sequence is:

1. an early startup draw binds a shader, say `Default`; `mActiveShader = Default`
2. the next `CompileNextShader()` loads another shader and leaves program **0** bound
3. every later draw asks for `Default` again, `SetActiveShader` short-circuits,
   and **no program is ever bound again**

From there the GL current program stays 0 for the life of the process. Every
`glUniform` fails and every draw produces nothing — which is why the frame was
black *including the status bar*, the detail that ruled out a scene-render fault.

**The fix** (`src/common/rendering/gl/gl_shader.cpp`, 15 lines): clear
`mActiveShader` at the end of `FShaderManager::CompileNextShader()`, after the
compile step rather than before it, so the last compile is covered too.

**How it was found**, in case a similar one turns up: env-gated `glReadPixels`
probes at four stage boundaries (scene FB, after each postprocess pass, after
`Draw2D`, after present) showed the scene framebuffer already empty at the
*first* probe, so nothing downstream was at fault. `MESA_DEBUG=1` then named the
failure outright — 17,423 `GL_INVALID_OPERATION in glUniform(program not
linked)` per run, Mesa's message for "no program bound". Adding
`GL_CURRENT_PROGRAM` to the probe confirmed `prog=0` on every black frame.
**`MESA_DEBUG=1` is worth reaching for early**; `gl_debug_level` produced nothing
here because the context is not a debug context, so the cvar is a dead end on
this machine.

**Why the map bias, the "race", and the console workaround all follow.** The
cache repairs itself the moment any *different* shader is selected, because then
`mActiveShader != sh` and a real `glUseProgram` happens. So:

- maps that always rendered (05, 08, 11, 12, 20, 21) contain something that
  selects a second shader early — `Stencil` and `No Texture` were both observed
- maps that were almost always black (01–04, 06, 07, 09, 10) draw only with the
  already-cached shader for the first ~120 frames
- `toggleconsole` fixed it because console 2D selects `No Texture`. Measured:
  the repair lands on the frame after the toggle, and the scene FB goes from
  0.000 to 21.423 in one frame. A benign `echo` does nothing because it selects
  no new shader — which is exactly why "any console command" never worked
- the ~8% of runs that rendered anyway are runs where startup happened not to
  leave a stale bind. It was never a timing race, though it looked exactly like one

**Verification.** Harness: one launch per data point, fresh config each time,
`+map <n> +shotafter 120 quit`, mean of the capture.

| | before | after |
|---|---|---|
| DOOM2 MAP01, native GL | 0.000 on 7 of 8 runs | 26.751–26.768 on 4 of 4 |
| MAP02, 03, 04, 06, 07, 09, 10 | 0.000 | 38.105, 29.655, 26.019, 24.215, 46.817, 28.743, 43.606 |
| MAP05, 08, 11, 12, 20, 21 (never broken) | rendered | rendered, MAP12 51.640 vs 51.640 recorded |
| MAP01, SDL build (`GZDOOM_NATIVE_LINUX=OFF`) | 0.000 | 24.415, 24.180 |

The one control run that rendered without the fix is the known ~8% flake, and it
carried `prog=0`→non-zero accordingly; all seven black control runs carried
`prog=0` on the final frame. `crossbackend.py --backends gl,vulkan` is **11/11
OK** after the fix.

**Open, and serious for the tooling: the full suite flakes on a config that
VARIES between runs.** This was written up twice before getting it right, and
both wrong versions are instructive. First as "a `tonemap_identity` flake" on
three samples. Then, on more samples, as "`tonemap_identity` is
context-dependent — it only fails inside the full suite". Both wrong in the same
direction: the config name was never the variable.

Measured across five full-suite runs and repeated isolated runs:

| config | in the full suite | run alone (`--only <config>`) |
|---|---|---|
| `tonemap_identity` | SUSPECT on 2 of 5 | OK on 5 of 5 |
| `ssao` | SUSPECT on 1 of 5 | OK on 2 of 2 |

So roughly half of full-suite runs report **some** config SUSPECT, and *which*
config it lands on changes. Isolated runs have never failed — and they reproduce
the passing median band mean exactly (`ssao` 0.207, `tonemap_identity` 0.209),
so the isolated result is not merely "different", it is the clean one.

The SUSPECT runs are large and inconsistent in shape: per-band means 15–70
against a whole-image mean of about 25, worst channel deltas 118–159, and tone
ratios of x1.05, x0.99 and x0.75 — varying in both direction and magnitude.

**Not caused by anything in the 2026-08-11/12 work.** Isolated runs pass on the
pre-shader-fix binary, the post-fix binary, after the harness merge, and after
the GLES removal; the first SUSPECT predates all of them.

**Nothing has been measured about the cause.** The shared `WORKDIR/matrix.ini`
that the engine rewrites on exit is the obvious first suspect, and
`gl_exposure_speed 1` with an adaptive tonemap the second. Neither has been
tested. Do not repeat either as a finding.

Practical consequence, and it is worse than a single flaky config: **a lone
SUSPECT anywhere in a full-suite run is not a regression signal.** Re-run the
named config in isolation before believing it. Equally, a clean full suite is
weaker evidence than it looks, because the failure moves. Fixing this should
probably come before the suite is trusted as a gate for anything load-bearing.

**Update 2026-08-12: it reproduces in isolation, and the variable is the
launch.** "Isolated runs have never failed" was true only of the default
(Ashes) scene. On the stock `doom2` scene, six isolated runs of `baseline` --
the identical config, no other config involved -- produced four distinct
decoded-pixel states:

    f6fced7b   cc8cd0cc   cc8cd0cc   6f66e8c2   43ecc003   cc8cd0cc

So it is not a suite-interaction effect and never was. `tonemap_identity`
FAILing its must_match on this scene is this and nothing else: an 18-pixel
element at (454,249), 9 brighter and 9 darker, max |delta| 162, on an otherwise
pixel-identical frame. The must_match relation cannot be evaluated on `doom2`
until this is fixed.

This also **contradicts `run.py`'s own docstring**, which states that with
`cl_capfps 1` two identical launches are byte-identical. That was established on
the Ashes scene and was never re-tested when the stock scenes were added.

The practical gain is cost: reproducing this no longer needs the mod, the
savegame, or a full suite run -- it is `--only baseline --scene doom2`, about
15s a sample, on any machine with DOOM2.wad. The two untested suspects above
(the shared `matrix.ini` rewrite, `gl_exposure_speed 1`) are now cheap to test,
and testing them is the obvious next step. Still untested; still not findings.

~~**Do not record a `doom2` baseline until this is understood**~~ -- SUPERSEDED
later the same day. It was understood, the scene moved to a measured map, and a
`doom2` baseline was recorded deliberately. See the resolution below and the
"choosing a map" entry further down.

**Resolved, same day: it is the status bar face, and both standing suspects are
wrong.** Measured by launching the binary directly, 8 samples per arm, with the
window geometry held fixed (see the geometry note below -- it contaminated the
first pass and produced a wrong intermediate reading that exposure mattered):

| arm | distinct states / 8 |
|---|---|
| `gl_exposure_speed 1`, shared ini | 6 |
| `gl_exposure_speed 0`, shared ini | 5 |
| fresh ini per launch | 5 of 6 |
| **`screenblocks 12`** (no status bar) | **2** -- 7 of 8 identical |

So neither the shared `matrix.ini` nor `gl_exposure_speed` is the variable.
Retire both; they were plausible and they are wrong.

The states are a small recurring family, not drift, which is the tell. Diffing
two of them full-frame gives a fixed bounding box of **92 px at x[391..408]
y[549..567]** -- 18x19 at bottom-centre, the Doomguy face. `ST_updateFaceWidget`
turns it with `M_Random` on an idle timer, so the captured tic samples an
arbitrary RNG phase. `localize.py` could not see it: its analysis region is the
central `(40,51)-(760,457)` and the face is outside it, so two captures with
different pixel hashes localized as **identical**. Anything comparing whole-frame
hashes against `localize.py` output must account for that gap.

A second, independent element remains at **x[455..458] y[249..267]** (18 px,
max delta 213, a thin in-scene strip), unaffected by `screenblocks` and seen on
roughly 1 sample in 8. Not identified. It is what `tonemap_identity` tripped on.

Practical: pinning `screenblocks 12` for captures removes the dominant source
and costs nothing the suite cares about -- it tests postprocess passes, and the
status bar is not one. It does change every capture, so it invalidates the
recorded default-scene baseline; that re-record is a deliberate decision and has
not been made here.

**Update, same day:** `screenblocks 12` is now pinned on the stock scenes, the
`doom2` scene moved from MAP12 to MAP06, and `tonemap_identity` reproduces the
baseline *exactly* there — so the residual element does not block `must_match`
on `doom2` any more. It was only ever a MAP12 problem. MAP12 still hosts the
bloom pair, which is why those three configs are declared `relations_only` and
kept out of the golden image.

**Geometry, which contaminates any run of this experiment done by hand.** The
first launch against a *fresh* config file captures at 1152x720, not the pinned
800x572: `win_w`/`win_h` are applied after the window is sized. `run.py` already
handles this with its discarded warmup launch, and its comment says so. Driving
the binary directly does not, so a hand-rolled arm silently mixes two
resolutions -- and since `compare()` returns None on a geometry mismatch while
raw pixel hashes simply differ, the mixture reads as "more nondeterminism".
Warm the config file first, then sample.

**Also noted, not fixed:** `FShaderProgram::Link()` (`gl_shaderprogram.cpp`, the
`glslversion < 4.20` branch) leaves its own program bound without restoring the
previous one — the same class of bug on the old-GL path. It leaves a *linked*
program bound rather than 0, and this machine is GL 4.6, so it was never
exercised. Untested, not a fix, just recorded.

**It was upstream's bug, and still is.** The defect is in code this fork
inherited unchanged; it reproduced on the upstream `master` mirror and on macOS.
The fix above has not been offered upstream.

---

### The investigation that preceded the fix

Kept because the eliminations below are what left only one place to look, and
because three conclusions recorded here had to be retracted.

**It looked like a race, and was not.** Across ~38 MAP01 runs, 35 were black and
3 rendered; MAP12 rendered on every attempt. That reads as a biased race, and it
was recorded as one. The real variable was whether a second shader ever got
selected — deterministic per map, but with enough startup variation to produce a
few successes. **A biased success rate is not evidence of a timing race.**

Do **not** trust a single run of this. Two conclusions were recorded and later
withdrawn because they rested on n=1: "bare IWAD versus loaded mod" (it was the
map — Ashes replaces MAP01 with a different one), and a config bisect that
fingered `hud_vertical`, a key that is **not a cvar in the source at all**, only
a stale entry in a hand-grown ini.

Also: **the engine rewrites the config file on exit**, so a `-config` copy stops
being the file you copied after its first launch. Copy it fresh per launch.

Map bias, measured 2026-08-10, same binary, only `+map` changing:

| DOOM2 map, GL | native backend | SDL backend |
|---|---|---|
| MAP01 | 0.000 | 0.000 |
| MAP02 | 0.000 | 0.000 |
| MAP07 | 0.000 | 0.000 |
| MAP12 | 51.640 | 51.816 |

**Ruled out by measurement**, all of it correctly:

- the platform layer — the SDL build (`-DGZDOOM_NATIVE_LINUX=OFF`, `libSDL2`
  linked, `Native Linux backend initialized` absent) reproduced identically
- the fork's shared-code changes — upstream `master` (`092b9c051`), with none of
  this fork's code, reproduced identically. Backend confirmed by inference
  because upstream prints nothing to stdout: Vulkan rendered MAP01 at 26.812, so
  a black MAP01 was not a silent fallback
- the GPU driver — llvmpipe reproduced exactly (MAP01 0.000, MAP12 53.915)
- the `!AppActive` early return in `D_Display` — instrumented, **never taken**,
  and `vid_activeinbackground 1` accordingly changed nothing
- frame dropping — `D_Display` ran to completion and `End2DAndUpdate()` was
  reached every frame in both the black and the working case
- the scene branch — entered in both, `viewactive=1`, viewports identical
- the 2D command list — `con_notifylines 0` / `con_notifytime 0`, separately and
  together, black either way. (The list was never empty: 26 commands were queued
  on black frames and drew nothing, which in hindsight was the whole answer.)
- `vid_setmode` (rebuilds the framebuffer), the screen wipe (`wipetype 0`),
  `screenblocks` 10 and 11, `cl_capfps` 0 and 1, window size, the maintainer's
  own config

**That it happens on macOS too** (maintainer, 2026-08-10) agreed with the SDL and
upstream-master controls.

**Bisecting upstream was attempted and abandoned**, blocked on build
dependencies rather than on the idea: every historical commit sampled (2025-07,
2024-04, 2021-05, 2017-04) failed to configure with `Could NOT find ZMusic`,
because old upstream wants a *system* ZMusic where this fork bundles one. It was
also the wrong tool — the defect is old and the bug is not a regression.

**No upstream report was found** for this shape (searched 2026-08-10), though
"open the console" circulates as a folk workaround for assorted GZDoom black
screens, which may well be this bug seen from outside.

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

**ZWidget Cocoa fixes are not upstream yet — but the branch is already PR-ready.**
`libraries/ZWidget` is a subtree. Verified 2026-08-11 against the live remotes,
so this does not need re-deriving:

- `dpjudas/ZWidget` master head is **`4cf65e59c`** (2026-05-11), and it
  **contains `155142207 "Add HaikuOS support"`**. The Haiku work from this fork
  is merged upstream — dpjudas takes contributions from here, which is the
  relevant fact when deciding whether a PR is worth preparing.
- `zwidget/cocoa-modal-fixes` is **exactly two commits sitting directly on that
  head**. No rebase, no conflict, nothing to prepare — it can be opened as-is.
- `zwidget/wayland-c-bindings` is **byte-identical to the subtree in this repo**
  (tree `a679350a7` on both sides). Nothing local is unpublished.

The Wayland work still does not cherry-pick — it is entangled with the waylandpp
replacement, so it is a large PR against a codebase that has not seen it, and a
conversation rather than a drive-by patch. Given Haiku landed, a new-platform-
shaped contribution is clearly something he is open to.

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

### Build the committed state, not your working tree

**Three separate instances in one session, 2026-08-14.** Each was a change made
in two halves where only one half was committed. The working tree on disk was
correct, so every local build passed; the commit was broken, so CI and any
fresh checkout failed. `AGENTS.md` recorded two of the three as *already fixed*,
because from the machine that wrote them they were.

| | committed half | uncommitted half | symptom |
|---|---|---|---|
| `in_keytrace` | `5210976d2` deleted the `CVAR` | the `EXTERN_CVAR` + use | every Linux CI job failed to link |
| X11 raw keyboard | the XInput2 feature | `XIAllMasterDevices` device fix | feature published upstream inert — silently drops every key |
| matrix item 3 | `run.py`'s `scene=` parameters | `crossbackend.py`'s call sites | bloom trio silently tested on the wrong map |

The X11 one is the worst shape: it was **published to the ZWidget fork**
without its second half, so the defect left the building.

The check costs about a minute and catches all three:

```bash
git stash push -u && cmake --build build --parallel $(nproc) && git stash pop
```

Do it before any push, and before any `git subtree`-style publish — a subtree
publish compares against the *committed* subtree, so an uncommitted fix is
invisible to it by construction. `gh run list --branch <branch>` is the cheap
confirmation afterwards; on 2026-08-14 CI had already caught the link error
before it was diagnosed locally.

Corollary for `AGENTS.md` itself: "fixed on <date>" written from the machine
that made the fix is not evidence the fix is in the branch. Cite the commit.

**A stale CMake cache hides the same class of breakage, and the stash-build
check above does NOT catch it.** Measured 2026-08-15: the committed tree could
not be configured from scratch on macOS — `find_package(Vulkan QUIET)` found the
SDK, set `HAVE_VULKAN`, and `libraries/ZVulkan` then hard-failed on the missing
MoltenVK component. Every local build passed, because `build/CMakeCache.txt`
carried `HAVE_VULKAN:BOOL=OFF` from before the detect was added and configure
never re-ran. A rebuild reuses the cache by design, so the only check that sees
this is configuring a throwaway directory:

```bash
cmake -B /tmp/cfgcheck -DCMAKE_BUILD_TYPE=RelWithDebInfo .   # rc must be 0
```

Do it after any change to `CMakeLists.txt`, and before pushing one.

### Choosing a map for the matrix suite — measure, never assume

**A map the suite runs on can silently disarm a relation, and it reports `ok`.**
`gl_ssao 3` changes **0.000%** of MAP12, which the suite ran on for months. The
`ssao` must_differ_from relation was being satisfied entirely by RNG noise from
the status bar face, so it would have reported `ok` with the AO pass completely
dead. That is precisely the failure the suite exists to prevent. The same hole
existed on the `doom1` scene: `gl_bloom` changes 0.01% of E1M1 against a noise
floor of 0.00%.

A map must satisfy **two** independent requirements, and passing one says
nothing about the other:

1. every pass visibly acts on it, by the suite's own `>2 levels` metric;
2. repeated identical launches are pixel-identical, over ~8 samples.

Measured 2026-08-12, %px differing from baseline (`n` = two identical launches):

| DOOM2 | n | ssao | bloom | | DOOM | n | ssao | bloom |
|---|---|---|---|---|---|---|---|---|
| MAP01 | 0.00 | 4.89 | dead | | E1M1 | 0.00 | 2.95 | **0.01 dead** |
| MAP02 | 0.00 | 8.23 | 0.15 | | E1M2 | 0.12 | 13.29 | 0.11 |
| MAP03 | 0.00 | 22.54 | **dead** | | **E1M3** | **0.00** | **44.82** | **15.55** |
| **MAP06** | **0.00** | **6.84** | dead | | E1M5 | 0.06 | 0.19 | 0.06 |
| MAP07 | 0.00 | **dead** | dead | | E1M7 | 0.00 | 42.91 | **dead** |
| MAP11 | 0.11 | 4.22 | 0.15 | | E2M2 | 0.00 | 19.35 | **dead** |
| MAP12 | 0.00 | **dead** | 38.61 | | E3M2 | 0.52 | 2.30 | 15.24 |
| MAP15 | 0.13 | 16.86 | 14.22 | | E4M2 | **100** | — | — |

Of eight Doom 2 candidates, **none** satisfied both — the maps where both passes
act (MAP11, MAP15) are the ones that fail determinism. Hence per-config maps:
doom2 is MAP06 with the bloom pair on MAP12. E1M3 is the only map found in
either IWAD that satisfies both alone, so `doom1` needs no override. E4M2 is
wholly nondeterministic — 100% of pixels differ between identical launches.

An 8-sample determinism check is load-bearing: MAP02 and MAP01 both showed
`0.00` noise on a single pair and then produced 5 and 4 distinct states
respectively over eight launches.

**This table disagrees with `configs.json`, and both claim the same date.**
Found 2026-08-12 while acting on it. `_map_choice_note` in
`tools/matrix/configs.json` carries the same experiment with different numbers:

| | table above | `_map_choice_note` |
|---|---|---|
| MAP06 ssao / bloom | 6.84 / **dead** | 57.4 / **6.9** |
| MAP01 ssao / bloom | 4.89 / dead | 19.2 / 4.4 |
| MAP03 bloom | dead | 0.0 |
| MAP07 ssao | dead | 0.0 |
| MAP12 ssao / bloom | dead / 38.61 | 0.0 / 50.9 |
| MAP15 ssao / bloom | 16.86 / 14.22 | 45.1 / 21.5 |

The **noise** column agrees throughout (MAP11 0.11/0.115, MAP15 0.13/0.132,
MAP12 0.00/0.004), so it is the pass-action scan that differs, not the
determinism check — which rules out "two different runs of the same procedure"
as an innocent explanation and points at the two being different measurements
with the same name. The *decisions* survive either way: MAP06 satisfies both
requirements on both readings, and MAP12 is the bloom map on both. What does not
survive is the shorthand "bloom is dead on MAP06", which is only in the table.

`configs.json` is the copy adjacent to the code and was written by the session
that made the choice, so prefer it until someone re-measures. Also wrong in
`configs.json` itself, in the other direction: three config `note` fields say
MAP12 is "the only stock map where [bloom] acts at all", which its own table
contradicts at MAP15 21.5 and MAP07 11.7.

**Do not quote either set as measured fact in a commit message or a PR** until
the scan is re-run. The scan is cheap — it is the same `--only` launches the
determinism check uses.

### `-iwad` does not fail when the IWAD is missing — it loads another one

Ask for `DOOM.wad` on a machine that has only `DOOM2.wad` and the engine loads
DOOM2, saying so only in an `adding <path>` line. It then fails the map, falls
to the title screen, and **the attract demos are `GS_LEVEL`** — so `shotafter`
fires and captures a frame of the wrong game. The suite compares those against
the baseline and reports ordinary-looking DIFFs.

This is worse than a crash: every capture in such a run is of a different IWAD
than the one declared, and nothing says so. `run.py` now hard-exits naming both
the requested and the actually-loaded IWAD. (Note `6ebd03671`'s message claims
this surfaces as `NO CAPTURE` — that was written before the attract-demo
behaviour was observed, and is wrong.)

### A silent segfault on any unreadable `-file` (fixed 2026-08-12, `5a567cd85`)

**Symptom:** the engine dies with no error at all. The log ends after the
version banner — 230 bytes — and nothing says why. Through the matrix harness
this appeared as `NO CAPTURE` on all nine configurations at once, with nine
byte-identical logs.

**Cause**, in `src/common/filesystem/source/filesystem.cpp`:

```
for (size_t i = 0; i < filenames.size(); i++)
{
    AddFile(filenames[i].c_str(), nullptr, filter, Printf);
    ...
    path += Files.back()->GetHash();   // <-- unconditional
}
```

`AddFile()` reports a missing or unopenable file by **returning without pushing
to `Files`**. So on the first filename, `Files` is still empty and `Files.back()`
dereferences an empty vector: SIGSEGV inside `D_DoomMain`, before the video
system is up. And on the `CheckGameInfo` path `Printf` is null, so `AddFile`'s
own "File or Directory not found" is never emitted either — hence the silence.

**What made it fire here:** the harness's default scene names a mod pk3 under
`~/Documents`, which macOS TCC blocks for a process launched from a terminal
without Documents access. The file exists; the engine cannot open it; the
engine crashes.

**Why it matters beyond that:** this is inherited common code, not fork-specific.
Any launch naming an unreadable `-file` before the console exists hits it, on
any platform, and reproduces on upstream master.

**Two lessons worth keeping separate from the fix.** First, an empty-container
`.back()` is UB, so the symptom is whatever the allocator felt like — here a
clean crash, but a corrupted read is equally permitted and would have been far
harder to find. Second, the diagnosis initially stopped at "TCC blocked the
file, environment problem" and was wrong: the environment only *triggered* it.
A missing `-file` should produce a message, not a crash, and treating the
trigger as the cause would have left the bug in place.

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
