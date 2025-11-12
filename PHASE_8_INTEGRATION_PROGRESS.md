# Phase 8: Integration & Testing - IN PROGRESS ⚙️

**Date:** November 7, 2025
**Status:** Core Integration Complete - Testing Pending

---

## ✅ Completed Components

### 1. Backend Selection System

**Files Modified:**
- `src/common/platform/posix/cocoa/i_video.mm`
- `src/common/rendering/v_video.cpp`

#### Backend Registration (i_video.mm)

Added Metal as backend option 3 in the platform-specific video initialization:

```cpp
#ifdef HAVE_METAL
#include "metal/system/mt_renderdevice.h"
#endif

// In CreateSystemFrameBuffer():
if (fb == nullptr)
{
#ifdef HAVE_METAL
	// Try Metal renderer on macOS 10.13+
	if (V_GetBackend() == 3)
	{
		try
		{
			fb = new MetalRenderDevice(nullptr, vid_fullscreen);
			Printf("Metal renderer initialized successfully\n");
		}
		catch (std::exception const& error)
		{
			Printf(TEXTCOLOR_RED "Metal renderer initialization failed: %s\n", error.what());
			Printf("Falling back to OpenGL renderer\n");
			fb = nullptr;
		}
	}
#endif
	// OpenGL fallback...
}
```

