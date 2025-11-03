/*
**  Metal backend
**  Copyright (c) 2025 GZDoom Contributors
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
*/

// Define private implementation for metal-cpp (only in this file!)
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "mt_renderdevice.h"
#include "mt_commandbuffer.h"
#include "mt_buffer.h"
#include "mt_hwbuffer.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_resourcebinding.h"
#include "metal/shaders/mt_shader.h"
#include "metal/textures/mt_texture.h"
#include "metal/textures/mt_sampler.h"

#include "v_video.h"
#include "m_png.h"
#include "r_videoscale.h"
#include "i_time.h"
#include "v_text.h"
#include "version.h"
#include "v_draw.h"
#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_cvars.h"
#include "hw_skydome.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "engineerrors.h"
#include "c_dispatch.h"

// For accessing CocoaNativeHandle
#ifdef __APPLE__
#include <zwidget/window/cocoanativehandle.h>
#endif

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)

CVAR(Bool, mt_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

void MetalError(const char* text)
{
	throw CMetalError(text);
}

void MetalPrintLog(const char* typestr, const std::string& msg)
{
	Printf(TEXTCOLOR_RED "[Metal %s] ", typestr);
	Printf(TEXTCOLOR_WHITE "%s\n", msg.c_str());
}

MetalRenderDevice::MetalRenderDevice(void* hMonitor, bool fullscreen)
	: Super(hMonitor, fullscreen)
{
	// Create Metal device
	device = std::make_shared<MetalDevice>();
	device->device = MTL::CreateSystemDefaultDevice();

	if (!device->device)
	{
		MetalError("Failed to create Metal device - Metal may not be supported on this system");
	}

	// Create command queue
	device->commandQueue = device->device->newCommandQueue();
	if (!device->commandQueue)
	{
		MetalError("Failed to create Metal command queue");
	}

	// Get the Metal layer from the native window handle
#ifdef __APPLE__
	CocoaNativeHandle nativeHandle = GetNativeHandle();
	if (nativeHandle.metalLayer)
	{
		CA::MetalLayer* metalLayer = (CA::MetalLayer*)nativeHandle.metalLayer;
		metalLayer->setDevice(device->device);
		metalLayer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);

		// VSync will be set later via SetVSync()
		metalLayer->setDisplaySyncEnabled(false);
	}
	else
	{
		MetalError("Failed to get Metal layer from window");
	}
#endif
}

MetalRenderDevice::~MetalRenderDevice()
{
	// Wait for GPU to finish all work
	if (mCommands)
	{
		mCommands->WaitForCommands(true);
	}

	// Clean up resources
	delete mVertexData;
	delete mSkyData;
	delete mViewpoints;
	delete mLights;
	delete mBones;
	mShadowMap.Reset();

	// Release managers (in reverse order)
	mRenderState.reset();
	mPipelineStateManager.reset();
	mResourceBindingManager.reset();
	mPostprocess.reset();
	mSaveBuffers.reset();
	mScreenBuffers.reset();
	mShaderManager.reset();
	mTextureManager.reset();
	mSamplerManager.reset();
	mBufferManager.reset();
	mCommands.reset();

	// Release Metal device objects
	if (device)
	{
		if (device->commandQueue)
		{
			device->commandQueue->release();
			device->commandQueue = nullptr;
		}
		if (device->device)
		{
			device->device->release();
			device->device = nullptr;
		}
	}
}

void MetalRenderDevice::InitializeState()
{
	static bool first = true;
	if (first)
	{
		PrintStartupLog();
		first = false;
	}

	// Set vendor string from device
	const char* deviceName = device->device->name()->utf8String();
	vendorstring = deviceName;

	// Set capabilities (Metal supports shader storage buffers via argument buffers)
	hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
	glslversion = 4.50f; // Report GLSL 4.5 for shader compatibility

	// Metal has specific alignment requirements
	uniformblockalignment = 256; // Metal constant buffer alignment
	maxuniformblock = 65536; // Metal max constant buffer size

	// Initialize managers in dependency order (following Vulkan pattern)
	// Order matters! Some managers depend on others being initialized first

	Printf(TEXTCOLOR_BLUE "Initializing Metal renderer...\n");

	// 1. Command buffer manager (needed by everything)
	mCommands.reset(new MtCommandBufferManager(this));
	Printf("  - Command buffer manager initialized\n");

	// 2. Sampler manager
	mSamplerManager.reset(new MtSamplerManager(this));
	Printf("  - Sampler manager initialized\n");

	// 3. Texture manager
	mTextureManager.reset(new MtTextureManager(this));
	Printf("  - Texture manager initialized\n");

	// 4. Buffer manager (with initialization)
	mBufferManager.reset(new MtBufferManager(this));
	Printf("  - Buffer manager initialized\n");

	// 5. Render buffers (screen and save)
	mScreenBuffers.reset(new MtRenderBuffers(this));
	mSaveBuffers.reset(new MtRenderBuffers(this));
	mActiveRenderBuffers = mScreenBuffers.get();
	Printf("  - Render buffers initialized\n");

	// 6. Post-processing
	mPostprocess.reset(new MtPostprocess(this));
	Printf("  - Post-process initialized\n");

	// 7. Resource binding manager (three-tier strategy)
	mResourceBindingManager.reset(new MtResourceBindingManager(this));
	Printf("  - Resource binding manager initialized\n");

	// 8. Pipeline state manager (needs shader manager)
	mPipelineStateManager.reset(new MtPipelineStateManager(this));
	Printf("  - Pipeline state manager initialized\n");

	// 9. Shader manager (GLSL→SPIR-V→MSL pipeline)
	mShaderManager.reset(new MtShaderManager(this));
	Printf("  - Shader manager initialized\n");

	// 10. Render state (core state machine)
	mRenderState.reset(new MtRenderState(this));
	Printf("  - Render state initialized\n");

	// Initialize vertex data, lights, etc. (same as Vulkan)
	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer;
	mLights = new FLightBuffer();
	mBones = new BoneBuffer();

	Printf(TEXTCOLOR_GREEN "Metal renderer initialized successfully!\n");
	Printf("  Device: %s\n", deviceName);
}

