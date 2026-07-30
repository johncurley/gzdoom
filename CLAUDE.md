# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A **fork of GZDoom** (C++17, CMake 3.16+, GPLv3) that diverges from upstream ZDoom/gzdoom in two major ways:

1. **Native POSIX backend** (`GZDOOM_NATIVE_LINUX`, default **ON**) — replaces SDL2 for windowing and input on Linux/BSD, using ZWidget + Wayland/X11 + libinput/libudev directly. **This is what the current branch works on.**
2. **Native Metal renderer** (`HAVE_METAL`, default **ON** on Apple) — a direct Metal 2 backend for macOS instead of MoltenVK. **The live version is on `origin/metal-audit`, not here.**

Upstream architecture (renderer abstraction, ZScript VM, playsim, PK3 assets) is intact, so upstream docs (https://zdoom.org/wiki/, "Programmer's Corner") still apply for engine-level questions.

Note upstream ZDoom/GZDoom is effectively frozen; active community development moved to UZDoom. This fork is independent of both.

## Branch topology

| Branch | Contains |
|---|---|
| `master` | Upstream-tracking mirror. Not an integration branch — commits are coelckers, dpjudas et al. |
| `native-platform-expansion` (current) | Native Linux/BSD windowing + input, raw keyboard input, ZWidget subtree. |
| `origin/metal-audit` | **Metal renderer, macOS, and compute-shader-conversion work.** |
| `origin/metal-final` | Shared ancestor (`9bfaf4780`). |

Both lines branched from `9bfaf4780`. As of this writing `native-platform-expansion` is 26 commits ahead, `origin/metal-audit` 22.

**The `src/common/rendering/metal/` tree here is a stale snapshot.** It lacks `mt_compute.*`, `mt_ao.*`, `mt_bloom.*`, `mt_metrics.*`, `mt_system_wrapper.h` and the native MSL shaders. Do not extend Metal code on this branch — check `origin/metal-audit` first.

### The pending merge

Files changed on **both** lines (excluding the ZWidget subtree):

```
CLAUDE.md   .gitignore   src/CMakeLists.txt   src/d_main.cpp
src/common/rendering/gl/gl_framebuffer.{cpp,h}   wadsrc/static/menudef.txt
```

Beyond textual conflicts, `metal-audit` changes shared interfaces this branch consumes:

- `DFrameBuffer::AmbientOccludeScene()` gains a `const HWViewpointUniforms*` parameter, plus a new `UseBottomLeft2DProjection()` virtual (`src/common/rendering/v_video.h`)
- new `hwrenderer/postprocessing/hw_compute.h` defining `HWComputeEffect`
- changes across `hw_postprocess.*`, `hw_drawinfo.cpp`, `hw_skyportal.cpp`, `i_time.*`
- shared GLSL: `wadsrc/static/shaders/pp/ssao.fp`, `ssaocombine.fp`
- new `tools/check_shader_parity.py`

`metal-audit` also carries `AGENTS.md` and `docs/engine-modernization.md`, which exist only there and are the source of truth for the Metal/compute work. Read via `git show origin/metal-audit:<path>`.

## Build & run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPK3_QUIET_ZIPDIR=ON .
cmake --build build --parallel $(nproc)
./build/gzdoom -iwad /path/to/doom2.wad
```

After editing anything under `wadsrc/`, repack the pk3 or the change won't appear:

```bash
build/tools/zipdir/zipdir -udf build/gzdoom.pk3 wadsrc/static
```

The CMake target is `zdoom`; the binary is `gzdoom`. It needs `gzdoom.pk3`, `lights.pk3`, `brightmaps.pk3`, `game_support.pk3` and `game_widescreen_gfx.pk3` beside it — the build handles this.

Backend override at runtime: `ZWIDGET_DISPLAY_BACKEND=Wayland|X11|SDL2`. Default probe order is Wayland → X11 → SDL2.

| Option | Default | Effect |
|---|---|---|
| `GZDOOM_NATIVE_LINUX` | ON | Native Wayland/X11 instead of SDL2. Requires `libinput`, `libudev`. |
| `HAVE_VULKAN` | OFF | Vulkan backend (pulls in `libraries/ZVulkan`). |
| `HAVE_METAL` | ON (Apple) | Metal backend + `libraries/ShaderTranslator`. |
| `ENABLE_SDL2` / `ENABLE_SDL3` | forced OFF | See the trap below. |

## Traps that have already cost time

**`ZWIDGET_NO_SDL` is dead.** ZWidget selects SDL backends with `ENABLE_SDL2`/`ENABLE_SDL3`, which default **ON** for non-Windows. They are forced OFF in the **root** `CMakeLists.txt` immediately before `add_subdirectory(libraries/ZWidget)` — setting them from `src/CMakeLists.txt` is too late, because ZWidget is configured at root line ~401 and `src` at ~503. Get this wrong and SDL silently relinks with no error.

**Generated Wayland protocol bindings versus system headers.** The tree vendors wayland-scanner output but compiles against the system `libwayland` headers, so the two can disagree. This has bitten twice: `WL_KEYBOARD_KEY_STATE_REPEATED` (wl_keyboard v10, absent on Ubuntu 22.04) and `wl_pointer_listener` gaining members. Guard version-dependent entries with the `*_SINCE_VERSION` macros the header defines; don't rely on newer enum constants existing.

**X11 macro pollution.** Include `x11_compat.h` (renames `GC`) and `#undef None` before ZWidget headers. `x11_remap.h` undefines more, which is why some X11 code compares against `0L` rather than `None` and uses `2` rather than `Always`.

**CVars, not config structs.** Runtime settings are `CVAR`/`EXTERN_CVAR` globals. Note that `CUSTOM_CVAR` clamp callbacks do **not** appear to run for command-line `+set` — `ui_theme` and others can hold out-of-range values, so selection logic needs a safe fallback rather than trusting the clamp.

**Savegame compatibility:** changing serialized level state means bumping `SAVEVER` (and `MINSAVEVER`) in `src/version.h`.

## Architecture

### Source layout

- **`src/common/`** — engine-agnostic subsystems: rendering backends, platform layers, filesystem, scripting VM, fonts, textures, audio, menus, console.
- **`src/`** (`playsim/`, `gamedata/`, `maploader/`, `rendering/`, `scripting/`) — Doom-specific logic.
- **`libraries/`** — `ZWidget` (**git subtree**, see below), `ZVulkan`, `ShaderTranslator` (subtree; glslang + SPIRV-Cross), `ZMusic`, `metal-cpp`, `asmjit`, `cppdap`.
- **`wadsrc*/`** — assets and all ZScript, packed to `.pk3` by `zipdir` at build time.

### Rendering

Everything renders through `DFrameBuffer` (`src/common/rendering/v_video.h`), reachable via the global `screen`. Platform code supplies a `SystemBaseFrameBuffer`; backends subclass it:

```
DFrameBuffer → SystemBaseFrameBuffer
  ├── VulkanRenderDevice   vulkan/system/
  ├── OpenGLFrameBuffer    gl/        (+ gl_load/glad)
  ├── OpenGLESFrameBuffer  gles/
  └── MetalRenderDevice    metal/system/   (macOS; stale here)
```

Vulkan and Metal use a **manager pattern** — one manager per GPU resource class (`Vk*`/`Mt*`). Metal deliberately mirrors Vulkan file-for-file, so **read the Vulkan equivalent first** when adding a Metal feature. Both defer state: `SetX()` mutates the render state object; nothing reaches the GPU until `Apply()`.

Shaders are GLSL in `wadsrc/static/shaders/`. GL consumes them directly; Vulkan and Metal go GLSL → SPIR-V (glslang) → MSL/GLSL (SPIRV-Cross) via `libraries/ShaderTranslator`, which also has an unused HLSL→SPIR-V path (`SHADER_TRANSLATOR_ENABLE_HLSL_INPUT`, default OFF, needs DXC).

The GL backend requires GL 3.3 and already version-gates features (`gl_version >= 4.3f`). glad loads `glDispatchCompute`, but **no GL compute path exists yet**.

### Platform layer

`src/common/platform/posix/native/` is the active Linux/BSD backend: `nativevideo.cpp` (framebuffer, ZWidget window, key routing), `i_input.cpp` (event pump, capture policy), `gl_sysfb.cpp` (GLX/EGL), `native_display.cpp`, `st_start.cpp`. The `sdl/` directory is the legacy path, only built with `-DGZDOOM_NATIVE_LINUX=OFF`.

Only `libinput`, `libudev` and `libgudev` are linked. **Wayland, xkbcommon, X11 and Xi are `dlopen`'d** via `wayland_dynamic.h` / `x11_dynamic.h`, so one binary runs under either or neither. Do not add link-time dependencies on them.

### ZWidget is a subtree

`libraries/ZWidget` tracks `johncurley/ZWidget` (remote `zwidget`), branch `wayland-c-bindings`, which sits directly on top of `dpjudas/ZWidget` master with no divergence.

```bash
git subtree pull --prefix=libraries/ZWidget zwidget wayland-c-bindings --squash
git subtree push --prefix=libraries/ZWidget zwidget <branch>
```

It was previously a plain directory copy with no recorded upstream ref, which let three lineages drift apart until **eight API families** had silently diverged — window creation (`WidgetType` vs a popup bool), `SetCursor`, the frame API, `OnWindowRawKey`, `ListView`, resource loading, themes, raw keyboard. None surfaced until the two halves were compiled together. **Keep the subtree relationship intact; do not hand-edit `libraries/ZWidget` without pushing back.**

Fork-side work living there: generated Wayland protocol bindings replacing waylandpp, runtime `dlopen` loaders, `POSIXNativeTheme` (desktop colour detection), raw keyboard dispatch, and fixes for stuck keys, a window-destroy use-after-free, missing punctuation mappings, and `GetKeyState` on mouse buttons.

### Input

Two paths, selected per context:

- **Cooked** — keysym → `InputKey` → gzkey. Used for GUI (menus, console) because only it produces text.
- **Raw** (`in_rawkeyboard`, default off) — evdev scancode passed through unchanged. `RawKeycode`, `DIK_*` and evdev are the same PC scancode set, so no mapping table is involved and the path is layout- and modifier-independent.

Raw drives gameplay only, when `in_rawkeyboard && !GUICapture`. The backend reports both, so cooked gameplay events are suppressed while raw is active. Switching paths under a held key resets button state, or the press and release land on different paths.

`in_keytrace 1` logs every key event to **stderr** (not `Printf` — that goes to the in-game console once video is up) as `down`/`up`/`rep`/`rawdn`/`rawup`/`mdn`/`mup`/`wheel`, plus the held-button set once per tic. `ZWIDGET_TRACE_REPEAT=1` logs client-side key repeat. **These are temporary diagnostics and should be removed once input is settled.**

**A trap worth knowing:** `I_StartTic()` must call `buttonMap.ResetButtonTriggers()` first. `bWentDown`/`bWentUp` are set by `PressKey`/`ReleaseKey` and cleared nowhere else, and `G_BuildTiccmd` reads `ButtonPressed()` (which *is* `bWentDown`) for jump and attack. Without it a single tap latches the button on for every subsequent tic. This was missing here and produced symptoms that looked exactly like stuck keys while every input trace showed balanced press/release pairs — because the latched flag was `bWentDown`, not `bDown`. The SDL backend still has the same omission, upstream included.

### Theming

ZWidget themes only the **launcher, error window and net-start window** — not in-game menus, which use GZDoom's own renderer. `InitWidgetResources()` runs once at startup, so `ui_theme` changes need a restart to show.

```
ui_theme 0   auto — native desktop colours on Unix   (default)
ui_theme 1   dark
ui_theme 2   light
ui_theme 3   follow system light/dark, built-in palette
```

`POSIXNativeTheme` reads KDE `kdeglobals`, GTK `settings.ini`, then Xresources. Desktops only report a background and foreground, so the other ten colours are **derived by luminance** — a heuristic, which is why `3` exists as an escape hatch.

GZDoom installs its own `ResourceLoader` because widget assets live in the pk3, not on disk, and maps ZWidget's abstract `"system"`/`"monospace"` families onto the bundled Noto Sans.

### Scripting

`src/common/scripting/` holds the ZScript compiler and VM (`frontend/`, `vm/`, `jit/` asmjit, `dap/`). `src/scripting/` binds C++ to script. **Engine C++ and `wadsrc/static/zscript/` are versioned together** — a native signature change needs the ZScript side updated in the same commit.

## CI

`.github/workflows/continuous_integration.yml` — Windows MSVC 2022, macOS 14, Linux GCC 9/12/latest and Clang 11/15/latest.

Linux jobs install `libinput-dev libudev-dev libwayland-dev libxkbcommon-dev libx11-dev libxi-dev libgl1-mesa-dev libegl-dev libdbus-1-dev libfontconfig1-dev`. The dbus and fontconfig requirements come from ZWidget's portal dialogs and font handling.

**A blocking smoke test runs the built binary** under Xvfb with `ZWIDGET_DISPLAY_BACKEND=X11`, asserting it reaches `W_Init` and brings up a GL context. It exists because a commit once passed all ten jobs while dying on startup — the workflow compiled and linked but never launched anything.

**What CI does not cover:** `+quit` short-circuits before a map loads, so script parsing, the texture manager and rendering a frame are untested. There is no unit test suite. Anything behavioural needs playing the game.

## Outstanding

- **Merge `metal-audit` ↔ `native-platform-expansion`** — gates all renderer work. See the pending-merge section.
- **Validate Metal on Apple Silicon** — developed on a macOS 12.7 Intel Mac. TBDR vs IMR differences (memoryless storage, store actions, `didModifyRange:`) mean Intel-correct code can be wrong on M-series.
- **Remove the input diagnostics** (`in_keytrace`, `ZWIDGET_TRACE_REPEAT`) once input is settled.
- **X11 raw input** — Wayland is done; X11 needs XInput2 raw events.
- **`check_shader_parity.py` into CI** — more valuable as GL becomes a partial reference (GL 4.1 on macOS cannot do compute).
- **Report ZWidget bugs upstream to dpjudas** — the stuck-key and use-after-free fixes affect every ZWidget application on Wayland.

## Other AI-agent docs

- `.github/copilot-instructions.md` — Metal renderer field guide: Y-flip patching, ring buffers, sampler key values, culling winding. macOS-centric build commands.
- `GEMINI.md` — one page on the native POSIX backend strategy.
- `gemini.md` — Vulkan architecture audit and Vulkan↔Metal parity table. Background, not a roadmap.
- On `origin/metal-audit` only: `AGENTS.md` (Metal compute state and benchmark workflow) and `docs/engine-modernization.md` (frame graph, visibility, simulation roadmap).
