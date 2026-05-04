#include "i_video.h"
#include "v_video.h"
#include "gl_sysfb.h"
#include "x11_compat.h"
#include "native_display.h"
#include "d_eventbase.h"
#include "common/console/keydef.h"

#undef None
#include <zwidget/window/window.h>
#include <zwidget/window/x11nativehandle.h>
#include <zwidget/window/waylandnativehandle.h>

#include <cstdio>
#include <cstdint>
#include <map>

// External global Display pointer needed by gl_sysfb.cpp
Display *X11NativeDisplay = nullptr;

int ZWidgetKeyToGZDoom(InputKey key)
{
    static const std::map<InputKey, int> mapping = {
        {InputKey::Escape, KEY_ESCAPE},
        {InputKey::Enter, KEY_ENTER},
        {InputKey::Space, KEY_SPACE},
        {InputKey::Tab, KEY_TAB},
        {InputKey::Backspace, KEY_BACKSPACE},
        {InputKey::Left, KEY_LEFTARROW},
        {InputKey::Right, KEY_RIGHTARROW},
        {InputKey::Up, KEY_UPARROW},
        {InputKey::Down, KEY_DOWNARROW},
        {InputKey::PageUp, KEY_PGUP},
        {InputKey::PageDown, KEY_PGDN},
        {InputKey::Home, KEY_HOME},
        {InputKey::End, KEY_END},
        {InputKey::Insert, KEY_INS},
        {InputKey::Delete, KEY_DEL},
        {InputKey::Shift, KEY_LSHIFT},
        {InputKey::Ctrl, KEY_LCTRL},
        {InputKey::Alt, KEY_LALT},
        {InputKey::F1, KEY_F1}, {InputKey::F2, KEY_F2}, {InputKey::F3, KEY_F3}, {InputKey::F4, KEY_F4},
        {InputKey::F5, KEY_F5}, {InputKey::F6, KEY_F6}, {InputKey::F7, KEY_F7}, {InputKey::F8, KEY_F8},
        {InputKey::F9, KEY_F9}, {InputKey::F10, KEY_F10}, {InputKey::F11, KEY_F11}, {InputKey::F12, KEY_F12},
        {InputKey::A, 'A'}, {InputKey::B, 'B'}, {InputKey::C, 'C'}, {InputKey::D, 'D'},
        {InputKey::E, 'E'}, {InputKey::F, 'F'}, {InputKey::G, 'G'}, {InputKey::H, 'H'},
        {InputKey::I, 'I'}, {InputKey::J, 'J'}, {InputKey::K, 'K'}, {InputKey::L, 'L'},
        {InputKey::M, 'M'}, {InputKey::N, 'N'}, {InputKey::O, 'O'}, {InputKey::P, 'P'},
        {InputKey::Q, 'Q'}, {InputKey::R, 'R'}, {InputKey::S, 'S'}, {InputKey::T, 'T'},
        {InputKey::U, 'U'}, {InputKey::V, 'V'}, {InputKey::W, 'W'}, {InputKey::X, 'X'},
        {InputKey::Y, 'Y'}, {InputKey::Z, 'Z'},
        {InputKey::_0, '0'}, {InputKey::_1, '1'}, {InputKey::_2, '2'}, {InputKey::_3, '3'},
        {InputKey::_4, '4'}, {InputKey::_5, '5'}, {InputKey::_6, '6'}, {InputKey::_7, '7'},
        {InputKey::_8, '8'}, {InputKey::_9, '9'},
    };
    auto it = mapping.find(key);
    if (it != mapping.end()) return it->second;
    return 0;
}

