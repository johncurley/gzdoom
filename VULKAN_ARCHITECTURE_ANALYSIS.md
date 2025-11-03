# GZDoom Vulkan Renderer Architecture Analysis
## Blueprint for Metal Renderer Implementation

---

## 1. DIRECTORY STRUCTURE

The Vulkan renderer is organized hierarchically in `/src/common/rendering/vulkan/`:

```
vulkan/
├── system/              # Low-level GPU abstractions
│   ├── vk_renderdevice.h/cpp       # Main device, manager factory
│   ├── vk_commandbuffer.h/cpp      # Command buffer management
│   ├── vk_buffer.h/cpp             # Buffer management (vertex, index, data)
│   └── vk_hwbuffer.h/cpp           # Hardware buffer abstraction
│
├── renderer/            # Rendering pipeline & state
│   ├── vk_renderstate.h/cpp        # Render state machine
│   ├── vk_renderpass.h/cpp         # Render passes & pipelines
│   ├── vk_descriptorset.h/cpp      # Descriptor set management (resource binding)
│   ├── vk_streambuffer.h/cpp       # Stream buffer management (dynamic data)
│   ├── vk_postprocess.h/cpp        # Post-processing pipeline
│   └── vk_raytrace.h/cpp           # Ray tracing support
│
├── shaders/             # Shader compilation & management
│   ├── vk_shader.h/cpp             # VkShaderManager - shader compilation
│   └── vk_ppshader.h/cpp           # Post-process shader management
│
└── textures/            # Texture & framebuffer resources
    ├── vk_hwtexture.h/cpp          # Hardware texture abstraction
    ├── vk_texture.h/cpp            # Texture manager
    ├── vk_samplers.h/cpp           # Sampler management
    ├── vk_renderbuffers.h/cpp      # Render target buffers (color, depth, normal)
    ├── vk_framebuffer.h/cpp        # Framebuffer management
    └── vk_imagetransition.h/cpp    # Image layout transitions
```

---

## 2. CORE CLASSES & RESPONSIBILITIES

### 2.1 VulkanRenderDevice (Main Orchestrator)
**File:** `/src/common/rendering/vulkan/system/vk_renderdevice.h`
**Base Class:** `SystemBaseFrameBuffer` (platform-specific base from `gl_sysfb.h`)

**Role:** Central manager that owns and coordinates all Vulkan subsystems.

**Key Responsibilities:**
- Owns `VulkanDevice` (via `std::shared_ptr<VulkanDevice>`)
- Initialization: Creates all manager subsystems in `InitializeState()`
- Manages frame lifecycle: `BeginFrame()`, `Update()` 
- Factory for GPU resources: Hardware textures, materials, buffers
- Bridge between game engine and GPU

**Manager Accessors:**
```cpp
VkCommandBufferManager* GetCommands()
VkShaderManager* GetShaderManager()
VkSamplerManager* GetSamplerManager()
VkBufferManager* GetBufferManager()
VkTextureManager* GetTextureManager()
VkFramebufferManager* GetFramebufferManager()
VkDescriptorSetManager* GetDescriptorSetManager()
VkRenderPassManager* GetRenderPassManager()
VkRenderState* GetRenderState()
VkPostprocess* GetPostprocess()
VkRenderBuffers* GetBuffers()
```

**Initialization Order (InitializeState):**
1. Create `VkCommandBufferManager` - command pool and buffers
2. Create `VkSamplerManager` - sampler cache
3. Create `VkTextureManager` - texture resource management
4. Create `VkFramebufferManager` - framebuffer resource management
5. Create `VkBufferManager` - vertex/index/data buffer management
6. Create render buffers - `VkRenderBuffers` (screen + save buffers)
7. Create `VkPostprocess` - post-processing pipeline
8. Create `VkDescriptorSetManager` - resource descriptor sets
9. Create `VkRenderPassManager` - render passes and pipelines
10. Create `VkRaytrace` - ray tracing support (optional)
11. Create shader manager: `VkShaderManager`
12. Create render state: `VkRenderState` (or `VkRenderStateMolten` on macOS)

---

### 2.2 VkRenderState (State Machine & Draw Dispatcher)
**File:** `/src/common/rendering/vulkan/renderer/vk_renderstate.h`
**Base Class:** `FRenderState` (abstract render state interface)

**Special Variant:** `VkRenderStateMolten` - macOS-specific implementation for MoltenVK

**Role:** Captures and applies all GPU render state changes.

**Key Responsibilities:**
- State tracking: Maintains all mutable GPU state
- Lazy evaluation: Queues state changes, applies only when needed
- Render pass management: Creates/manages Vulkan render passes
- Draw command dispatch: Actual `vkCmdDraw*` calls
- Resource binding: Descriptor sets, buffers, textures

**State Categories Tracked:**
```cpp
// Depth/Stencil State
bool mDepthClamp, mDepthTest, mDepthWrite;
int mDepthFunc;
float mViewportDepthMin/Max;

// Stencil State
bool mStencilTest;
int mStencilRef, mStencilOp;

// Rasterization
int mCullMode;
int mColorMask;

// Dynamic State
int mScissor{X,Y,Width,Height};
int mViewport{X,Y,Width,Height};

// Pipeline State  
VkPipelineKey mPipelineKey;
VkRenderPassSetup *mPassSetup;

// Dynamic Buffer Data
PushConstants mPushConstants;
uint32_t mViewpointOffset;
VkStreamBufferWriter mStreamBufferWriter;
VkMatrixBufferWriter mMatrixBufferWriter;

// Render Target
RenderTarget mRenderTarget;
```

