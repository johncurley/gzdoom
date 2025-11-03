# GZDoom Vulkan Renderer - Quick Reference

## Critical Files to Study

### Foundation Classes
1. **VulkanRenderDevice** (`src/common/rendering/vulkan/system/vk_renderdevice.h/cpp`)
   - Main orchestrator, owns all managers
   - Initialization: `InitializeState()`
   - Frame lifecycle: `BeginFrame()`, `Update()`

2. **VkRenderState** (`src/common/rendering/vulkan/renderer/vk_renderstate.h/cpp`)
   - State machine tracking all GPU state
   - Lazy evaluation: state changes queued, applied in `Apply()`
   - Critical methods: `Apply()`, `Draw()`, `DrawIndexed()`

3. **VkCommandBufferManager** (`src/common/rendering/vulkan/system/vk_commandbuffer.h/cpp`)
   - GPU work submission
   - Dual command buffer tracks: transfer + graphics
   - Synchronization: fences, semaphores, deferred deletion

### Rendering Pipeline
4. **VkRenderPassManager** (`src/common/rendering/vulkan/renderer/vk_renderpass.h/cpp`)
   - Pipeline creation & caching
   - Render pass management
   - Key concepts: `VkPipelineKey`, `VkRenderPassKey`

5. **VkDescriptorSetManager** (`src/common/rendering/vulkan/renderer/vk_descriptorset.h/cpp`)
   - Resource binding management
   - 3-level descriptor set strategy (Fixed, HWBuffer, Texture)
   - Dynamic offset management

### Resource Management
6. **VkBufferManager** (`src/common/rendering/vulkan/system/vk_buffer.h/cpp`)
   - Vertex, index, and data buffers
   - Ring buffers: MatrixBuffer, StreamBuffer
   - UBO, SSBO management

7. **VkShaderManager** (`src/common/rendering/vulkan/shaders/vk_shader.h/cpp`)
   - Incremental shader compilation
   - Material, effect, and post-process shaders
   - Shader lookup and caching

8. **VkHardwareTexture** (`src/common/rendering/vulkan/textures/vk_hwtexture.h`)
   - GPU texture representation
   - Material descriptor set caching

### Supporting Systems
9. **VkRenderBuffers** (`src/common/rendering/vulkan/textures/vk_renderbuffers.h`)
   - Scene buffers (color, depth, normal)
   - Pipeline images for post-processing

10. **VkPostprocess** (`src/common/rendering/vulkan/renderer/vk_postprocess.h`)
    - Post-processing effects
    - Final presentation

## Key Data Structures

### Pipeline Configuration
```cpp
// Identifies unique graphics pipeline
struct VkPipelineKey {
    FRenderStyle RenderStyle;
    int SpecialEffect, AlphaTest;
    int DepthWrite, DepthTest, DepthFunc;
    int StencilTest, ColorMask, CullMode;
    int VertexFormat, DrawType, NumTextureLayers;
};

// Identifies unique render pass
struct VkRenderPassKey {
    int DepthStencil, Samples, DrawBuffers;
    VkFormat DrawBufferFormat;
};
```

### Dynamic Data
```cpp
struct PushConstants {
    int uTextureMode;
    float uAlphaThreshold;
    FVector2 uClipSplit;
    float uLightLevel, uFogDensity, uLightFactor, uLightDist;
    int uFogEnabled;
    int uLightIndex;
    FVector2 uSpecularMaterial;
    int uBoneIndexBase;
    int uDataIndex;
};
```

## Execution Flow

### Initialization
```
VulkanRenderDevice::InitializeState()
├─ VkCommandBufferManager
├─ VkSamplerManager
├─ VkTextureManager
├─ VkFramebufferManager
├─ VkBufferManager
├─ VkRenderBuffers (screen + save)
├─ VkPostprocess
├─ VkDescriptorSetManager
├─ VkRenderPassManager
├─ VkRaytrace
└─ VkRenderState (or VkRenderStateMolten on macOS)
```

### Per-Frame
```
VulkanRenderDevice::Update()
├─ GetPostprocess()->SetActiveRenderTarget()
├─ Draw2D()
├─ mRenderState->EndRenderPass()
├─ mRenderState->EndFrame()
├─ mCommands->WaitForCommands(true)
└─ Super::Update() [present to screen]
```

### Per-Draw
```
VkRenderState::Draw()
├─ Apply(dt) [if needed]
│  ├─ ApplyStreamData()
│  ├─ ApplyMatrices()
│  ├─ ApplyRenderPass(dt)
│  ├─ ApplyScissor/Viewport/StencilRef/DepthBias/PushConstants()
│  ├─ ApplyVertexBuffers()
│  ├─ ApplyMaterial() [Bind texture descriptor set]
│  └─ ApplyHWBufferSet() [Bind uniform/storage buffers]
└─ mCommandBuffer->draw()
```

