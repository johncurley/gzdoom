#ifndef __POSIX_NATIVE_GL_SYSFB_H__
#define __POSIX_NATIVE_GL_SYSFB_H__

#include "v_video.h"
#include "native_display.h"

class SystemBaseFrameBuffer : public DFrameBuffer {
public:
  SystemBaseFrameBuffer(void *hMonitor, bool fullscreen);
  bool IsFullscreen() override { return false; }
  int GetClientWidth() override { return 640; }
  int GetClientHeight() override { return 480; }
  void ToggleFullscreen(bool yes) override;
  void SetWindowSize(int client_w, int client_h) override;
  virtual NativeHandle GetNativeHandle() const { return {}; }
protected:
  SystemBaseFrameBuffer();
};

class SystemGLFrameBuffer : public SystemBaseFrameBuffer {
    void* WindowHandle = nullptr;
    bool mIsWayland = false;

#ifdef HAVE_WAYLAND_EGL
    void* mEglDisplay = nullptr;
    void* mEglConfig = nullptr;
    void* mEglContext = nullptr;
    void* mEglSurface = nullptr;
    void* mEglWindow = nullptr; // wl_egl_window
    bool InitEGL(void* native_display, void* native_window);
#endif

public:
  void InitializeState() override;
  IIndexBuffer* CreateIndexBuffer() override;
  IVertexBuffer* CreateVertexBuffer() override;
  IDataBuffer* CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;
  SystemGLFrameBuffer(void *hMonitor, bool fullscreen);
  SystemGLFrameBuffer(void* display, void* surface, bool wayland);
  SystemGLFrameBuffer();
  ~SystemGLFrameBuffer();
  int GetClientWidth() override;
  int GetClientHeight() override;
  virtual void SetVSync(bool vsync) override;
  void SwapBuffers();
};

#endif // __POSIX_NATIVE_GL_SYSFB_H__
