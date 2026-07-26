#include "gl_sysfb.h"
#include "x11_compat.h"

// glad/glad.h MUST come before any EGL or GLX headers so its inclusion
// guard (__gl_h_) prevents them from pulling in a conflicting <GL/gl.h>.
#include <glad/glad.h>

#ifdef HAVE_WAYLAND_EGL
// Protect against EGL including X11 headers without protection
#define GC XGC
#include <glad/glad_egl.h>
#include <wayland-egl.h>
#undef GC

#ifndef EGL_CAST
#define EGL_CAST(type, value) ((type)(value))
#endif
#endif

// Pull in GLX types only — GL is already provided by glad above.
#define GLX_GLXEXT_LEGACY
#include <GL/glx.h>

#include "c_cvars.h"
#include "flatvertices.h"
#include "gl/gl_buffers.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#undef None
#include <zwidget/window/window.h>

#ifndef EGL_CONTEXT_MAJOR_VERSION
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#endif
#ifndef EGL_CONTEXT_MINOR_VERSION
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#endif
#ifndef EGL_CONTEXT_OPENGL_PROFILE_MASK
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#endif
#ifndef EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR 0x00000001
#endif

typedef void (*PFN_GLXSWAPBUFFERS)(Display*, GLXDrawable);
typedef XVisualInfo* (*PFN_GLXCHOOSEVISUAL)(Display*, int, int*);
typedef GLXContext (*PFN_GLXCREATECONTEXT)(Display*, XVisualInfo*, GLXContext, Bool);
typedef Bool (*PFN_GLXMAKECURRENT)(Display*, GLXDrawable, GLXContext);

// X11 functions
typedef Status (*PFN_XGETWINDOWATTRIBUTES)(Display*, Window, XWindowAttributes*);
typedef int (*PFN_XDEFAULTSCREEN)(Display*);
typedef int (*PFN_XFREE)(void*);

static PFN_GLXSWAPBUFFERS zd_glXSwapBuffers = nullptr;
static PFN_GLXCHOOSEVISUAL zd_glXChooseVisual = nullptr;
static PFN_GLXCREATECONTEXT zd_glXCreateContext = nullptr;
static PFN_GLXMAKECURRENT zd_glXMakeCurrent = nullptr;

static PFN_XGETWINDOWATTRIBUTES zd_XGetWindowAttributes = nullptr;
static PFN_XDEFAULTSCREEN zd_XDefaultScreen = nullptr;
static PFN_XFREE zd_XFree = nullptr;

static bool InitGLX() {
    static bool initialized = false;
    static bool result = false;
    if (initialized) return result;
    initialized = true;

    void* gl_lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!gl_lib) gl_lib = dlopen("libGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!gl_lib) return false;

    zd_glXSwapBuffers = (PFN_GLXSWAPBUFFERS)dlsym(gl_lib, "glXSwapBuffers");
    zd_glXChooseVisual = (PFN_GLXCHOOSEVISUAL)dlsym(gl_lib, "glXChooseVisual");
    zd_glXCreateContext = (PFN_GLXCREATECONTEXT)dlsym(gl_lib, "glXCreateContext");
    zd_glXMakeCurrent = (PFN_GLXMAKECURRENT)dlsym(gl_lib, "glXMakeCurrent");

    void* x11_lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (x11_lib) {
        zd_XGetWindowAttributes = (PFN_XGETWINDOWATTRIBUTES)dlsym(x11_lib, "XGetWindowAttributes");
        zd_XDefaultScreen = (PFN_XDEFAULTSCREEN)dlsym(x11_lib, "XDefaultScreen");
        zd_XFree = (PFN_XFREE)dlsym(x11_lib, "XFree");
    }

    if (!zd_glXSwapBuffers || !zd_glXChooseVisual || !zd_glXCreateContext || !zd_glXMakeCurrent) return false;
    result = true;
    return true;
}


extern Display *X11NativeDisplay;

EXTERN_CVAR(Int, vid_defwidth)
EXTERN_CVAR(Int, vid_defheight)
EXTERN_CVAR(Bool, vid_vsync)

SystemBaseFrameBuffer::SystemBaseFrameBuffer(void *hMonitor, bool fullscreen)
    : DFrameBuffer(640, 480) {}
SystemBaseFrameBuffer::SystemBaseFrameBuffer() : DFrameBuffer(640, 480) {}

