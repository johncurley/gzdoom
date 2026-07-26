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

void I_StartTic() {
	// Mirror SDL/cocoa behavior: GUI capture and mouse capture policy is evaluated per-tic,
	// not just on input events.
	I_CheckGUICapture();
	I_CheckNativeMouse();
	I_ReconcileMouseButtons();
	I_GetEvent();
}

void I_StartFrame() {}
bool NativeMouseCaptured = false;

bool GUICapture = false;
static bool NativeMouse = true;
static bool HasFocus = true;

CVAR (Bool, use_mouse, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

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
    }
}

void I_ReleaseMouseCapture() {
    NativeMouseCaptured = false;
    if (auto window = GetActiveZWidgetWindow()) {
        window->UnlockCursor();
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
			window->SetCursor(StandardCursor::arrow);
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