**Critical Methods:**

```cpp
// State setters (queue state for later application)
void SetDepthMask(bool on);
void SetDepthFunc(int func);
void SetDepthRange(float min, float max);
void SetStencil(int offs, int op, int flags);
void SetCulling(int mode);
void SetColorMask(bool r, bool g, bool b, bool a);
void SetScissor(int x, int y, int w, int h);
void SetViewport(int x, int y, int w, int h);
void SetRenderTarget(VkTextureImage*, VulkanImageView*, int w, int h, VkFormat, VkSampleCountFlagBits);

// Draw dispatch
void Draw(int dt, int index, int count, bool apply = true);
void DrawIndexed(int dt, int index, int count, bool apply = true);

// Lifecycle
void BeginFrame();
void EndFrame();
void BeginRenderPass(VulkanCommandBuffer*);
void EndRenderPass();

// Resource binding
void Bind(int bindingpoint, uint32_t offset);  // Update dynamic buffer offsets
```

**Apply Pipeline (Apply() method - ~200 lines):**
```cpp
void Apply(int dt) {
    ApplyStreamData();      // Update per-vertex stream data
    ApplyMatrices();        // Update model/texture matrices
    ApplyRenderPass(dt);    // Create/switch render passes
    ApplyScissor();
    ApplyViewport();
    ApplyStencilRef();
    ApplyDepthBias();
    ApplyPushConstants();   // Push constant updates
    ApplyVertexBuffers();   // Bind vertex/index buffers
    ApplyMaterial();        // Bind texture descriptor sets
    ApplyHWBufferSet();     // Bind uniform/storage buffer descriptor sets
}
```

---

### 2.3 VkCommandBufferManager (GPU Work Submission)
**File:** `/src/common/rendering/vulkan/system/vk_commandbuffer.h`

**Role:** Manages command buffer recording and submission to GPU queue.

**Key Responsibilities:**
- Command pool management
- Dual command buffer tracks: transfer + graphics
- Synchronization with fences/semaphores
- Frame pacing and GPU wait
- Deferred resource deletion (frame-based cleanup)

**Key Methods:**
```cpp
void BeginFrame();
VulkanCommandBuffer* GetTransferCommands();   // For GPU uploads
VulkanCommandBuffer* GetDrawCommands();       // For rendering
void FlushCommands(bool finish, bool lastsubmit = false);
void WaitForCommands(bool finish, bool uploadOnly = false);
void DeleteFrameObjects(bool uploadOnly = false);
```

**Synchronization Strategy:**
- Circular queue of 8 concurrent submits (ringbuffer pattern)
- Fence per submit to track GPU completion
- Semaphore signaling between submits for ordering
- Deferred deletion lists: `TransferDeleteList`, `DrawDeleteList`
- Resources queued for deletion are freed after GPU finishes

**DeleteList Pattern:**
Contains vectors for each resource type:
- Buffers, Images, ImageViews, Framebuffers, Samplers
- Descriptor sets, Descriptor pools
- Shaders, Command buffers
- Tracks total size for memory management

---

### 2.4 VkRenderPassManager (Pipeline Configuration)
**File:** `/src/common/rendering/vulkan/renderer/vk_renderpass.h`

**Role:** Manages render pass objects and graphics pipelines.

**Key Concepts:**

**VkRenderPassKey:** Identifies unique render pass configurations
```cpp
struct VkRenderPassKey {
    int DepthStencil;        // Has depth/stencil attachment?
    int Samples;             // MSAA sample count
    int DrawBuffers;         // Number of color attachments
    VkFormat DrawBufferFormat;
};
```

**VkPipelineKey:** Identifies unique graphics pipeline configurations
```cpp
struct VkPipelineKey {
    FRenderStyle RenderStyle;        // Blend mode, write masks
    int SpecialEffect;               // Fog boundary, spheremap, etc.
    int AlphaTest;                   // Alpha test enabled?
    int DepthWrite, DepthTest;
    int DepthFunc;                   // Less, LEqual, Always
    int DepthClamp;
    int DepthBias;
    int StencilTest, StencilPassOp;
    int ColorMask;
    int CullMode;                    // CCW, CW, None
    int VertexFormat;                // Vertex attribute layout
    int DrawType;                    // Points, Lines, Triangles, etc.
    int NumTextureLayers;            // Descriptor set size
};
```

**VkRenderPassSetup:** Container for a specific render pass + all its pipelines
```cpp
class VkRenderPassSetup {
    VkRenderPassKey PassKey;
    std::unique_ptr<VulkanRenderPass> RenderPasses[8];  // 8 clear target combinations
    std::map<VkPipelineKey, std::unique_ptr<VulkanPipeline>> Pipelines;
    
    VulkanRenderPass* GetRenderPass(int clearTargets);
    VulkanPipeline* GetPipeline(const VkPipelineKey& key);
};
```

**Key Methods:**
```cpp
VkRenderPassSetup* GetRenderPass(const VkRenderPassKey& key);
VulkanPipeline* GetPipeline(const VkPipelineKey& key);  // Cached lookup
int GetVertexFormat(int numBindingPoints, int numAttributes, 
                   size_t stride, const FVertexBufferAttribute* attrs);
VulkanPipelineLayout* GetPipelineLayout(int numLayers);
```

