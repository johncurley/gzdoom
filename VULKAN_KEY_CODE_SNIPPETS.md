# GZDoom Vulkan - Key Code Snippets

## 1. VulkanRenderDevice Initialization

**File:** `src/common/rendering/vulkan/system/vk_renderdevice.cpp:158-211`

```cpp
void VulkanRenderDevice::InitializeState()
{
    // Vendor/GPU info
    switch (device->PhysicalDevice.Properties.Properties.vendorID) {
        case 0x1002: vendorstring = "ATI Technologies Inc."; break;
        case 0x10DE: vendorstring = "NVIDIA Corporation"; break;
        case 0x8086: vendorstring = "Intel"; break;
        default: vendorstring = "Unknown"; break;
    }

    // GPU capabilities
    hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
    glslversion = 4.50f;
    uniformblockalignment = device->PhysicalDevice.Properties.Properties
                           .limits.minUniformBufferOffsetAlignment;
    maxuniformblock = device->PhysicalDevice.Properties.Properties
                     .limits.maxUniformBufferRange;

    // Create subsystem managers (ORDER MATTERS)
    mCommands.reset(new VkCommandBufferManager(this));
    mSamplerManager.reset(new VkSamplerManager(this));
    mTextureManager.reset(new VkTextureManager(this));
    mFramebufferManager.reset(new VkFramebufferManager(this));
    mBufferManager.reset(new VkBufferManager(this));
    mBufferManager->Init();

    // Render targets
    mScreenBuffers.reset(new VkRenderBuffers(this));
    mSaveBuffers.reset(new VkRenderBuffers(this));
    mActiveRenderBuffers = mScreenBuffers.get();

    // Post-processing and descriptor sets
    mPostprocess.reset(new VkPostprocess(this));
    mDescriptorSetManager.reset(new VkDescriptorSetManager(this));
    mRenderPassManager.reset(new VkRenderPassManager(this));
    mRaytrace.reset(new VkRaytrace(this));

    // CPU-side vertex/lighting data
    mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
    mSkyData = new FSkyVertexBuffer;
    mViewpoints = new HWViewpointBuffer;
    mLights = new FLightBuffer();
    mBones = new BoneBuffer();

    // Shaders and render state
    mShaderManager.reset(new VkShaderManager(this));
    mDescriptorSetManager->Init();
    
#ifdef __APPLE__
    mRenderState.reset(new VkRenderStateMolten(this));
#else
    mRenderState.reset(new VkRenderState(this));
#endif
}
```

## 2. Per-Frame Flow

**File:** `src/common/rendering/vulkan/system/vk_renderdevice.cpp:213-234`

```cpp
void VulkanRenderDevice::Update()
{
    twoD.Reset();                  // Reset 2D drawing state
    Flush3D.Reset();               // Reset timer
    Flush3D.Clock();               // Start timing

    GetPostprocess()->SetActiveRenderTarget();  // Switch render target
    
    Draw2D();                      // Render HUD
    twod->Clear();                 // Clear 2D state
    
    mRenderState->EndRenderPass(); // Finalize rendering
    mRenderState->EndFrame();      // Reset per-frame state
    
    Flush3D.Unclock();             // End timing
    
    mCommands->WaitForCommands(true);   // Block until GPU done
    mCommands->UpdateGpuStats();        // Gather timing data
    
    Super::Update();               // Platform: present to screen
}
```

## 3. Lazy State Application Pattern

**File:** `src/common/rendering/vulkan/renderer/vk_renderstate.cpp:58-72`

```cpp
void VkRenderState::Draw(int dt, int index, int count, bool apply)
{
    if (apply || mNeedApply)
        Apply(dt);  // Lazy: only apply if state changed

    mCommandBuffer->draw(count, 1, index, 0);
}

void VkRenderState::DrawIndexed(int dt, int index, int count, bool apply)
{
    if (apply || mNeedApply)
        Apply(dt);

    mCommandBuffer->drawIndexed(count, 1, index, 0, 0);
}
```

## 4. State Application Pipeline

**File:** `src/common/rendering/vulkan/renderer/vk_renderstate.cpp:181-210`

