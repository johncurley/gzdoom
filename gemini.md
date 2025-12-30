# Vulkan Renderer Architecture Overview (GZDoom)

This document outlines the architecture of the existing Vulkan renderer in GZDoom, as derived from an audit of `src/common/rendering/vulkan/system/vk_renderdevice.cpp` and `src/common/rendering/vulkan/system/vk_renderdevice.h`. This understanding will serve as a foundation for developing a Metal renderer.

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

*   **Manager-Based Architecture:** The existing architecture is highly modular, using distinct manager classes for different rendering aspects. This pattern should be replicated for the Metal renderer (e.g., `MtCommandBufferManager`, `MtTextureManager`, etc.).
*   **Abstraction Layer:** The `SystemBaseFrameBuffer` base class provides a good starting point for abstracting render device functionalities, allowing a Metal implementation to plug in.
*   **Shared High-Level Logic:** Much of the engine's higher-level rendering logic (e.g., scene graph traversal, material handling, 2D drawing integration) is likely decoupled from the low-level Vulkan API calls and can be reused or adapted.
*   **Shader Translation:** The `ShaderTranslator` (which now contains `glslang` and `SPIRV-Cross`) is already set up to translate SPIR-V to MSL, making it directly usable for the Metal backend.
*   **Resource Conversion:** Existing resource types (e.g., `FMaterial`, `FGameTexture`, various buffers) will need corresponding Metal implementations and conversion logic.

This audit provides a solid understanding of the Vulkan renderer's structure, which will guide the design and implementation of the Metal renderer.

## Vulkan to Metal Feature Parity Audit

This section details a comparison between the GZDoom Vulkan renderer and the nascent Metal renderer, highlighting core functionalities and architectural components present in Vulkan that are either missing or significantly different in Metal. This audit builds upon the understanding of the Vulkan architecture outlined above.

### Key Manager and Functional Differences:

| Vulkan Manager/Feature         | Metal Equivalent/Handling                                     | Status in Metal      | Notes                                                                                                                                                                                                                                                                  |
| :----------------------------- | :------------------------------------------------------------ | :------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `VkCommandBufferManager`       | `MtCommandBufferManager`                                      | **Present**          | Direct equivalent.                                                                                                                                                                                                                                                     |
| `VkSamplerManager`             | `MtSamplerManager`                                            | **Present**          | Direct equivalent.                                                                                                                                                                                                                                                     |
| `VkTextureManager`             | `MtTextureManager`                                            | **Present**          | Direct equivalent.                                                                                                                                                                                                                                                     |
| `VkBufferManager`              | `MtBufferManager`                                             | **Present**          | Direct equivalent.                                                                                                                                                                                                                                                     |
| `VkPostprocess`                | `MtPostprocess`                                               | **Present**          | Direct equivalent.                                                                                                                                                                                                                                                     |
| `VkShaderManager`              | `MtShaderManager`                                             | **Present**          | Direct equivalent, now with corrected fragment shader handling and initialization order.                                                                                                                                                                             |
| `VkRenderState`                | `MtRenderState`                                               | **Present**          | Direct equivalent.                                                                                                                                                                                                                                                     |
| `VkRenderBuffers`              | `MtRenderBuffers`                                             | **Present**          | Handles similar framebuffer-like concepts for screen/save.                                                                                                                                                                                                             |
| `VkDescriptorSetManager`       | `MtResourceBindingManager`                                    | **Present (Equivalent)** | Metal uses resource binding managers for descriptor set-like functionality. Requires validation to ensure equivalent flexibility and efficiency.                                                                                                                       |
| `VkRenderPassManager`          | `MtPipelineStateManager`                                      | **Present (Equivalent)** | Metal's pipeline states encapsulate render pass information directly. `MtPipelineStateManager` appears to handle render pass equivalent configuration within pipeline state creation. Requires validation for complex render pass graphs and optimal load/store operations. |
| `VkFramebufferManager`         | *None directly* (`MtRenderBuffers`/`MtRenderState` partially) | **Missing/Different**| Vulkan explicitly manages `VkFramebuffer` objects. Metal relies on `MtRenderBuffers` for backing textures and `MtRenderState::SetRenderTarget` for direct texture attachments. Need to ensure full coverage of Vulkan framebuffer functionalities.                            |
| `VkRaytrace`                   | *None*                                                        | **Missing**          | Vulkan has a dedicated manager for ray tracing. This functionality is currently absent in the Metal backend.                                                                                                                                                           |
| `ZVulkan` Abstraction Layer    | *None* (direct `metal-cpp` usage)                             | **Missing (Design Choice)** | Metal backend directly uses `metal-cpp`. High-level abstractions and utilities provided by `ZVulkan` for Vulkan are not present in a dedicated `ZMetal` library and must be managed directly within Metal managers.                                                     |

