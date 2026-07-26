# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A **fork of GZDoom** (C++17, CMake 3.16+, GPLv3) that diverges from upstream ZDoom/gzdoom in two major ways:

1. **Native POSIX backend** (`GZDOOM_NATIVE_LINUX`, default **ON**) — replaces SDL2 for windowing and input on Linux/BSD, using ZWidget + Wayland/X11 + libinput/libudev directly. **This is what the current branch works on.**
2. **Native Metal renderer** (`HAVE_METAL`, default **ON** on Apple) — a direct Metal 2 backend for macOS instead of MoltenVK. **The live version of this work is on `origin/metal-audit`, not here** — see below.

Upstream architecture (renderer abstraction, ZScript VM, playsim, PK3 assets) is otherwise intact, so upstream docs (https://zdoom.org/wiki/, "Programmer's Corner") still apply for engine-level questions.

## Branch topology — read before touching Metal or postprocess code

| Branch | Contains |
|---|---|
| `master` | Upstream-tracking. |
| `native-platform-expansion` (current) | Native Linux/BSD windowing + input. Branched off `metal-final`. |
| `origin/metal-audit` | **The authoritative Metal renderer, macOS, and compute-shader-conversion work.** |
| `origin/metal-final` | The shared ancestor (`9bfaf4780`) both lines branched from. |

The two lines diverged at `9bfaf4780`: this branch is 3 commits ahead, `origin/metal-audit` is 22 commits ahead. **The `src/common/rendering/metal/` tree in this checkout is a stale snapshot** — it is missing `mt_compute.*`, `mt_ao.*`, `mt_bloom.*`, `mt_metrics.*`, `mt_system_wrapper.h`, and the native MSL shaders (`shaders/native/mt_ao.metal`, `mt_bloom.metal`, `ssao_simple.compute.glsl`). Do not "fix" or extend Metal code here; check `origin/metal-audit` first, or the work will be thrown away at merge.

The compute-shader conversion is **not confined to macOS files**. On `metal-audit` it also changes backend-neutral code that this branch touches:

- `src/common/rendering/v_video.h` — `DFrameBuffer::AmbientOccludeScene()` **changed signature** to `AmbientOccludeScene(float m5, const HWViewpointUniforms* currentViewpoint)`, plus a new `UseBottomLeft2DProjection()` virtual.
- `src/common/rendering/hwrenderer/postprocessing/hw_compute.h` — new, defines the backend-neutral `HWComputeEffect` enum (`AmbientOcclusion`, `Bloom`). Reuse these identities rather than adding backend-specific effect enums.
- `hw_postprocess.{cpp,h}`, `hw_postprocess_cvars.*`, `hw_drawinfo.cpp`, `hw_skyportal.cpp`, `hw_draw2d.cpp`, `hw_viewpointbuffer.cpp`, `i_time.*`.
- Shared GLSL: `wadsrc/static/shaders/pp/ssao.fp`, `ssaocombine.fp`; also `wadsrc/static/menudef.txt` and a new `tools/check_shader_parity.py`.

Expect real merge conflicts in those shared files. **`CLAUDE.md` itself conflicts** — `origin/metal-audit` has its own, written from the macOS/Metal perspective (it documents `AGENTS.md` and `docs/engine-modernization.md`, which exist only on that branch). Reconcile the two rather than clobbering either.

## Build & run

```bash
# Configure (build/ already exists here as a Debug Unix Makefiles tree)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPK3_QUIET_ZIPDIR=ON .
cmake --build build --parallel $(nproc)

# Run — binary and its .pk3s land side by side in build/
./build/gzdoom
./build/gzdoom -iwad doom.wad +map e1m1
```

After editing anything under `wadsrc/` (shaders, `menudef.txt`, other baked-in resources), the change will **not** appear at runtime until the pk3 is repacked. A full build does this, but to skip straight to it:

```bash
build/tools/zipdir/zipdir -udf build/gzdoom.pk3 wadsrc/static
```

The CMake target is named `zdoom`; the output binary is `gzdoom` (`ZDOOM_EXE_NAME`). It is placed in `ZDOOM_OUTPUT_DIR`, which defaults to the build root — **the executable will not run unless `gzdoom.pk3`, `lights.pk3`, `brightmaps.pk3`, `game_support.pk3`, and `game_widescreen_gfx.pk3` sit next to it**, which the build handles automatically.

Notable options (all in the top-level `CMakeLists.txt`):

| Option | Default | Effect |
|---|---|---|
| `GZDOOM_NATIVE_LINUX` | ON | Native Wayland/X11 backend instead of SDL2. Forces `ZWIDGET_NO_SDL`, requires `libinput` + `libudev`, defines `USE_X11 USE_WAYLAND GZDOOM_NATIVE_LINUX`. |
| `HAVE_VULKAN` | OFF | Builds the Vulkan backend (pulls in `libraries/ZVulkan`). |
| `HAVE_METAL` | ON (Apple only) | Metal backend + `libraries/ShaderTranslator`. |
| `HAVE_GLES2` | OFF | GLES2 backend. |
| `NO_OPENAL` | OFF | Disable OpenAL sound. |
| `PK3_QUIET_ZIPDIR` | OFF | Silence the zipdir asset packing spam; use it, the output is huge. |

Build types: `Debug`, `Release`, `RelWithDebInfo` (best for development), `MinSizeRel`.

Runtime backend override for the native path: `ZWIDGET_DISPLAY_BACKEND=Wayland|X11|SDL2` (see `libraries/ZWidget/src/window/window.cpp`; default probe order is Wayland → X11 → SDL2).

## Testing

There is no unit test suite. "Testing" means: (1) the build succeeds, (2) the binary launches a WAD and renders, (3) the CI matrix in `.github/workflows/continuous_integration.yml` stays green (Windows MSVC 2022, macOS 14, Linux GCC 9/12/latest, Clang 11/15/latest). CI installs `libsdl2-dev libvpx-dev libwebp-dev` — it does *not* install libinput/libudev, so native-backend dependency changes are a real CI break risk.

Since there is nothing to assert against, verify renderer/input changes by actually running the game and watching console output.

## Architecture

### Source layout

- **`src/common/`** — engine-agnostic subsystems shared with other Raze-family ports: rendering backends, platform layers, filesystem, scripting VM, fonts, textures, audio, menus, console.
- **`src/`** (top level, `playsim/`, `gamedata/`, `maploader/`, `rendering/`, `scripting/`) — Doom-specific game logic: actors, physics, map loading, DECORATE/ZScript definitions, software renderer, hardware scene renderer.
- **`libraries/`** — vendored deps. `ZWidget` (windowing/UI toolkit — heavily modified in this fork), `ZVulkan`, `ShaderTranslator` (glslang + SPIRV-Cross, git *subtree* not submodule), `ZMusic`, `metal-cpp`, `asmjit` (VM JIT), `cppdap` (ZScript debug adapter).
- **`wadsrc*/`** — game assets and all ZScript source (`wadsrc/static/zscript/`), packed into `.pk3` archives by the `zipdir` tool at build time. `wadsrc` → `gzdoom.pk3`, `wadsrc_bm` → `brightmaps.pk3`, etc.

### Rendering

Everything renders through the abstract `DFrameBuffer` (`src/common/rendering/v_video.h`), reachable via the global `screen`. Platform code supplies a `SystemBaseFrameBuffer`; each backend subclasses it:

```
DFrameBuffer → SystemBaseFrameBuffer (platform-specific)
  ├── VulkanRenderDevice   src/common/rendering/vulkan/system/
  ├── OpenGLFrameBuffer    src/common/rendering/gl/   (+ gl_load/glad for GL loading)
  ├── OpenGLESFrameBuffer  src/common/rendering/gles/
  └── MetalRenderDevice    src/common/rendering/metal/system/  (macOS only; stale here — see Branch topology)
```

Vulkan and Metal both use a **manager pattern** — the render device owns one manager per GPU resource class (`Vk*`/`Mt*`: CommandBuffer, Shader, Buffer, Texture, Sampler, PipelineState/RenderPass, RenderBuffers, Postprocess, DescriptorSet/ResourceBinding). Metal was deliberately built to mirror the Vulkan structure file-for-file, so **when adding a Metal feature, read the Vulkan equivalent first**.

Both defer state: `SetX()` calls only mutate the render state object; nothing hits the GPU until `Apply()`.

Shaders live as GLSL in `wadsrc/static/shaders/`. GL consumes them directly; Vulkan and Metal go GLSL → SPIR-V (glslang) → SPIR-V → MSL/GLSL (SPIRV-Cross), via `libraries/ShaderTranslator`.

### Platform layer

- `src/common/platform/posix/native/` — **the active Linux/BSD backend.** `nativevideo.cpp` (frame buffer creation, ZWidget window, key mapping), `i_input.cpp` (libinput/ZWidget event pump), `gl_sysfb.cpp` (GLX/EGL context), `native_display.cpp`, `st_start.cpp`.
- `src/common/platform/posix/sdl/` — legacy SDL2 path, only compiled with `-DGZDOOM_NATIVE_LINUX=OFF`.
- `src/common/platform/posix/cocoa/` — macOS.
- `src/common/platform/win32/` — Windows.

ZWidget is not just a widget toolkit here; it is the windowing/input abstraction the native backend routes all events through, and it also drives the launcher (`src/launcher/`) and error/net dialogs (`src/common/widgets/`).

### Scripting

`src/common/scripting/` holds the ZScript compiler and VM (`frontend/` parser+codegen, `vm/` interpreter, `jit/` asmjit x86-64 JIT, `backend/`, `dap/` debug adapter). `src/scripting/` binds engine C++ to script (`vmthunks*.cpp`, `thingdef*.cpp` for DECORATE). Objects are GC'd `DObject`s (`src/common/objects/`).

**Engine C++ and `wadsrc/static/zscript/` are versioned together** — a native function signature change requires updating the ZScript side in the same commit, or the game fails at startup with a script compile error.

## Conventions & gotchas

- **X11 header pollution**: always include `x11_compat.h` (it renames `GC`), and `#undef None` before including ZWidget headers — X11's `None`/`Window`/`GC` macros collide with engine and ZWidget identifiers. Several files forward-declare `Display`/`Window` rather than pull in Xlib.
- **CVars, not config structs**: runtime settings are `CVAR`/`EXTERN_CVAR` globals (`c_cvars.h`). Add new toggles as CVars.
- **Savegame compatibility**: changing serialized level state means bumping `SAVEVER` (and `MINSAVEVER` if you break old saves) in `src/version.h`.
- **Debug output**: `mt_debug` for Metal, `vk_debug` for Vulkan, `stat fps` in console for frame timing.
- **Metal Y-flip**: GZDoom is Y-up (GL convention), Metal is Y-down. `MtShaderManager::PatchVertexShader` rewrites `gl_Position` assignments (regex-based) to flip Y and remap Z from `[-1,1]` to `[0,1]`; this also inverts winding, so Metal front-face is Clockwise.
- The working tree tends to accumulate scratch artifacts at the repo root (`log*.txt`, `*.patch`, `backtrace.txt`, `test.cpp`, `error.log`). These are debugging leftovers, not part of the build — don't treat them as source.

## Other AI-agent docs

On this branch:

- `.github/copilot-instructions.md` — long, Metal-renderer-focused; good detail on Metal pitfalls (seam leaking, texture flashing, ring buffers, storage modes), but its build commands are macOS-centric.
- `GEMINI.md` — one-page summary of the native POSIX backend strategy.
- `gemini.md` — Vulkan architecture audit + Vulkan↔Metal parity table; useful when porting a Vulkan feature to Metal. Background only, not an active roadmap.

Only on `origin/metal-audit` (read via `git show origin/metal-audit:<path>`) — these are the current source of truth for Metal/compute work:

- `AGENTS.md` — implementation state, benchmark workflow, and session log for the Metal compute AO/bloom conversion.
- `docs/engine-modernization.md` — durable roadmap: frame graph, deterministic visibility kernel, simulation modernization. Consult before proposing architectural changes.
- `CLAUDE.md` — that branch's own version of this file.

Metal-specific benchmarking uses the `mt_metrics` / `mt_metrics_reset` console commands (metal-audit only); `mt_debug` for Metal logging, `vk_debug` for Vulkan.
