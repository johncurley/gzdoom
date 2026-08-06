/*
** mt_capture.cpp
** Programmatic Metal GPU frame capture, armed from the console.
**
**---------------------------------------------------------------------------
** Copyright 2026 John Curley
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
** Why this exists rather than capturing from Xcode's debug bar:
**
** Capturing from Xcode needs a second, Xcode-generator build, and the launch
** arguments that pin the viewpoint (-iwad, -file, -loadgame capspot.zds) have
** to be retyped into the scheme by hand. Getting those wrong captures a
** different viewpoint, and unlike a screenshot A/B there is no HUD or title
** bar in the trace to catch the mistake. Arming from the console keeps the
** existing one-launch-per-configuration protocol exactly as it is: cvars on
** the command line, operator types one word.
**
** A second build directory would also carry its own PSO binary archive, which
** is the last thing wanted when a stale archive masking a source change is one
** of the hypotheses under test.
*/

#ifdef __APPLE__

#include "../system/mt_renderdevice.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "i_specialpaths.h"
#include "printf.h"
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <time.h>

// mt_debug.cpp and mt_bloomdump.cpp each have an identical helper, both in
// anonymous namespaces or file scope, so it cannot be shared. Same two-line
// check, kept local for the third time.
static MetalRenderDevice *ActiveMetalDevice() {
  if (!screen || !screen->IsMetal())
    return nullptr;
  return static_cast<MetalRenderDevice *>(screen);
}

static bool gCaptureArmed = false;
static bool gCaptureRunning = false;
// Frames to wait before the capture starts. The console pauses the game and the
// world is not re-rendered while it is fully open, so this only counts down
// once the console is closed -- the same constraint the engine's `screenshot`
// command has, and the reason mt_bloom_dump takes a frame count too.
static int gCaptureFramesLeft = 0;
static FString gCapturePath;

// Whether programmatic capture is permitted at all. macOS gates this behind an
// opt-in, so without it startCapture fails at the point of use rather than at
// arming time, which would waste the launch.
static bool CaptureIsEnabled() {
  MTL::CaptureManager *mgr = MTL::CaptureManager::sharedCaptureManager();
  if (!mgr)
    return false;
  return mgr->supportsDestination(MTL::CaptureDestinationGPUTraceDocument);
}

static void PrintEnableInstructions() {
  Printf(PRINT_HIGH,
         TEXTCOLOR_YELLOW
         "Metal capture is not enabled for this process.\n" TEXTCOLOR_NORMAL
         "  Relaunch with METAL_CAPTURE_ENABLED=1 set, e.g.\n"
         "    METAL_CAPTURE_ENABLED=1 ./build/gzdoom.app/Contents/MacOS/gzdoom "
         "...\n"
         "  (the bundle's Info.plist also carries MetalCaptureEnabled, but the\n"
         "   environment variable is what works when running the binary\n"
         "   directly, which is how the capture protocol launches it.)\n");
}

void MtCaptureBeginFrameIfArmed(MetalRenderDevice *fb) {
  if (!gCaptureArmed || gCaptureRunning || !fb || !fb->device ||
      !fb->device->device)
    return;

  if (--gCaptureFramesLeft > 0)
    return;
  gCaptureArmed = false;

  MTL::CaptureManager *mgr = MTL::CaptureManager::sharedCaptureManager();
  if (!mgr || !CaptureIsEnabled()) {
    PrintEnableInstructions();
    return;
  }

  FString dir = M_GetDocumentsPath();
  dir += "gputrace";
  CreatePath(dir.GetChars());

  // Metal refuses to overwrite an existing trace, so the name carries a
  // timestamp rather than being fixed. Several captures per session is the
  // normal case when comparing configurations.
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &lt);
  gCapturePath.Format("%s/frame-%s.gputrace", dir.GetChars(), stamp);

  NS::String *pathStr = NS::String::string(gCapturePath.GetChars(),
                                           NS::StringEncoding::UTF8StringEncoding);
  NS::URL *url = NS::URL::fileURLWithPath(pathStr);

  MTL::CaptureDescriptor *desc = MTL::CaptureDescriptor::alloc()->init();
  desc->setCaptureObject(fb->device->device);
  desc->setDestination(MTL::CaptureDestinationGPUTraceDocument);
  desc->setOutputURL(url);

  NS::Error *error = nullptr;
  bool ok = mgr->startCapture(desc, &error);
  desc->release();

  if (!ok) {
    Printf(PRINT_HIGH, TEXTCOLOR_RED "Metal capture failed to start: %s\n",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
    PrintEnableInstructions();
    return;
  }

  gCaptureRunning = true;
  Printf(PRINT_HIGH, "Metal frame capture STARTED -> %s\n",
         gCapturePath.GetChars());
}

void MtCaptureEndFrameIfCapturing() {
  if (!gCaptureRunning)
    return;
  gCaptureRunning = false;

  MTL::CaptureManager *mgr = MTL::CaptureManager::sharedCaptureManager();
  if (mgr)
    mgr->stopCapture();

  Printf(PRINT_HIGH,
         TEXTCOLOR_GREEN "Metal frame capture written -> %s\n" TEXTCOLOR_NORMAL,
         gCapturePath.GetChars());
  Printf(PRINT_HIGH, "  Open it with:  open \"%s\"\n", gCapturePath.GetChars());
  Printf(PRINT_HIGH,
         "  One frame only. Find the draw you want in the frame navigator;\n"
         "  the bound pipeline state, its blend and stencil configuration, the\n"
         "  attachment load actions, and every input texture are all\n"
         "  inspectable there.\n");
}

// mt_capture [frames] -- capture one whole frame, `frames` frames from now.
//
// The delay exists for the same reason mt_bloom_dump's does: the console pauses
// the world, so the countdown advances only once it is closed. Unlike the
// engine's `screenshot` command, this captures a frame that is genuinely
// rendered with the console shut, so it does not inherit the re-present caveat
// in the capture protocol.
CCMD(mt_capture) {
  MetalRenderDevice *fb = ActiveMetalDevice();
  if (!fb) {
    Printf(PRINT_HIGH, "mt_capture: the Metal backend is not active.\n");
    return;
  }

  if (!CaptureIsEnabled()) {
    PrintEnableInstructions();
    return;
  }

  gCaptureArmed = true;
  gCaptureFramesLeft = 30;
  if (argv.argc() > 1) {
    int n = atoi(argv[1]);
    if (n > 0)
      gCaptureFramesLeft = n;
  }

  Printf(PRINT_HIGH,
         "Metal frame capture armed; it runs after %d further rendered "
         "frames.\n",
         gCaptureFramesLeft);
  Printf(PRINT_HIGH, TEXTCOLOR_YELLOW
         "  CLOSE THE CONSOLE NOW" TEXTCOLOR_NORMAL
         " -- the world does not render while it is open,\n"
         "  so the countdown does not advance and the capture never fires.\n");
  Printf(PRINT_HIGH,
         "  At this viewpoint frames are slow, so budget well over a minute.\n");
}

#endif // __APPLE__