void MetalRenderDevice::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();
	Draw2D();
	Flush3D.Unclock();

	Super::Update();
}

void MetalRenderDevice::BeginFrame()
{
	if (mCommands)
		mCommands->BeginFrame();

	if (mRenderState)
		mRenderState->BeginFrame();

	if (mResourceBindingManager)
		mResourceBindingManager->BeginFrame();
}

bool MetalRenderDevice::CompileNextShader()
{
	if (mShaderManager)
		return mShaderManager->CompileNextShader();
	return false;
}

void MetalRenderDevice::SetVSync(bool vsync)
{
	mVSync = vsync;

#ifdef __APPLE__
	CocoaNativeHandle nativeHandle = GetNativeHandle();
	if (nativeHandle.metalLayer)
	{
		CA::MetalLayer* metalLayer = (CA::MetalLayer*)nativeHandle.metalLayer;
		metalLayer->setDisplaySyncEnabled(vsync);
	}
#endif
}

void MetalRenderDevice::PrintStartupLog()
{
	const char* deviceName = device->device->name()->utf8String();

	Printf(TEXTCOLOR_CYAN "Metal Renderer for GZDoom\n");
	Printf("  Device: %s\n", deviceName);
	Printf("  API Version: Metal 2.0+\n");
	Printf("  Backend: Native Metal (metal-cpp)\n");
	Printf("\n");
}

const char* MetalRenderDevice::DeviceName() const
{
	if (device && device->device)
		return device->device->name()->utf8String();
	return "Metal Device";
}

FRenderState* MetalRenderDevice::RenderState()
{
	return mRenderState.get();
}

void MetalRenderDevice::WaitForCommands(bool finish)
{
	if (mCommands)
		mCommands->WaitForCommands(finish);
}

unsigned int MetalRenderDevice::GetLightBufferBlockSize() const
{
	// Metal uses 256-byte alignment for constant buffers
	return 256;
}

// Stubs for methods that will be implemented later
void MetalRenderDevice::PrecacheMaterial(FMaterial* mat, int translation) { }
void MetalRenderDevice::UpdatePalette() { }
void MetalRenderDevice::SetTextureFilterMode() { }
void MetalRenderDevice::StartPrecaching() { }
void MetalRenderDevice::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData) { }
void MetalRenderDevice::BlurScene(float amount) { if (mPostprocess) mPostprocess->BlurScene(amount); }
void MetalRenderDevice::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()>& afterBloomDrawEndScene2D) { if (mPostprocess) mPostprocess->PostProcessScene(swscene, fixedcm, flash, afterBloomDrawEndScene2D); }
void MetalRenderDevice::AmbientOccludeScene(float m5) { if (mPostprocess) mPostprocess->AmbientOccludeScene(m5); }
void MetalRenderDevice::SetSceneRenderTarget(bool useSSAO) { if (mPostprocess) mPostprocess->SetSceneRenderTarget(useSSAO); }
void MetalRenderDevice::SetLevelMesh(hwrenderer::LevelMesh* mesh) { }
void MetalRenderDevice::UpdateShadowMap() { if (mPostprocess) mPostprocess->UpdateShadowMap(); }
void MetalRenderDevice::SetSaveBuffers(bool yes) { mActiveRenderBuffers = yes ? mSaveBuffers.get() : mScreenBuffers.get(); }
void MetalRenderDevice::ImageTransitionScene(bool unknown) { }
void MetalRenderDevice::SetActiveRenderTarget() { }
void MetalRenderDevice::Draw2D() { }
void MetalRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc) { }
void MetalRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t* data) { }
FTexture* MetalRenderDevice::WipeStartScreen() { return nullptr; }
FTexture* MetalRenderDevice::WipeEndScreen() { return nullptr; }
TArray<uint8_t> MetalRenderDevice::GetScreenshotBuffer(int& pitch, ESSType& color_type, float& gamma) { return TArray<uint8_t>(); }

// Hardware object creation
IHardwareTexture* MetalRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new MtHardwareTexture(this, numchannels);
}

FMaterial* MetalRenderDevice::CreateMaterial(FGameTexture* tex, int scaleflags)
{
	return new FMaterial(tex, scaleflags);
}

IVertexBuffer* MetalRenderDevice::CreateVertexBuffer()
{
	return new MtVertexBuffer(this);
}

IIndexBuffer* MetalRenderDevice::CreateIndexBuffer()
{
	return new MtIndexBuffer(this);
}

IDataBuffer* MetalRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return new MtHardwareDataBuffer(this, bindingpoint, ssbo, needsresize);
}