**Pipeline Caching:**
- Uses Vulkan pipeline cache to avoid recompilation
- Cache file: Persistent disk storage across runs
- Lazy pipeline creation on first use

---

### 2.5 VkShaderManager (Shader Compilation)
**File:** `/src/common/rendering/vulkan/shaders/vk_shader.h`

**Role:** Manages shader compilation, loading, and caching.

**Shader Organization:**

Three categories of shaders:
1. **Material Shaders** - Game geometry rendering
   - Base set from `defaultshaders[]` table
   - "With Alpha Test" (NAT) variant for transparency
   - User custom shaders

2. **Effect Shaders** - Special rendering effects
   - Fog boundary, sphere map, burn, stencil, dither

3. **Post-Process Shaders** - Post-processing effects
   - Managed by `VkPPShader` wrapper

**Key Members:**
```cpp
class VkShaderManager {
    std::vector<VkShaderProgram> mMaterialShaders[MAX_PASS_TYPES];
    std::vector<VkShaderProgram> mMaterialShadersNAT[MAX_PASS_TYPES];
    std::vector<VkShaderProgram> mEffectShaders[MAX_PASS_TYPES];
    
    uint8_t compilePass = 0, compileState = 0;  // Incremental compilation state
    int compileIndex = 0;
    
    std::list<VkPPShader*> PPShaders;  // Post-process shaders
};
```

**VkShaderProgram:**
```cpp
struct VkShaderProgram {
    std::unique_ptr<VulkanShader> vert;
    std::unique_ptr<VulkanShader> frag;
};
```

**Compilation Flow (CompileNextShader):**
Incremental multi-state compilation:
1. **State 0:** Compile material shaders (with alpha test)
2. **State 1:** Compile NAT (no alpha test) variants
3. **State 2:** Compile user custom shaders
4. **State 3:** Compile effect shaders

Multiple pass types: `NORMAL_PASS`, `GBUFFER_PASS`, etc.

**Key Methods:**
```cpp
VkShaderProgram* GetEffect(int effect, EPassType passType);
VkShaderProgram* Get(unsigned int eff, bool alphateston, EPassType passType);
bool CompileNextShader();  // Called progressively during loading

VkPPShader* GetVkShader(PPShader* shader);
void AddVkPPShader(VkPPShader* shader);
void RemoveVkPPShader(VkPPShader* shader);
```

**Shader Source Loading:**
```cpp
std::unique_ptr<VulkanShader> LoadVertShader(
    FString shadername, const char *vert_lump, const char *defines);
    
std::unique_ptr<VulkanShader> LoadFragShader(
    FString shadername, const char *frag_lump, 
    const char *material_lump, const char *light_lump,
    const char *defines, bool alphatest, bool gbufferpass);
```

---

### 2.6 VkDescriptorSetManager (Resource Binding)
**File:** `/src/common/rendering/vulkan/renderer/vk_descriptorset.h`

**Role:** Manages Vulkan descriptor sets for resource binding to shaders.

**Descriptor Set Layouts (Fixed):**

1. **Fixed Set (Set 0)** - Rarely changed resources
   - Shadowmap (sampled image)
   - Lightmap (sampled image)
   - Raytracing acceleration structure (optional)

2. **HWBuffer Set (Set 1)** - Dynamic uniform/storage buffers
   - Viewpoint UBO (camera/lighting uniforms)
   - Matrix buffer UBO (model/texture matrices)
   - Stream buffer UBO (per-vertex data)
   - Light buffer SSBO
   - Bone buffer SSBO

3. **Texture Set (Set 2)** - Material textures (variable size)
   - Multiple texture layers (variable from SHADER_MIN_REQUIRED_TEXTURE_LAYERS=11 max)

**Key Members:**
```cpp
class VkDescriptorSetManager {
    std::unique_ptr<VulkanDescriptorSetLayout> HWBufferSetLayout;
    std::unique_ptr<VulkanDescriptorSetLayout> FixedSetLayout;
    std::vector<std::unique_ptr<VulkanDescriptorSetLayout>> TextureSetLayouts;
    
    std::unique_ptr<VulkanDescriptorPool> HWBufferDescriptorPool;
    std::unique_ptr<VulkanDescriptorPool> FixedDescriptorPool;
    std::unique_ptr<VulkanDescriptorPool> PPDescriptorPool;
    std::vector<std::unique_ptr<VulkanDescriptorPool>> TextureDescriptorPools;
    
    std::unique_ptr<VulkanDescriptorSet> HWBufferSet;
    std::unique_ptr<VulkanDescriptorSet> FixedSet;
    std::unique_ptr<VulkanDescriptorSet> NullTextureDescriptorSet;
    
    std::list<VkMaterial*> Materials;
};
```

**Key Methods:**
```cpp
void Init();
void Deinit();
void BeginFrame();
void UpdateFixedSet();        // Update shadowmap/lightmap
void UpdateHWBufferSet();     // Update dynamic buffers
void ResetHWTextureSets();    // Invalidate material texture descriptors

VulkanDescriptorSet* GetHWBufferDescriptorSet();
VulkanDescriptorSet* GetFixedDescriptorSet();
VulkanDescriptorSet* GetNullTextureDescriptorSet();
std::unique_ptr<VulkanDescriptorSet> AllocateTextureDescriptorSet(int numLayers);
VulkanDescriptorSetLayout* GetTextureSetLayout(int numLayers);
```

