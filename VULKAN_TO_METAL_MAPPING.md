# Vulkan to Metal API Mapping Guide
## For GZDoom Metal Renderer Implementation

Based on comprehensive Vulkan renderer architecture analysis.

---

## Core Architectural Patterns (PRESERVE)

These patterns work well and should be kept in the Metal renderer:

### ✅ 1. Manager Factory Pattern
**Vulkan:**
```cpp
VulkanRenderDevice (owns everything)
├── VkRenderState
├── VkCommandBufferManager
├── VkShaderManager
├── VkBufferManager
└── ...10 managers total
```

**Metal:** Keep the same!
```cpp
MetalRenderDevice (owns everything)
├── MtRenderState
├── MtCommandBufferManager
├── MtShaderManager
├── MtBufferManager
└── ...same structure
```

### ✅ 2. Lazy State Evaluation (Apply Pattern)
**Vulkan:** VkRenderState::Apply() - Batches state changes
**Metal:** Keep this! Metal benefits from batching too

### ✅ 3. Ring Buffers
**Vulkan:** Circular allocation for dynamic data
**Metal:** Keep - avoids GPU stalls, works great with Metal

### ✅ 4. Three-Tier Resource Binding
**Vulkan:**
- Set 0: Fixed (shadowmap, lightmap)
- Set 1: Per-frame (UBOs with dynamic offsets)
- Set 2: Per-material (textures)

**Metal:** Adapt to argument buffers/resource heaps

### ✅ 5. Pipeline Caching
**Vulkan:** Lazy compile + disk cache
**Metal:** Keep - Metal also has slow pipeline compilation

### ✅ 6. Deferred Deletion
**Vulkan:** Delete resources after GPU finishes
**Metal:** Keep - use completion handlers

---

## API Translation Table

### Device & Queue

| Vulkan | Metal | Notes |
|--------|-------|-------|
| `VkDevice` | `id<MTLDevice>` | Core device object |
| `VkQueue` | `id<MTLCommandQueue>` | Work submission |
| `VkPhysicalDevice` | `MTLCopyAllDevices()` | Device enumeration |
| `vkGetDeviceQueue()` | Create via device | Queues created by device |

### Command Recording

| Vulkan | Metal | Notes |
|--------|-------|-------|
| `VkCommandBuffer` | `id<MTLCommandBuffer>` | **Major difference!** |
| `vkBeginCommandBuffer()` | `[queue commandBuffer]` | Simpler in Metal |
| `vkEndCommandBuffer()` | Implicit | Metal auto-completes |
| `VkCommandPool` | N/A | Metal doesn't need pools |
| `vkCmdBeginRenderPass()` | `[cmdBuffer renderCommandEncoderWithDescriptor:]` | |
| `vkCmdEndRenderPass()` | `[encoder endEncoding]` | |

**Key Difference:**
- Vulkan: Deferred recording → submit later
- Metal: Immediate recording → commit when done

### Render Passes & Pipelines

| Vulkan | Metal | Notes |
|--------|-------|-------|
| `VkRenderPass` | `MTLRenderPassDescriptor` | Metal is simpler |
| `VkFramebuffer` | Textures in pass descriptor | No separate object |
| `VkPipeline` | `id<MTLRenderPipelineState>` | Similar concept |
| `VkPipelineCache` | N/A | Metal caches automatically |
| `VkPipelineLayout` | Part of pipeline state | Simpler in Metal |

**Simplification:**
Metal combines render pass + framebuffer into one descriptor.

### Shaders

| Vulkan | Metal | Notes |
|--------|-------|-------|
| `VkShaderModule` | `id<MTLFunction>` | Metal functions |
| SPIR-V bytecode | MSL source or AIR | We translate via shader-translator! |
| `vkCreateShaderModule()` | `[library newFunctionWithName:]` | |
| `VkPipelineShaderStageCreateInfo` | `pipelineDescriptor.vertexFunction = ...` | |

