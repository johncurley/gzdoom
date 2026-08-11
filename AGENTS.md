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

**Do not record a `doom2` baseline until this is understood** -- it would
enshrine one arbitrary state out of at least four.

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