**Material Integration:**
- Each `VkMaterial` manages its own texture descriptor sets
- Descriptor sets are keyed by: clamp mode + remap translation
- Pools use lazy allocation with backpressure: wait for GPU if full

---

### 2.7 VkBufferManager (Memory Management)
**File:** `/src/common/rendering/vulkan/system/vk_buffer.h`

**Role:** Creates and manages vertex buffers, index buffers, and data buffers.

**Key Members:**
```cpp
class VkBufferManager {
    VkHardwareDataBuffer* ViewpointUBO;         // Camera uniforms
    VkHardwareDataBuffer* LightBufferSSO;       // Light data storage
    VkHardwareDataBuffer* LightNodes;           // Light spatial structure
    VkHardwareDataBuffer* LightLines;           // Light connected data
    VkHardwareDataBuffer* LightList;            // Light list array
    VkHardwareDataBuffer* BoneBufferSSO;        // Skeletal animation data
    
    std::unique_ptr<VkStreamBuffer> MatrixBuffer;
    std::unique_ptr<VkStreamBuffer> StreamBuffer;
    
    std::unique_ptr<IIndexBuffer> FanToTrisIndexBuffer;  // Triangle fan conversion
};
```

**VkStreamBuffer (Ring Buffer for Dynamic Data):**
```cpp
class VkStreamBuffer {
    VkHardwareDataBuffer* UniformBuffer;  // Underlying GPU buffer
    uint32_t mBlockSize;
    uint32_t mStreamDataOffset;  // Current write position
    
    uint32_t NextStreamDataBlock();  // Allocate next block
    void Reset();                     // Reset to beginning
};
```

---

### 2.8 VkHardwareTexture (Texture Resources)
**File:** `/src/common/rendering/vulkan/textures/vk_hwtexture.h`

**Role:** GPU-side texture representation.

**Key Members:**
```cpp
class VkHardwareTexture : public IHardwareTexture {
    VkTextureImage mImage;          // Main texture image
    VkTextureImage mDepthStencil;   // Optional depth/stencil
    uint8_t* mappedSWFB;            // Software framebuffer mapping
    int mTexelsize = 4;
};

class VkMaterial : public FMaterial {
    struct DescriptorEntry {
        int clampmode;
        intptr_t remap;
        std::unique_ptr<VulkanDescriptorSet> descriptor;
    };
    std::vector<DescriptorEntry> mDescriptorSets;  // Cached descriptor sets
};
```

**VkTextureImage (Image wrapper):**
```cpp
struct VkTextureImage {
    std::unique_ptr<VulkanImage> Image;           // VkImage
    std::unique_ptr<VulkanImageView> View;        // Default VkImageView
    VkImageLayout Layout;                         // Current layout
    std::map<VkRenderPassKey, std::unique_ptr<VulkanFramebuffer>> RSFramebuffers;
};
```

---

### 2.9 VkRenderBuffers (Render Targets)
**File:** `/src/common/rendering/vulkan/textures/vk_renderbuffers.h`

**Role:** Manages main render target images and intermediate pipeline images.

**Scene Buffers:**
```cpp
VkTextureImage SceneColor;           // HDR scene rendering (R16G16B16A16_SFLOAT)
VkTextureImage SceneDepthStencil;    // Depth/stencil (D24_UNORM_S8_UINT)
VkTextureImage SceneNormal;          // G-buffer normal (A2R10G10B10_UNORM)
VkTextureImage SceneFog;             // Fog data
```

**Pipeline Images (Post-processing chain):**
```cpp
VkTextureImage PipelineDepthStencil;
VkTextureImage PipelineImage[2];     // Double-buffered for ping-pong processing
```

**Methods:**
```cpp
void BeginFrame(int width, int height, int sceneWidth, int sceneHeight);
int GetWidth(), GetHeight(), GetSceneWidth(), GetSceneHeight();
VkSampleCountFlagBits GetSceneSamples();
VulkanFramebuffer* GetOutput(VkPPRenderPassSetup*, const PPOutput&, WhichDepthStencil, int& w, int& h);
```

---

### 2.10 VkPostprocess (Post-Processing Pipeline)
**File:** `/src/common/rendering/vulkan/renderer/vk_postprocess.h`

**Role:** Manages post-processing effects and final presentation.

**Key Methods:**
```cpp
void SetActiveRenderTarget();           // Switch to postprocess render target
void PostProcessScene(int fixedcm, float flash, 
                     const std::function<void()>& afterBloomDrawEndScene2D);
void AmbientOccludeScene(float m5);
void BlurScene(float gameinfobluramount);
void ClearTonemapPalette();
void UpdateShadowMap();
void ImageTransitionScene(bool undefinedSrcLayout);
void BlitSceneToPostprocess();
void BlitCurrentToImage(VkTextureImage*, VkImageLayout finalLayout);
void DrawPresentTexture(const IntRect& box, bool applyGamma, bool screenshot);

int GetCurrentPipelineImage();  // Ping-pong tracking
```

---

## 3. INITIALIZATION FLOW

### Sequence Diagram: VulkanRenderDevice Initialization