```cpp
void VkRenderState::Apply(int dt)
{
    drawcalls.Clock();

    mApplyCount++;
    if (mApplyCount >= vk_submit_size)  // Default: 1000
    {
        fb->GetCommands()->FlushCommands(false);
        mApplyCount = 0;
    }

    // Update dynamic data
    ApplyStreamData();
    ApplyMatrices();
    
    // Setup render pass and pipeline
    ApplyRenderPass(dt);
    
    // Set rasterization state
    ApplyScissor();
    ApplyViewport();
    ApplyStencilRef();
    ApplyDepthBias();
    ApplyPushConstants();
    
    // Bind resources
    ApplyVertexBuffers();
    ApplyMaterial();
    ApplyHWBufferSet();
}
```

## 5. Material Binding

**File:** `src/common/rendering/vulkan/renderer/vk_renderstate.cpp:431-446`

```cpp
void VkRenderState::ApplyMaterial()
{
    if (mMaterial.mChanged)
    {
        auto passManager = fb->GetRenderPassManager();
        auto descriptors = fb->GetDescriptorSetManager();

        // Update GPU canvas textures if needed
        if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas())
            static_cast<FCanvasTexture*>(mMaterial.mMaterial->Source()->GetTexture())->NeedUpdate();

        // Get descriptor set for this material
        VulkanDescriptorSet* descriptorset = mMaterial.mMaterial ?
            static_cast<VkMaterial*>(mMaterial.mMaterial)->GetDescriptorSet(mMaterial) :
            descriptors->GetNullTextureDescriptorSet();

        // Bind fixed set (Set 0) and material texture set (Set 2)
        mCommandBuffer->bindDescriptorSet(
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers),
            0, 
            fb->GetDescriptorSetManager()->GetFixedDescriptorSet()
        );
        
        mCommandBuffer->bindDescriptorSet(
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers),
            2,  // Material texture set
            descriptorset
        );
        
        mMaterial.mChanged = false;
    }
}
```

## 6. HW Buffer Binding (Dynamic Offsets)

**File:** `src/common/rendering/vulkan/renderer/vk_renderstate.cpp:448-465`

```cpp
void VkRenderState::ApplyHWBufferSet()
{
    uint32_t matrixOffset = mMatrixBufferWriter.Offset();
    uint32_t streamDataOffset = mStreamBufferWriter.StreamDataOffset();
    
    if (mViewpointOffset != mLastViewpointOffset || 
        matrixOffset != mLastMatricesOffset || 
        streamDataOffset != mLastStreamDataOffset)
    {
        auto passManager = fb->GetRenderPassManager();
        auto descriptors = fb->GetDescriptorSetManager();

        // Three dynamic offsets for the three dynamic buffers in Set 1
        uint32_t offsets[3] = { 
            mViewpointOffset,        // For viewpoint UBO
            matrixOffset,            // For matrix UBO
            streamDataOffset         // For stream UBO
        };
        
        mCommandBuffer->bindDescriptorSet(
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers),
            1,  // HW buffer set
            descriptors->GetHWBufferDescriptorSet(),
            3,  // Number of dynamic offsets
            offsets
        );

        mLastViewpointOffset = mViewpointOffset;
        mLastMatricesOffset = matrixOffset;
        mLastStreamDataOffset = streamDataOffset;
    }
}
```

## 7. Descriptor Set Pooling with Backpressure

**File:** `src/common/rendering/vulkan/renderer/vk_descriptorset.cpp:72-90`

```cpp
void VkDescriptorSetManager::UpdateHWBufferSet()
{
    // Deferred delete old set
    fb->GetCommands()->DrawDeleteList->Add(std::move(HWBufferSet));

    // Try to allocate new set
    HWBufferSet = HWBufferDescriptorPool->tryAllocate(HWBufferSetLayout.get());
    
    // If pool full, wait for GPU then allocate
    if (!HWBufferSet)
    {
        fb->GetCommands()->WaitForCommands(false);  // Stall for GPU
        HWBufferSet = HWBufferDescriptorPool->allocate(HWBufferSetLayout.get());
    }

    // Write descriptor bindings
    WriteDescriptors()
        .AddBuffer(HWBufferSet.get(), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 
                   fb->GetBufferManager()->ViewpointUBO->mBuffer.get(), 0, sizeof(HWViewpointUniforms))
        .AddBuffer(HWBufferSet.get(), 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 
                   fb->GetBufferManager()->MatrixBuffer->UniformBuffer->mBuffer.get(), 0, sizeof(MatricesUBO))
        .AddBuffer(HWBufferSet.get(), 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 
                   fb->GetBufferManager()->StreamBuffer->UniformBuffer->mBuffer.get(), 0, sizeof(StreamUBO))
        .AddBuffer(HWBufferSet.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 
                   fb->GetBufferManager()->LightBufferSSO->mBuffer.get())
        .AddBuffer(HWBufferSet.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 
                   fb->GetBufferManager()->BoneBufferSSO->mBuffer.get())
        .Execute(fb->device.get());
}
```

