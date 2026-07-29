#include "i_video.h"
#include "i_input.h"
#include "engineerrors.h"
#include "v_video.h"
#include "gl_framebuffer.h"
#include "gl_sysfb.h"
#include "gles/gles_framebuffer.h"
#include "x11_compat.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include "native_display.h"
#include "d_eventbase.h"
#include "common/console/keydef.h"
#include "common/platform/posix/dikeys.h"
#include "common/engine/d_gui.h"
#include "i_interface.h"

#undef None
#include <zwidget/window/window.h>
#include <zwidget/window/x11nativehandle.h>
#include <zwidget/window/waylandnativehandle.h>
#include <zwidget/core/image.h>

#include <cstdio>
#include <cstdint>
#include <map>
#include <cstdlib>

// Forward declarations for X11 types to avoid header dependency
typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef XID Window;

EXTERN_CVAR(Int, vid_defwidth)
EXTERN_CVAR(Int, vid_defheight)
EXTERN_CVAR(Bool, vid_fullscreen)
EXTERN_CVAR(Bool, vk_debug)

// External global Display pointer needed by gl_sysfb.cpp
// Note: In a future "Pure Native" update, we could also dlopen libX11.so.6
// and resolve all Xlib symbols dynamically to remove the link-time dependency.
Display *X11NativeDisplay = nullptr;

#ifdef HAVE_VULKAN
#include "vulkan/system/vk_renderdevice.h"
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include <zvulkan/vulkansurface.h>

static std::shared_ptr<VulkanSurface> m_vulkanSurface;
#endif

static void ApplyNativeWindowClientBounds(DisplayWindow *win)
{
	Size scr = DisplayWindow::GetScreenSize();
	double default_width = vid_defwidth;
	double default_height = vid_defheight;

	double cx = std::max(0.0, (scr.width - default_width) * 0.5);
	double cy = std::max(0.0, (scr.height - default_height) * 0.5);
	win->SetClientFrame(Rect(cx, cy, default_width, default_height));
}