## Resource Binding Strategy

### Three-Tier Descriptor Layout
```
Pipeline Layout
├─ Set 0 (Fixed): Shadowmap, Lightmap, RTX acceleration structure
├─ Set 1 (HW Buffers): Viewpoint UBO, Matrix UBO, Stream UBO, Light SSBO, Bone SSBO
│  └─ Uses dynamic offsets for per-draw data
└─ Set 2 (Material): Texture layers (variable count)
```

### Dynamic Data via Ring Buffers
```
StreamBuffer (per-vertex data)
├─ Ring buffer pattern
├─ Allocation: VkStreamBufferWriter::Write()
└─ Passed to shader via Set 1 dynamic offset

MatrixBuffer (model/texture matrices)
├─ Ring buffer pattern
├─ Per-draw allocation
└─ Passed via Set 1 dynamic offset
```

## State Application Pattern (Lazy Evaluation)

State changes are recorded, applied only when necessary:

1. User calls `SetDepthMask(bool)`, `SetCullMode(int)`, etc.
2. Each setter marks `mNeedApply = true`
3. Next `Draw()` or `DrawIndexed()` call triggers `Apply(dt)`
4. `Apply()` batches all pending state changes
5. Reduces command buffer recording overhead

## Shader Management

### Three Shader Categories

1. **Material Shaders**
   - Base set: `mMaterialShaders[passType]`
   - Alpha test variant: `mMaterialShadersNAT[passType]`
   - User custom shaders

2. **Effect Shaders**
   - Fog boundary, sphere map, burn, stencil, dither
   - `mEffectShaders[passType]`

3. **Post-Process Shaders**
   - Managed as `VkPPShader` wrappers

### Compilation Flow
```
CompileNextShader()
├─ State 0: Material shaders (with alpha test)
├─ State 1: NAT (no alpha test) variants
├─ State 2: User custom shaders
└─ State 3: Effect shaders
```

## Synchronization & Submission

### Command Buffer Flushing
- Default: Every 1000 draw calls (vk_submit_size = 1000)
- Explicit: `GetCommands()->FlushCommands(bool finish)`
- Dual tracks: transfer (for uploads) + graphics (for rendering)

### GPU Synchronization
- Circular queue of 8 concurrent submits
- Fence per submit to track GPU completion
- Semaphore signaling between submits
- Deferred deletion lists per command buffer

### Frame Pacing
```
WaitForCommands(true)
├─ Blocks CPU until GPU finishes current frame
├─ Enables immediate deletion of GPU resources
└─ Called at end of VulkanRenderDevice::Update()
```

## Integration Points

### Platform Integration
- Base class: `SystemBaseFrameBuffer` (from `gl_sysfb.h`)
- macOS variant: `VkRenderStateMolten` for MoltenVK
- Window integration: VulkanSurface created from CocoaWindow

### Hardware Renderer Abstraction
- Implements: `FRenderState`, `IHardwareTexture`, `IVertexBuffer`, etc.
- Game engine calls abstract interfaces
- Vulkan implementation provides concrete classes

### Game Loop Integration
- Called from `v_video.cpp` rendering
- Material setup → `SetMaterial(FMaterial*)`
- Geometry submission → `SetVertexBuffer()`, `SetIndexBuffer()`
- Draw calls → `Draw()`, `DrawIndexed()`

## Performance Optimizations

1. **Batched Submissions**: Reduces submission overhead
2. **Dynamic Offsets**: Avoids descriptor set rebinding
3. **Pipeline Caching**: Persistent disk cache
4. **Lazy Pipeline Creation**: On first use
5. **Ring Buffers**: Circular allocation avoids stalls
6. **MSAA Support**: Configurable sample counts
7. **Deferred Deletion**: Prevents GPU stalls on resource destruction

## Metal Implementation Notes

### Class Mapping
- VulkanRenderDevice → MetalRenderDevice
- VkRenderState → MetRenderState
- VkCommandBufferManager → MetCommandBufferManager
- VkRenderPassManager → MetPipelineManager (MTLRenderPipelineState)
- VkDescriptorSetManager → MetArgumentBufferManager
- VkShaderManager → MetShaderManager

### API Differences
- Metal uses immediate encoding (vs. Vulkan's deferred)
- No explicit render passes (use MTLRenderPassDescriptor per operation)
- No descriptor sets (use MTLArgumentBuffer)
- No pipeline cache file (MTLPipelineState is already cached)

### Preservation Strategy
- Keep: Lazy state application pattern
- Keep: Ring buffer patterns for dynamic data
- Keep: Three-level resource binding strategy
- Adapt: Command encoding and resource binding syntax