```
VulkanRenderDevice Constructor
    ↓
Create VulkanDevice (via VulkanDeviceBuilder)
    ↓ (in InitializeState())
├─ Create VkCommandBufferManager
│  └─ Create command pool + semaphores/fences
├─ Create VkSamplerManager
│  └─ Create sampler cache for filtering modes
├─ Create VkTextureManager
├─ Create VkFramebufferManager
├─ Create VkBufferManager
│  ├─ Create ViewpointUBO
│  ├─ Create LightBufferSSO, BoneBufferSSO
│  ├─ Create MatrixBuffer (ring buffer)
│  ├─ Create StreamBuffer (ring buffer)
│  └─ Create FanToTrisIndexBuffer
├─ Create VkRenderBuffers (screen buffers)
│  └─ Allocate scene color/depth/normal images
├─ Create VkRenderBuffers (save buffers)
├─ Create VkPostprocess
├─ Create VkDescriptorSetManager
│  ├─ Create HWBuffer descriptor layout
│  ├─ Create Fixed descriptor layout
│  ├─ Create pools
│  └─ Create initial null texture descriptor set
├─ Create VkRenderPassManager
│  └─ Load/create pipeline cache
├─ Create VkRaytrace
├─ Create vertex/light/bone data buffers (CPU-side)
├─ Create VkShaderManager
└─ Create VkRenderState (or VkRenderStateMolten on macOS)
```

### Key Points:
- Order matters: Managers referenced by subsequent managers
- Buffer manager must initialize before shader manager
- Descriptor sets depend on buffer manager
- All managers store `VulkanRenderDevice* fb` back-reference

---

## 4. RENDERING PIPELINE

### Per-Frame Flow: VulkanRenderDevice::Update()

```cpp
void VulkanRenderDevice::Update() {
    twoD.Reset();                      // Reset 2D drawing state
    Flush3D.Reset();                   // Reset 3D timer
    Flush3D.Clock();                   // Start timing 3D rendering
    
    // Switch to post-process target
    GetPostprocess()->SetActiveRenderTarget();
    
    // Render 2D elements (HUD, etc.)
    Draw2D();
    twod->Clear();
    
    // Finalize GPU work
    mRenderState->EndRenderPass();     // End any active render pass
    mRenderState->EndFrame();          // Reset per-frame state
    
    Flush3D.Unclock();
    
    // GPU sync and presentation
    mCommands->WaitForCommands(true);  // Block until GPU finishes
    mCommands->UpdateGpuStats();       // Gather timing info
    
    Super::Update();                   // Platform-specific update (present to screen)
}
```

### Per-Draw Call Flow: VkRenderState::Draw()

```cpp
void Draw(int dt, int index, int count, bool apply = true) {
    if (apply || mNeedApply)
        Apply(dt);  // Lazy state application
    
    mCommandBuffer->draw(count, 1, index, 0);
}
```

### State Application Flow: VkRenderState::Apply()

```cpp
void Apply(int dt) {
    // Periodic GPU flush (batched submissions)
    mApplyCount++;
    if (mApplyCount >= vk_submit_size)  // Default: 1000 draw calls
        fb->GetCommands()->FlushCommands(false);
    
    // Data updates
    ApplyStreamData();              // Per-vertex dynamic data
    ApplyMatrices();                // Model/texture matrices
    
    // Pipeline/render pass setup
    ApplyRenderPass(dt);            // Create/switch render pass & pipeline
    
    // Rasterization state
    ApplyScissor();                 // Scissor rectangle
    ApplyViewport();                // Viewport rectangle
    ApplyStencilRef();              // Stencil reference value
    ApplyDepthBias();               // Polygon offset
    ApplyPushConstants();           // Push constant data
    
    // Resource binding
    ApplyVertexBuffers();           // Bind VBO, IBO
    ApplyMaterial();                // Bind texture descriptor set (Set 2)
    ApplyHWBufferSet();             // Bind buffer descriptor set (Set 1)
}
```

### Render Pass Setup: VkRenderState::ApplyRenderPass()

```
Check if pipeline key changed
    ↓ (yes)
├─ End current render pass
├─ Look up VkRenderPassSetup by target format/samples/buffers
├─ Get VulkanPipeline from setup by pipeline key
├─ Begin new render pass with appropriate framebuffer
├─ Bind pipeline
└─ Record: Clear targets if needed
```

### Resource Binding Strategy

**Descriptor Set Layout:**
```
Pipeline Layout: 3 descriptor sets
├─ Set 0: Fixed resources (Shadowmap, Lightmap, RTX)
├─ Set 1: Dynamic HW buffers (Viewpoint, Matrices, Stream, Light, Bones)
│         └─ Uses dynamic offsets for per-draw data
└─ Set 2: Material textures (variable number of layers)
```

**ApplyHWBufferSet() - Dynamic Buffer Binding:**
```cpp
uint32_t offsets[3] = { 
    mViewpointOffset,           // For viewpoint UBO
    mMatrixBufferWriter.Offset(), // For matrix UBO
    mStreamBufferWriter.StreamDataOffset() // For stream UBO
};
mCommandBuffer->bindDescriptorSet(
    VK_PIPELINE_BIND_POINT_GRAPHICS,
    passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers),
    1,  // Set 1
    descriptors->GetHWBufferDescriptorSet(),
    3,  // 3 dynamic offsets
    offsets
);
```

