# GZDoom Rendering Architecture

**Last updated:** 2026-07-22 (independent audit completed)

> **Status:** This document was originally written as a Vulkan architecture audit to seed the
> Metal renderer design. It is now a **historical reference** alongside the newer Metal backend
> documentation. For active work, see the following **living documents**:
>
> - `AGENTS.md` — Current implementation state, benchmark data, session change log (Metal compute
>   AO/bloom work, Intel optimizations, texture upload instrumentation).
> - `docs/engine-modernization.md` — Durable roadmap (frame graph, deterministic visibility,
>   simulation modernization).
> - `.github/copilot-instructions.md` — Metal renderer field guide (conventions, pitfalls,
>   debugging workflow, build commands).
> - `FINDINGS.md` — 2026-07-22 independent audit results (13 findings, 2 critical).
>
> The remainder of this document preserves the original Vulkan architecture audit that informed
> the Metal backend's manager-pattern design.

---

## Core Component: `VulkanRenderDevice`

The `VulkanRenderDevice` class (`src/common/rendering/vulkan/system/vk_renderdevice.h`/`.cpp`) is the central orchestrator for the Vulkan rendering backend. It inherits from `SystemBaseFrameBuffer`, suggesting a common interface for different rendering APIs.

### Key Responsibilities:
*   **Device Management:** Initializes, manages, and destroys the Vulkan logical device (`std::shared_ptr<VulkanDevice> device`).
*   **Resource Orchestration:** Coordinates various manager classes responsible for specific Vulkan resources and functionalities.
*   **Frame Lifecycle:** Manages the setup, rendering, and presentation of each frame.
*   **Shader Management:** Integrates with a shader management system for compilation and usage.
*   **Error Handling:** Provides mechanisms for Vulkan-specific error reporting.

### Initialization (`VulkanRenderDevice` Constructor and `InitializeState()`):
1.  **Device Creation:** The constructor uses `VulkanDeviceBuilder` to discover compatible physical devices (GPUs) and create a logical Vulkan device. This process involves selecting a device based on `vk_device` cvar and potentially enabling ray tracing.
2.  **Manager Instantiation:** `InitializeState()` instantiates and initializes a comprehensive set of manager classes, each owning a specific aspect of the Vulkan rendering pipeline:
    *   `VkCommandBufferManager` (`mCommands`): Handles Vulkan command buffer allocation, recording, and submission.
    *   `VkSamplerManager` (`mSamplerManager`): Manages Vulkan sampler objects (texture filtering, addressing modes).
    *   `VkTextureManager` (`mTextureManager`): Manages hardware textures (`VkHardwareTexture`) and their lifecycle.
    *   `VkFramebufferManager` (`mFramebufferManager`): Manages Vulkan framebuffer objects.
    *   `VkBufferManager` (`mBufferManager`): Manages various Vulkan buffers (vertex buffers, index buffers, uniform buffers, storage buffers).
    *   `VkPostprocess` (`mPostprocess`): Handles post-processing effects and their application.
    *   `VkDescriptorSetManager` (`mDescriptorSetManager`): Manages Vulkan descriptor sets for binding resources to shaders.
    *   `VkRenderPassManager` (`mRenderPassManager`): Manages Vulkan render pass objects, defining rendering operations.
    *   `VkRaytrace` (`mRaytrace`): Manages ray tracing functionalities if enabled and supported.
    *   `VkShaderManager` (`mShaderManager`): Oversees the loading, compilation (GLSL to SPIR-V), and management of shaders.
    *   `VkRenderState` (`mRenderState`): Manages the current rendering pipeline state (e.g., blend states, depth states, stencil states). Notably, `VkRenderStateMolten` is used on Apple platforms, hinting at platform-specific adaptations for Vulkan on macOS (via MoltenVK).

