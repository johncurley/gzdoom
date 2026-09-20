# GZDoom Copilot Instructions

## Overview

GZDoom is a modder-friendly Doom engine source port supporting multiple rendering backends: OpenGL, Vulkan (via MoltenVK), and a native Metal renderer (macOS). This document provides guidance for working effectively in this codebase.

- **Language:** C++17
- **Build System:** CMake (3.16+)
- **Platform Support:** Windows, Linux, macOS
- **License:** GPLv3

## Implementer Mandate: High Reasoning, Scoped Blast Radius

1. **Zero Sycophancy (The Emergency Brake):**
   - Exercise full analytical reasoning on any specification or contract before implementing.
   - If you spot an unhandled edge case, invariant violation, timing/alignment hazard, or logical contradiction in the architectural plan: **DO NOT silently implement broken logic, and DO NOT unilaterally rewrite the architecture.**
2. **Halt & Flag Protocol:**
   - Immediately pause execution.
   - Concisely state:
     1. The exact location and nature of the defect/contradiction.
     2. Why the existing contract fails or produces undefined behavior.
     3. A minimal, concrete proposal to correct the contract or interface.
   - Wait for confirmation or contract adjustment before writing implementation code.
3. **Deep Local Rigor:**
   - Once the contract is verified sound, apply deep static rigor to the assigned scope (50–150 lines).
   - Ensure all boundaries, sign/width conventions, error paths, and resource lifecycles are 100% airtight without introducing external scope creep.

## Hard Constraints ("The Never List")

1. **NO Unsolicited Modernization:** Do NOT replace GZDoom idioms (`TArray`, `FString`, `PClass`, `AActor*`, custom allocators) with `std::` alternatives (`std::vector`, `std::unique_ptr`, `std::string`). Respect the engine's memory model and GC (`DObject`).
2. **NO Demo/Tick Desynchronization:** Game-logic hot paths must remain strictly deterministic. No unseeded randoms, non-deterministic floating-point math, or unstable iteration order in game-state updates.
3. **NO ZScript/VM ABI Breakage:** Do not modify exported engine symbols or VM bytecode layouts without updating bindings and reflection tables.
4. **NO Root Directory or Build Tree Pollution:** Temporary test scripts, scratch code dumps, and unapproved CMake targets must never be committed.

## Build & Test Commands

### Quick Build (macOS)

```bash
# Configure (first time only)
cmake -B build -DCMAKE_BUILD_TYPE=Release .

# Build
cmake --build build --parallel 3

# Run GZDoom
./build/gzdoom.app/Contents/MacOS/gzdoom
```

### Platform-Specific Builds

**macOS (with Metal/Vulkan support):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Xcode .
cmake --build build --config Release --parallel 3
```

**Linux (GCC):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12 .
cmake --build build --parallel 3
```

**Windows (Visual Studio 2022):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17" .
cmake --build build --config Release --parallel 3
```

### Build Variants

- **Release:** Optimized build, no debug info
- **Debug:** Full debug symbols, no optimizations
- **RelWithDebInfo:** Optimized with debug symbols (recommended for development)
- **MinSizeRel:** Smallest binary size

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo .
```

### Running Tests

GZDoom does not have a traditional unit test suite. Testing involves:
1. **Compilation validation** - Ensure the build succeeds
2. **Game execution** - Load a WAD and verify rendering
3. **Platform validation** - Test on target OS (see CI matrix in `.github/workflows/continuous_integration.yml`)

### Continuous Integration

The CI pipeline tests:
- Windows (MSVC 2022, Release + Debug)
- macOS 14 (both Release and Debug with Xcode)
- Linux (GCC 9, 12, Clang 11, 15, and latest)

Run CI locally with:
```bash
# Simulate CI configuration
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPK3_QUIET_ZIPDIR=ON .
cmake --build build --config RelWithDebInfo --parallel 3
```

## Architecture Overview

### Rendering System

GZDoom has a **multi-backend rendering architecture**:

```
SystemBaseFrameBuffer (abstract base, platform-specific)
  ├── VulkanRenderDevice (src/common/rendering/vulkan/system/)
  │   └── 10+ manager classes (shaders, buffers, textures, etc.)
  ├── OpenGLFrameBuffer (src/common/rendering/gl/)
  └── MetalRenderDevice (src/common/rendering/metal/system/)
      └── 10+ manager classes (Metal equivalent)
```

