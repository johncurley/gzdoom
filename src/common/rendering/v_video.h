/*
** v_video.h
**
**---------------------------------------------------------------------------
** Copyright 1998-2008 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#ifndef __V_VIDEO_H__
#define __V_VIDEO_H__

#include <functional>
#include <vector>
#include <chrono>
#include "basics.h"
#include "hwrenderer/frame/hw_resources.h"
#include "vectors.h"
#include "m_png.h"
#include "renderstyle.h"
#include "c_cvars.h"
#include "v_2ddrawer.h"
#include "intrect.h"
#include "hw_shadowmap.h"
#include "hw_levelmesh.h"
#include "buffers.h"
#include "files.h"


struct FPortalSceneState;
class FSkyVertexBuffer;
class IIndexBuffer;
class IVertexBuffer;
class IDataBuffer;
class FFlatVertexBuffer;
class HWViewpointBuffer;
class FLightBuffer;
struct HWDrawInfo;
struct HWViewpointUniforms;
class FMaterial;
class FGameTexture;
class FRenderState;
class BoneBuffer;

enum EHWCaps
{
	// [BB] Added texture compression flags.
	RFL_TEXTURE_COMPRESSION = 1,
	RFL_TEXTURE_COMPRESSION_S3TC = 2,

	RFL_SHADER_STORAGE_BUFFER = 4,
	RFL_BUFFER_STORAGE = 8,

	RFL_NO_CLIP_PLANES = 32,

	RFL_INVALIDATE_BUFFER = 64,
	RFL_DEBUG = 128,
};


extern int DisplayWidth, DisplayHeight;

void V_UpdateModeSize (int width, int height);
void V_OutputResized (int width, int height);

EXTERN_CVAR(Bool, vid_fullscreen)
EXTERN_CVAR(Int, win_x)
EXTERN_CVAR(Int, win_y)
EXTERN_CVAR(Int, win_w)
EXTERN_CVAR(Int, win_h)
EXTERN_CVAR(Bool, win_maximized)

struct FColormap;
enum FTextureFormat : uint32_t;
class FModelRenderer;
struct SamplerUniform;

//
// VIDEO
//
//
class DCanvas
{
public:
	DCanvas (int width, int height, bool bgra);
	~DCanvas ();
	void Resize(int width, int height, bool optimizepitch = true);

	// Member variable access
	inline uint8_t *GetPixels () const { return Pixels.Data(); }
	inline int GetWidth () const { return Width; }
	inline int GetHeight () const { return Height; }
	inline int GetPitch () const { return Pitch; }
	inline bool IsBgra() const { return Bgra; }

protected:
	TArray<uint8_t> Pixels;
	int Width;
	int Height;
	int Pitch;
	bool Bgra;
};

class IHardwareTexture;
class FTexture;


class DFrameBuffer
{
private:
	int Width = 0;
	int Height = 0;

public:
	// Hardware render state that needs to be exposed to the API independent part of the renderer. For ease of access this is stored in the base class.
	int hwcaps = 0;								// Capability flags
	float glslversion = 0;						// This is here so that the differences between old OpenGL and new OpenGL/Vulkan can be handled by platform independent code.
	int instack[2] = { 0,0 };					// this is globally maintained state for portal recursion avoidance.
	int stencilValue = 0;						// Global stencil test value
	unsigned int uniformblockalignment = 256;	// Hardware dependent uniform buffer alignment.
	unsigned int maxuniformblock = 65536;
	const char *vendorstring;					// We have to account for some issues with particular vendors.
	FSkyVertexBuffer *mSkyData = nullptr;		// the sky vertex buffer
	FFlatVertexBuffer *mVertexData = nullptr;	// Global vertex data
	HWViewpointBuffer *mViewpoints = nullptr;	// Viewpoint render data.
	FLightBuffer *mLights = nullptr;			// Dynamic lights
	BoneBuffer* mBones = nullptr;				// Model bones
	IShadowMap mShadowMap;
	FrameResources mResources;					// frame graph resource registry (phase 1: record + report)

	int mGameScreenWidth = 0;
	int mGameScreenHeight = 0;
	IntRect mScreenViewport;
	IntRect mSceneViewport;
	IntRect mOutputLetterbox;
	float mSceneClearColor[4]{ 0,0,0,255 };

	int mPipelineNbr = 1;						// Number of HW buffers to pipeline
	int mPipelineType = 0;

public:
	DFrameBuffer (int width=1, int height=1);
	virtual ~DFrameBuffer();
	virtual void InitializeState() = 0;	// For stuff that needs 'screen' set.
	virtual bool IsVulkan() { return false; }
	virtual bool IsMetal() { return false; }
	virtual bool IsPoly() { return false; }
	virtual int GetShaderCount();
	virtual bool CompileNextShader() { return true; }
	void SetAABBTree(hwrenderer::LevelAABBTree * tree)
	{
		mShadowMap.SetAABBTree(tree);
	}
	FrameResources &Resources() { return mResources; }
	virtual void SetLevelMesh(hwrenderer::LevelMesh *mesh) { }
	bool allowSSBO() const
	{
#ifndef HW_BLOCK_SSBO
		return true;
#else
		return mPipelineType == 0;
#endif
	}

	// SSBOs have quite worse performance for read only data, so keep this around only as long as Vulkan has not been adapted yet.
	bool useSSBO() 
	{
		return IsVulkan();
	}

	virtual DCanvas* GetCanvas() { return nullptr; }

	void SetSize(int width, int height);
	void SetVirtualSize(int width, int height)
	{
		Width = width;
		Height = height;
	}
	inline int GetWidth() const { return Width; }
	inline int GetHeight() const { return Height; }

	FVector2 SceneScale() const
	{
		return { mSceneViewport.width / (float)mScreenViewport.width, mSceneViewport.height / (float)mScreenViewport.height };
	}

	FVector2 SceneOffset() const
	{
		return { mSceneViewport.left / (float)mScreenViewport.width, mSceneViewport.top / (float)mScreenViewport.height };
	}

	// Make the surface visible.
	virtual void Update ();

	// Stores the palette with flash blended in into 256 dwords
	// Mark the palette as changed. It will be updated on the next Update().
	virtual void UpdatePalette() {}

	// Returns true if running fullscreen.
	virtual bool IsFullscreen () = 0;
	virtual void ToggleFullscreen(bool yes) {}

	// Changes the vsync setting, if supported by the device.
	virtual void SetVSync (bool vsync);

	// Delete any resources that need to be deleted after restarting with a different IWAD
	virtual void SetTextureFilterMode() {}
	virtual IHardwareTexture *CreateHardwareTexture(int numchannels) { return nullptr; }
	virtual void PrecacheMaterial(FMaterial *mat, int translation) {}
	virtual FMaterial* CreateMaterial(FGameTexture* tex, int scaleflags);
	virtual void BeginFrame() {}
	virtual void SetWindowSize(int w, int h) {}
	virtual void StartPrecaching() {}
	virtual FRenderState* RenderState() { return nullptr; }

	virtual int GetClientWidth() = 0;
	virtual int GetClientHeight() = 0;
	virtual void BlurScene(float amount) {}

	virtual void InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData) {}

    // Interface to hardware rendering resources
	virtual IVertexBuffer *CreateVertexBuffer() { return nullptr; }
	virtual IIndexBuffer *CreateIndexBuffer() { return nullptr; }
	virtual IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) { return nullptr; }
	bool BuffersArePersistent() { return !!(hwcaps & RFL_BUFFER_STORAGE); }

	// This is overridable in case Vulkan does it differently.
	virtual bool RenderTextureIsFlipped() const
	{
		return true;
	}

	virtual bool UseBottomLeft2DProjection() const
	{
		return false;
	}

	virtual bool IsReverseZ() const
	{
		return false;
	}

	// Report a game restart
	void SetClearColor(int color);
	virtual int Backend() { return 0; }
	virtual const char* DeviceName() const { return "Unknown"; }
	// currentViewpoint: the caller's own HWViewpointUniforms (VPUniforms),
	// passed directly rather than relying on a shared "last set viewpoint"
	// value on the render device -- that shared value is NOT reliably the
	// current view's by the time AmbientOccludeScene runs (sky/skybox
	// rendering, which happens earlier in the same DrawScene() call, sets
	// its own viewpoint and does not restore the caller's afterward). Only
	// Metal's world-locked AO noise currently uses this -- GL/Vulkan ignore
	// it, since neither backend's SSAO needs world-space reconstruction.
	virtual void AmbientOccludeScene(float m5, const HWViewpointUniforms* currentViewpoint) {}
	virtual void FirstEye() {}
	virtual void NextEye(int eyecount) {}
	virtual void SetSceneRenderTarget(bool useSSAO) {}
	virtual void UpdateShadowMap() {}
	virtual void WaitForCommands(bool finish) {}
	virtual void SetSaveBuffers(bool yes) {}
	virtual void ImageTransitionScene(bool unknown) {}
	virtual void CopyScreenToBuffer(int width, int height, uint8_t* buffer)	{ memset(buffer, 0, width* height); }
	virtual bool FlipSavePic() const { return false; }
	virtual void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc) {}
	virtual void SetActiveRenderTarget() {}

	// Screen wiping
	virtual FTexture *WipeStartScreen();
	virtual FTexture *WipeEndScreen();

	virtual void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) { if (afterBloomDrawEndScene2D) afterBloomDrawEndScene2D(); }

	void ScaleCoordsFromWindow(int16_t &x, int16_t &y);

	virtual void Draw2D() {}

	virtual void SetViewportRects(IntRect *bounds);
	int ScreenToWindowX(int x);
	int ScreenToWindowY(int y);

	void FPSLimit();

	// Retrieves a buffer containing image data for a screenshot.
	// Hint: Pitch can be negative for upside-down images, in which case buffer
	// points to the last row in the buffer, which will be the first row output.
	virtual TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) { return TArray<uint8_t>(); }

	// Ask the backend to keep a copy of the NEXT frame it presents, so a later
	// GetScreenshotBuffer() has something valid to return.
	//
	// GetScreenshotBuffer() runs after the buffer swap, and after a swap the
	// window back buffer is undefined -- EGL defaults to
	// EGL_SWAP_BEHAVIOR = EGL_BUFFER_DESTROYED and GLX promises nothing either.
	// The OpenGL backend reads that back buffer, so on Linux every capture came
	// out solid black. Measured 2026-08-10 through tools/matrix/crossbackend.py
	// on AshesHardReset save01.zds: mean 0.000 without this, mean 25.318 with
	// it, against Vulkan's 25.366 on the same frame.
	//
	// Vulkan and Metal do not need it -- they re-present the last frame into
	// their own image rather than touching the swapchain -- so it is a no-op
	// there, and a caller that never arms gets the old behaviour unchanged.
	virtual void ArmScreenshotCapture() {}

	virtual float GetZNear() const { return 5.f; }
	virtual float GetZFar() const { return 65536.f; }

	// The original size of the framebuffer as selected in the video menu.
	uint64_t FrameTime = 0;

private:
	uint64_t fpsLimitTime = 0;

	bool isIn2D = false;

	// vid_frametrace: per-frame wall-clock interval trace during actual play,
	// backend-agnostic (called from Update(), which every backend's own
	// override chains to via Super::Update()). See TraceFrameInterval() in
	// v_framebuffer.cpp for why this exists and why it reports percentiles
	// rather than a mean.
	std::vector<float> mFrameTraceSamples;
	std::chrono::steady_clock::time_point mFrameTraceWindowStart;
	std::chrono::steady_clock::time_point mFrameTraceLastFrame;
	bool mFrameTraceStarted = false;
	void TraceFrameInterval();
};


// vid_stalltrace: scoped timer naming one phase of the game loop, so an
// interval that vid_frametrace flags as a hitch can be attributed to playsim,
// level load, sound, display or I/O rather than merely reported as slow.
//
// Place one around each phase in D_DoomLoop. Cost when vid_stalltrace is 0 is a
// single int compare -- no clock read, no accumulation.
//
//    { VLoopPhase _p("playsim"); TryRunTics(); }
//
// The name must outlive the object and should be a string literal; the bucket
// table compares by pointer.
class VLoopPhase
{
public:
	// wrapper: this phase CONTAINS other phases (e.g. "display" contains
	// "beginframe"). Wrappers are printed but excluded from the accounted
	// total, which would otherwise double-count and drive (unaccounted)
	// negative -- observed at -1007.50ms before this existed.
	explicit VLoopPhase(const char *name, bool wrapper = false);
	~VLoopPhase();

	VLoopPhase(const VLoopPhase &) = delete;
	VLoopPhase &operator=(const VLoopPhase &) = delete;

private:
	const char *mName = nullptr;
	bool mWrapper = false;
	std::chrono::steady_clock::time_point mStart;
};

// vid_stalltrace: marks a region that drives screen->Update() from its own
// inner loop instead of returning to D_DoomLoop -- the screen wipe, the startup
// screen, a resize pump. Intervals measured inside such a region contain no
// VLoopPhase at all, so without this they report as 100% unaccounted and look
// like a hole in the instrument rather than the answer.
class VLoopContext
{
public:
	explicit VLoopContext(const char *name);
	~VLoopContext();

	VLoopContext(const VLoopContext &) = delete;
	VLoopContext &operator=(const VLoopContext &) = delete;

private:
	const char *mPrevious = nullptr;
};

// vid_stalltrace: call once per game-loop iteration, from a point where no
// VLoopPhase is open. Measures the iteration and reports the phase breakdown if
// it exceeded the threshold. See V_LoopTraceBoundary() in v_framebuffer.cpp for
// why reporting cannot happen inside Update().
void V_LoopTraceBoundary();

// This is the screen updated by I_FinishUpdate.
extern DFrameBuffer *screen;

#define SCREENWIDTH (screen->GetWidth ())
#define SCREENHEIGHT (screen->GetHeight ())

EXTERN_CVAR (Float, vid_gamma)


// Allocates buffer screens, call before R_Init.
void V_InitScreenSize();
void V_InitScreen();

// Initializes graphics mode for the first time.
void V_Init2 ();

void V_Shutdown ();
int V_GetBackend();

inline bool IsRatioWidescreen(int ratio) { return (ratio & 3) != 0; }
extern bool setsizeneeded, setmodeneeded;


#endif // __V_VIDEO_H__