## 8. Ring Buffer Allocation

**File:** `src/common/rendering/vulkan/system/vk_buffer.h:51-65`

```cpp
class VkStreamBuffer
{
public:
    VkStreamBuffer(VkBufferManager* buffers, size_t structSize, size_t count);
    ~VkStreamBuffer();

    uint32_t NextStreamDataBlock();  // Allocate next block, wraps around
    void Reset() { mStreamDataOffset = 0; }

    VkHardwareDataBuffer* UniformBuffer = nullptr;

private:
    uint32_t mBlockSize = 0;
    uint32_t mStreamDataOffset = 0;  // Current write position
};
```

Usage in `vk_renderstate.cpp`:
```cpp
void VkRenderState::ApplyStreamData()
{
    if (!mStreamBufferWriter.Write(mStreamData))
    {
        WaitForStreamBuffers();  // Ring buffer full, wait for GPU
        mStreamBufferWriter.Write(mStreamData);
    }
}

void VkRenderState::WaitForStreamBuffers()
{
    fb->WaitForCommands(false);  // Stall for GPU
    mApplyCount = 0;
    mStreamBufferWriter.Reset();
    mMatrixBufferWriter.Reset();
}
```

## 9. Shader Compilation (Incremental)

**File:** `src/common/rendering/vulkan/shaders/vk_shader.cpp:59-140`

```cpp
bool VkShaderManager::CompileNextShader()
{
    const char *mainvp = "shaders/glsl/main.vp";
    const char *mainfp = "shaders/glsl/main.fp";
    int i = compileIndex;

    if (compileState == 0)
    {
        // State 0: Regular material shaders
        VkShaderProgram prog;
        prog.vert = LoadVertShader(defaultshaders[i].ShaderName, mainvp, 
                                   defaultshaders[i].Defines);
        prog.frag = LoadFragShader(defaultshaders[i].ShaderName, mainfp, 
                                   defaultshaders[i].gettexelfunc, 
                                   defaultshaders[i].lightfunc, 
                                   defaultshaders[i].Defines, true, 
                                   compilePass == GBUFFER_PASS);
        mMaterialShaders[compilePass].push_back(std::move(prog));
        
        compileIndex++;
        if (defaultshaders[compileIndex].ShaderName == nullptr)
        {
            compileIndex = 0;
            compileState++;  // Move to next state
        }
    }
    else if (compileState == 1)
    {
        // State 1: NAT (no alpha test) variants
        VkShaderProgram natprog;
        natprog.vert = LoadVertShader(defaultshaders[i].ShaderName, mainvp, 
                                      defaultshaders[i].Defines);
        natprog.frag = LoadFragShader(defaultshaders[i].ShaderName, mainfp, 
                                      defaultshaders[i].gettexelfunc, 
                                      defaultshaders[i].lightfunc, 
                                      defaultshaders[i].Defines, false,  // NO alpha test
                                      compilePass == GBUFFER_PASS);
        mMaterialShadersNAT[compilePass].push_back(std::move(natprog));
        // ... more states
    }
    // ... State 2 (user shaders), State 3 (effect shaders)
    
    return false;  // Continue compilation
}
```

## 10. Command Buffer Submission

**File:** `src/common/rendering/vulkan/system/vk_commandbuffer.cpp:101-131`