**Key File Locations:**
- Rendering backends: `src/common/rendering/{vulkan,metal,gl}/`
- Platform integration: `src/common/platform/posix/cocoa/` (macOS), `src/common/platform/win32/` (Windows)
- Shader system: `libraries/ShaderTranslator/` (SPIR-V↔MSL/GLSL translation)
- Core game logic: `src/` (d_main.cpp, d_net.cpp, etc.)

### Manager Pattern

Each rendering backend follows a **manager pattern** to organize functionality:

| Component | Responsibility |
|-----------|-----------------|
| **RenderDevice** | Orchestrates all managers, device lifecycle |
| **RenderState** | State machine for draw calls, pipeline caching |
| **CommandBufferManager** | GPU command submission, frame synchronization |
| **ShaderManager** | Shader compilation (GLSL→SPIR-V→MSL/GLSL) |
| **TextureManager** | Texture creation, uploads, mipmap generation |
| **BufferManager** | GPU memory allocation, ring buffers |
| **SamplerManager** | Sampler state caching |
| **PipelineStateManager** | Pipeline state caching |
| **RenderBuffers** | Framebuffer targets |
| **PostProcess** | Post-processing effects |

**Design Principle:** Each manager owns a specific GPU resource type and handles its lifecycle independently.

### Shader Translation Pipeline

```
GLSL source (in wadsrc/)
  ↓ [glslang - GLSL to SPIR-V]
SPIR-V bytecode
  ↓ [shader-translator - SPIR-V to target language]
MSL (Metal) or GLSL (OpenGL)
  ↓ [Native compiler]
GPU-ready shader
```

**Key File:** `libraries/ShaderTranslator/` contains the SPIR-V translation logic.

### Key Code Organization

- **`src/CMakeLists.txt`** - Main build configuration, selects rendering backend
- **`src/common/rendering/`** - All rendering backend implementations
- **`wadsrc/`** - Game resources (sprites, textures, shaders)
- **`libraries/`** - Third-party dependencies (ShaderTranslator, ZMusic, etc.)
- **`build/`** - Build artifacts (generated, .gitignored)

## Key Conventions

### 1. Metal Renderer Patterns

