/*
** The base framebuffer class
**
**---------------------------------------------------------------------------
** Copyright 1999-2016 Randy Heit
** Copyright 2005-2018 Christoph Oelckers
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

#include <stdio.h>


#include "v_video.h"

#include "c_dispatch.h"
#include "hardware.h"
#include "r_videoscale.h"
#include "i_time.h"
#include "v_font.h"
#include "v_draw.h"
#include "i_time.h"
#include "v_2ddrawer.h"
#include "vm.h"
#include "i_interface.h"
#include "flatvertices.h"
#include "version.h"
#include "hw_material.h"

#include <algorithm>
#include <chrono>
#include <thread>


CVAR(Bool, gl_scale_viewport, true, CVAR_ARCHIVE);

EXTERN_CVAR(Int, vid_maxfps)
CVAR(Bool, cl_capfps, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Int, screenblocks)

// Frame-interval trace for ACTUAL GAMEPLAY, in seconds between reports (0 = off).
// Backend-agnostic counterpart to the Metal-only mt_frametrace
// (mt_debug.cpp): this hooks DFrameBuffer::Update(), which every backend's
// own Update() override chains to via Super::Update(), so GL and Vulkan get
// the same instrument for free without a separate per-backend copy.
//
// Exists for the same reason mt_frametrace does: a benchmark harness sampling
// a settled viewpoint cannot see hitching that only happens while the camera
// moves in real play, and a mean/max pair cannot distinguish "one startup
// stall" from "stutters constantly" -- see mt_debug.cpp's mt_frametrace
// comment for the measured case that motivated it. Percentiles instead: p50
// stays healthy while p99 blows out, which is what hitching looks like.
//
// stderr, not Printf: this runs during play, where console output covers the
// screen it is measuring.
CVAR(Int, vid_frametrace, 0, 0)

//==========================================================================
//
// DFrameBuffer Constructor
//
// A frame buffer canvas is the most common and represents the image that
// gets drawn to the screen.
//
//==========================================================================

DFrameBuffer::DFrameBuffer (int width, int height)
{
	SetSize(width, height);
}

DFrameBuffer::~DFrameBuffer()
{
}

void DFrameBuffer::SetSize(int width, int height)
{
	Width = ViewportScaledWidth(width, height);
	Height = ViewportScaledHeight(width, height);
}

//==========================================================================
//
// Palette stuff.
//
//==========================================================================

void DFrameBuffer::Update()
{
	int initialWidth = GetClientWidth();
	int initialHeight = GetClientHeight();
	int clientWidth = ViewportScaledWidth(initialWidth, initialHeight);
	int clientHeight = ViewportScaledHeight(initialWidth, initialHeight);
	if (clientWidth < VID_MIN_WIDTH) clientWidth = VID_MIN_WIDTH;
	if (clientHeight < VID_MIN_HEIGHT) clientHeight = VID_MIN_HEIGHT;
	if (clientWidth > 0 && clientHeight > 0 && (GetWidth() != clientWidth || GetHeight() != clientHeight))
	{
		SetVirtualSize(clientWidth, clientHeight);
		SetViewportRects(nullptr);
		V_OutputResized(clientWidth, clientHeight);
		mVertexData->OutputResized(clientWidth, clientHeight);
	}

	TraceFrameInterval();
}

// One frame's wall-clock interval, self-measured with steady_clock since the
// base class is not handed a frame time by its callers. Accumulates until
// vid_frametrace seconds have passed, then reports the distribution and
// starts a fresh window -- so a long session reads as a series of
// independent windows rather than an average that slowly buries a bad patch.
// Samples are unbounded within a window on purpose: a display-average ring
// would discard exactly the frames a hitching report needs.
void DFrameBuffer::TraceFrameInterval()
{
	const int period = vid_frametrace;
	if (period <= 0)
	{
		if (!mFrameTraceSamples.empty()) mFrameTraceSamples.clear();
		mFrameTraceStarted = false;
		return;
	}

	using clock = std::chrono::steady_clock;
	const auto now = clock::now();

	if (!mFrameTraceStarted)
	{
		mFrameTraceStarted = true;
		mFrameTraceWindowStart = now;
		mFrameTraceLastFrame = now;
		mFrameTraceSamples.clear();
		fprintf(stderr,
			"vid_frametrace: reporting every %ds. p99 and the >100ms count are "
			"the hitching signal; avg is not.\n", period);
		return;
	}

	const float frameTimeMs = std::chrono::duration<float, std::milli>(now - mFrameTraceLastFrame).count();
	mFrameTraceLastFrame = now;
	mFrameTraceSamples.push_back(frameTimeMs);

	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - mFrameTraceWindowStart).count();
	if (elapsed < (long long)period * 1000 || mFrameTraceSamples.size() < 2)
		return;

	std::vector<float> sorted = mFrameTraceSamples;
	std::sort(sorted.begin(), sorted.end());
	auto pct = [&sorted](double p) {
		const size_t i = (size_t)(p * (double)(sorted.size() - 1) + 0.5);
		return sorted[i < sorted.size() ? i : sorted.size() - 1];
	};

	double total = 0.0;
	size_t over33 = 0, over100 = 0;
	for (float v : mFrameTraceSamples)
	{
		total += v;
		if (v > 33.3f) ++over33;
		if (v > 100.0f) ++over100;
	}
	const double mean = total / (double)mFrameTraceSamples.size();

	fprintf(stderr,
		"vid_frametrace  n=%zu  avg=%6.2fms (%5.1f fps)  p50=%6.2f  p95=%6.2f  "
		"p99=%6.2f  max=%7.2f  >33ms=%zu (%.1f%%)  >100ms=%zu\n",
		mFrameTraceSamples.size(), mean, mean > 0.0 ? 1000.0 / mean : 0.0,
		pct(0.50), pct(0.95), pct(0.99), sorted.back(), over33,
		100.0 * (double)over33 / (double)mFrameTraceSamples.size(), over100);

	mFrameTraceSamples.clear();
	mFrameTraceWindowStart = now;
}

void DFrameBuffer::SetClearColor(int color)
{
	PalEntry pe = GPalette.BaseColors[color];
	mSceneClearColor[0] = pe.r / 255.f;
	mSceneClearColor[1] = pe.g / 255.f;
	mSceneClearColor[2] = pe.b / 255.f;
	mSceneClearColor[3] = 1.f;
}

//==========================================================================
//
// DFrameBuffer :: SetVSync
//
// Turns vertical sync on and off, if supported.
//
//==========================================================================

void DFrameBuffer::SetVSync (bool vsync)
{
}

//==========================================================================
//
// DFrameBuffer :: WipeStartScreen
//
// Grabs a copy of the screen currently displayed to serve as the initial
// frame of a screen wipe. Also determines which screenwipe will be
// performed.
//
//==========================================================================

FTexture *DFrameBuffer::WipeStartScreen()
{
	return nullptr;
}

//==========================================================================
//
// DFrameBuffer :: WipeEndScreen
//
// Grabs a copy of the most-recently drawn, but not yet displayed, screen
// to serve as the final frame of a screen wipe.
//
//==========================================================================

FTexture *DFrameBuffer::WipeEndScreen()
{
    return nullptr;
}

//==========================================================================
//
// Calculates the viewport values needed for 2D and 3D operations
//
//==========================================================================

void DFrameBuffer::SetViewportRects(IntRect *bounds)
{
	if (bounds)
	{
		mSceneViewport = *bounds;
		mScreenViewport = *bounds;
		mOutputLetterbox = *bounds;
		mGameScreenWidth = mScreenViewport.width;
		mGameScreenHeight = mScreenViewport.height;
		return;
	}

	// Back buffer letterbox for the final output
	int clientWidth = GetClientWidth();
	int clientHeight = GetClientHeight();
	if (clientWidth == 0 || clientHeight == 0)
	{
		// When window is minimized there may not be any client area.
		// Pretend to the rest of the render code that we just have a very small window.
		clientWidth = 160;
		clientHeight = 120;
	}
	int screenWidth = GetWidth();
	int screenHeight = GetHeight();
	float scaleX, scaleY;
	scaleX = min(clientWidth / (float)screenWidth, clientHeight / ((float)screenHeight * ViewportPixelAspect()));
	scaleY = scaleX * ViewportPixelAspect();
	mOutputLetterbox.width = (int)round(screenWidth * scaleX);
	mOutputLetterbox.height = (int)round(screenHeight * scaleY);
	mOutputLetterbox.left = (clientWidth - mOutputLetterbox.width) / 2;
	mOutputLetterbox.top = (clientHeight - mOutputLetterbox.height) / 2;

	// The entire renderable area, including the 2D HUD
	mScreenViewport.left = 0;
	mScreenViewport.top = 0;
	mScreenViewport.width = screenWidth;
	mScreenViewport.height = screenHeight;

	// Viewport for the 3D scene
	if (sysCallbacks.GetSceneRect) mSceneViewport = sysCallbacks.GetSceneRect();
	else mSceneViewport = mScreenViewport;

	// Scale viewports to fit letterbox
	bool notScaled = ((mScreenViewport.width == ViewportScaledWidth(mScreenViewport.width, mScreenViewport.height)) &&
		(mScreenViewport.width == ViewportScaledHeight(mScreenViewport.width, mScreenViewport.height)) &&
		(ViewportPixelAspect() == 1.0));
	if (gl_scale_viewport && !IsFullscreen() && notScaled)
	{
		mScreenViewport.width = mOutputLetterbox.width;
		mScreenViewport.height = mOutputLetterbox.height;
		mSceneViewport.left = (int)round(mSceneViewport.left * scaleX);
		mSceneViewport.top = (int)round(mSceneViewport.top * scaleY);
		mSceneViewport.width = (int)round(mSceneViewport.width * scaleX);
		mSceneViewport.height = (int)round(mSceneViewport.height * scaleY);
	}

	mGameScreenWidth = GetWidth();
	mGameScreenHeight = GetHeight();
}

//===========================================================================
// 
// Calculates the OpenGL window coordinates for a zdoom screen position
//
//===========================================================================

int DFrameBuffer::ScreenToWindowX(int x)
{
	return mScreenViewport.left + (int)round(x * mScreenViewport.width / (float)mGameScreenWidth);
}

int DFrameBuffer::ScreenToWindowY(int y)
{
	return mScreenViewport.top + mScreenViewport.height - (int)round(y * mScreenViewport.height / (float)mGameScreenHeight);
}

void DFrameBuffer::ScaleCoordsFromWindow(int16_t &x, int16_t &y)
{
	int letterboxX = mOutputLetterbox.left;
	int letterboxY = mOutputLetterbox.top;
	int letterboxWidth = mOutputLetterbox.width;
	int letterboxHeight = mOutputLetterbox.height;

	x = int16_t((x - letterboxX) * Width / letterboxWidth);
	y = int16_t((y - letterboxY) * Height / letterboxHeight);
}

void DFrameBuffer::FPSLimit()
{
	using namespace std::chrono;
	using namespace std::this_thread;

	if (vid_maxfps <= 0 || cl_capfps)
		return;

	uint64_t targetWakeTime = fpsLimitTime + 1'000'000 / vid_maxfps;

	while (true)
	{
		fpsLimitTime = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
		int64_t timeToWait = targetWakeTime - fpsLimitTime;

		if (timeToWait > 1'000'000 || timeToWait <= 0)
		{
			break;
		}

		if (timeToWait <= 2'000)
		{
			// We are too close to the deadline. OS sleep is not precise enough to wake us before it elapses.
			// Yield execution and check time again.
			sleep_for(nanoseconds(0));
		}
		else
		{
			// Sleep, but try to wake before deadline.
			sleep_for(microseconds(timeToWait - 2'000));
		}
	}
}

FMaterial* DFrameBuffer::CreateMaterial(FGameTexture* tex, int scaleflags)
{
	return new FMaterial(tex, scaleflags);
}


//==========================================================================
//
// ZScript wrappers for inlines
//
//==========================================================================

static int ScreenGetWidth() { return twod->GetWidth(); }
static int ScreenGetHeight() { return twod->GetHeight(); }

DEFINE_ACTION_FUNCTION_NATIVE(_Screen, GetWidth, ScreenGetWidth)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(twod->GetWidth());
}

DEFINE_ACTION_FUNCTION_NATIVE(_Screen, GetHeight, ScreenGetHeight)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(twod->GetHeight());
}

DEFINE_ACTION_FUNCTION(_Screen, PaletteColor)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	if (index < 0 || index > 255) index = 0;
	else index = GPalette.BaseColors[index];
	ACTION_RETURN_INT(index);
}