**ApplyMaterial() - Texture Binding:**
```cpp
VulkanDescriptorSet* descriptorset = mMaterial.mMaterial ?
    static_cast<VkMaterial*>(mMaterial.mMaterial)->GetDescriptorSet(mMaterial) :
    descriptors->GetNullTextureDescriptorSet();

mCommandBuffer->bindDescriptorSet(
    VK_PIPELINE_BIND_POINT_GRAPHICS,
    passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers),
    2,  // Set 2
    descriptorset
);
```

---

## 5. SHADER MANAGEMENT

### Shader Storage & Compilation

**Compilation Trigger:**
- Incremental during game loading via `CompileNextShader()`
- Threaded compilation in background

**Shader Categories:**

1. **Material Shaders** (Per pass type):
   - Alpha test variant: `mMaterialShaders[passType]`
   - No-alpha-test variant: `mMaterialShadersNAT[passType]`

2. **Effect Shaders** (Per pass type):
   - Fog boundary, spheremap, burn, etc.
   - `mEffectShaders[passType]`

3. **Post-Process Shaders**:
   - Dynamic PP shader compilation
   - Managed as `VkPPShader` wrappers

**Lookup:**
```cpp
VkShaderProgram* Get(unsigned int eff, bool alphateston, EPassType passType) {
    if (alphateston) {
        return &mMaterialShaders[passType][eff];
    } else {
        return &mMaterialShadersNAT[passType][eff];
    }
}
```

### Shader Compilation Details

**Load Sequence (LoadFragShader):**
1. Load main fragment shader template from lump
2. Load material-specific texture coordinate function from lump
3. Load lighting calculation function from lump
4. Combine with defines
5. Preprocess and compile to SPIR-V

**Defines Injection:**
- Per-shader type (normal, alpha test, gbuffer)
- Per-effect (fog, spheremap, etc.)
- User-defined custom defines

### Push Constants

```cpp
struct PushConstants {
    int uTextureMode;
    float uAlphaThreshold;
    FVector2 uClipSplit;
    
    // Lighting + Fog
    float uLightLevel;
    float uFogDensity;
    float uLightFactor;
    float uLightDist;
    int uFogEnabled;
    
    // Dynamic lights
    int uLightIndex;
    
    // Blinn glossiness
    FVector2 uSpecularMaterial;
    
    // Bone animation
    int uBoneIndexBase;
    
    int uDataIndex;
    int padding1, padding2, padding3;
};
```

**ApplyPushConstants():**
- Updates push constants once per render pass change
- Embedded in command buffer, no descriptor set needed

---

## 6. RESOURCE BINDING

### Texture/Sampler Binding Chain

```
User calls SetMaterial(FMaterial* mat)
    ↓
VkRenderState records material change
    ↓
Apply() called
    ↓
ApplyMaterial()
    ├─ Get VkMaterial from FMaterial
    ├─ Get descriptor set from VkMaterial->GetDescriptorSet(state)
    │  └─ Lookup/create descriptor set for this material + clamp mode
    └─ Bind descriptor set (Set 2) to pipeline
        └─ Contains: texture image views + samplers
```

### VkMaterial Descriptor Set Management

```cpp
class VkMaterial : public FMaterial {
    std::vector<DescriptorEntry> mDescriptorSets;  // Cached by clamp + remap
    
    VulkanDescriptorSet* GetDescriptorSet(const FMaterialState& state) {
        // Look up by state.mClampMode + state.mTranslation
        // Create if missing
        // Return cached descriptor set
    }
};
```

### Buffer Binding Chain

**Uniform Buffers:**
```
Per-frame:
├─ ViewpointUBO: Updated by HWViewpointBuffer
├─ MatrixBuffer: Ring buffer, per-draw matrix data
└─ StreamBuffer: Ring buffer, per-vertex stream data

Ring buffer allocation:
├─ VkStreamBufferWriter::Write() allocates next block
├─ Returns offset in GPU buffer
└─ Offset passed to ApplyHWBufferSet() as dynamic offset
```

**Storage Buffers:**
```
Rarely changing:
├─ LightBufferSSO: Light data
├─ BoneBufferSSO: Skeletal animation data
├─ LightNodes, LightLines, LightList: Spatial structures
```

### Descriptor Set Allocation Strategy

**HW Buffer Pool:**
- Single large pool, pre-allocated
- Rarely recreated

**Fixed Set Pool:**
- Single static descriptor set
- Updated every frame (shadowmap/lightmap changes)

**Texture Descriptor Pools:**
- Lazy allocation: Creates new pool when full
- Allocates 1000 sets per pool
- Destroyed on texture filter mode change

**Backpressure Mechanism:**
```cpp
HWBufferSet = HWBufferDescriptorPool->tryAllocate(HWBufferSetLayout.get());
if (!HWBufferSet) {
    fb->GetCommands()->WaitForCommands(false);  // Stall for GPU
    HWBufferSet = HWBufferDescriptorPool->allocate(HWBufferSetLayout.get());
}
```

---

## 7. INTEGRATION POINTS

### 7.1 Windowing System Integration

**macOS/Cocoa Integration:**

File: `/src/common/platform/posix/cocoa/gl_sysfb.h`

```cpp
class SystemBaseFrameBuffer : public DFrameBuffer {
    virtual CocoaNativeHandle GetNativeHandle() const;  // For Metal integration
    CocoaWindow* m_window;
};
```

**Platform Specializations:**
- `SystemGLFrameBuffer` (OpenGL)
- VulkanRenderDevice extends SystemBaseFrameBuffer
- macOS uses MoltenVK bridge to Vulkan
- `VkRenderStateMolten` variant for platform-specific rendering