void SystemBaseFrameBuffer::ToggleFullscreen(bool yes) {
  // Native backend uses ZWidget for window state. Map fullscreen toggles to
  // xdg_toplevel.set_fullscreen / X11 EWMH equivalents via DisplayWindow.
  if (auto *win = GetActiveZWidgetWindow()) {
    if (yes)
      win->ShowFullscreen();
    else
      win->ShowNormal();
  }
}

void SystemBaseFrameBuffer::SetWindowSize(int client_w, int client_h) {
  // Keep the actual window dimensions in sync with the engine's render size.
  // On Wayland, "window size" is negotiated via configure, but ZWidget will
  // request the size and update buffers on the next commit.
  if (client_w <= 0 || client_h <= 0)
    return;

  // Persist the requested size for the next startup (native backend has no
  // win_w/win_h vars).
  vid_defwidth = client_w;
  vid_defheight = client_h;

  if (auto *win = GetActiveZWidgetWindow()) {
    Rect rect = win->GetWindowFrame();
    rect.width = client_w;
    rect.height = client_h;
    win->SetClientFrame(rect);
  }
}

void SystemGLFrameBuffer::InitializeState() {}
IIndexBuffer *SystemGLFrameBuffer::CreateIndexBuffer() {
  return new OpenGLRenderer::GLIndexBuffer;
}
IVertexBuffer *SystemGLFrameBuffer::CreateVertexBuffer() {
  return new OpenGLRenderer::GLVertexBuffer;
}
IDataBuffer *SystemGLFrameBuffer::CreateDataBuffer(int bindingpoint, bool ssbo,
                                                   bool needsresize) {
  return new OpenGLRenderer::GLDataBuffer(bindingpoint, ssbo);
}

#include <fstream>
void LogToDisk(const char *msg) {
  std::ofstream f("/tmp/gzdoom_debug.log", std::ios::app);
  if (f.is_open())
    f << msg << std::endl;
}

#ifdef HAVE_WAYLAND_EGL

typedef EGLDisplay (*PFN_EGLGETDISPLAY)(EGLNativeDisplayType display_id);
typedef EGLBoolean (*PFN_EGLINITIALIZE)(EGLDisplay dpy, EGLint *major, EGLint *minor);
typedef EGLBoolean (*PFN_EGLCHOOSECONFIG)(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config);
typedef EGLBoolean (*PFN_EGLBINDAPI)(EGLenum api);
typedef EGLContext (*PFN_EGLCREATECONTEXT)(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list);
typedef EGLint (*PFN_EGLGETERROR)(void);
typedef EGLSurface (*PFN_EGLCREATEWINDOWSURFACE)(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list);
typedef EGLBoolean (*PFN_EGLMAKECURRENT)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
typedef EGLBoolean (*PFN_EGLSWAPINTERVAL)(EGLDisplay dpy, EGLint interval);
typedef EGLBoolean (*PFN_EGLDESTROYCONTEXT)(EGLDisplay dpy, EGLContext ctx);
typedef EGLBoolean (*PFN_EGLDESTROYSURFACE)(EGLDisplay dpy, EGLSurface surface);

// Wayland-EGL functions
struct wl_egl_window;
struct wl_surface;
typedef struct wl_egl_window* (*PFN_WLEGLWINDOWCREATE)(struct wl_surface* surface, int width, int height);
typedef void (*PFN_WLEGLWINDOWDESTROY)(struct wl_egl_window* window);
typedef void (*PFN_WLEGLWINDOWRESIZE)(struct wl_egl_window* window, int width, int height, int dx, int dy);

static PFN_WLEGLWINDOWCREATE zd_wl_egl_window_create = nullptr;
static PFN_WLEGLWINDOWDESTROY zd_wl_egl_window_destroy = nullptr;
static PFN_WLEGLWINDOWRESIZE zd_wl_egl_window_resize = nullptr;

typedef EGLBoolean (*PFN_EGLTERMINATE)(EGLDisplay dpy);
typedef EGLBoolean (*PFN_EGLSWAPBUFFERS)(EGLDisplay dpy, EGLSurface surface);