int ZWidgetKeyToGZDoom(InputKey key)
{
    switch (key)
    {
        case InputKey::Escape: return KEY_ESCAPE;
        case InputKey::Enter: return KEY_ENTER;
        case InputKey::Space: return KEY_SPACE;
        case InputKey::Tab: return KEY_TAB;
        case InputKey::Backspace: return KEY_BACKSPACE;
        case InputKey::Left: return KEY_LEFTARROW;
        case InputKey::Right: return KEY_RIGHTARROW;
        case InputKey::Up: return KEY_UPARROW;
        case InputKey::Down: return KEY_DOWNARROW;
        case InputKey::PageUp: return KEY_PGUP;
        case InputKey::PageDown: return KEY_PGDN;
        case InputKey::Home: return KEY_HOME;
        case InputKey::End: return KEY_END;
        case InputKey::Insert: return KEY_INS;
        case InputKey::Delete: return KEY_DEL;
        case InputKey::Shift:
        case InputKey::LShift: return KEY_LSHIFT;
        case InputKey::RShift: return KEY_RSHIFT;
        case InputKey::Ctrl:
        case InputKey::LControl: return KEY_LCTRL;
        case InputKey::RControl: return KEY_RCTRL;
        case InputKey::Alt: return KEY_LALT;
        case InputKey::F1: return KEY_F1;
        case InputKey::F2: return KEY_F2;
        case InputKey::F3: return KEY_F3;
        case InputKey::F4: return KEY_F4;
        case InputKey::F5: return KEY_F5;
        case InputKey::F6: return KEY_F6;
        case InputKey::F7: return KEY_F7;
        case InputKey::F8: return KEY_F8;
        case InputKey::F9: return KEY_F9;
        case InputKey::F10: return KEY_F10;
        case InputKey::F11: return KEY_F11;
        case InputKey::F12: return KEY_F12;
        case InputKey::A: return DIK_A;
        case InputKey::B: return DIK_B;
        case InputKey::C: return DIK_C;
        case InputKey::D: return DIK_D;
        case InputKey::E: return DIK_E;
        case InputKey::F: return DIK_F;
        case InputKey::G: return DIK_G;
        case InputKey::H: return DIK_H;
        case InputKey::I: return DIK_I;
        case InputKey::J: return DIK_J;
        case InputKey::K: return DIK_K;
        case InputKey::L: return DIK_L;
        case InputKey::M: return DIK_M;
        case InputKey::N: return DIK_N;
        case InputKey::O: return DIK_O;
        case InputKey::P: return DIK_P;
        case InputKey::Q: return DIK_Q;
        case InputKey::R: return DIK_R;
        case InputKey::S: return DIK_S;
        case InputKey::T: return DIK_T;
        case InputKey::U: return DIK_U;
        case InputKey::V: return DIK_V;
        case InputKey::W: return DIK_W;
        case InputKey::X: return DIK_X;
        case InputKey::Y: return DIK_Y;
        case InputKey::Z: return DIK_Z;
        case InputKey::_0: return DIK_0;
        case InputKey::_1: return DIK_1;
        case InputKey::_2: return DIK_2;
        case InputKey::_3: return DIK_3;
        case InputKey::_4: return DIK_4;
        case InputKey::_5: return DIK_5;
        case InputKey::_6: return DIK_6;
        case InputKey::_7: return DIK_7;
        case InputKey::_8: return DIK_8;
        case InputKey::_9: return DIK_9;
        case InputKey::Tilde: return DIK_GRAVE;
        case InputKey::Minus: return DIK_MINUS;
        case InputKey::Equals: return DIK_EQUALS;
        case InputKey::LeftBracket: return DIK_LBRACKET;
        case InputKey::RightBracket: return DIK_RBRACKET;
        case InputKey::Backslash: return DIK_BACKSLASH;
        case InputKey::Semicolon: return DIK_SEMICOLON;
        case InputKey::SingleQuote: return DIK_APOSTROPHE;
        case InputKey::Comma: return DIK_COMMA;
        case InputKey::Period: return DIK_PERIOD;
        case InputKey::Slash: return DIK_SLASH;
        case InputKey::NumPad0: return DIK_NUMPAD0;
        case InputKey::NumPad1: return DIK_NUMPAD1;
        case InputKey::NumPad2: return DIK_NUMPAD2;
        case InputKey::NumPad3: return DIK_NUMPAD3;
        case InputKey::NumPad4: return DIK_NUMPAD4;
        case InputKey::NumPad5: return DIK_NUMPAD5;
        case InputKey::NumPad6: return DIK_NUMPAD6;
        case InputKey::NumPad7: return DIK_NUMPAD7;
        case InputKey::NumPad8: return DIK_NUMPAD8;
        case InputKey::NumPad9: return DIK_NUMPAD9;
        case InputKey::GreySlash: return DIK_DIVIDE;
        case InputKey::GreyStar: return DIK_MULTIPLY;
        case InputKey::GreyMinus: return DIK_SUBTRACT;
        case InputKey::GreyPlus: return DIK_ADD;
        case InputKey::NumPadPeriod: return DIK_DECIMAL;
        case InputKey::Pause: return DIK_PAUSE;
        case InputKey::CapsLock: return DIK_CAPITAL;
        case InputKey::ScrollLock: return DIK_SCROLL;
        case InputKey::NumLock: return DIK_NUMLOCK;
        case InputKey::PrintScrn: return DIK_SYSRQ;
        default: break;
    }
    
    // If it's a character, return it directly
    if ((int)key >= 32 && (int)key < 127) return (int)key;
    
    return 0;
}

extern bool NativeMouseCaptured;
extern bool GUICapture;
extern bool RawKeyboardActive;

static bool WantGuiCaptureNow()
{
	return sysCallbacks.WantGuiCapture && sysCallbacks.WantGuiCapture();
}

