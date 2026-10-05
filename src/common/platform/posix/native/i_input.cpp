#include "i_input.h"
#include "c_buttons.h"
#include "c_cvars.h"
#include "d_gui.h"
#include "i_interface.h"
#include "keydef.h"
#include "d_eventbase.h"
#include "m_haptics.h"
#include "bitmap.h"
#include "textures.h"
#include "gametexture.h"
#include "native_display.h"
#include <zwidget/core/image.h>
#include <zwidget/window/window.h>
#include <array>
#include <cstdio>
#include "engineerrors.h"
#include "printf.h"
#include "zstring.h"

// Forward declaration from nativevideo.cpp
extern DisplayWindow* GetActiveZWidgetWindow();

// Serialize ZWidget event processing: callbacks may call back into I_GetEvent while
// DisplayWindow::ProcessEvents() is still active. Re-entering ProcessEvents() recurses
// through the same backend and can amplify engine-side work (e.g. button resets).
// Nested requests set deferred_event_pump; the outer pump runs follow-up ProcessEvents()
// calls after the current one returns (same stack, no re-entrancy).
static bool event_processing_in_progress = false;
static bool deferred_event_pump = false;

static constexpr int kMaxDeferredPumps = 1024;

static void I_CheckGUICapture();
static void I_CheckRawKeyboard();
static void I_CheckNativeMouse();
static void I_ReconcileMouseButtons();
extern void I_PollJoystickDeviceChanges();
extern void I_ProcessJoysticks();

void I_GetEvent() {
	if (event_processing_in_progress) {
		deferred_event_pump = true;
		return;
	}

	event_processing_in_progress = true;
	int pumps = 0;
	try {
		do {
			deferred_event_pump = false;
			DisplayWindow::ProcessEvents();
			pumps++;
			if (pumps > kMaxDeferredPumps) {
				break;
			}
		} while (deferred_event_pump);
	} catch (const CExitEvent& e) {
		throw; // Rethrow CExitEvent to exit the game
	} catch (const std::exception& e) {
		fprintf(stderr, "ERROR in DisplayWindow::ProcessEvents(): %s\n", e.what());
	} catch (...) {
		fprintf(stderr, "ERROR: Unknown exception in DisplayWindow::ProcessEvents()\n");
	}
	event_processing_in_progress = false;
}

void I_StartTic() {
	// Clear the per-tic edge flags before pumping new input, as the win32 and
	// cocoa backends do. bWentDown/bWentUp are set by PressKey/ReleaseKey and
	// are only ever cleared here, and G_BuildTiccmd reads them:
	//
	//   if (ButtonDown(Button_Jump) || ButtonPressed(Button_Jump)) buttons |= BT_JUMP;
	//
	// ButtonPressed() is bWentDown, so without this a single tap latches the
	// button on for every subsequent tic -- one press produces continuous
	// jumping or firing until something else happens to reset button state.
	buttonMap.ResetButtonTriggers();

	// Mirror SDL/cocoa behavior: GUI capture and mouse capture policy is evaluated per-tic,
	// not just on input events.
	I_CheckGUICapture();
	I_CheckRawKeyboard();
	I_CheckNativeMouse();
	I_ReconcileMouseButtons();
	I_PollJoystickDeviceChanges();
	I_ProcessJoysticks();
	I_GetEvent();
	Joy_RumbleTick();
}

void I_StartFrame() {}
bool NativeMouseCaptured = false;

bool GUICapture = false;
static bool NativeMouse = true;
static bool HasFocus = true;