static PFN_EGLGETDISPLAY zd_eglGetDisplay = nullptr;
static PFN_EGLINITIALIZE zd_eglInitialize = nullptr;
static PFN_EGLCHOOSECONFIG zd_eglChooseConfig = nullptr;
static PFN_EGLBINDAPI zd_eglBindAPI = nullptr;
static PFN_EGLCREATECONTEXT zd_eglCreateContext = nullptr;
static PFN_EGLGETERROR zd_eglGetError = nullptr;
static PFN_EGLCREATEWINDOWSURFACE zd_eglCreateWindowSurface = nullptr;
static PFN_EGLMAKECURRENT zd_eglMakeCurrent = nullptr;
static PFN_EGLSWAPINTERVAL zd_eglSwapInterval = nullptr;
static PFN_EGLDESTROYCONTEXT zd_eglDestroyContext = nullptr;
static PFN_EGLDESTROYSURFACE zd_eglDestroySurface = nullptr;
static PFN_EGLTERMINATE zd_eglTerminate = nullptr;
static PFN_EGLSWAPBUFFERS zd_eglSwapBuffers = nullptr;

bool SystemGLFrameBuffer::InitEGL(void *native_display, void *native_window) {
  LogToDisk("Initializing EGL...");

  static void *egl_lib = nullptr;
  if (!egl_lib) {
    egl_lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!egl_lib)
      egl_lib = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
  }

  if (!egl_lib) {
    LogToDisk("EGL: Failed to load libEGL.so.1");
    return false;
  }

  void *egl_gpa = dlsym(egl_lib, "eglGetProcAddress");
  if (!egl_gpa) {
    LogToDisk("EGL: Failed to find eglGetProcAddress");
    return false;
  }
  
  zd_eglGetDisplay = (PFN_EGLGETDISPLAY)dlsym(egl_lib, "eglGetDisplay");
  zd_eglInitialize = (PFN_EGLINITIALIZE)dlsym(egl_lib, "eglInitialize");
  zd_eglChooseConfig = (PFN_EGLCHOOSECONFIG)dlsym(egl_lib, "eglChooseConfig");
  zd_eglBindAPI = (PFN_EGLBINDAPI)dlsym(egl_lib, "eglBindAPI");
  zd_eglCreateContext = (PFN_EGLCREATECONTEXT)dlsym(egl_lib, "eglCreateContext");
  if (!zd_eglCreateContext) zd_eglCreateContext = (PFN_EGLCREATECONTEXT)dlsym(egl_lib, "eglCreateContext");
  zd_eglGetError = (PFN_EGLGETERROR)dlsym(egl_lib, "eglGetError");
  zd_eglCreateWindowSurface = (PFN_EGLCREATEWINDOWSURFACE)dlsym(egl_lib, "eglCreateWindowSurface");
  zd_eglMakeCurrent = (PFN_EGLMAKECURRENT)dlsym(egl_lib, "eglMakeCurrent");
  zd_eglSwapInterval = (PFN_EGLSWAPINTERVAL)dlsym(egl_lib, "eglSwapInterval");
  zd_eglDestroyContext = (PFN_EGLDESTROYCONTEXT)dlsym(egl_lib, "eglDestroyContext");
  zd_eglDestroySurface = (PFN_EGLDESTROYSURFACE)dlsym(egl_lib, "eglDestroySurface");
  zd_eglTerminate = (PFN_EGLTERMINATE)dlsym(egl_lib, "eglTerminate");


  zd_eglSwapBuffers = (PFN_EGLSWAPBUFFERS)dlsym(egl_lib, "eglSwapBuffers");

  if (!gladLoadEGLLoader((GLADloadproc)egl_gpa)) {
    LogToDisk("EGL: Failed to initialize GLAD EGL loader");
  }

  EGLDisplay egl_dpy = zd_eglGetDisplay((EGLNativeDisplayType)native_display);
  mEglDisplay = (void *)egl_dpy;
  if (egl_dpy == EGL_NO_DISPLAY) {
    LogToDisk("EGL: Failed to get display!");
    return false;
  }
  // ... rest of the function ...

  if (!zd_eglInitialize(egl_dpy, nullptr, nullptr)) {
    fprintf(stderr, "EGL: Failed to initialize!\n");
    return false;
  }

  EGLint attributes[] = {EGL_RENDERABLE_TYPE,
                         EGL_OPENGL_BIT,
                         EGL_SURFACE_TYPE,
                         EGL_WINDOW_BIT,
                         EGL_RED_SIZE,
                         8,
                         EGL_GREEN_SIZE,
                         8,
                         EGL_BLUE_SIZE,
                         8,
                         EGL_ALPHA_SIZE,
                         0,
                         EGL_DEPTH_SIZE,
                         24,
                         EGL_NONE};

  EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 3,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 3,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                 EGL_NONE};

  EGLConfig config;
  EGLint num_config;
  if (!zd_eglChooseConfig(egl_dpy, attributes, &config, 1, &num_config) ||
      num_config == 0) {
    fprintf(stderr, "EGL: Failed to choose config!\n");
    return false;
  }

  zd_eglBindAPI(EGL_OPENGL_API);
  EGLContext egl_ctx =
      zd_eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT, context_attributes);
  mEglContext = (void *)egl_ctx;
  if (egl_ctx == EGL_NO_CONTEXT) {
    fprintf(stderr, "EGL: Failed to create context! Error: 0x%x\n",
            zd_eglGetError());
    return false;
  }

  EGLSurface egl_surf = zd_eglCreateWindowSurface(
      egl_dpy, config, (EGLNativeWindowType)native_window, nullptr);
  mEglSurface = (void *)egl_surf;
  if (egl_surf == EGL_NO_SURFACE) {
    fprintf(stderr, "EGL: Failed to create surface! Error: 0x%x\n",
            zd_eglGetError());
    return false;
  }

  fprintf(stderr, "EGL: Context created: %p, Surface: %p, Display: %p\n",
          egl_ctx, egl_surf, egl_dpy);

  if (!zd_eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx)) {
    fprintf(stderr, "EGL: Failed to make context current! Error: 0x%x\n",
            zd_eglGetError());
  }

  // Populate all glad GL function pointers using EGL as the proc loader
  typedef void* (*egl_gpa_t)(const char*);
  gladLoadGLLoader((GLADloadproc)(egl_gpa_t)egl_gpa);

  const char *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
  fprintf(stderr, "[NATIVE] Current GL extensions: %s\n",
          gl_exts ? gl_exts : "NULL");

  zd_eglSwapInterval(egl_dpy, vid_vsync ? 1 : 0);
  fprintf(stderr, "[NATIVE] EGL initialized successfully\n");
  return true;
}
#endif