// Temporary diagnostic for stuck keys. Enable with `in_keytrace 1` in the
// console. Prints one line per key event as it is handed to the engine, so the
// route decision and the posted/dropped outcome are visible; i_input.cpp prints
// the resulting set of held buttons whenever it changes. A key-up that shows
// "posted route=Game" but leaves its button in the held set is the failure we
// are looking for.
CVAR(Bool, in_keytrace, false, 0)

void I_TraceKeyEvent(const char* dir, int inputkey, int gzkey, const char* route, bool posted)
{
	if (!in_keytrace)
		return;
	// stderr rather than Printf: Printf goes to the in-game console once video
	// is up, and this needs to be capturable from a piped run.
	fprintf(stderr, "[keytrace] %-4s ik=%-3d gz=%-3d route=%-4s %-7s gui=%d\n",
		dir, inputkey, gzkey, route, posted ? "posted" : "DROPPED", (int)GUICapture);
	fflush(stderr);
}

static int16_t GetModifierMask(bool shiftDown, bool ctrlDown, bool altDown)
{
	return (shiftDown ? GKM_SHIFT : 0) | (ctrlDown ? GKM_CTRL : 0) | (altDown ? GKM_ALT : 0);
}

static int InputKeyToGUIKey(InputKey key)
{
	// Mirrors SDL's behavior: UI keys are ASCII (uppercased) or GK_* codes.
	switch (key)
	{
	case InputKey::Enter: return GK_RETURN;
	case InputKey::PageUp: return GK_PGUP;
	case InputKey::PageDown: return GK_PGDN;
	case InputKey::End: return GK_END;
	case InputKey::Home: return GK_HOME;
	case InputKey::Left: return GK_LEFT;
	case InputKey::Right: return GK_RIGHT;
	case InputKey::Up: return GK_UP;
	case InputKey::Down: return GK_DOWN;
	case InputKey::Delete: return GK_DEL;
	case InputKey::Escape: return GK_ESCAPE;
	case InputKey::F1: return GK_F1;
	case InputKey::F2: return GK_F2;
	case InputKey::F3: return GK_F3;
	case InputKey::F4: return GK_F4;
	case InputKey::F5: return GK_F5;
	case InputKey::F6: return GK_F6;
	case InputKey::F7: return GK_F7;
	case InputKey::F8: return GK_F8;
	case InputKey::F9: return GK_F9;
	case InputKey::F10: return GK_F10;
	case InputKey::F11: return GK_F11;
	case InputKey::F12: return GK_F12;
	default: break;
	}

	// Character-ish keys (uppercased).
	switch (key)
	{
	case InputKey::_0: return '0';
	case InputKey::_1: return '1';
	case InputKey::_2: return '2';
	case InputKey::_3: return '3';
	case InputKey::_4: return '4';
	case InputKey::_5: return '5';
	case InputKey::_6: return '6';
	case InputKey::_7: return '7';
	case InputKey::_8: return '8';
	case InputKey::_9: return '9';
	case InputKey::A: return 'A';
	case InputKey::B: return 'B';
	case InputKey::C: return 'C';
	case InputKey::D: return 'D';
	case InputKey::E: return 'E';
	case InputKey::F: return 'F';
	case InputKey::G: return 'G';
	case InputKey::H: return 'H';
	case InputKey::I: return 'I';
	case InputKey::J: return 'J';
	case InputKey::K: return 'K';
	case InputKey::L: return 'L';
	case InputKey::M: return 'M';
	case InputKey::N: return 'N';
	case InputKey::O: return 'O';
	case InputKey::P: return 'P';
	case InputKey::Q: return 'Q';
	case InputKey::R: return 'R';
	case InputKey::S: return 'S';
	case InputKey::T: return 'T';
	case InputKey::U: return 'U';
	case InputKey::V: return 'V';
	case InputKey::W: return 'W';
	case InputKey::X: return 'X';
	case InputKey::Y: return 'Y';
	case InputKey::Z: return 'Z';
	case InputKey::Space: return ' ';
	case InputKey::Tab: return GK_TAB;
	case InputKey::Backspace: return GK_BACKSPACE;
	case InputKey::Tilde: return '~';
	case InputKey::Minus: return '-';
	case InputKey::Equals: return '=';
	case InputKey::LeftBracket: return '[';
	case InputKey::RightBracket: return ']';
	case InputKey::Backslash: return '\\';
	case InputKey::Semicolon: return ';';
	case InputKey::SingleQuote: return '\'';
	case InputKey::Comma: return ',';
	case InputKey::Period: return '.';
	case InputKey::Slash: return '/';
	default: break;
	}
	return 0;
}

