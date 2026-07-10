# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

GZDoom is a modder-friendly Doom-engine source port (C++17, GPLv3) supporting three rendering
backends — OpenGL, Vulkan (incl. MoltenVK), and a native Metal backend on macOS. This checkout is
mid-refactor on the `metal-audit` branch: the Metal backend is being brought to parity with Vulkan
and pushed toward a compute-postprocess / frame-graph architecture. See **Living docs** below before
starting any nontrivial Metal or rendering work — they contain state that changes faster than this file.

## Living docs (read these first for active work)

- `AGENTS.md` — current implementation state, benchmark workflow, session-by-session change log for
  the Metal compute AO/bloom work. This is the most up-to-date source of truth for "what's currently
  true about the Metal renderer."
- `docs/engine-modernization.md` — the durable roadmap (frame graph, deterministic visibility kernel,
  simulation modernization tracks). Consult before proposing architectural changes.
- `src/common/rendering/metal/README_METAL_RENDERER.md` — Metal renderer design notes and
  Vulkan-vs-Metal parity audit, written while bootstrapping the backend.
- `.github/copilot-instructions.md` — detailed Metal renderer conventions (Y-flip patching, ring
  buffers, sampler key values, culling winding gotchas, push constants vs uniform buffers, debugging
  workflow). Treat it as a Metal-renderer field guide; skim it before touching
  `src/common/rendering/metal/`.
- `gemini.md` — historical Vulkan architecture audit that seeded the Metal design. Background only,
  not an active roadmap.

## Build

First-time configure + build (Makefiles, this is how the checked-in `build/` dir here is configured):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo .
cmake --build build --target zdoom -j 8
```

Run:

```bash
./build/gzdoom.app/Contents/MacOS/gzdoom
```

After changing anything under `wadsrc/` (shaders, menudef, other engine resources baked into the
pk3), rebuild the pk3 explicitly — the shader/resource change will not appear otherwise:

```bash
build/tools/zipdir/zipdir -udf build/gzdoom.pk3 wadsrc/static
```

Other generators (Xcode for GPU frame debugging, Linux/Windows) are documented in
`.github/copilot-instructions.md` under "Platform-Specific Builds."

Build variants: `Release`, `Debug`, `RelWithDebInfo` (recommended for dev — optimized with symbols),
`MinSizeRel`.

## Testing

There is no unit test suite. Validation is manual:
1. Build succeeds.
2. Load a WAD/pk3 and verify rendering (`./build/gzdoom.app/Contents/MacOS/gzdoom +map e1m1`).
3. `stat fps` in console for frame timing; Metal-specific benchmarking uses `mt_metrics_reset` /
   `mt_metrics` (see `AGENTS.md` for the current baseline numbers and workflow).
4. CI matrix (Windows MSVC, macOS Xcode Release+Debug, Linux GCC/Clang) is defined in
   `.github/workflows/continuous_integration.yml`.

## Architecture

### Rendering backends

`SystemBaseFrameBuffer` is the abstract base; each backend implements it via a **manager pattern**
where each manager owns one GPU resource type's lifecycle (command buffers, samplers, textures,
buffers, shaders, pipeline state, render buffers, postprocess):

- `src/common/rendering/vulkan/system/` — `VulkanRenderDevice`, the reference implementation; other
  backends are designed to mirror its manager set.
- `src/common/rendering/metal/` — `MetalRenderDevice`, native macOS backend using `metal-cpp`.
  Split into `system/` (device, command buffers, buffer allocation), `renderer/` (state machine,
  pipeline state, render buffers, postprocess — `MtRenderState` is the most critical class here),
  `shaders/` (GLSL→SPIR-V→MSL pipeline), `textures/`. File/class naming: `mt_*.h`/`.cpp`, `Mt` prefix.
- `src/common/rendering/gl/` — OpenGL backend.
- Shared postprocess/hardware-renderer glue lives in `src/common/rendering/hwrenderer/`; backend-
  neutral compute effect names live in `hwrenderer/postprocessing/hw_compute.h` — reuse these rather
  than adding backend-specific effect enums when adding a new compute pass.

Shader source lives in `wadsrc/static/shaders/`, authored in GLSL, compiled GLSL→SPIR-V via
`glslang` then SPIR-V→MSL/GLSL via `libraries/ShaderTranslator/` for the target backend.

Coordinate system: engine-internal is Y-up (OpenGL-style). Metal is Y-down, so
`MtShaderManager::PatchVertexShader` regex-patches vertex shaders at compile time to flip Y and remap
Z from `[-1,1]` to `[0,1]`. This also inverts winding order, which is why Metal's `FrontFacingWinding`
is set to Clockwise to match GZDoom's CW convention under the Y-flip — see
`.github/copilot-instructions.md` for the full list of Metal-specific gotchas (seam leaking from wrong
sampler clamp modes, ring-buffer GPU/CPU sync, texture cache-key composition, etc.) before debugging
visual artifacts in the Metal path.

### Core engine

- `src/` top level — game logic entry points (`d_main.cpp`, `d_net.cpp`, `g_game.cpp`, `p_*` playsim
  files, save/serialization).
- `src/playsim/` — gameplay simulation (thinkers, actors, physics).
- `src/scripting/` and `src/common/scripting/` — the ZScript/DECORATE VM: `backend`/`vm`/`jit` compile
  and execute ZScript; `decorate` is the legacy DECORATE-to-ZScript-equivalent frontend. Thinker
  execution and ACS/ZScript ordering is intentionally serial and authoritative — do not parallelize
  `Tick()` calls transparently (scripts observe mutation/insertion/destruction order and RNG
  sequencing; see `docs/engine-modernization.md` Track C).
- `src/common/` — engine-wide subsystems shared across game logic and rendering: `textures`, `models`,
  `fonts`, `audio`, `console`, `filesystem` (WAD/pk3 virtual filesystem), `menu`, `2d`.
- `wadsrc/`, `wadsrc_bm/`, `wadsrc_extra/`, `wadsrc_lights/`, `wadsrc_widepix/` — resource trees zipped
  into the engine's own pk3 files at build time (menus, shaders, default configs, built-in assets).
  Changes here require the `zipdir` rebuild step above to take effect at runtime.
- `libraries/` — vendored/third-party deps: `ShaderTranslator` (SPIR-V↔MSL/GLSL), `ZMusic`, others.

### Build system

Root `CMakeLists.txt` handles cross-platform config, vcpkg manifest features, and the pk3-zipping
custom commands. `src/CMakeLists.txt` defines the `zdoom` executable target, selects rendering
backends via `HAVE_VULKAN` / `HAVE_METAL` / `HAVE_GLES2` options, and sets up per-platform bundling
(`MACOSX_BUNDLE`, `WIN32`).
