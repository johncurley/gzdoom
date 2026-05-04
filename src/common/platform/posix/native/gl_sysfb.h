#ifndef __POSIX_NATIVE_GL_SYSFB_H__
#define __POSIX_NATIVE_GL_SYSFB_H__

#include "v_video.h"
#include "native_display.h"

class SystemBaseFrameBuffer : public DFrameBuffer {
public:
  SystemBaseFrameBuffer(void *hMonitor, bool fullscreen) : DFrameBuffer(0, 0) {}
  bool IsFullscreen() override { return false; }
  int GetClientWidth() override { return 640; }
  int GetClientHeight() override { return 480; }
  void ToggleFullscreen(bool yes) override {}
  void SetWindowSize(int client_w, int client_h) override {}
  virtual NativeHandle GetNativeHandle() const { return {}; }
protected:
  SystemBaseFrameBuffer() : DFrameBuffer(0, 0) {}
};

class SystemGLFrameBuffer : public SystemBaseFrameBuffer {
    void* WindowHandle;
public:
  void InitializeState() override;
  IIndexBuffer* CreateIndexBuffer() override;
  IVertexBuffer* CreateVertexBuffer() override;
  SystemGLFrameBuffer(void *hMonitor, bool fullscreen);
  SystemGLFrameBuffer();
  ~SystemGLFrameBuffer();
  int GetClientWidth() override;
  int GetClientHeight() override;
  virtual void SetVSync(bool vsync) override;
  void SwapBuffers();
};

#endif // __POSIX_NATIVE_GL_SYSFB_H__