**Window Callback Integration:**
```cpp
VulkanRenderDevice(void *hMonitor, bool fullscreen, 
                   std::shared_ptr<VulkanSurface> surface)
```
- VulkanSurface created from platform window
- Passed to VulkanDeviceBuilder
- Device selects graphics queue with present capability

### 7.2 Hardware Renderer Abstraction Layer

**Abstract Interfaces (Implemented by Vulkan):**

From `hwrenderer/`:

```cpp
// Base framebuffer interface
class SystemBaseFrameBuffer : public DFrameBuffer {
    virtual void InitializeState() = 0;
    virtual void BeginFrame() = 0;
    virtual void Update() = 0;
    virtual FRenderState* RenderState() = 0;
    virtual IHardwareTexture* CreateHardwareTexture(int numchannels) = 0;
    virtual FMaterial* CreateMaterial(FGameTexture*, int scaleflags) = 0;
    virtual IVertexBuffer* CreateVertexBuffer() = 0;
    virtual IIndexBuffer* CreateIndexBuffer() = 0;
    virtual IDataBuffer* CreateDataBuffer(int bindingpoint, bool ssbo, bool needresize) = 0;
};

// Render state interface
class FRenderState {
    virtual void ClearScreen() = 0;
    virtual void Draw(int dt, int index, int count, bool apply) = 0;
    virtual void DrawIndexed(int dt, int index, int count, bool apply) = 0;
    virtual void SetDepthMask(bool on) = 0;
    // ... many state setters
};

// Hardware texture interface
class IHardwareTexture {
    virtual void AllocateBuffer(int w, int h, int texelsize) = 0;
    virtual uint8_t* MapBuffer() = 0;
    virtual unsigned int CreateTexture(unsigned char* buffer, int w, int h,
                                      int texunit, bool mipmap, const char* name) = 0;
};

// Buffer interfaces
class IVertexBuffer, IIndexBuffer, IDataBuffer;
```

**Vulkan Implementation Classes:**
- `VulkanRenderDevice` extends `SystemBaseFrameBuffer`
- `VkRenderState` extends `FRenderState`
- `VkHardwareTexture` extends `IHardwareTexture`
- `VkHardwareVertexBuffer`, `VkHardwareIndexBuffer`, `VkHardwareDataBuffer` extend respective interfaces

### 7.3 Game Loop Integration

**Main Update Flow:**

```
Game main loop (d_main.cpp)
    ↓
Screen rendering code (v_video.cpp)
    ↓
VulkanRenderDevice::Update()
    ├─ Finalize 3D rendering
    ├─ Render 2D elements
    ├─ EndRenderPass()
    ├─ EndFrame()
    ├─ WaitForCommands(true)
    ├─ UpdateGpuStats()
    └─ Super::Update() [Platform-specific present to screen]
        └─ SDL_GL_SwapWindow() or equivalent
```

**Material/Texture Binding:**

```
Game code calls material setup
    ↓
FRenderState::SetMaterial(FMaterial*)
    └─ VkRenderState::SetMaterial(FMaterial*)
        └─ mMaterial.mChanged = true
            └─ Applied lazily in Apply()
                └─ ApplyMaterial() binds descriptor set
```

**Vertex/Index Buffer Binding:**

```
HardwareRenderer submits geometry
    ↓
FRenderState::SetVertexBuffer(IVertexBuffer*, offset)
FRenderState::SetIndexBuffer(IIndexBuffer*)
    ↓
VkRenderState records in mVertexBuffer, mIndexBuffer
    ↓
Draw/DrawIndexed called
    ↓
Apply() → ApplyVertexBuffers()
    └─ mCommandBuffer->bindVertexBuffers()
    └─ mCommandBuffer->bindIndexBuffer()
```

---

## 8. ARCHITECTURAL PATTERNS & DESIGN DECISIONS

### Pattern 1: Manager Factory Pattern
- VulkanRenderDevice owns all subsystem managers
- Each manager stored as `unique_ptr`
- Getters provide access: `GetCommandManager()`, `GetShaderManager()`, etc.
- Ensures single instance per renderer
- **Translation to Metal:** Create `MetalRenderDevice` with equivalent manager getters

### Pattern 2: Lazy Evaluation (Command Buffer Recording)
- State changes queued, applied later
- `Apply()` method batches all pending state changes
- Reduces command buffer recording overhead
- **Translation to Metal:** Keep same pattern; Metal encoding to command buffers

### Pattern 3: Ring Buffer (Dynamic Data)
- VkStreamBuffer for per-vertex data
- MatrixBuffer for per-draw matrices
- Circular allocation avoids stalls
- **Translation to Metal:** Use Metal argument buffers with dynamic offsets

### Pattern 4: Deferred Deletion
- GPU resources queued for deletion
- Freed only after GPU finishes processing
- Avoids use-after-free bugs
- **Translation to Metal:** Use Metal's autoreleasepool mechanism

### Pattern 5: Lazy Pipeline Compilation
- Pipelines created on first use
- Cached using `std::map<VkPipelineKey, unique_ptr<VulkanPipeline>>`
- Pipeline cache to disk for persistence
- **Translation to Metal:** Use MTLRenderPipelineState cache with similar keying

