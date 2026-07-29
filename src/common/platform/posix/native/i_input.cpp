#include "i_input.h"
#include "c_buttons.h"
#include "c_cvars.h"
#include "d_gui.h"
#include "i_interface.h"
#include "keydef.h"
#include "d_eventbase.h"
#include "native_display.h"
#include <zwidget/window/window.h>
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

// Temporary diagnostic, paired with the [keytrace] output in nativevideo.cpp.
// Enable with `in_keytrace 1`. Key events are posted to a queue and only applied
// when the engine drains it, so the button state cannot be read back at post
// time -- it is reported here instead, once per tic and only when it changes.
//
// Reading the two streams together: every "[keytrace] up ... route=Game posted"
// should be followed by that key's action leaving the held set. If the action
// stays held, the key-up reached the engine and ButtonMap did not act on it.
// If no "up" line appears at all, the release never made it this far.
EXTERN_CVAR(Bool, in_keytrace)

static void I_TraceHeldButtons()
{
	if (!in_keytrace)
		return;

	static FString last;
	static bool everPrinted = false;

	FString now;
	for (int i = 0; i < buttonMap.NumButtons(); i++)
	{
		if (buttonMap.ButtonDown(i))
		{
			if (now.IsNotEmpty()) now << ' ';
			now << buttonMap.GetButtonName(i);
		}
	}
	// Always emit the first sample, so enabling the cvar visibly confirms the
	// trace is live even when nothing is held yet.
	if (!everPrinted || now.Compare(last) != 0)
	{
		// stderr, not Printf: once the video backend is up, Printf goes to the
		// in-game console rather than the terminal, and this trace needs to be
		// capturable from a piped run.
		fprintf(stderr, "[keytrace] held: %s\n", now.IsNotEmpty() ? now.GetChars() : "(none)");
		fflush(stderr);
		last = now;
		everPrinted = true;
	}
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
	I_GetEvent();
	I_TraceHeldButtons();
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
		// If cursor is null, we show the default arrow.
		// If it's a game texture, the engine handles drawing it in the 2D pass,
		// so we should ideally hide the hardware cursor.
		if (cursor == nullptr)
			window->SetCursor(StandardCursor::arrow, nullptr);
		else
			window->ShowCursor(false);
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