### Rendering Pipeline (High-Level):
1.  **Begin Frame (`BeginFrame()`):** Prepares the rendering context for a new frame, including viewport setup and initializing managers.
2.  **Scene Rendering:** The actual 3D scene rendering logic is abstracted but relies on `mRenderState` and data provided by managers (e.g., `mVertexData`, `mSkyData`, `mViewpoints`, `mLights`, `mBones`).
3.  **2D Drawing (`Draw2D()`):** Integrates with a separate 2D drawing system (`twod`) to render UI elements and overlays.
4.  **Post-Processing (`PostProcessScene()`):** Applied via the `mPostprocess` manager, potentially blurring the scene or applying other effects.
5.  **Frame Update (`Update()`):** Orchestrates the rendering passes, ending render passes and frames, waiting for GPU commands, and updating GPU statistics.
6.  **Presentation:** Implicitly handled by `SystemBaseFrameBuffer::Update()` and `VkCommandBufferManager` submitting commands to the GPU and presenting the swapchain.

### Resource Management:
*   **Textures:** `VkTextureManager` and `VkHardwareTexture` handle texture creation, allocation, and management. Textures can be precached (`PrecacheMaterial`).
*   **Buffers:** `VkBufferManager` handles creation of vertex, index, and general data buffers.
*   **Materials:** `FMaterial` objects (which encapsulate textures and their properties) are managed, with `VkMaterial` being the Vulkan-specific implementation.

### Shader Compilation:
*   `VkShaderManager` is responsible for loading and compiling shaders.
*   GLSL shaders are compiled into SPIR-V, which is then used by the Vulkan backend. The `ShaderTranslator` library, now detached from `ZVulkan`, is likely used in this process.

## Implications for Metal Renderer Development:

| Principle | Status in Metal |
|-----------|----------------|
| **Manager-Based Architecture** | Fully implemented: `MtRenderState`, `MtCommandBufferManager`, `MtTextureManager`, `MtBufferManager`, `MtShaderManager`, `MtSamplerManager`, `MtPipelineStateManager`, `MtResourceBindingManager`, `MtRenderBuffers`, `MtPostprocess`, `MtAOModule`. |
| **`SystemBaseFrameBuffer` Abstraction** | `MetalRenderDevice` inherits from it. |
| **Shared High-Level Logic** | Reused — scene traversal, material handling, 2D drawing are shared. |
| **Shader Translation (SPIR-V→MSL)** | Uses `ShaderTranslator` library. Native `.metal` files compiled into `native_shaders.metallib` loaded first; inline C++ strings are fallback. |
| **Resource Conversion** | `MtHardwareTexture`, `MtVertexBuffer`, `MtIndexBuffer`, etc. |

## Metal Backend Status (as of 2026-07-22)

The Metal backend is mature with compute AO/bloom, real GPU frame timing, and Intel optimizations.
For current priority ordering, benchmark data, and known issues, see `AGENTS.md`.

### Known Findings (from 2026-07-22 audit — see `FINDINGS.md` for full details)

| # | Severity | Description |
|---|----------|-------------|
| 1 | CRITICAL | `MTL::RenderPassDescriptor` leaked on every `BeginRenderPass` |
| 2 | CRITICAL | `WaitForCommands(true)` doesn't actually synchronize GPU |
| 3 | HIGH | `PatchFragmentShader()` never called — shadows broken |
| 4 | HIGH | ShadowMap null dereference in color clear path |
| 5 | MEDIUM | Texture re-upload races (due to finding #2) |
| 6 | MEDIUM | Missing normal-attribute detection in `ApplyStreamData` |
| 7 | MEDIUM | `mPassDescriptor` member is dead code |
| 8 | MEDIUM | `ClearScreen()` missing scissor/viewport reset |

### Key Conventions (Metal-specific)

All documented in `.github/copilot-instructions.md`:
- Y-flip via `PatchVertexShader`, Reverse-Z (1.0=near, 0.0=far), `FrontFacingWinding=Clockwise`
- Lazy state evaluation via `Apply()` pattern
- Ring buffers for per-frame dynamic data
- Push constants at buffer index 21
- Native `.metal` files are authoritative over inline C++ shader strings

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo .
cmake --build build --target zdoom -j 8
./build/gzdoom.app/Contents/MacOS/gzdoom
```

After `wadsrc/static/` changes: `build/tools/zipdir/zipdir -udf build/gzdoom.pk3 wadsrc/static`