**Our Pipeline:**
```
GLSL → glslang → SPIR-V → shader-translator → MSL → MTLLibrary → MTLFunction
```

### Descriptor Sets (Resource Binding)

| Vulkan | Metal | Notes |
|--------|-------|-------|
| `VkDescriptorSet` | `id<MTLArgumentEncoder>` + buffers | Metal 2.0+ |
| `VkDescriptorSetLayout` | `MTLArgumentDescriptor` | |
| `VkDescriptorPool` | N/A | Not needed |
| `vkUpdateDescriptorSets()` | Set buffer bindings | Simpler |
| Dynamic offsets | `setVertexBufferOffset:atIndex:` | Built-in support |

**Vulkan Approach:**
```cpp
// Set 0: Textures
vkCmdBindDescriptorSets(cmd, BIND_GRAPHICS, layout, 0, 1, &textureSet, 0, nullptr);

// Set 1: UBOs with dynamic offsets
uint32_t offsets[] = {viewpointOffset, streamOffset};
vkCmdBindDescriptorSets(cmd, BIND_GRAPHICS, layout, 1, 1, &uboSet, 2, offsets);
```

**Metal Approach:**
```objc
// Textures
[encoder setFragmentTexture:shadowMap atIndex:0];
[encoder setFragmentTexture:lightMap atIndex:1];

// Buffers with dynamic offsets
[encoder setVertexBuffer:viewpointBuffer offset:viewpointOffset atIndex:0];
[encoder setVertexBuffer:streamBuffer offset:streamOffset atIndex:1];
```

### Buffers

| Vulkan | Metal | Notes |
|--------|-------|-------|
| `VkBuffer` | `id<MTLBuffer>` | Similar |
| `VkDeviceMemory` | N/A | Metal handles internally |
| `vkMapMemory()` | `buffer.contents` | Simpler in Metal |
| `vkFlushMappedMemoryRanges()` | `didModifyRange:` | For managed buffers |
| `VK_BUFFER_USAGE_VERTEX_BUFFER` | Just use as vertex | No explicit usage |

**Buffer Creation:**

Vulkan:
```cpp
VkBufferCreateInfo info = {};
info.size = size;
info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
vkCreateBuffer(device, &info, nullptr, &buffer);

VkMemoryAllocateInfo allocInfo = {};
// ...bind memory separately
```

Metal:
```objc
id<MTLBuffer> buffer = [device newBufferWithLength:size
                                            options:MTLResourceStorageModeShared];
// Done! Memory allocation is automatic
```

### Textures & Images

| Vulkan | Metal | Notes |
|--------|-------|-------|
| `VkImage` | `id<MTLTexture>` | Similar |
| `VkImageView` | Texture itself or texture view | Simpler |
| `VkSampler` | `id<MTLSamplerState>` | Similar |
| `VkImageLayout` | N/A | Metal handles automatically |
| Layout transitions | N/A | Not needed! |

**Major Simplification:**
Metal doesn't have explicit layout transitions!

### Synchronization

| Vulkan | Metal | Notes |
|--------|-------|-------|
| `VkFence` | `[cmdBuffer waitUntilCompleted]` | or completion handler |
| `VkSemaphore` | `[cmdBuffer addCompletedHandler:]` | Callback-based |
| `vkQueueWaitIdle()` | `[queue waitUntilIdle]` | Rare usage |
| `VkEvent` | N/A | Not needed |
| Pipeline barriers | `memoryBarrier:` | Simpler |

**Vulkan Approach:**
```cpp
vkQueueSubmit(queue, 1, &submitInfo, fence);
vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
```

**Metal Approach:**
```objc
[cmdBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
    // Cleanup completed work
}];
[cmdBuffer commit];
```

---

## Class-by-Class Mapping

### VulkanRenderDevice → MetalRenderDevice