### Summary of Key Gaps and Future Work:

Based on the parity audit, the primary areas requiring attention for the Metal renderer to achieve functional equivalence with the Vulkan backend are:

1.  **Ray Tracing Implementation:** This is the most significant missing feature. Implementing ray tracing in Metal would involve leveraging Metal Performance Shaders Ray Tracing or a similar API, requiring substantial development.
2.  **Comprehensive Framebuffer Management:** While `MtRenderBuffers` and `MtRenderState` handle render targets, a deeper dive is needed to ensure all aspects of `VkFramebufferManager` (e.g., render pass compatibility, attachment management, efficient memory allocation for framebuffers) are adequately covered or planned for.
3.  **Advanced Render Pass Handling:** Validate that `MtPipelineStateManager` and `MtRenderState` can effectively manage complex render pass equivalent state transitions, attachment configurations, and performance optimizations (like explicit load/store actions) that `VkRenderPassManager` provides.
4.  **Resource Binding Robustness:** Confirm that `MtResourceBindingManager` provides the necessary flexibility, efficiency, and resource management capabilities equivalent to `VkDescriptorSetManager`, especially for dynamic updates, complex binding patterns, and pipeline layout considerations.
5.  **Abstraction Layer Parity:** While a design choice, the absence of a `ZVulkan`-like layer for Metal means that common utilities and helper functions present in `ZVulkan` for Vulkan will need to be implemented or carefully managed within the Metal backend's managers.

This audit provides a roadmap for future development, prioritizing the implementation of ray tracing and ensuring robust framebuffer and render pass management.

## Latest Progress (December 28, 2025)

### Metal Renderer Stabilization & Screen Wipe Investigation:

1.  **Culling Correction:** Fixed a persistent culling inversion where front faces were being culled instead of back faces. This was resolved by setting `FrontFacingWinding` to `Clockwise` in `MtRenderState::BeginRenderPass` to correctly account for the Y-flipped coordinate system used in Metal.
2.  **Robust Shader Patching:** Replaced brittle literal string matching with a regex-based approach in `MtShaderManager::PatchVertexShader`. This ensures that all assignments to `gl_Position` are correctly identified and patched with the necessary Y-flip and Z-mapping ([ -1, 1 ] to [ 0, 1 ]), regardless of whitespace or formatting variations in the GLSL source.
3.  **Wipe Capture Diagnostics:** Added extensive diagnostic logging to `WipeStartScreen`, `WipeEndScreen`, and `BlitCurrentToImage`. This allows for real-time monitoring of texture formats, capture methods (fast path vs. format conversion), and potential sequencing issues during screen wipes when `mt_debug` is enabled.
4.  **Wipe Texture Preservation:** Fixed a critical bug in `MtHardwareTexture::CreateImage` where it would always recreate the Metal texture, even if one already existed. This was causing captured screen data during wipes to be overwritten by an empty buffer. Added an idempotency check to reuse existing textures if they match the requested dimensions and format.
5.  **Coordinate System Synchronization:** Implemented a consistent Y-flip in the vertex shader for all passes to align rendering with Metal's Y-down framebuffer. Standard Y-down projections (0 at top) are used for 2D.
6.  **Orientation Correction:** Restored the vertical flip in `DrawPresentTexture` (Scale {1, -1}, Offset {0, 1}) to correctly orient the upside-down `PipelineImage` for the swapchain.
7.  **Wipe Orientation:** Confirmed `RenderTextureIsFlipped()` returns `true`, allowing engine-level wipe logic to correctly handle the inverted internal textures.
8.  **Dynamic Texture Support:** Implemented `mNeedsUpload` flag and persistent staging buffer in `MtHardwareTexture` to ensure reliable GPU updates for dynamic content.
9.  **Enhanced Diagnostics:** Added `mDebugName` and detailed logging across texture creation and binding paths.
10. **Build Verification:** Confirmed that the renderer builds successfully with these orientation and stability fixes.

## Latest Progress (December 28, 2025 - Evening)

### Breakthrough: Palette Texture Support & Texture Loading Fixes:

1.  **Palette Support Implemented:** Successfully added `GetPaletteTexture` management to `MtTextureManager` and registered the necessary callback in `MetalRenderDevice`. This allows the Metal backend to correctly handle indexed (paletted) textures, which has resolved major issues with "popping geometry" caused by failed texture loads.
2.  **Color Accuracy:** Corrected a red/blue channel swap in the 3-channel (RGB) to 4-channel (BGRA) conversion loop. Colors are now accurately represented in the Metal backend.
3.  **Refined Vertex Mapping:** Improved the fallback vertex descriptor to dynamically adjust the color attribute offset based on the vertex stride (24 bytes for 2D vs 32 bytes for 3D).
4.  **Wipe Stability:** Confirmed that screen wipes are now fully functional and correctly oriented.
5.  **Mipmap Generation:** Implemented safe, asynchronous hardware mipmap generation using a dedicated command buffer. This eliminates "flashing" artifacts by ensuring the entire mip chain is synchronized with base level updates without interrupting the main render pass.
6.  **Blending Refinement:** Restored standard alpha blending for `STYLE_Normal` to ensure correct transparency for masked sprites and avoid black backgrounds caused by aggressive opaque-copy optimizations.
7.  **Final Synchronization:** Verified perfect alignment of `PushConstants` and `StreamData` between C++ and GLSL, ensuring stable lighting and animation timer performance.

## Architectural Guidelines for Metal (Intel vs. Apple Silicon)

Optimizing for both Intel (x86_64) and Apple Silicon (aarch64) requires managing Immediate Mode Rendering (IMR) vs. Tile-Based Deferred Rendering (TBDR).

### 1. Memory Storage Strategy
*   **Unified Memory (Apple Silicon):** Use `MTL::StorageModeShared` for CPU-updated resources. Use `MTL::StorageModeMemoryless` for transient render targets (depth/stencil).
*   **Managed Memory (Intel):** Use `MTL::StorageModeManaged` for buffers written by CPU (backwards compatibility for 10.13). **Crucial:** Always call `didModifyRange:` after CPU writes.
*   **Private Memory:** Use `MTL::StorageModePrivate` for GPU-only data (textures, vertex buffers that don't change).

### 2. TBDR Optimization (Apple Silicon)
*   **Load Actions:** Use `.clear` or `.dontCare`. Avoid `.load` unless necessary.
*   **Store Actions:** Use `.dontCare` for depth/stencil and intermediate G-buffers to avoid wasting bandwidth writing back to RAM.

### 3. C++17 Runtime Capability Detection (`GPUContext`)

```cpp
struct GPUContext {
    MTL::Device* device;
    bool isTBDR;               // True for Apple Silicon / TBDR
    bool supportsMemoryless;   // True for A11+ / M1+
    MTL::StorageMode sharedMode;
    
    GPUContext(MTL::Device* pDevice) : device(pDevice) {
        if (pDevice->supportsFamily(MTL::GPUFamilyApple7)) {
            isTBDR = true;
            supportsMemoryless = true;
            sharedMode = MTL::StorageModeShared;
        } else {
            isTBDR = false;
            supportsMemoryless = false;
            sharedMode = MTL::StorageModeManaged; 
        }
    }
};
```

### 4. Universal Resource Allocation Example

```cpp
MTL::Texture* CreateDepthTexture(const GPUContext& ctx, int width, int height) {
    auto desc = MTL::TextureDescriptor::alloc()->init();
    desc->setPixelFormat(MTL::PixelFormatDepth32Float);
    desc->setWidth(width);
    desc->setHeight(height);
    desc->setUsage(MTL::TextureUsageRenderTarget);
    
    if (ctx.supportsMemoryless) {
        desc->setStorageMode(MTL::StorageModeMemoryless);
    } else {
        desc->setStorageMode(MTL::StorageModePrivate);
    }
    
    MTL::Texture* pTex = ctx.device->newTexture(desc);
    desc->release();
    return pTex;
}
```

### 5. Universal Render Pass Example

```cpp
MTL::RenderPassDescriptor* CreateRenderPass(const GPUContext& ctx, 
                                            MTL::Texture* pColorTex, 
                                            MTL::Texture* pDepthTex) {
    auto pPass = MTL::RenderPassDescriptor::renderPassDescriptor();
    
    auto pColorAtt = pPass->colorAttachments()->object(0);
    pColorAtt->setTexture(pColorTex);
    pColorAtt->setLoadAction(MTL::LoadActionClear);
    pColorAtt->setStoreAction(MTL::StoreActionStore);
    
    auto pDepthAtt = pPass->depthAttachment();
    pDepthAtt->setTexture(pDepthTex);
    pDepthAtt->setLoadAction(MTL::LoadActionClear);
    
    if (ctx.isTBDR) {
        pDepthAtt->setStoreAction(MTL::StoreActionDontCare);
    } else {
        pDepthAtt->setStoreAction(MTL::StoreActionDontCare); // Prefer DontCare if not read back
    }
    
    return pPass;
}
```