static int16_t InputKeyToMouseKey(InputKey key)
{
	switch (key)
	{
	case InputKey::LeftMouse: return KEY_MOUSE1;
	case InputKey::RightMouse: return KEY_MOUSE2;
	case InputKey::MiddleMouse: return KEY_MOUSE3;
	default: return 0;
	}
}

static int16_t InputKeyToWheelKey(InputKey key)
{
	switch (key)
	{
	case InputKey::MouseWheelUp: return KEY_MWHEELUP;
	case InputKey::MouseWheelDown: return KEY_MWHEELDOWN;
	default: return 0;
	}
}

class GZDoomWindowHost : public DisplayWindowHost
{
    double lastMouseX = -1, lastMouseY = -1;
	bool shiftDown = false;
	bool ctrlDown = false;
	bool altDown = false;
	enum class EventRoute : uint8_t { None, GUI, Game };
	EventRoute leftRoute = EventRoute::None;
	EventRoute rightRoute = EventRoute::None;
	EventRoute middleRoute = EventRoute::None;
	std::map<InputKey, EventRoute> keyRoutes;

	int16_t ModMask() const { return GetModifierMask(shiftDown, ctrlDown, altDown); }

	void UpdateModifierKey(InputKey key, bool down)
	{
		if (key == InputKey::Shift || key == InputKey::LShift || key == InputKey::RShift) shiftDown = down;
		if (key == InputKey::Ctrl || key == InputKey::LControl || key == InputKey::RControl) ctrlDown = down;
		if (key == InputKey::Alt) altDown = down;
	}
public:
    void OnWindowPaint() override {}
    void OnWindowMouseMove(const Point& pos) override {
		const bool gui = WantGuiCaptureNow();
        if (!NativeMouseCaptured && gui) {
            event_t ev = {EV_GUI_Event, EV_GUI_MouseMove};
            ev.data1 = (int16_t)pos.x;
            ev.data2 = (int16_t)pos.y;
			ev.data3 = ModMask();
            D_PostEvent(&ev);
        }
    }
    void OnWindowMouseLeave() override {}
    void OnWindowMouseDown(const Point& pos, InputKey key) override {
		// Route press/release consistently. Capture state can change during the click
		// (e.g. menu click causing capture transitions), and splitting routes can leave
		// a game key stuck down.
		EventRoute& route =
			(key == InputKey::LeftMouse) ? leftRoute :
			(key == InputKey::RightMouse) ? rightRoute :
			(key == InputKey::MiddleMouse) ? middleRoute :
			leftRoute; // unused

		const bool guiNow = WantGuiCaptureNow();
		route = guiNow ? EventRoute::GUI : EventRoute::Game;

		if (route == EventRoute::GUI)
		{
			event_t ev = {EV_GUI_Event};
			if (key == InputKey::LeftMouse) ev.subtype = EV_GUI_LButtonDown;
			else if (key == InputKey::RightMouse) ev.subtype = EV_GUI_RButtonDown;
			else if (key == InputKey::MiddleMouse) ev.subtype = EV_GUI_MButtonDown;
			else return;
			ev.data1 = (int16_t)pos.x;
			ev.data2 = (int16_t)pos.y;
			ev.data3 = ModMask();
			D_PostEvent(&ev);
		}
		else
		{
			const int16_t mouseKey = InputKeyToMouseKey(key);
			if (!mouseKey) return;
			event_t ev = {EV_KeyDown};
			ev.data1 = mouseKey;
			ev.data3 = ModMask();
			D_PostEvent(&ev);
			I_TraceKeyEvent("mdn", (int)key, mouseKey, "Game", true);
		}
    }
    void OnWindowMouseUp(const Point& pos, InputKey key) override {
		EventRoute& route =
			(key == InputKey::LeftMouse) ? leftRoute :
			(key == InputKey::RightMouse) ? rightRoute :
			(key == InputKey::MiddleMouse) ? middleRoute :
			leftRoute; // unused

		// If we don't have a recorded route (missed press), fall back to current state.
		const EventRoute effectiveRoute =
			(route == EventRoute::None) ? (WantGuiCaptureNow() ? EventRoute::GUI : EventRoute::Game) : route;

		route = EventRoute::None;

		if (effectiveRoute == EventRoute::GUI)
		{
			event_t ev = {EV_GUI_Event};
			if (key == InputKey::LeftMouse) ev.subtype = EV_GUI_LButtonUp;
			else if (key == InputKey::RightMouse) ev.subtype = EV_GUI_RButtonUp;
			else if (key == InputKey::MiddleMouse) ev.subtype = EV_GUI_MButtonUp;
			else return;
			ev.data1 = (int16_t)pos.x;
			ev.data2 = (int16_t)pos.y;
			ev.data3 = ModMask();
			D_PostEvent(&ev);
		}
		else
		{
			const int16_t mouseKey = InputKeyToMouseKey(key);
			if (!mouseKey) return;
			event_t ev = {EV_KeyUp};
			ev.data1 = mouseKey;
			ev.data3 = ModMask();
			D_PostEvent(&ev);
			I_TraceKeyEvent("mup", (int)key, mouseKey, "Game", true);
		}
    }
    void OnWindowMouseDoubleclick(const Point& pos, InputKey key) override {}
    void OnWindowMouseWheel(const Point& pos, InputKey key) override {
		const bool gui = WantGuiCaptureNow();
		if (gui)
		{
			event_t ev = {EV_GUI_Event};
			if (key == InputKey::MouseWheelUp) ev.subtype = EV_GUI_WheelUp;
			else if (key == InputKey::MouseWheelDown) ev.subtype = EV_GUI_WheelDown;
			else return;
			ev.data1 = (int16_t)pos.x;
			ev.data2 = (int16_t)pos.y;
			ev.data3 = ModMask();
			D_PostEvent(&ev);
		}
		else
		{
			const int16_t wheelKey = InputKeyToWheelKey(key);
			I_TraceKeyEvent("wheel", (int)key, wheelKey, "Game", wheelKey != 0);
			if (!wheelKey) return;
			event_t ev = {EV_KeyDown};
			ev.data1 = wheelKey;
			ev.data3 = ModMask();
			D_PostEvent(&ev);
			ev.type = EV_KeyUp;
			D_PostEvent(&ev);
		}
    }
    void OnWindowRawMouseMove(int dx, int dy) override {
		const bool gui = WantGuiCaptureNow();
        if (NativeMouseCaptured && !gui && (dx || dy)) {
            // fprintf(stderr, "[INPUT] RawMouseMove: dx=%d, dy=%d\n", dx, dy);
            PostMouseMove(dx, dy);
        }
    }
    void OnWindowKeyChar(std::string chars) override {
		const bool gui = WantGuiCaptureNow();
		if (!gui)
			return;
        for (unsigned char c : chars) {
			// Text-input callbacks deliver the UTF-8 encoding of the keysym,
			// which for BackSpace/Return/Escape/Tab is the corresponding
			// control character (0x08, 0x0D, 0x1B, 0x09). Those keys already
			// arrive as EV_GUI_KeyDown with their GK_* code, so posting the
			// control character as well makes the consumer act twice: the
			// console both deletes a character and then inserts a literal
			// 0x08, which looks like backspace not working at all.
			//
			// Only genuine printable text belongs here. UTF-8 lead and
			// continuation bytes are all >= 0x80, so this cannot split a
			// multi-byte sequence.
			if (c < 0x20 || c == 0x7F)
				continue;

            event_t ev = {EV_GUI_Event, EV_GUI_Char};
            ev.data1 = c;
			ev.data3 = ModMask();
            D_PostEvent(&ev);
        }
    }
    void OnWindowKeyDown(InputKey key) override {
        UpdateModifierKey(key, true);

		// A key-down for a key we already have a route for is an auto-repeat:
		// routes are erased on key-up, so a live entry means the key is still
		// held. The GUI expects those as EV_GUI_KeyRepeat, which is what drives
		// held-key behaviour in the console and menus.
		auto it = keyRoutes.find(key);
		const bool isRepeat = (it != keyRoutes.end());

		// A repeat is generated on a timer, so the capture state can change
		// under a key that is still held -- pressing Enter to close a menu, for
		// instance. Feeding repeats to the route latched at press time then
		// sends them somewhere that is no longer listening. Drop them instead;
		// the release still follows the latched route, which is what keeps the
		// press/release pair symmetric.
		if (isRepeat && ((it->second == EventRoute::GUI) != WantGuiCaptureNow()))
			return;
		if (!isRepeat)
		{
			const bool guiNow = WantGuiCaptureNow();
			keyRoutes[key] = guiNow ? EventRoute::GUI : EventRoute::Game;
			it = keyRoutes.find(key);
		}

        int gzkey = ZWidgetKeyToGZDoom(key);

		if (it->second == EventRoute::GUI)
		{
			// Assigned rather than brace-initialised: subtype is uint8_t, and
			// narrowing a non-constant EGUIEvent inside an initializer list is
			// ill-formed. GCC only warns; Clang rejects it.
			event_t guiev = {EV_GUI_Event};
			guiev.subtype = isRepeat ? EV_GUI_KeyRepeat : EV_GUI_KeyDown;
			guiev.data1 = InputKeyToGUIKey(key);
			guiev.data3 = ModMask();
			const bool post = guiev.data1 < 128; // match SDL behavior: uppercase ASCII range only
			if (post)
				D_PostEvent(&guiev);
			I_TraceKeyEvent(isRepeat ? "rep" : "down", (int)key, gzkey, "GUI", post);
		}
		else if (!isRepeat && !RawKeyboardActive)
		{
			// Gameplay bindings are level-triggered: ButtonMap ignores a repeat
			// of a key already held, so there is nothing to post. When raw input
			// is driving, OnWindowRawKey posts these instead.
			event_t ev = {EV_KeyDown};
			ev.data1 = gzkey;
			ev.data2 = 0;
			ev.data3 = ModMask();
			const bool post = ev.data1 != 0;
			if (post)
				D_PostEvent(&ev);
			I_TraceKeyEvent("down", (int)key, gzkey, "Game", post);
		}
    }
    void OnWindowKeyUp(InputKey key) override {
		UpdateModifierKey(key, false);

		auto it = keyRoutes.find(key);
		const EventRoute route = (it != keyRoutes.end()) ? it->second : (WantGuiCaptureNow() ? EventRoute::GUI : EventRoute::Game);
		if (it != keyRoutes.end())
			keyRoutes.erase(it);

        int gzkey = ZWidgetKeyToGZDoom(key);

		if (route == EventRoute::GUI)
		{
			event_t guiev = {EV_GUI_Event, EV_GUI_KeyUp};
			guiev.data1 = InputKeyToGUIKey(key);
			guiev.data3 = ModMask();
			const bool post = guiev.data1 < 128;
			if (post)
				D_PostEvent(&guiev);
			I_TraceKeyEvent("up", (int)key, gzkey, "GUI", post);
		}
		else if (!RawKeyboardActive)
		{
			event_t ev = {EV_KeyUp};
			ev.data1 = gzkey;
			ev.data2 = 0;
			ev.data3 = ModMask();
			const bool post = ev.data1 != 0;
			if (post)
				D_PostEvent(&ev);
			I_TraceKeyEvent("up", (int)key, gzkey, "Game", post);
		}
    }
    void OnWindowRawKey(RawKeycode keycode, bool down) override {
        // Only acted on while raw input is driving gameplay; the backend
        // reports these alongside the translated events regardless.
        if (!RawKeyboardActive)
            return;

        // RawKeycode is the PC scancode set, which is exactly what the engine
        // uses for keyboard bindings, so the value passes through unchanged.
        event_t ev = {};
        ev.type = down ? EV_KeyDown : EV_KeyUp;
        ev.data1 = (int)keycode;
        ev.data3 = ModMask();
        const bool post = ev.data1 != 0;
        if (post)
            D_PostEvent(&ev);
        I_TraceKeyEvent(down ? "rawdn" : "rawup", 0, (int)keycode, "Raw", post);
    }
    void OnWindowGeometryChanged() override {
        if (sysCallbacks.OnScreenSizeChanged)
            sysCallbacks.OnScreenSizeChanged();
    }
    void OnWindowClose() override {
        throw CExitEvent(0);
    }
    void OnWindowActivated() override {
		I_SetWindowFocus(true);
    }
    void OnWindowDeactivated() override {
		I_SetWindowFocus(false);
    }
    void OnWindowDpiScaleChanged() override {
        // Guard: this can fire during window construction (e.g. from the
        // wp_fractional_scale protocol during InitializeToplevel's roundtrip)
        // before NativeWindow has been assigned. Skip the callback safely.
        if (!GetActiveZWidgetWindow()) return;
        if (sysCallbacks.OnScreenSizeChanged)
            sysCallbacks.OnScreenSizeChanged();
    }
};

