#include "gl_sysfb.h"
#include "x11_compat.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "flatvertices.h"

extern Display *X11NativeDisplay;

class NativeIndexBuffer : public IIndexBuffer {
public:
    void SetData(size_t size, const void *data, BufferUsageType type) override {}
    void SetSubData(size_t offset, size_t size, const void *data) override {}
    void *Lock(unsigned int size) override { return nullptr; }
    void Unlock() override {}
    void Resize(size_t newsize) override {}
};

class NativeVertexBuffer : public IVertexBuffer {
public:
    NativeVertexBuffer() { }
    ~NativeVertexBuffer() { delete[] (uint8_t*)map; }
    void SetData(size_t size, const void *data, BufferUsageType type) override {
        delete[] (uint8_t*)map;
        buffersize = size;
        map = new uint8_t[size];
        if (data) memcpy(map, data, size);
        else memset(map, 0, size);
    }
    void SetSubData(size_t offset, size_t size, const void *data) override {
        if (map && offset + size <= buffersize) memcpy((uint8_t*)map + offset, data, size);
    }
    void *Lock(unsigned int size) override { return map; }
    void Unlock() override {}
    void Resize(size_t newsize) override {
        void* newData = new uint8_t[newsize];
        memset(newData, 0, newsize);
        if (map) memcpy(newData, map, std::min(buffersize, newsize));
        delete[] (uint8_t*)map;
        map = newData;
        buffersize = newsize;
    }
    void Map() override {}
    void Unmap() override {}
    void SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs) override {}
    void Upload(size_t start, size_t size) override {}
    void GPUDropSync() override {}
    void GPUWaitSync() override {}
};

void SystemGLFrameBuffer::InitializeState() {
    if (X11NativeDisplay && WindowHandle) {
         glViewport(0, 0, GetClientWidth(), GetClientHeight());
    }
    if (!mVertexData) {
        if (screen) {
            mVertexData = new FFlatVertexBuffer(GetClientWidth(), GetClientHeight());
            mVertexData->OutputResized(GetClientWidth(), GetClientHeight());
        } else {
            fprintf(stderr, "SystemGLFrameBuffer: screen is null, skipping mVertexData initialization!\n");
        }
    }
}

IIndexBuffer* SystemGLFrameBuffer::CreateIndexBuffer() {
    return new NativeIndexBuffer();
}

IVertexBuffer* SystemGLFrameBuffer::CreateVertexBuffer() {
    return new NativeVertexBuffer();
}

SystemGLFrameBuffer::SystemGLFrameBuffer(void *hMonitor, bool fullscreen) : SystemBaseFrameBuffer(hMonitor, fullscreen), WindowHandle(hMonitor) {
    if (X11NativeDisplay) {
        if (hMonitor == nullptr) {
             fprintf(stderr, "SystemGLFrameBuffer: WindowHandle is nullptr while X11NativeDisplay is set!\n");
             return;
        }
        Window window = (Window)(uintptr_t)hMonitor;
        static int visual_attribs[] = {
            GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None
        };
        XVisualInfo *vi = glXChooseVisual(X11NativeDisplay, DefaultScreen(X11NativeDisplay), visual_attribs);
        if (vi) {
            GLXContext context = glXCreateContext(X11NativeDisplay, vi, NULL, GL_TRUE);
            glXMakeCurrent(X11NativeDisplay, window, context);
        }
    }
}
SystemGLFrameBuffer::SystemGLFrameBuffer() : SystemBaseFrameBuffer(), WindowHandle(nullptr) {}
SystemGLFrameBuffer::~SystemGLFrameBuffer() {}
int SystemGLFrameBuffer::GetClientWidth() {
    if (X11NativeDisplay && WindowHandle) {
        XWindowAttributes xwa;
        XGetWindowAttributes(X11NativeDisplay, (Window)(uintptr_t)WindowHandle, &xwa);
        return xwa.width;
    }
    return 640; 
}
int SystemGLFrameBuffer::GetClientHeight() {
    if (X11NativeDisplay && WindowHandle) {
        XWindowAttributes xwa;
        XGetWindowAttributes(X11NativeDisplay, (Window)(uintptr_t)WindowHandle, &xwa);
        return xwa.height;
    }
    return 480; 
}
void SystemGLFrameBuffer::SetVSync(bool vsync) {}
void SystemGLFrameBuffer::SwapBuffers() {
    if (X11NativeDisplay && WindowHandle) {
        glXSwapBuffers(X11NativeDisplay, (Window)(uintptr_t)WindowHandle);
    }
}