The Metal renderer is a native macOS backend using `metal-cpp` (Apple's C++ Metal bindings).

**File Naming:**
- `mt_*.h` / `mt_*.mm` for Metal classes
- `Mt` class prefix (e.g., `MtRenderState`, `MtTextureManager`)
- Compare with Vulkan: `vk_*.h` / `vk_*.cpp`, `Vk` prefix

**Initialization Order (Critical):**
1. Create `MetalRenderDevice` (device + command queue)
2. Instantiate all manager classes
3. Initialize managers in dependency order (shaders → pipelines → state)

**Resource Cleanup:**
- Use deferred deletion via completion handlers: `[cmdBuffer addCompletedHandler:^{...}]`
- Never delete GPU resources directly while GPU may use them

### 2. Coordinate Systems

GZDoom uses **Y-up (OpenGL-style) internally** but Metal uses **Y-down**. This requires careful handling:

**Vulkan (Y-up):**
```glsl
gl_Position = projection * viewModel * position;
```

**Metal (Y-down) - Patching Required:**
```glsl
// Y-flip + Z remap for Metal
gl_Position.y = -gl_Position.y;
gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; // [-1,1] → [0,1]
```

**Application:** The Metal shader manager patches vertex shaders at compile time using regex-based transformations (see `MtShaderManager::PatchVertexShader`).

### 3. Texture Clamping & Sampling

**Sampler Key Values (critical for correctness):**
- `0` = Repeat (wrap)
- `1` = Mirrored Repeat
- `2` = Clamp to Border
- `3` = Clamp to Edge

**Issue:** If geometry has **seam leaking**, check sampler address modes:
```cpp
// Postprocess samplers - fixed to clamp edges
samplerKey.AddressU = (wrap == Repeat) ? 0 : 3;  // 3 = ClampToEdge
samplerKey.AddressV = (wrap == Repeat) ? 0 : 3;
```

### 4. Ring Buffers

Dynamic GPU data (viewpoint matrices, per-frame lights, animation timers) use **ring buffers** (circular allocation):

- **Buffer Size:** 8-16 MB per frame (configurable)
- **Pattern:** Allocate sequentially, wrap at end
- **Synchronization:** GPU must finish frame N before CPU overwrites it

**Common Issue:** GPU stalls if you don't wait for the GPU to finish before wrapping. Always call `WaitForCommands()` if necessary.

### 5. Lazy State Evaluation

Both Vulkan and Metal use the **Apply() pattern** to batch state changes:

```cpp
// State is NOT applied immediately
mRenderState->SetViewport(x, y, w, h);
mRenderState->SetScissor(sx, sy, sw, sh);
mRenderState->SetDepthBias(bias);

// Apply all accumulated state in one shot
mRenderState->Apply(drawtime);

// Now submit draw call
mRenderState->Draw(dt, index, count, false); // false = don't re-apply
```

**Benefit:** Minimizes API calls, reduces GPU stalls.

### 6. Framebuffer Management

**Color/Depth Targets:**
- Main color: Screen framebuffer (from `CAMetalLayer`)
- Render targets: Custom textures for deferred rendering, post-processing
- Depth: Dedicated depth texture (memoryless on TBDR, private on IMR)

**Load Actions (Metal-specific optimization):**
- `.clear` - Clear target at start of pass
- `.load` - Preserve previous contents (slower!)
- `.dontCare` - Don't care what's there (fastest)

Use `.dontCare` for intermediate targets to avoid wasted bandwidth.

### 7. Mipmaps & Texture Lifecycle

**Mipmap Generation:**
- Use dedicated blit command buffers to avoid interrupting render passes
- Call after uploading base texture level
- Safe async generation using `generateMipmaps()` on encoder

**Texture Reuse (Performance):**
- Cache textures by (width, height, format, mipmap count)
- Reuse if dimensions match; only upload new data
- Prevents constant GPU allocations ("flashing" artifacts)

### 8. Push Constants & Uniform Buffers

Two types of shader data:

| Type | Size | Update Freq | Use Case |
|------|------|-------------|----------|
| **Push Constants** | <256 bytes | Per-draw | Color, transform overrides |
| **Uniform Buffers** | Unlimited | Per-frame | Matrices, lighting, animations |

**Metal Translation:**
- Push constants → `setVertexBytes:` / `setFragmentBytes:` (inline buffer)
- Uniform buffers → `setVertexBuffer:` / `setFragmentBuffer:` (GPU buffer)

**Synchronization Critical:** Ensure push constant values in C++ match shader expectations exactly.

### 9. Blend Modes & Render State

GZDoom supports ~12 blend modes (Normal, Add, Subtract, Reverse Subtract, etc.). Render state tracks:

- Depth test/write
- Stencil test/write
- Blend factors (source, destination)
- Color write mask
- Culling (CW vs CCW)

**Culling Gotcha:** Metal Y-flip inverts winding order:
- GZDoom CW (OpenGL) → Metal FrontFace = CW (not MTL::CullModeBack!)
- This is counter-intuitive but accounts for coordinate system difference

### 10. Platform-Specific Code

**macOS-specific patterns:**
- Check `ifdef HAVE_METAL` (CMake sets this)
- Use `CocoaNativeHandle` to access `CAMetalLayer` for display
- Link Metal framework: `-framework Metal -framework MetalKit`

**Code Example:**
```cpp
#ifdef HAVE_METAL
    auto* metalLayer = GetMetalLayer(); // from CocoaNativeHandle
    metalLayer.displaySyncEnabled = vsync_enabled ? YES : NO;
#endif
```

## Testing Metal Renderer

### Enable Debug Output

Set console variables to log Metal operations:
```
mt_debug 1        // Enable Metal-specific logging
r_debug 1         // General rendering debug info
r_showmaps 1      // Visualize render targets
```

### Common Debug Tasks

**Verify shader compilation:**
- Check console for shader compilation errors
- If shader fails, the fallback is a white placeholder

**Check texture uploads:**
- Enable `mt_debug` to see texture allocation messages
- Look for "Metal: Created GPU texture" and format info

**Validate render passes:**
- `mt_debug` logs render pass begin/end events
- Verify load/store actions match expectations

**Profile performance:**
- GZDoom console: `stat fps` for frame timing
- Metal debugger in Xcode: Capture frame and inspect GPU work

## Debugging Tips

### 1. Debugging Metal Code

Use Xcode's Metal debugger:
1. Add breakpoint in render loop
2. Run in Xcode: `Product → Scheme → Edit Scheme → Diagnostics → Metal`
3. Capture frame with GPU Frame Debugger
4. Inspect textures, buffers, pipelines in-frame

### 2. Shader Debugging

Broken shaders appear as white geometry. To debug:
1. Enable `mt_debug` to see compile errors
2. Export the MSL source:
   - Add temporary file write in `MtShaderManager::CompileMSLToLibrary`
   - Check generated MSL for Y-flip patches
3. Test MSL separately in Metal Shader Converter tool

### 3. Performance Profiling

```bash
# Run with frame timing
./gzdoom +map e1m1
# Then in console: stat fps
```

Use Instruments.app (Xcode) to profile:
- System Trace
- Metal System Trace
- Allocations

### 4. Memory Leaks

Enable Metal API validation:
```bash
# Run with GPU validation
MTL_DEBUG_LAYER=1 ./gzdoom
```

Check for:
- Unreleased command buffers
- Textures not recycled to texture pool
- Buffers not returned to ring buffer

## Common Pitfalls

### 1. Y-Flip Bugs
- **Symptom:** Geometry is upside down
- **Fix:** Verify `MtShaderManager::PatchVertexShader` is applying Y-flip
- **Check:** Confirm patch regex matches all `gl_Position` assignments

### 2. Seam Leaking
- **Symptom:** Visible lines/pixels at texture boundaries
- **Cause:** Sampler address modes set to Repeat instead of ClampToEdge
- **Fix:** Check `AddressU`, `AddressV` values in sampler creation (should be `3` for clamping)

### 3. "Flashing" Artifacts
- **Symptom:** Textures appear and disappear frame-to-frame
- **Cause:** Texture being recreated instead of reused
- **Fix:** Check texture cache key includes (width, height, format, mipLevelCount)

### 4. Black Geometry
- **Symptom:** Some meshes render as solid black
- **Cause:** Uninitialized buffer data or invalid texture bindings
- **Fix:** 
  - Verify texture pool isn't exhausted
  - Check buffer writes complete before reading
  - Enable `mt_debug` to see texture binding info

### 5. Missing Assets
- **Symptom:** "Missing texture" placeholder everywhere
- **Cause:** Shader translation failure or missing texture resources
- **Fix:**
  - Rebuild with fresh CMake config
  - Check shader compile errors in `mt_debug` output
  - Verify PK3 files are generated (see `build/*.pk3`)

## File Structure Reference

### Critical Paths for Common Tasks

| Task | Files |
|------|-------|
| Add render feature | `src/common/rendering/metal/renderer/mt_renderstate.*` |
| Fix shader bug | `src/common/rendering/metal/shaders/mt_shader.*` |
| Optimize textures | `src/common/rendering/metal/textures/mt_texture.*` |
| Adjust sampler behavior | `src/common/rendering/metal/textures/mt_sampler.*` |
| Change memory allocation | `src/common/rendering/metal/system/mt_buffer.*` |
| Debug render passes | `src/common/rendering/metal/renderer/mt_pipelinestate.*` |
| Add post-processing | `src/common/rendering/metal/renderer/mt_postprocess.*` |

## Resources

- **Official Wiki:** https://zdoom.org/wiki/ → Programmer's Corner
- **Metal Documentation:** https://developer.apple.com/metal/
- **Vulkan Reference (for architecture):** `src/common/rendering/vulkan/` (reference implementation)
- **Metal Renderer README:** `src/common/rendering/metal/README_METAL_RENDERER.md`
- **Shader Translator:** `libraries/ShaderTranslator/` (SPIR-V→MSL translation)
- **Forum:** https://forum.zdoom.org/ (community support)

## Repository Info

- **Main Branch:** Usually `master` (stable release)
- **Development:** Feature branches (e.g., `metal-final` for Metal work)
- **Metal Specific:** Look for branches with `metal` in the name
- **Git Remotes:** Usually `origin` points to `github.com/ZDoom/gzdoom`

See `SECURITY.md` for security policy and `LICENSE` for GPL v3 details.