static std::unique_ptr<GZDoomWindowHost> WindowHost;
static std::unique_ptr<DisplayWindow> NativeWindow;

void I_InitNativeWindow()
{
    WindowHost = std::make_unique<GZDoomWindowHost>();
    
    RenderAPI api = RenderAPI::OpenGL;
    int backend = V_GetBackend();
    if (backend == 1) api = RenderAPI::Vulkan;
    else if (backend == 2) api = RenderAPI::OpenGL; // GLES
    
    NativeWindow = DisplayWindow::Create(WindowHost.get(), WidgetType::Window, nullptr, api);
    
    static bool atexit_registered = false;
    if (!atexit_registered) {
        std::atexit([]() {
            NativeWindow.reset();
            WindowHost.reset();
        });
        atexit_registered = true;
    }
    
    ApplyNativeWindowClientBounds(NativeWindow.get());
    NativeWindow->SetWindowTitle("GZDoom (Native POSIX)");
    NativeWindow->Show();
    
    // Set window icon (Simple GZ pattern)
    uint32_t icon_pixels[32 * 32];
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            bool is_g = (x >= 8 && x <= 24 && y >= 8 && y <= 10) || (x >= 8 && x <= 10 && y >= 8 && y <= 24) || (x >= 8 && x <= 24 && y >= 22 && y <= 24) || (x >= 22 && x <= 24 && y >= 16 && y <= 24) || (x >= 16 && x <= 24 && y >= 16 && y <= 18);
            icon_pixels[y * 32 + x] = is_g ? 0xFFFFFFFF : 0xFF333333; // White G on Dark Gray
        }
    }
    auto icon_image = Image::Create(32, 32, ImageFormat::B8G8R8A8, icon_pixels);
    NativeWindow->SetWindowIcon({ icon_image });

    // Set global X11 display for gl_sysfb.cpp
    if (DisplayBackend::Get()->IsX11()) {
        X11NativeHandle* x11 = (X11NativeHandle*)NativeWindow->GetNativeHandle();
        X11NativeDisplay = x11->display;
    } else if (DisplayBackend::Get()->IsWayland()) {
        // Wayland handles are retrieved differently in gl_sysfb.cpp
    }

