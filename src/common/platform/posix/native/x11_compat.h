#ifndef __NATIVE_X11_COMPAT_H__
#define __NATIVE_X11_COMPAT_H__

#define GC XGC
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>
#undef GC

#endif