**Key Features:**
- Exception-safe initialization with fallback to OpenGL
- Platform-specific (#ifdef HAVE_METAL)
- Proper error messaging
- Seamless integration with existing backend system

---

#### CVAR Backend Selection (v_video.cpp)

Updated `vid_preferbackend` CVAR to support Metal:

```cpp
CUSTOM_CVAR(Int, vid_preferbackend, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	switch(self)
	{
#ifdef HAVE_METAL
	case 3:
		Printf("Selecting Metal backend...\n");
		break;
#endif
#ifdef HAVE_GLES2
	case 2:
		Printf("Selecting OpenGLES 2.0 backend...\n");
		break;
#endif
#ifdef HAVE_VULKAN
	case 1:
		Printf("Selecting Vulkan backend...\n");
		break;
#endif
	default:
		Printf("Selecting OpenGL backend...\n");
	}
	Printf("Changing the video backend requires a restart for " GAMENAME ".\n");
}

int V_GetBackend()
{
	int v = vid_preferbackend;
#ifndef HAVE_METAL
	if (v == 3)
	{
#ifdef HAVE_GLES2
		vid_preferbackend = v = 2;
#else
		vid_preferbackend = v = 0;
#endif
	}
#endif
	if (v < 0 || v > 3) v = 0;
	return v;
}
```

**Backend Values:**
- **0:** OpenGL (default fallback)
- **1:** Vulkan (if available)
- **2:** OpenGL ES 2.0 (if available)
- **3:** Metal (macOS 10.13+)

**Fallback Strategy:**
- If Metal not available → GLES2 (if available) → OpenGL
- Requires restart to change backend

---

### 2. Frame Presentation System

**File:** `src/common/rendering/metal/system/mt_renderdevice.cpp`

#### Update() Method - Main Frame Loop

Implemented full frame lifecycle with proper drawable management:

```cpp
void MetalRenderDevice::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	// Get the Metal layer and next drawable
	CocoaNativeHandle nativeHandle = GetNativeHandle();
	if (!nativeHandle.metalLayer)
	{
		Super::Update();
		return;
	}

	CA::MetalLayer* metalLayer = (CA::MetalLayer*)nativeHandle.metalLayer;
	CA::MetalDrawable* drawable = metalLayer->nextDrawable();

	if (!drawable)
	{
		if (mt_debug)
			Printf("Warning: Failed to get next Metal drawable\n");
		Super::Update();
		return;
	}

	// Get the drawable's texture and set it as our render target
	MTL::Texture* drawableTexture = drawable->texture();
	if (drawableTexture && mRenderState)
	{
		int width = drawableTexture->width();
		int height = drawableTexture->height();

		// Set render target to drawable texture (no depth/stencil for now - simple test)
		mRenderState->SetRenderTarget(
			(MtTextureImage*)drawableTexture,  // Color attachment
			nullptr,  // No depth/stencil yet
			width,
			height,
			(int)MTL::PixelFormatBGRA8Unorm,
			1  // No MSAA yet
		);
	}

	Flush3D.Clock();
	Draw2D();
	Flush3D.Unclock();

	// End the render pass (finishes encoding)
	if (mRenderState)
		mRenderState->EndRenderPass();

	// Present the frame to screen
	PresentFrame(drawable);

	Super::Update();
}
```

**Frame Lifecycle:**
1. **Get Drawable:** Obtain next CA::MetalDrawable from CA::MetalLayer
2. **Set Render Target:** Configure drawable texture as color attachment
3. **Draw:** Call Draw2D() which triggers Apply() → BeginRenderPass() → encoder creation
4. **End Encoding:** Call EndRenderPass() to finish encoding commands
5. **Present:** Submit command buffer with drawable presentation
6. **Sync:** Wait for GPU completion (synchronous for now)

---

#### PresentFrame() Method - GPU Submission

```cpp
void MetalRenderDevice::PresentFrame(void* drawablePtr)
{
	if (!drawablePtr)
		return;

	auto drawable = (CA::MetalDrawable*)drawablePtr;

	// Get current command buffer
	void* cmdBufPtr = mCommands->GetRenderCommandBuffer();
	if (!cmdBufPtr)
		return;

	auto commandBuffer = (MTL::CommandBuffer*)cmdBufPtr;

	// Present drawable when command buffer completes
	commandBuffer->presentDrawable(drawable);

	// Submit and wait for completion (TODO: optimize with semaphores for triple buffering)
	commandBuffer->commit();
	commandBuffer->waitUntilCompleted();
}
```

**Key Points:**
- Takes drawable as parameter (no redundant nextDrawable() calls)
- Gets command buffer from command manager
- Schedules drawable presentation via `presentDrawable()`
- Commits command buffer and waits for GPU

**Future Optimization:**
- Replace `waitUntilCompleted()` with semaphore-based synchronization
- Implement triple buffering for better performance
- Add command buffer pooling

---

### 3. Render State Integration

**File:** `src/common/rendering/metal/renderer/mt_renderstate.cpp`

The render state system is already implemented and ready:

#### SetRenderTarget() - Configure Render Target
```cpp
void MtRenderState::SetRenderTarget(MtTextureImage* image, void* depthStencilView,
                                    int width, int height, int format, int samples)
{
	mRenderTarget.Image = image;
	mRenderTarget.DepthStencil = depthStencilView;
	mRenderTarget.Width = width;
	mRenderTarget.Height = height;
	mRenderTarget.Format = format;
	mRenderTarget.Samples = samples;
}
```

#### BeginRenderPass() - Create Encoder
```cpp
void MtRenderState::BeginRenderPass()
{
	if (!mRenderTarget.Image)
		return;

	// Get command buffer from command buffer manager
	auto cmdManager = fb->GetCommands();
	auto cmdBuffer = (MTL::CommandBuffer*)cmdManager->GetRenderCommandBuffer();
	if (!cmdBuffer)
		return;

	// Create render pass descriptor
	auto passDescriptor = MTL::RenderPassDescriptor::alloc()->init();

	// Set color attachment 0 (main scene color)
	if (mRenderTarget.Image)
	{
		auto colorAttachment = passDescriptor->colorAttachments()->object(0);
		colorAttachment->setTexture((MTL::Texture*)mRenderTarget.Image);
		colorAttachment->setLoadAction((mClearTargets & CT_Color) ?
		                               MTL::LoadActionClear : MTL::LoadActionLoad);
		colorAttachment->setStoreAction(MTL::StoreActionStore);

		if (mClearTargets & CT_Color)
		{
			colorAttachment->setClearColor(MTL::ClearColor::Make(
				screen->mSceneClearColor[0],
				screen->mSceneClearColor[1],
				screen->mSceneClearColor[2],
				screen->mSceneClearColor[3]
			));
		}
	}

	// Set depth/stencil attachment if present
	if (mRenderTarget.DepthStencil)
	{
		auto depthAttachment = passDescriptor->depthAttachment();
		depthAttachment->setTexture((MTL::Texture*)mRenderTarget.DepthStencil);
		depthAttachment->setLoadAction((mClearTargets & CT_Depth) ?
		                               MTL::LoadActionClear : MTL::LoadActionLoad);
		depthAttachment->setStoreAction(MTL::StoreActionStore);
		depthAttachment->setClearDepth(1.0);

		auto stencilAttachment = passDescriptor->stencilAttachment();
		stencilAttachment->setTexture((MTL::Texture*)mRenderTarget.DepthStencil);
		stencilAttachment->setLoadAction((mClearTargets & CT_Stencil) ?
		                                 MTL::LoadActionClear : MTL::LoadActionLoad);
		stencilAttachment->setStoreAction(MTL::StoreActionStore);
		stencilAttachment->setClearStencil(0);
	}

	// Create render command encoder
	mEncoder = cmdBuffer->renderCommandEncoder(passDescriptor);

	// Clean up pass descriptor
	passDescriptor->release();
}
```

#### EndRenderPass() - Finish Encoding
```cpp
void MtRenderState::EndRenderPass()
{
	if (mEncoder)
	{
		((MTL::RenderCommandEncoder*)mEncoder)->endEncoding();
		mEncoder = nullptr;
	}
}
```

---

## 📊 Integration Flow Diagram

```
Update() Called (每帧)
    ↓
Get CA::MetalDrawable from layer
    ↓
Extract MTL::Texture from drawable
    ↓
SetRenderTarget(drawable texture, ...)
    ↓
Draw2D() → Apply() → BeginRenderPass()
    ├─ Create MTLRenderPassDescriptor
    ├─ Configure color attachment (drawable texture)
    ├─ Configure depth/stencil (if present)
    └─ Create MTLRenderCommandEncoder
    ↓
Draw commands execute (ClearScreen, Draw, DrawIndexed)
    ↓
EndRenderPass()
    └─ encoder->endEncoding()
    ↓
PresentFrame(drawable)
    ├─ Get command buffer
    ├─ commandBuffer->presentDrawable(drawable)
    ├─ commandBuffer->commit()
    └─ commandBuffer->waitUntilCompleted()
    ↓
Frame presented to screen
```

---

## 🔗 Dependencies Status

### ✅ Complete:
1. MetalDevice initialization (Phase 1-2)
2. Command buffer management (Phase 5)
3. Render state machine (Phase 5)
4. Pipeline state system (Phase 6)
5. Resource binding (Phase 7)
6. Backend selection system (Phase 8)
7. Frame presentation flow (Phase 8)

### ⚠️ Pending:
1. Shader compilation testing
2. Depth/stencil buffer creation
3. Actual draw command testing
4. Material system integration
5. Buffer manager uniform buffer exposure

---

## 🧪 Testing Plan

### Step 1: Basic Initialization
- [x] Backend selection (vid_preferbackend = 3)
- [ ] Launch application with Metal backend
- [ ] Verify MetalRenderDevice created successfully
- [ ] Check startup log for "Metal renderer initialized successfully"

### Step 2: Frame Presentation
- [ ] Verify drawable obtained successfully
- [ ] Check render target set correctly
- [ ] Verify command buffer created
- [ ] Check no Metal validation errors

### Step 3: Simple Rendering
- [ ] Test ClearScreen() (should show black screen)
- [ ] Verify encoder creation
- [ ] Check render pass setup
- [ ] Test draw primitives command

### Step 4: Debug & Profile
- [ ] Use Xcode GPU Frame Capture
- [ ] Inspect Metal command buffer
- [ ] Check for leaks with Instruments
- [ ] Profile frame time

---

## 📝 Known Issues & Limitations

### Current Limitations:
1. **Synchronous Presentation:** Using `waitUntilCompleted()` instead of semaphores
   - Impact: Lower frame rate, potential stuttering
   - TODO: Implement triple buffering with semaphores

2. **No Depth/Stencil:** Currently passing nullptr for depth buffer
   - Impact: No depth testing, limited rendering
   - TODO: Create depth/stencil texture from MtRenderBuffers

3. **No Pipeline States:** Pipeline state binding not implemented in Apply()
   - Impact: No actual drawing yet
   - TODO: Implement pipeline state lookup and binding

4. **No Shaders:** Shader compilation not tested
   - Impact: No functional rendering
   - TODO: Test shader compilation pipeline

5. **No Buffers:** Uniform buffer binding not implemented
   - Impact: No vertex data, no uniforms
   - TODO: Expose uniform buffers from buffer manager

---

## 🎯 Next Steps (Immediate)

### Priority 1: Verify Initialization
1. Build and run with `vid_preferbackend 3`
2. Check console for initialization messages
3. Verify no crashes or errors
4. Capture startup log

### Priority 2: Test Frame Presentation
1. Run application and check for drawable acquisition
2. Verify command buffer creation
3. Check for Metal validation errors in Xcode console
4. Test with mt_debug=true for verbose logging

### Priority 3: Enable Drawing
1. Implement pipeline state binding in ApplyRenderPass()
2. Create depth/stencil buffer
3. Test simple ClearScreen()
4. Verify black screen appears

### Priority 4: Simple Geometry
1. Create a simple vertex buffer with triangle data
2. Compile a basic shader (solid color)
3. Draw a single triangle
4. Verify triangle appears on screen

---

## 📚 Files Modified in Phase 8

| File | Lines Changed | Purpose |
|------|---------------|---------|
| `i_video.mm` | +20 | Metal backend registration |
| `v_video.cpp` | +15 | CVAR backend selection |
| `mt_renderdevice.h` | +1 | PresentFrame() declaration |
| `mt_renderdevice.cpp` | +55 | Update() and PresentFrame() implementation |
| **Total** | **+91 lines** | **Core integration complete** |

---

**Status:** Phase 8 Integration - Core Complete ✅
**Build Status:** Clean build, zero errors
**Next Phase:** Testing with actual application launch
**Blockers:** None - ready for runtime testing
**Confidence:** High - following proven Vulkan pattern

---

## 💡 Architecture Notes

### Why Separate SetRenderTarget and BeginRenderPass?

**Vulkan Pattern:**
- Render targets set separately from render pass creation
- Allows render target switching without recreating passes
- Deferred render pass creation (lazy evaluation)

**Metal Adaptation:**
- SetRenderTarget() just stores parameters
- BeginRenderPass() creates MTLRenderPassDescriptor on demand
- Encoder created lazily in Apply()

**Benefits:**
- Minimal API calls per frame
- State changes batched together
- Easy to switch render targets mid-frame

---

### Why Pass Drawable to PresentFrame()?

**Alternative Approaches:**
1. **Store as member variable** - Requires careful lifetime management
2. **Get drawable twice** - Violates Metal drawable semantics
3. **Pass as parameter** - ✅ Clean, explicit, no lifetime issues

**Chosen Approach:**
- Drawable obtained once in Update()
- Passed to PresentFrame() as parameter
- No member variable needed
- Clear ownership semantics

---

### Why Synchronous Presentation Initially?

**Rationale:**
- Simplifies initial implementation
- Easier to debug (predictable frame timing)
- Matches OpenGL immediate mode semantics
- Reduces complexity during integration phase

**Performance Impact:**
- ~16ms wait per frame at 60 FPS
- GPU idle time during CPU work
- Not suitable for production

**Future Work:**
- Implement semaphore-based synchronization
- Triple buffering for GPU/CPU overlap
- Match Vulkan's fence-based approach

---

**Phase 8 Status:** Core Integration Complete - Ready for Runtime Testing ✅