| Vulkan Method | Metal Equivalent | Notes |
|---------------|------------------|-------|
| `device->device` | `device` (`id<MTLDevice>`) | Core device |
| `device->graphicsQueue` | `commandQueue` (`id<MTLCommandQueue>`) | |
| `InitializeState()` | `InitializeState()` | Same pattern! |
| Manager creation order | Keep same order | Proven dependencies |

### VkRenderState → MtRenderState

| Vulkan Pattern | Metal Pattern | Notes |
|----------------|---------------|-------|
| Lazy Apply() | Keep Apply()! | Still beneficial |
| State tracking | Same state vars | Keep all |
| `BeginRenderPass()` | Create encoder | Different API |
| `Draw()` / `DrawIndexed()` | `drawPrimitives` / `drawIndexedPrimitives` | Similar |

### VkCommandBufferManager → MtCommandBufferManager

| Vulkan | Metal | Notes |
|--------|-------|-------|
| Transfer + Draw pools | Single queue | Simpler |
| `GetTransferCommands()` | `GetBlitCommandBuffer()` | For uploads |
| `GetDrawCommands()` | `GetRenderCommandBuffer()` | For rendering |
| `FlushCommands()` | `[cmdBuffer commit]` | Simpler |

### VkShaderManager → MtShaderManager

| Vulkan | Metal | Notes |
|--------|-------|-------|
| Load GLSL → Compile SPIR-V | Load GLSL → Compile SPIR-V | **Same!** |
| Create VkShaderModule | **SPIR-V → MSL → MTLLibrary** | **NEW: Use shader-translator!** |
| Shader variants | Same variants | Keep structure |
| Incremental compilation | Keep this! | Still useful |

**Metal Addition:**
```cpp
// After glslang compilation to SPIR-V
ShaderTranslator::SPIRVTranslator translator;
auto result = translator.TranslateToMSL(spirv, 20);

NSString* msl = [NSString stringWithUTF8String:result.source.c_str()];
id<MTLLibrary> library = [device newLibraryWithSource:msl options:nil error:&error];
id<MTLFunction> function = [library newFunctionWithName:@"main0"];
```

### VkBufferManager → MtBufferManager

Keep the ring buffer pattern! Metal benefits from this too.

### VkDescriptorSetManager → MtResourceBindingManager

**Vulkan's Three-Tier Strategy:**
```
Set 0 (Fixed): Shadowmap, Lightmap, RTX
Set 1 (Per-Frame): Viewpoint, Matrices, Stream, Lights, Bones
Set 2 (Per-Material): Texture layers
```

**Metal Adaptation:**
```
Tier 0 (Fixed Textures): Bind once at init
Tier 1 (Per-Frame Buffers): Update offsets per-frame
Tier 2 (Per-Material Textures): Bind per-material
```

---

## Push Constants

**Vulkan:**
```cpp
struct PushConstants {
    int uTextureMode;
    float uAlphaThreshold;
    vec2 uClipSplit;
    // ...32-64 bytes
};

vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                   0, sizeof(PushConstants), &mPushConstants);
```

**Metal:**
Push constants work via buffer binding:
```objc
// Option 1: Small inline buffer (< 4KB)
[encoder setVertexBytes:&pushConstants
                 length:sizeof(PushConstants)
                atIndex:10];

// Option 2: Offset into dynamic buffer
[encoder setFragmentBufferOffset:offset atIndex:10];
```

---

## Memory Management

### Vulkan (Complex)
```cpp
// Create buffer
vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);

// Allocate memory
VkMemoryRequirements memReq;
vkGetBufferMemoryRequirements(device, buffer, &memReq);
VkMemoryAllocateInfo allocInfo = { memReq.size, memoryTypeIndex };
vkAllocateMemory(device, &allocInfo, nullptr, &memory);

// Bind buffer to memory
vkBindBufferMemory(device, buffer, memory, 0);

// Map memory
void* data;
vkMapMemory(device, memory, 0, size, 0, &data);
memcpy(data, srcData, size);
vkUnmapMemory(device, memory);
```