```cpp
void VkCommandBufferManager::FlushCommands(VulkanCommandBuffer** commands, 
                                          size_t count, bool finish, bool lastsubmit)
{
    int currentIndex = mNextSubmit % maxConcurrentSubmitCount;  // Ringbuffer of 8

    // Wait for this submit slot if GPU still using it
    if (mNextSubmit >= maxConcurrentSubmitCount)
    {
        vkWaitForFences(fb->device->device, 1, 
                       &mSubmitFence[currentIndex]->fence, VK_TRUE, 
                       std::numeric_limits<uint64_t>::max());
        vkResetFences(fb->device->device, 1, 
                     &mSubmitFence[currentIndex]->fence);
    }

    QueueSubmit submit;

    // Add all command buffers
    for (size_t i = 0; i < count; i++)
        submit.AddCommandBuffer(commands[i]);

    // Chain with previous submit (GPU will wait)
    if (mNextSubmit > 0)
        submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 
                      mSubmitSemaphore[(mNextSubmit - 1) % maxConcurrentSubmitCount].get());

    // Handle presentation sync
    auto framebuffers = fb->GetFramebufferManager();
    if (finish && framebuffers->PresentImageIndex != -1)
    {
        submit.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 
                      framebuffers->SwapChainImageAvailableSemaphore.get());
        submit.AddSignal(framebuffers->RenderFinishedSemaphores[framebuffers->PresentImageIndex].get());
    }

    // Signal next submit
    if (!lastsubmit)
        submit.AddSignal(mSubmitSemaphore[currentIndex].get());

    // Actually submit to GPU queue
    submit.Execute(fb->device.get(), fb->device->GraphicsQueue, 
                  mSubmitFence[currentIndex].get());
    mNextSubmit++;
}
```

## 11. Render Pass and Pipeline Creation

**File:** `src/common/rendering/vulkan/renderer/vk_renderstate.cpp:533-550`

```cpp
void VkRenderState::BeginRenderPass(VulkanCommandBuffer *cmdbuffer)
{
    // Create key for this render pass configuration
    VkRenderPassKey key = {};
    key.DrawBufferFormat = mRenderTarget.Format;
    key.Samples = mRenderTarget.Samples;
    key.DrawBuffers = mRenderTarget.DrawBuffers;
    key.DepthStencil = !!mRenderTarget.DepthStencil;

    // Get or create render pass setup
    mPassSetup = fb->GetRenderPassManager()->GetRenderPass(key);

    // Get or create framebuffer for this render target + render pass combo
    auto &framebuffer = mRenderTarget.Image->RSFramebuffers[key];
    if (!framebuffer)
    {
        auto buffers = fb->GetBuffers();
        FramebufferBuilder builder;
        builder.RenderPass(mPassSetup->GetRenderPass(0));
        builder.Size(mRenderTarget.Width, mRenderTarget.Height);
        builder.AddAttachment(mRenderTarget.Image->View.get());
        if (mRenderTarget.DepthStencil)
            builder.AddAttachment(mRenderTarget.DepthStencil);
        framebuffer = builder.Create(fb->device.get());
    }
    
    // Begin render pass
    VkRenderPassBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = mPassSetup->GetRenderPass(mClearTargets)->renderPass;
    beginInfo.framebuffer = framebuffer->framebuffer;
    beginInfo.renderArea.extent = { (uint32_t)mRenderTarget.Width, (uint32_t)mRenderTarget.Height };
    
    mCommandBuffer = cmdbuffer;
    cmdbuffer->beginRenderPass(&beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // Bind pipeline for this configuration
    mCommandBuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, 
                                mPassSetup->GetPipeline(mPipelineKey)->pipeline);
}
```

## 12. Push Constants Update

**File:** `src/common/rendering/vulkan/shaders/vk_shader.h:35-59`

```cpp
struct PushConstants
{
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

    // Blinn glossiness and specular level
    FVector2 uSpecularMaterial;

    // Bone animation
    int uBoneIndexBase;

    int uDataIndex;
    int padding1, padding2, padding3;
};
```

Applied in `ApplyPushConstants()`:
```cpp
mCommandBuffer->pushConstants(
    fb->GetRenderPassManager()->GetPipelineLayout(mPipelineKey.NumTextureLayers),
    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT,
    0, sizeof(PushConstants), &mPushConstants
);
```

---

## Key Takeaways for Metal Adaptation

1. **Manager Architecture:** Keep the same hierarchical structure
2. **Lazy Evaluation:** Preserve the Apply() pattern for state batching
3. **Ring Buffers:** Use Metal's buffer offsets with same circular pattern
4. **Descriptor Strategy:** Map to MTLArgumentBuffer with similar 3-level binding
5. **Immediate Encoding:** Metal requires encoding directly (no deferred recording)
6. **Pipeline Caching:** Use MTLRenderPipelineState (already immutable and cached)
7. **Synchronization:** Use Metal command buffer completion handlers instead of fences