SystemGLFrameBuffer::SystemGLFrameBuffer(void *hMonitor, bool fullscreen)
    : SystemBaseFrameBuffer(hMonitor, fullscreen), WindowHandle(hMonitor),
      mIsWayland(false) {
  if (X11NativeDisplay) {
    if (hMonitor == nullptr) {
      fprintf(stderr, "SystemGLFrameBuffer: WindowHandle is nullptr while "
                      "X11NativeDisplay is set!\n");
      return;
    }
    Window window = (Window)(uintptr_t)hMonitor;

#ifdef HAVE_WAYLAND_EGL
    // Prioritize EGL on X11 as requested
    if (InitEGL(X11NativeDisplay, (void *)window)) {
      return;
    }
    fprintf(stderr, "[NATIVE] EGL on X11 failed, falling back to GLX\n");
#endif

    if (InitGLX()) {
      static int visual_attribs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE,
                                     24, 0};
      int screen = zd_XDefaultScreen ? zd_XDefaultScreen((Display*)X11NativeDisplay) : 0;
      XVisualInfo *vi = zd_glXChooseVisual(
          (Display*)X11NativeDisplay, screen, visual_attribs);
      if (vi) {
        GLXContext context =
            zd_glXCreateContext((Display*)X11NativeDisplay, vi, NULL, GL_TRUE);
        zd_glXMakeCurrent((Display*)X11NativeDisplay, window, context);
        if (zd_XFree) zd_XFree(vi);
      }
    }
  }
}