### Metal (Simple!)
```objc
// Create + allocate + bind in one call
id<MTLBuffer> buffer = [device newBufferWithLength:size
                                           options:MTLResourceStorageModeShared];

// Map memory (always mapped!)
memcpy(buffer.contents, srcData, size);
[buffer didModifyRange:NSMakeRange(0, size)]; // Only if MTLStorageModeManaged
```

---

## VSync Implementation

### Vulkan
Uses swap chain presentation:
```cpp
VkPresentInfoKHR presentInfo = {};
presentInfo.swapchainCount = 1;
presentInfo.pSwapchains = &swapChain;
vkQueuePresentKHR(presentQueue, &presentInfo);
```

### Metal
Uses CAMetalLayer (as we discussed!):
```objc
CAMetalLayer* metalLayer = (CAMetalLayer*)view.layer;
metalLayer.displaySyncEnabled = vid_vsync ? YES : NO;

id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
[commandBuffer presentDrawable:drawable];
[commandBuffer commit];
```

---

## Performance Considerations

### Similarities (Keep Vulkan Patterns)
- ✅ Ring buffers for dynamic data
- ✅ Pipeline state caching
- ✅ Lazy state evaluation
- ✅ Deferred resource deletion
- ✅ Shader variant compilation

### Differences (Metal-Specific)
- ⚠️ **No explicit layout transitions** - Metal handles automatically (simpler!)
- ⚠️ **No descriptor pools** - Metal manages internally (simpler!)
- ⚠️ **Immediate command encoding** - Different paradigm from Vulkan
- ⚠️ **Automatic synchronization** - Metal is more permissive

---

## Implementation Strategy

### Phase 1: Core Infrastructure
1. Create `MetalRenderDevice` class
2. Create `id<MTLDevice>` and `id<MTLCommandQueue>`
3. Implement manager factory pattern (same as Vulkan)
4. Create `MtRenderState` with state tracking

### Phase 2: Command Encoding
1. Create `MtCommandBufferManager`
2. Implement `GetRenderCommandBuffer()`
3. Implement `GetBlitCommandBuffer()` for uploads
4. Add completion handlers for synchronization

### Phase 3: Shader Translation
1. Create `MtShaderManager`
2. Reuse Vulkan's GLSL preprocessing
3. Add SPIR-V → MSL step (use shader-translator!)
4. Compile MSL → MTLLibrary → MTLFunction

### Phase 4: Resource Management
1. Create `MtBufferManager` (ring buffers)
2. Create `MtTextureManager`
3. Create `MtSamplerManager`
4. Implement resource binding (three-tier strategy)

### Phase 5: Pipeline State
1. Create `MtRenderPassManager`
2. Implement `MTLRenderPipelineState` creation
3. Add pipeline caching
4. Implement lazy compilation

### Phase 6: Integration
1. Hook into `CocoaNativeHandle` (already have this!)
2. Implement `BeginFrame()` / `EndFrame()`
3. Connect to game loop
4. Test rendering

---

## Key Takeaways

### 1. Architecture: 90% The Same ✅
Keep the manager pattern, lazy evaluation, ring buffers, three-tier binding.

### 2. API Translation: Mostly 1:1 ✅
Most Vulkan concepts map directly to Metal equivalents.

### 3. Simplifications in Metal ✅
- No layout transitions
- No descriptor pools
- Automatic memory management
- Simpler render pass setup

### 4. Shader Pipeline: Already Solved ✅
Our shader-translator handles SPIR-V → MSL translation!

### 5. Integration: Already Prepared ✅
`CocoaNativeHandle` gives us NSWindow/NSView/CAMetalLayer access.

---

## Next Steps

1. ✅ **Study Complete** - Vulkan architecture understood
2. ✅ **Mapping Complete** - This document
3. **Create Metal Renderer Structure** - Next phase
4. **Implement Core Classes** - Follow Vulkan patterns
5. **Test with Simple Scene** - Validate architecture

**Ready to start implementation!** 🚀
