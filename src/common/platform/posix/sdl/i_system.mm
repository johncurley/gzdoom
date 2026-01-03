#include <CoreFoundation/CoreFoundation.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include "gl_sysfb.h"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

namespace Priv {
    extern SDL_Window *window;
}

void Mac_I_FatalError(const char *errortext) {
  // Close window or exit fullscreen and release mouse capture
  SDL_Quit();

  const CFStringRef errorString = CFStringCreateWithCStringNoCopy(
      kCFAllocatorDefault, errortext, kCFStringEncodingASCII, kCFAllocatorNull);
  if (NULL != errorString) {
    CFOptionFlags dummy;

    CFUserNotificationDisplayAlert(
        0, kCFUserNotificationStopAlertLevel, NULL, NULL, NULL,
        CFSTR("Fatal Error"), errorString, CFSTR("Exit"), NULL, NULL, &dummy);
    CFRelease(errorString);
  }
}

CocoaNativeHandle SystemBaseFrameBuffer::GetNativeHandle() const
{
	CocoaNativeHandle handle = {};
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (SDL_GetWindowWMInfo(Priv::window, &info))
	{
		if (info.subsystem == SDL_SYSWM_COCOA)
		{
			NSWindow* window = info.info.cocoa.window;
			handle.nsWindow = (::NSWindow*)window;
			handle.nsView = (::NSView*)[window contentView];
            
            if (handle.nsView != nil)
            {
                CALayer* layer = [handle.nsView layer];
                if (layer != nil && [layer isKindOfClass:NSClassFromString(@"CAMetalLayer")])
                {
                    handle.metalLayer = (__bridge void*)layer;
                }
            }
		}
	}
	return handle;
}

void I_UseHiDPI(bool enable)
{
    // SDL2 handles HiDPI differently or not at all depending on configuration
}

// Implement the C++-friendly function declared in metal_common.h
#include "metal/metal_common.h"

extern bool vid_hidpi;

MetalViewSize GetMetalViewDrawableSize(void* nsWindowPtr)
{
    NSWindow* const window = (__bridge NSWindow*)nsWindowPtr;
    if (window == nil) return { 0, 0 };
    NSView* const view = [window contentView];
    if (view == nil) return { 0, 0 };

    const NSSize frameSize = [view frame].size;
    const NSSize size = (vid_hidpi)
        ? [view convertSizeToBacking:frameSize]
        : frameSize;

    return { (float)size.width, (float)size.height };
}