class GZDoomWindowHost : public DisplayWindowHost
{
public:
    void OnWindowPaint() override {}
    void OnWindowMouseMove(const Point& pos) override {
        PostMouseMove(pos.x, pos.y);
    }
    void OnWindowMouseLeave() override {}
    void OnWindowMouseDown(const Point& pos, InputKey key) override {
        event_t ev = {EV_KeyDown};
        if (key == InputKey::LeftMouse) ev.data1 = KEY_MOUSE1;
        else if (key == InputKey::RightMouse) ev.data1 = KEY_MOUSE2;
        else if (key == InputKey::MiddleMouse) ev.data1 = KEY_MOUSE3;
        else return;
        D_PostEvent(&ev);
    }
    void OnWindowMouseDoubleclick(const Point& pos, InputKey key) override {}
    void OnWindowMouseUp(const Point& pos, InputKey key) override {
        event_t ev = {EV_KeyUp};
        if (key == InputKey::LeftMouse) ev.data1 = KEY_MOUSE1;
        else if (key == InputKey::RightMouse) ev.data1 = KEY_MOUSE2;
        else if (key == InputKey::MiddleMouse) ev.data1 = KEY_MOUSE3;
        else return;
        D_PostEvent(&ev);
    }
    void OnWindowMouseWheel(const Point& pos, InputKey key) override {
        event_t ev = {EV_KeyDown};
        if (key == InputKey::MouseWheelUp) ev.data1 = KEY_MWHEELUP;
        else if (key == InputKey::MouseWheelDown) ev.data1 = KEY_MWHEELDOWN;
        else return;
        D_PostEvent(&ev);
        ev.type = EV_KeyUp;
        D_PostEvent(&ev);
    }
    void OnWindowRawMouseMove(int dx, int dy) override {}
    void OnWindowKeyChar(std::string chars) override {}
    void OnWindowKeyDown(InputKey key) override {
        event_t ev = {EV_KeyDown};
        ev.data1 = ZWidgetKeyToGZDoom(key);
        if (ev.data1 != 0) D_PostEvent(&ev);
    }
    void OnWindowKeyUp(InputKey key) override {
        event_t ev = {EV_KeyUp};
        ev.data1 = ZWidgetKeyToGZDoom(key);
        if (ev.data1 != 0) D_PostEvent(&ev);
    }
    void OnWindowGeometryChanged() override {}
    void OnWindowClose() override {
        exit(0);
    }
    void OnWindowActivated() override {}
    void OnWindowDeactivated() override {}
    void OnWindowDpiScaleChanged() override {}
};

class NativeVideo : public IVideo {
    GZDoomWindowHost host;
    std::unique_ptr<DisplayWindow> window;
public:
    static DisplayWindow* ActiveWindow;

    NativeVideo() {
        auto backend = DisplayBackend::TryCreateBackend();
        if (!backend) {
            fprintf(stderr, "NativeVideo: Could not create any ZWidget display backend!\n");
            return;
        }
        DisplayBackend::Set(std::move(backend));

        window = DisplayWindow::Create(&host, false, nullptr, RenderAPI::OpenGL);
        if (!window) {
            fprintf(stderr, "NativeVideo: Could not create ZWidget window!\n");
            return;
        }

        window->SetWindowTitle("GZDoom (Native POSIX)");
        window->Show();
        ActiveWindow = window.get();
    }
    ~NativeVideo() {
        ActiveWindow = nullptr;
    }
    DFrameBuffer *CreateFrameBuffer() override { 
        if (!window) {
            fprintf(stderr, "NativeVideo: window is nullptr in CreateFrameBuffer!\n");
            return new SystemGLFrameBuffer(nullptr, false);
        }

        fprintf(stderr, "NativeVideo: Retrieving native window handle from ZWidget.\n");
        void* handle = window->GetNativeHandle();

        if (handle == nullptr) {
            fprintf(stderr, "NativeVideo: ZWidget returned nullptr handle!\n");
            return new SystemGLFrameBuffer(nullptr, false);
        }

        if (DisplayBackend::Get()->IsX11()) {
            X11NativeHandle* x11_handle = (X11NativeHandle*)handle;
            X11NativeDisplay = x11_handle->display;
            return new SystemGLFrameBuffer((void*)x11_handle->window, false);
        } else if (DisplayBackend::Get()->IsWayland()) {
            WaylandNativeHandle* wl_handle = (WaylandNativeHandle*)handle;
            // For now GZDoom's SystemGLFrameBuffer is X11-only. 
            // We need a Wayland implementation of SystemGLFrameBuffer.
            fprintf(stderr, "NativeVideo: Wayland backend detected, but SystemGLFrameBuffer is X11-only.\n");
            return new SystemGLFrameBuffer(nullptr, false);
        }

        return new SystemGLFrameBuffer(nullptr, false); 
    }

    void SetWindowTitle(const char *title) override {
        if (window) window->SetWindowTitle(title);
    }
};

IVideo *gl_CreateVideo() { return new NativeVideo(); }

DisplayWindow* NativeVideo::ActiveWindow = nullptr;

DisplayWindow* GetActiveZWidgetWindow() {
    return NativeVideo::ActiveWindow;
}