#ifdef HAVE_VULKAN
    if (V_GetBackend() == 1)
    {
        try {
            unsigned int count = 0;
            I_GetVulkanPlatformExtensions(&count, nullptr);
            std::vector<const char*> names(count);
            I_GetVulkanPlatformExtensions(&count, names.data());

            VulkanInstanceBuilder builder;
            builder.DebugLayer(vk_debug);
            for (auto name : names) builder.RequireExtension(name);
            auto instance = builder.Create();

            VkSurfaceKHR surfacehandle = nullptr;
            if (!I_CreateVulkanSurface(instance->Instance, &surfacehandle))
                throw std::runtime_error("I_CreateVulkanSurface failed");

            m_vulkanSurface = std::make_shared<VulkanSurface>(instance, surfacehandle);
            fprintf(stderr, "[VULKAN] Native Vulkan surface created successfully\n");
        } catch (const std::exception& e) {
            fprintf(stderr, "[VULKAN] Initialization failed: %s\n", e.what());
        }
    }
#endif
}

DisplayWindow* GetActiveZWidgetWindow()
{
    return NativeWindow.get();
}

class NativeVideo : public IVideo
{
public:
    DFrameBuffer *CreateFrameBuffer() override {
        if (V_GetBackend() == 1) {
#ifdef HAVE_VULKAN
            if (m_vulkanSurface)
                return new VulkanRenderDevice(nullptr, vid_fullscreen, m_vulkanSurface);
#endif
        }

        if (V_GetBackend() == 2) { // GLES
            DisplayWindow* win = GetActiveZWidgetWindow();
            if (win) {
                void* display = win->GetEGLNativeDisplay();
                void* surface = win->GetEGLNativeWindow();
                bool isWayland = DisplayBackend::Get()->IsWayland();
                return new OpenGLESRenderer::OpenGLFrameBuffer(display, surface, isWayland);
            }
        }

        DisplayWindow* win = GetActiveZWidgetWindow();
        if (win) {
            void* display = nullptr;
            void* surface = nullptr;
            bool isWayland = false;
            
            void* handle = win->GetNativeHandle();
            if (DisplayBackend::Get()->IsX11()) {
                X11NativeHandle* x11 = (X11NativeHandle*)handle;
                display = x11->display;
                surface = (void*)x11->window;
            } else if (DisplayBackend::Get()->IsWayland()) {
                WaylandNativeHandle* wayland = (WaylandNativeHandle*)handle;
                display = wayland->display;
                surface = wayland->surface;
                isWayland = true;
            }
            return new OpenGLRenderer::OpenGLFrameBuffer(display, surface, isWayland);
        }
        return new OpenGLRenderer::OpenGLFrameBuffer(0, false);
    }
    void SetWindowTitle(const char* title) override {
        if (NativeWindow) NativeWindow->SetWindowTitle(title);
    }
};

IVideo *gl_CreateVideo()
{
	return new NativeVideo();
}

#include <zvulkan/vulkanobjects.h>

bool I_CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface)
{
    if (auto window = GetActiveZWidgetWindow())
    {
        try {
            *surface = window->CreateVulkanSurface(instance);
            return true;
        } catch (const std::exception& e) {
            fprintf(stderr, "[VULKAN] CreateVulkanSurface failed: %s\n", e.what());
            return false;
        }
    }
    return false;
}

bool I_GetVulkanPlatformExtensions(unsigned int *count, const char **names)
{
    if (auto window = GetActiveZWidgetWindow())
    {
        static std::vector<std::string> extensions;
        static std::vector<const char*> extension_pointers;

        extensions = window->GetVulkanInstanceExtensions();
        extension_pointers.clear();
        for (const auto& ext : extensions) extension_pointers.push_back(ext.c_str());

        *count = (unsigned int)extension_pointers.size();
        if (names) {
            for (unsigned int i = 0; i < *count; i++) names[i] = extension_pointers[i];
        }
        return true;
    }
    return false;
}