SystemGLFrameBuffer::SystemGLFrameBuffer(void *display, void *surface,
                                         bool wayland)
    : SystemBaseFrameBuffer(), WindowHandle(surface), mIsWayland(wayland) {
  if (mIsWayland) {
#ifdef HAVE_WAYLAND_EGL
    int width = 640, height = 480;
    DisplayWindow *win = GetActiveZWidgetWindow();
    if (win) {
      width = std::max(1, win->GetPixelWidth());
      height = std::max(1, win->GetPixelHeight());
    }

    if (!zd_wl_egl_window_create) {
      void* wle_lib = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
      if (wle_lib) {
        zd_wl_egl_window_create = (PFN_WLEGLWINDOWCREATE)dlsym(wle_lib, "wl_egl_window_create");
        zd_wl_egl_window_destroy = (PFN_WLEGLWINDOWDESTROY)dlsym(wle_lib, "wl_egl_window_destroy");
        zd_wl_egl_window_resize = (PFN_WLEGLWINDOWRESIZE)dlsym(wle_lib, "wl_egl_window_resize");
      }
    }

    if (zd_wl_egl_window_create) {
      mEglWindow = (void *)zd_wl_egl_window_create((struct wl_surface *)surface,
                                                width, height);
      if (!InitEGL(display, mEglWindow)) {
        fprintf(stderr, "EGL: Failed to initialize for Wayland!\n");
      }
    } else {
      fprintf(stderr, "EGL: libwayland-egl not found, cannot create window!\n");
    }
#endif
  } else {
    if (display && surface) {
      Window window = (Window)(uintptr_t)surface;
      LogToDisk("Calling InitEGL for X11...");
      if (InitEGL(display, (void *)window)) {
        LogToDisk("InitEGL for X11 succeeded.");
        return;
      }
      if (InitGLX()) {
        static int visual_attribs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE,
                                       24, 0};
        int screen = zd_XDefaultScreen ? zd_XDefaultScreen((Display *)display) : 0;
        XVisualInfo *vi = zd_glXChooseVisual((Display *)display, screen, visual_attribs);
        if (vi) {
          GLXContext context = zd_glXCreateContext((Display *)display, vi, NULL, GL_TRUE);
          zd_glXMakeCurrent((Display *)display, window, context);
          fprintf(stderr, "[NATIVE] GLX initialized successfully\n");
          if (zd_XFree)
            zd_XFree(vi);
        }
      }
    }
  }
}

SystemGLFrameBuffer::SystemGLFrameBuffer()
    : SystemBaseFrameBuffer(), WindowHandle(nullptr) {}

SystemGLFrameBuffer::~SystemGLFrameBuffer() {
#ifdef HAVE_WAYLAND_EGL
  if (mEglDisplay) {
    EGLDisplay egl_dpy = (EGLDisplay)mEglDisplay;
    zd_eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (mEglContext)
      zd_eglDestroyContext(egl_dpy, (EGLContext)mEglContext);
    if (mEglSurface)
      zd_eglDestroySurface(egl_dpy, (EGLSurface)mEglSurface);
    if (mEglWindow && mIsWayland && zd_wl_egl_window_destroy)
      zd_wl_egl_window_destroy((struct wl_egl_window *)mEglWindow);
    zd_eglTerminate(egl_dpy);
  }
#endif
}

int SystemGLFrameBuffer::GetClientWidth() {
  if (mIsWayland) {
    DisplayWindow *win = GetActiveZWidgetWindow();
    if (win)
      return std::max(1, win->GetPixelWidth());
    return 640;
  }
  if (X11NativeDisplay && WindowHandle && zd_XGetWindowAttributes) {
    XWindowAttributes xwa;
    zd_XGetWindowAttributes(X11NativeDisplay, (Window)(uintptr_t)WindowHandle,
                         &xwa);
    return xwa.width;
  }
  return 640;
}

int SystemGLFrameBuffer::GetClientHeight() {
  if (mIsWayland) {
    DisplayWindow *win = GetActiveZWidgetWindow();
    if (win)
      return std::max(1, win->GetPixelHeight());
    return 480;
  }
  if (X11NativeDisplay && WindowHandle && zd_XGetWindowAttributes) {
    XWindowAttributes xwa;
    zd_XGetWindowAttributes(X11NativeDisplay, (Window)(uintptr_t)WindowHandle,
                         &xwa);
    return xwa.height;
  }
  return 480;
}

void SystemGLFrameBuffer::SetVSync(bool vsync) {
#ifdef HAVE_WAYLAND_EGL
  if (mEglDisplay) {
    zd_eglSwapInterval((EGLDisplay)mEglDisplay, vsync ? 1 : 0);
  }
#endif
}

void SystemGLFrameBuffer::SwapBuffers() {
#ifdef HAVE_WAYLAND_EGL
  if (mEglDisplay && mEglSurface) {
    if (mIsWayland) {
      DisplayWindow *win = GetActiveZWidgetWindow();
      if (win && mEglWindow && zd_wl_egl_window_resize) {
        zd_wl_egl_window_resize((struct wl_egl_window *)mEglWindow,
                             win->GetPixelWidth(), win->GetPixelHeight(), 0, 0);
      }
    }
    zd_eglSwapBuffers((EGLDisplay)mEglDisplay, (EGLSurface)mEglSurface);
    return;
  }
#endif

  if (X11NativeDisplay && WindowHandle) {
    if (zd_glXSwapBuffers) {
      zd_glXSwapBuffers(X11NativeDisplay, (Window)(uintptr_t)WindowHandle);
    }
  }
}