### Pattern 6: Descriptor Set Pooling
- Multiple pools, lazy expansion
- Backpressure: GPU stall if pool full
- Reduces fragmentation
- **Translation to Metal:** Map to MTLArgumentBuffer pooling

### Pattern 7: Dynamic Descriptor Set Updates
- Fixed set (Set 0): Rarely changes (shadowmap, lightmap)
- HW Buffer set (Set 1): per-frame update with dynamic offsets
- Texture set (Set 2): per-material
- **Translation to Metal:** Use argument buffer encodings + update pattern

---

## 9. KEY FILES FOR METAL IMPLEMENTATION

### Must Study:
1. `/src/common/rendering/vulkan/system/vk_renderdevice.h/cpp` - Manager architecture
2. `/src/common/rendering/vulkan/renderer/vk_renderstate.h/cpp` - State machine & draw dispatch
3. `/src/common/rendering/vulkan/system/vk_commandbuffer.h/cpp` - GPU work submission
4. `/src/common/rendering/vulkan/renderer/vk_renderpass.h/cpp` - Pipeline configuration
5. `/src/common/rendering/vulkan/renderer/vk_descriptorset.h/cpp` - Resource binding
6. `/src/common/rendering/vulkan/shaders/vk_shader.h/cpp` - Shader management

### Architecture Reference:
7. `/src/common/platform/posix/cocoa/gl_sysfb.h` - Cocoa/macOS integration
8. `/src/common/rendering/hwrenderer/data/hw_renderstate.h` - Abstract interfaces
9. `/src/common/rendering/vulkan/textures/vk_hwtexture.h` - Texture abstraction
10. `/src/common/rendering/vulkan/system/vk_buffer.h` - Buffer abstractions

---

## 10. METAL IMPLEMENTATION STRATEGY

### Class Mapping (Vulkan → Metal)

| Vulkan | Metal | Purpose |
|--------|-------|---------|
| VulkanRenderDevice | MetalRenderDevice | Device manager |
| VkRenderState | MetRenderState | State machine |
| VkCommandBufferManager | MetCommandBufferManager | Work submission |
| VkRenderPassManager | MetPipelineManager | Pipeline management |
| VkDescriptorSetManager | MetArgumentBufferManager | Resource binding |
| VkShaderManager | MetShaderManager | Shader compilation |
| VkBufferManager | MetBufferManager | Memory management |
| VkSamplerManager | MetSamplerManager | Sampler cache |
| VkTextureManager | MetTextureManager | Texture resources |
| VulkanCommandBuffer | MTLCommandBuffer | GPU commands |
| VulkanPipeline | MTLRenderPipelineState | Graphics pipeline |
| VulkanDescriptorSet | MTLArgumentBuffer | Resource bindings |
| VulkanBuffer | MTLBuffer | GPU memory |
| VulkanImage | MTLTexture | GPU texture |
| VulkanSampler | MTLSamplerState | Texture sampler |

### Key Behavioral Differences:
1. **Metal uses immediate encoding** (less lazy than Vulkan)
2. **No descriptor sets** → Use MTLArgumentBuffer
3. **No render passes** → Use MTLRenderPassDescriptor per operation
4. **No pipeline cache** → MTLPipelineState is immutable once created
5. **No deferred deletion** → Use autorelease pools for timing

---

## 11. CRITICAL CODE SECTIONS FOR ADAPTATION

### VkRenderState::Apply() Structure
- **Keep:** Lazy state application pattern
- **Adapt:** Encode to MTLRenderCommandEncoder instead of VkCommand
- **Change:** No explicit render pass creation (implicit in encoder)

### Descriptor Set Binding (ApplyMaterial, ApplyHWBufferSet)
- **Keep:** Three-level descriptor binding strategy
- **Adapt:** Encode MTLArgumentBuffer instead of vkCmdBindDescriptorSets
- **Change:** Use MTL argument buffer encoder methods

### Ring Buffer Pattern (StreamBuffer, MatrixBuffer)
- **Keep:** Circular allocation strategy
- **Adapt:** Use Metal's dynamic buffer offsets
- **Change:** Different API for setting per-draw offsets

### Pipeline Compilation (VkRenderPassManager)
- **Keep:** Lazy compilation with caching
- **Adapt:** Compile MTLRenderPipelineState
- **Change:** No render pass object; attributes in descriptor

### Shader Compilation (VkShaderManager)
- **Keep:** GLSL → SPIR-V translation
- **Adapt:** Use existing Metal shader compilation
- **Change:** Might use Metal Shading Language or continue SPIR-V translation

---

## 12. PERFORMANCE CONSIDERATIONS

### Vulkan-Specific Optimizations Seen:
1. **Batched submissions** - Flushes every 1000 draw calls (vk_submit_size)
2. **Dynamic offsets** - Avoids descriptor set rebinding
3. **Pipeline cache** - Persistent disk cache
4. **Lazy pipeline creation** - On first use
5. **Multithreaded shader compilation** - Incremental CompileNextShader
6. **MSAA support** - Configurable sample counts

### Metal Equivalents:
1. **Command buffer pooling** - Reuse MTLCommandBuffer
2. **Indirect command buffers** - Reduce encoding overhead
3. **Render pass optimization** - Reuse MTLRenderPassDescriptor
4. **Pipeline state caching** - MTLRenderPipelineState immutable
5. **Parallel rendering** - MTLParallelRenderCommandEncoder
6. **MSAA** - MTLPixelFormatDepth32Float_Stencil8 with sample count