CVAR (Bool, use_mouse, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

// Physical key positions instead of translated symbols for gameplay input.
// Scancodes are layout- and modifier-independent, so W is the same physical key
// on QWERTY, AZERTY and QWERTZ, and none of the keysym translation that the
// cooked path depends on is involved.
//
// GUI input deliberately stays on the translated path: menus and the console
// need symbols and text, which a scancode cannot provide.
CVAR (Bool, in_rawkeyboard, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

// Read by nativevideo.cpp to suppress the cooked gameplay events while raw is
// driving, since the backend reports both.
bool RawKeyboardActive = false;

static void I_CheckRawKeyboard()
{
	const bool want = in_rawkeyboard && !GUICapture;
	if (want == RawKeyboardActive)
		return;

	RawKeyboardActive = want;
	if (auto* window = GetActiveZWidgetWindow())
	{
		if (want)
			window->LockKeyboard();
		else
			window->UnlockKeyboard();
	}

	// Switching paths mid-keypress would strand whatever is held: the press
	// arrived on one path and the release will arrive on the other.
	buttonMap.ResetButtonStates();
}

static void I_CheckGUICapture()
{
	bool wantCapt = sysCallbacks.WantGuiCapture && sysCallbacks.WantGuiCapture();
	if (wantCapt != GUICapture)
	{
		GUICapture = wantCapt;
		if (wantCapt)
			buttonMap.ResetButtonStates();
	}
}

static void I_CheckNativeMouse()
{
	bool captureModeInGame = sysCallbacks.CaptureModeInGame && sysCallbacks.CaptureModeInGame();
	bool wantNative = !HasFocus || (!use_mouse || GUICapture || !captureModeInGame);

	if (!wantNative && sysCallbacks.WantNativeMouse && sysCallbacks.WantNativeMouse())
		wantNative = true;

	if (wantNative != NativeMouse)
	{
		NativeMouse = wantNative;
		if (wantNative)
			I_ReleaseMouseCapture();
		else
			I_SetMouseCapture();
	}
	else if (!GUICapture && NativeMouseCaptured == wantNative)
	{
		// Menu drag capture can call I_SetMouseCapture/I_ReleaseMouseCapture
		// directly without changing NativeMouse. Reconcile that actual state
		// after leaving the UI so gameplay capture cannot stay released.
		if (wantNative)
			I_ReleaseMouseCapture();
		else
			I_SetMouseCapture();
	}
}

static void I_ReconcileMouseButtons()
{
	// If we miss a wl_pointer button release (or it gets delivered while focus/capture flips),
	// the game can get a stuck KEY_MOUSE* down state. As a safety net, reconcile current
	// ZWidget button state once per tic and synthesize missing releases/presses for gameplay.
	//
	// We only do this for gameplay routing (GUICapture == false).
	if (GUICapture)
		return;

	auto* window = GetActiveZWidgetWindow();
	if (!window)
		return;

	struct Btn { InputKey ik; int16_t key; };
	static const Btn btns[] = {
		{ InputKey::LeftMouse,  KEY_MOUSE1 },
		{ InputKey::RightMouse, KEY_MOUSE2 },
		{ InputKey::MiddleMouse,KEY_MOUSE3 },
	};

	static bool lastDown[3] = { false, false, false };

	for (int i = 0; i < 3; i++)
	{
		const bool downNow = window->GetKeyState(btns[i].ik);
		if (downNow == lastDown[i])
			continue;

		event_t ev = {};
		ev.type = downNow ? EV_KeyDown : EV_KeyUp;
		ev.data1 = btns[i].key;
		D_PostEvent(&ev);
		lastDown[i] = downNow;
	}
}

void I_SetMouseCapture() {
    NativeMouseCaptured = true;
    if (auto window = GetActiveZWidgetWindow()) {
        window->LockCursor();
        // Locking confines the pointer but still draws it. In windowed mode
        // that leaves a cursor sitting in the middle of the view during
        // mouse-look, so hide it for the duration of the capture.
        window->ShowCursor(false);
    }
}

void I_ReleaseMouseCapture() {
    NativeMouseCaptured = false;
    if (auto window = GetActiveZWidgetWindow()) {
        window->UnlockCursor();
        window->ShowCursor(true);
    }
}

void I_SetNativeMouse(bool wantNative)
{
	// NativeMouse is a hint from the engine. We re-evaluate capture policy immediately.
	I_CheckNativeMouse();
}

bool I_SetCursor(FGameTexture* cursor)
{
	if (auto window = GetActiveZWidgetWindow())
	{
		if (!cursor || !cursor->isValid())
		{
			window->SetCursor(StandardCursor::arrow, nullptr);
			return true;
		}

		FBitmap source = cursor->GetTexture()->GetBgraBitmap(nullptr);
		if (source.GetWidth() <= 0 || source.GetHeight() <= 0 ||
			source.GetWidth() > 32 || source.GetHeight() > 32)
		{
			window->SetCursor(StandardCursor::arrow, nullptr);
			return false;
		}

		// Match the existing SDL cursor contract: a transparent 32x32 canvas,
		// the texture at its top-left corner, and a (0, 0) hotspot.
		std::array<uint8_t, 32 * 32 * 4> pixels = {};
		FBitmap bitmap(pixels.data(), 32 * 4, 32, 32);
		bitmap.Blit(0, 0, source);
		auto image = Image::Create(32, 32, ImageFormat::B8G8R8A8, pixels.data());
		std::vector<CustomCursorFrame> frames;
		frames.emplace_back(std::move(image));
		auto custom = CustomCursor::Create(std::move(frames), Point(0, 0));
		window->SetCursor(StandardCursor::arrow, std::move(custom));
		return true;
	}
	return false;
}

void I_SetWindowFocus(bool focused)
{
	HasFocus = focused;
	// On focus loss, release capture immediately and drop any latched button state.
	if (!HasFocus)
	{
		buttonMap.ResetButtonStates();
		I_ReleaseMouseCapture();
	}
	// Re-evaluate capture policy on focus transitions.
	I_CheckNativeMouse();
}
