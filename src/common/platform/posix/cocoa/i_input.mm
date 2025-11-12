/*
 ** i_input.mm
 **
 **---------------------------------------------------------------------------
 ** Copyright 2012-2015 Alexey Lysiuk
 ** All rights reserved.
 **
 ** Redistribution and use in source and binary forms, with or without
 ** modification, are permitted provided that the following conditions
 ** are met:
 **
 ** 1. Redistributions of source code must retain the above copyright
 **    notice, this list of conditions and the following disclaimer.
 ** 2. Redistributions in binary form must reproduce the above copyright
 **    notice, this list of conditions and the following disclaimer in the
 **    documentation and/or other materials provided with the distribution.
 ** 3. The name of the author may not be used to endorse or promote products
 **    derived from this software without specific prior written permission.
 **
 ** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 ** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 ** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 ** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 ** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 ** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 ** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 ** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 ** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 ** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **---------------------------------------------------------------------------
 **
 */

#include "i_common.h"
#include <cmath>

// macOS Virtual Key Codes (kVK_* constants)
// These were previously from Carbon.h which is deprecated
// Defining them locally for modern macOS compatibility (10.13+)
// Values are standardized and documented in IOKit/hidsystem/IOLLEvent.h
enum : unsigned short
{
	kVK_ANSI_A              = 0x00,
	kVK_ANSI_F              = 0x03,
	kVK_Return              = 0x24,
	kVK_Tab                 = 0x30,
	kVK_Delete              = 0x33,
	kVK_Escape              = 0x35,
	kVK_F1                  = 0x7A,
	kVK_F2                  = 0x78,
	kVK_F3                  = 0x63,
	kVK_F4                  = 0x76,
	kVK_F5                  = 0x60,
	kVK_F6                  = 0x61,
	kVK_F7                  = 0x62,
	kVK_F8                  = 0x64,
	kVK_F9                  = 0x65,
	kVK_F10                 = 0x6D,
	kVK_F11                 = 0x67,
	kVK_F12                 = 0x6F,
	kVK_Home                = 0x73,
	kVK_PageUp              = 0x74,
	kVK_ForwardDelete       = 0x75,
	kVK_End                 = 0x77,
	kVK_PageDown            = 0x79,
	kVK_LeftArrow           = 0x7B,
	kVK_RightArrow          = 0x7C,
	kVK_DownArrow           = 0x7D,
	kVK_UpArrow             = 0x7E
};

#include "c_console.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "d_eventbase.h"
#include "c_buttons.h"
#include "d_gui.h"
#include "dikeys.h"
#include "v_video.h"
#include "i_interface.h"
#include "menustate.h"
#include "engineerrors.h"
#include "keydef.h"


EXTERN_CVAR(Int, m_use_mouse)

CVAR(Bool, use_mouse,    true,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, k_allowfullscreentoggle, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

extern int paused;
extern bool ToggleFullscreen;
bool GUICapture;


namespace
{

// Skip mouse move events counter
// When programmatically repositioning the cursor (e.g., centering for FPS),
// macOS generates synthetic mouse move events that we need to ignore.
// This counter tracks how many events to skip after a cursor reposition.
size_t s_skipMouseMoves;


// ---------------------------------------------------------------------------


void CheckGUICapture()
{
	bool wantCapt = sysCallbacks.WantGuiCapture && sysCallbacks.WantGuiCapture();

	if (wantCapt != GUICapture)
	{
		GUICapture = wantCapt;
		if (wantCapt)
		{
			buttonMap.ResetButtonStates();
		}
	}

}

// Programmatically set cursor position (used when transitioning to/from raw input)
//
// When we programmatically move the cursor, macOS generates synthetic
// NSMouseMoved events that we don't want to process as actual mouse input.
// We set s_skipMouseMoves to ignore these synthetic events.
//
// The value of 2 is empirically determined:
// - 1 event for the CGEventPost
// - 1 additional event that sometimes occurs due to event coalescing
void SetCursorPosition(const NSPoint position)
{
	NSWindow* window = [NSApp keyWindow];
	if (nil == window)
	{
		return;
	}

	const NSRect  displayRect = [[window screen] frame];
	const CGPoint eventPoint = CGPointMake(position.x, displayRect.size.height - position.y);

	CGEventSourceRef eventSource = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);

	if (NULL != eventSource)
	{
		CGEventRef mouseMoveEvent = CGEventCreateMouseEvent(eventSource,
			kCGEventMouseMoved, eventPoint, kCGMouseButtonLeft);

		if (NULL != mouseMoveEvent)
		{
			CGEventPost(kCGHIDEventTap, mouseMoveEvent);
			CFRelease(mouseMoveEvent);
		}

		CFRelease(eventSource);
	}

	// Skip the synthetic mouse move events generated by programmatic cursor movement
	s_skipMouseMoves = 2;
}

void CenterCursor()
{
	NSWindow* window = [NSApp keyWindow];
	if (nil == window)
	{
		return;
	}

	const NSRect  displayRect = [[window screen] frame];
	const NSRect   windowRect = [window frame];
	const NSPoint centerPoint = { NSMidX(windowRect), NSMidY(windowRect) };

	SetCursorPosition(centerPoint);
}

void CheckNativeMouse()
{
	const bool windowed = (NULL == screen) || !screen->IsFullscreen();
	bool wantNative;

	if (windowed)
	{
		if (![NSApp isActive] || !use_mouse)
		{
			wantNative = true;
		}
		else if (MENU_WaitKey == menuactive)
		{
			wantNative = false;
		}
		else
		{
			bool captureModeInGame = sysCallbacks.CaptureModeInGame && sysCallbacks.CaptureModeInGame();
			wantNative = (!m_use_mouse || MENU_WaitKey != menuactive)
				&& (!captureModeInGame || GUICapture);
		}
	}
	else
	{
		// ungrab mouse when in the menu with mouse control on.
		wantNative = m_use_mouse
			&& (MENU_On == menuactive || MENU_OnNoPause == menuactive);
	}

	if (!wantNative && sysCallbacks.WantNativeMouse && sysCallbacks.WantNativeMouse())
		wantNative = true;

	I_SetNativeMouse(wantNative);
}

} // unnamed namespace


void I_GetEvent()
{
	[[NSRunLoop currentRunLoop] limitDateForMode:NSDefaultRunLoopMode];
}

void I_StartTic()
{
	CheckGUICapture();
	CheckNativeMouse();

	I_ProcessJoysticks();
	I_GetEvent();
}

void I_StartFrame()
{
}


void I_SetMouseCapture()
{
}

void I_ReleaseMouseCapture()
{
}

// Toggle between native cursor and raw mouse input mode
//
// Native Mode (wantNative = true):
// - Cursor visible and free-moving
// - Used for menus, console, windowed mode
// - Mouse acceleration applied by system
//
// Raw Input Mode (wantNative = false):
// - Cursor hidden and locked
// - Used for FPS gameplay
// - CGAssociateMouseAndMouseCursorPosition disables cursor coupling
// - Provides raw, unaccelerated mouse deltas via deltaX/deltaY
// - Cursor is centered to prevent edge-of-screen issues
//
// This is the macOS equivalent of Windows raw input for gaming
void I_SetNativeMouse(bool wantNative)
{
	static bool nativeMouse = true;
	static NSPoint mouseLocation;

	if (wantNative != nativeMouse)
	{
		nativeMouse = wantNative;

		if (!wantNative)
		{
			// Entering raw input mode for gameplay
			mouseLocation = [NSEvent mouseLocation];
			CenterCursor();  // Center cursor to prevent edge issues
		}

		// Key call: disables cursor-mouse coupling for raw input
		// When false, mouse movement is reported via deltaX/deltaY only
		CGAssociateMouseAndMouseCursorPosition(wantNative);

		if (wantNative)
		{
			// Returning to native cursor mode
			SetCursorPosition(mouseLocation);  // Restore cursor position

			[NSCursor unhide];
		}
		else
		{
			// Hide cursor for raw input mode
			[NSCursor hide];
		}
	}
}


// ---------------------------------------------------------------------------


namespace
{

const size_t KEY_COUNT = 128;


// macOS Virtual Key Code to DirectInput Key mapping
// Translates NSEvent keycodes (0x00-0x7F) to GZDoom DIK_ constants
// Based on standard macOS keyboard layout

const uint8_t KEYCODE_TO_DIK[KEY_COUNT] =
{
	DIK_A,        DIK_S,             DIK_D,         DIK_F,        DIK_H,           DIK_G,       DIK_Z,        DIK_X,          // 0x00 - 0x07
	DIK_C,        DIK_V,             0,             DIK_B,        DIK_Q,           DIK_W,       DIK_E,        DIK_R,          // 0x08 - 0x0F
	DIK_Y,        DIK_T,             DIK_1,         DIK_2,        DIK_3,           DIK_4,       DIK_6,        DIK_5,          // 0x10 - 0x17
	DIK_EQUALS,   DIK_9,             DIK_7,         DIK_MINUS,    DIK_8,           DIK_0,       DIK_RBRACKET, DIK_O,          // 0x18 - 0x1F
	DIK_U,        DIK_LBRACKET,      DIK_I,         DIK_P,        DIK_RETURN,      DIK_L,       DIK_J,        DIK_APOSTROPHE, // 0x20 - 0x27
	DIK_K,        DIK_SEMICOLON,     DIK_BACKSLASH, DIK_COMMA,    DIK_SLASH,       DIK_N,       DIK_M,        DIK_PERIOD,     // 0x28 - 0x2F
	DIK_TAB,      DIK_SPACE,         DIK_GRAVE,     DIK_BACK,     0,               DIK_ESCAPE,  0,            DIK_LWIN,       // 0x30 - 0x37
	DIK_LSHIFT,   DIK_CAPITAL,       DIK_LMENU,     DIK_LCONTROL, DIK_RSHIFT,      DIK_RMENU,   DIK_RCONTROL, 0,              // 0x38 - 0x3F
	0,            DIK_DECIMAL,       0,             DIK_MULTIPLY, 0,               DIK_ADD,     0,            0,              // 0x40 - 0x47
	DIK_VOLUMEUP, DIK_VOLUMEDOWN,    DIK_MUTE,      DIK_SLASH,    DIK_NUMPADENTER, 0,           DIK_SUBTRACT, 0,              // 0x48 - 0x4F
	0,            DIK_NUMPAD_EQUALS, DIK_NUMPAD0,   DIK_NUMPAD1,  DIK_NUMPAD2,     DIK_NUMPAD3, DIK_NUMPAD4,  DIK_NUMPAD5,    // 0x50 - 0x57
	DIK_NUMPAD6,  DIK_NUMPAD7,       0,             DIK_NUMPAD8,  DIK_NUMPAD9,     0,           0,            0,              // 0x58 - 0x5F
	DIK_F5,       DIK_F6,            DIK_F7,        DIK_F3,       DIK_F8,          DIK_F9,      0,            DIK_F11,        // 0x60 - 0x67
	0,            DIK_F13,           0,             DIK_F14,      0,               DIK_F10,     0,            DIK_F12,        // 0x68 - 0x6F
	0,            DIK_F15,           0,             DIK_HOME,     0,               DIK_DELETE,  DIK_F4,       DIK_END,        // 0x70 - 0x77
	DIK_F2,       0,                 DIK_F1,        DIK_LEFT,     DIK_RIGHT,       DIK_DOWN,    DIK_UP,       0,              // 0x78 - 0x7F
};

const uint8_t KEYCODE_TO_ASCII[KEY_COUNT] =
{
	'a', 's',  'd', 'f', 'h', 'g', 'z',  'x', // 0x00 - 0x07
	'c', 'v',    0, 'b', 'q', 'w', 'e',  'r', // 0x08 - 0x0F
	'y', 't',  '1', '2', '3', '4', '6',  '5', // 0x10 - 0x17
	'=', '9',  '7', '-', '8', '0', ']',  'o', // 0x18 - 0x1F
	'u', '[',  'i', 'p',  13, 'l', 'j', '\'', // 0x20 - 0x27
	'k', ';', '\\', ',', '/', 'n', 'm',  '.', // 0x28 - 0x2F
	  9, ' ',  '`',  12,   0,  27,   0,    0, // 0x30 - 0x37
	  0,   0,    0,   0,   0,   0,   0,    0, // 0x38 - 0x3F
	  0,   0,    0,   0,   0,   0,   0,    0, // 0x40 - 0x47
	  0,   0,    0,   0,   0,   0,   0,    0, // 0x48 - 0x4F
	  0,   0,    0,   0,   0,   0,   0,    0, // 0x50 - 0x57
	  0,   0,    0,   0,   0,   0,   0,    0, // 0x58 - 0x5F
	  0,   0,    0,   0,   0,   0,   0,    0, // 0x60 - 0x67
	  0,   0,    0,   0,   0,   0,   0,    0, // 0x68 - 0x6F
	  0,   0,    0,   0,   0,   0,   0,    0, // 0x70 - 0x77
	  0,   0,    0,   0,   0,   0,   0,    0, // 0x78 - 0x7F
};


uint8_t ModifierToDIK(const uint32_t modifier)
{
	switch (modifier)
	{
		case NSEventModifierFlagCapsLock: return DIK_CAPITAL;
		case NSEventModifierFlagShift:    return DIK_LSHIFT;
		case NSEventModifierFlagControl:  return DIK_LCONTROL;
		case NSEventModifierFlagOption:   return DIK_LMENU;
		case NSEventModifierFlagCommand:  return DIK_LWIN;
	}

	return 0;
}

int16_t ModifierFlagsToGUIKeyModifiers(NSEvent* theEvent)
{
	const NSUInteger modifiers([theEvent modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask);
	return ((modifiers & NSEventModifierFlagShift  ) ? GKM_SHIFT : 0)
		 | ((modifiers & NSEventModifierFlagControl) ? GKM_CTRL  : 0)
		 | ((modifiers & NSEventModifierFlagOption ) ? GKM_ALT   : 0)
		 | ((modifiers & NSEventModifierFlagCommand) ? GKM_META  : 0);
}

bool ShouldGenerateGUICharEvent(NSEvent* theEvent)
{
	const NSUInteger modifiers([theEvent modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask);
	return !(modifiers & NSEventModifierFlagControl)
		&& !(modifiers & NSEventModifierFlagOption)
		&& !(modifiers & NSEventModifierFlagCommand)
		&& !(modifiers & NSEventModifierFlagFunction);
}


NSStringEncoding GetEncodingForUnicodeCharacter(const unichar character)
{
	if (character >= L'\u0100' && character <= L'\u024F')
	{
		return NSWindowsCP1250StringEncoding; // Central and Eastern Europe
	}
	else if (character >= L'\u0370' && character <= L'\u03FF')
	{
		return NSWindowsCP1253StringEncoding; // Greek
	}
	else if (character >= L'\u0400' && character <= L'\u04FF')
	{
		return NSWindowsCP1251StringEncoding; // Cyrillic
	}

	// TODO: add handling for other characters
	// TODO: Turkish should use NSWindowsCP1254StringEncoding

	return NSWindowsCP1252StringEncoding;
}

unsigned char GetCharacterFromNSEvent(NSEvent* theEvent, unichar *realchar)
{
	const NSString* unicodeCharacters = [theEvent characters];

	if (0 == [unicodeCharacters length])
	{
		return '\0';
	}

	const unichar unicodeCharacter = [unicodeCharacters characterAtIndex:0];
	const NSStringEncoding encoding = GetEncodingForUnicodeCharacter(unicodeCharacter);

	unsigned char character = '\0';

	if (NSWindowsCP1252StringEncoding == encoding)
	{
		// TODO: make sure that the following is always correct
		character = unicodeCharacter & 0xFF;
	}
	else
	{
		const NSData* const characters =
			[[theEvent characters] dataUsingEncoding:encoding];

		character = [characters length] > 0
			? *static_cast<const unsigned char*>([characters bytes])
			: '\0';
	}

	*realchar = unicodeCharacter;
	return character;
}

// Process keyboard events for menu/console/chat input
//
// This function posts TWO types of events for different purposes:
// 1. Key events (EV_GUI_KeyDown/Up) - for key binding and menu navigation
// 2. Character events (EV_GUI_Char) - for text input (console, chat)
//
// For key binding in the options menu:
// - The menu should only listen to EV_GUI_KeyDown (initial press)
// - It should ignore EV_GUI_KeyRepeat (held key repeats)
// - It should ignore EV_GUI_Char (character input events)
//
// This fixes a bug where keys were registered multiple times when binding hotkeys
void ProcessKeyboardEventInMenu(NSEvent* theEvent)
{
	event_t event = {};

	unichar realchar;
	event.type    = EV_GUI_Event;
	event.subtype = NSEventTypeKeyDown == [theEvent type] ? EV_GUI_KeyDown : EV_GUI_KeyUp;
	event.data2   = GetCharacterFromNSEvent(theEvent, &realchar);
	event.data3   = ModifierFlagsToGUIKeyModifiers(theEvent);

	// Handle key repeats - convert to EV_GUI_KeyRepeat for menu navigation
	// Note: Key binding should ignore repeats to prevent double-registration
	const bool isRepeat = [theEvent isARepeat];
	if (EV_GUI_KeyDown == event.subtype && isRepeat)
	{
		event.subtype = EV_GUI_KeyRepeat;
	}

	const unsigned short keyCode = [theEvent keyCode];

	switch (keyCode)
	{
		case kVK_Return:        event.data1 = GK_RETURN;    break;
		case kVK_PageUp:        event.data1 = GK_PGUP;      break;
		case kVK_PageDown:      event.data1 = GK_PGDN;      break;
		case kVK_End:           event.data1 = GK_END;       break;
		case kVK_Home:          event.data1 = GK_HOME;      break;
		case kVK_LeftArrow:     event.data1 = GK_LEFT;      break;
		case kVK_RightArrow:    event.data1 = GK_RIGHT;     break;
		case kVK_UpArrow:       event.data1 = GK_UP;        break;
		case kVK_DownArrow:     event.data1 = GK_DOWN;      break;
		case kVK_Delete:        event.data1 = GK_BACKSPACE; break;
		case kVK_ForwardDelete: event.data1 = GK_DEL;       break;
		case kVK_Escape:        event.data1 = GK_ESCAPE;    break;
		case kVK_F1:            event.data1 = GK_F1;        break;
		case kVK_F2:            event.data1 = GK_F2;        break;
		case kVK_F3:            event.data1 = GK_F3;        break;
		case kVK_F4:            event.data1 = GK_F4;        break;
		case kVK_F5:            event.data1 = GK_F5;        break;
		case kVK_F6:            event.data1 = GK_F6;        break;
		case kVK_F7:            event.data1 = GK_F7;        break;
		case kVK_F8:            event.data1 = GK_F8;        break;
		case kVK_F9:            event.data1 = GK_F9;        break;
		case kVK_F10:           event.data1 = GK_F10;       break;
		case kVK_F11:           event.data1 = GK_F11;       break;
		case kVK_F12:           event.data1 = GK_F12;       break;
		default:
			event.data1 = KEYCODE_TO_ASCII[keyCode];
			break;
	}

	// Post the key event (for key binding and menu navigation)
	// This is the primary event that should be used for key binding
	if (event.data1 < 128)
	{
		event.data1 = toupper(event.data1);

		D_PostEvent(&event);
	}

	// Post a separate character event for text input (console, chat, etc.)
	// Note: This should NOT be used for key binding, only for text entry
	// Skip this for KeyUp events and control characters
	if (!iscntrl(event.data2)
		&& EV_GUI_KeyUp != event.subtype
		&& EV_GUI_KeyRepeat != event.subtype  // Don't send char events for repeats
		&& ShouldGenerateGUICharEvent(theEvent))
	{
		event.subtype = EV_GUI_Char;
		event.data1   = realchar;
		event.data2   = event.data3 & GKM_ALT;

		D_PostEvent(&event);
	}
}


void NSEventToGameMousePosition(NSEvent* inEvent, event_t* outEvent)
{
	const NSWindow* window = [inEvent window];
	const NSView*     view = [window contentView];

	const NSPoint screenPos = [NSEvent mouseLocation];
	const NSRect screenRect = NSMakeRect(screenPos.x, screenPos.y, 0, 0);
	const NSRect windowRect = [window convertRectFromScreen:screenRect];

	NSPoint viewPos;
	NSSize  viewSize;
	CGFloat scale;

	if (view.layer == nil)
	{
		viewPos  = [view convertPointToBacking:windowRect.origin];
		viewSize = [view convertSizeToBacking:view.frame.size];
		scale    = 1.0;
	}
	else
	{
		viewPos  = windowRect.origin;
		viewSize = view.frame.size;
		scale    = view.layer.contentsScale;
	}

	const CGFloat posX =                    viewPos.x  * scale;
	const CGFloat posY = (viewSize.height - viewPos.y) * scale;

	outEvent->data1 = static_cast<int16_t>(posX);
	outEvent->data2 = static_cast<int16_t>(posY);

	screen->ScaleCoordsFromWindow(outEvent->data1, outEvent->data2);
}

void ProcessMouseMoveInMenu(NSEvent* theEvent)
{
	event_t event = {};

	event.type    = EV_GUI_Event;
	event.subtype = EV_GUI_MouseMove;
	event.data3   = ModifierFlagsToGUIKeyModifiers(theEvent);

	NSEventToGameMousePosition(theEvent, &event);

	D_PostEvent(&event);
}

// Raw mouse input for gaming (macOS 10.13+)
//
// This uses NSEvent deltaX/deltaY for precise, unaccelerated mouse movement.
// When the cursor is hidden (via CGAssociateMouseAndMouseCursorPosition),
// macOS provides raw delta values directly from the mouse hardware.
//
// Benefits:
// - Sub-pixel precision (CGFloat deltas)
// - No mouse acceleration curve applied
// - Direct hardware input for competitive gaming
// - Consistent with Windows raw input behavior
//
// The deltas are converted to integers for the engine's PostMouseMove(),
// which applies user sensitivity settings (m_sensitivity_x/y)
void ProcessMouseMoveInGame(NSEvent* theEvent)
{
	// Get raw delta values (sub-pixel precision)
	const CGFloat deltaX = [theEvent deltaX];
	const CGFloat deltaY = [theEvent deltaY];

	// Convert to integers (engine expects int)
	// Rounding provides better precision than truncation
	const int x = static_cast<int>(std::round(deltaX));
	const int y = static_cast<int>(std::round(deltaY));

	PostMouseMove(x, y);
}


void ProcessKeyboardEvent(NSEvent* theEvent)
{
	const unsigned short keyCode = [theEvent keyCode];
	if (keyCode >= KEY_COUNT)
	{
		assert(!"Unknown keycode");
		return;
	}

	const bool isARepeat = [theEvent isARepeat];

	if (k_allowfullscreentoggle
		&& (kVK_ANSI_F == keyCode)
		&& (NSEventModifierFlagCommand & [theEvent modifierFlags])
		&& (NSEventTypeKeyDown == [theEvent type])
		&& !isARepeat)
	{
		ToggleFullscreen = !ToggleFullscreen;
		return;
	}

	if (GUICapture)
	{
		ProcessKeyboardEventInMenu(theEvent);
	}
	else if (!isARepeat)
	{
		event_t event = {};

		event.type  = NSEventTypeKeyDown == [theEvent type] ? EV_KeyDown : EV_KeyUp;
		event.data1 = KEYCODE_TO_DIK[ keyCode ];

		if (0 != event.data1)
		{
			event.data2 = KEYCODE_TO_ASCII[ keyCode ];

			D_PostEvent(&event);
		}
	}
}

void ProcessKeyboardFlagsEvent(NSEvent* theEvent)
{
	if (GUICapture)
	{
		// Ignore events from modifier keys in menu/console/chat
		return;
	}

	static const uint32_t FLAGS_MASK =
		NSEventModifierFlagDeviceIndependentFlagsMask & ~NSEventModifierFlagNumericPad;

	const  uint32_t      modifiers = [theEvent modifierFlags] & FLAGS_MASK;
	static uint32_t   oldModifiers = 0;
	const  uint32_t deltaModifiers = modifiers ^ oldModifiers;

	if (0 == deltaModifiers)
	{
		return;
	}

	event_t event = {};
	event.type  = modifiers > oldModifiers ? EV_KeyDown : EV_KeyUp;
	event.data1 = ModifierToDIK(deltaModifiers);

	oldModifiers = modifiers;

	if (DIK_CAPITAL == event.data1)
	{
		// Caps Lock is a modifier key which generates one event per state change
		// but not per actual key press or release. So treat any event as key down
		event.type = EV_KeyDown;
	}

	D_PostEvent(&event);
}


// Process all mouse movement events (menu and game)
//
// Note: When in raw input mode (cursor hidden via CGAssociateMouseAndMouseCursorPosition),
// the deltaX/deltaY values are already unaccelerated and come directly from the mouse hardware.
// This is superior to Windows DirectInput and equivalent to Windows raw input.
void ProcessMouseMoveEvent(NSEvent* theEvent)
{
	if (!use_mouse)
	{
		return;
	}

	// Skip synthetic events from programmatic cursor movement
	if (s_skipMouseMoves > 0)
	{
		--s_skipMouseMoves;
		return;
	}

	if (GUICapture)
	{
		ProcessMouseMoveInMenu(theEvent);
	}
	else
	{
		ProcessMouseMoveInGame(theEvent);
	}
}

void ProcessMouseButtonEvent(NSEvent* theEvent)
{
	if (!use_mouse)
	{
		return;
	}

	event_t event = {};

	const NSEventType cocoaEventType = [theEvent type];

	if (GUICapture)
	{
		event.type  = EV_GUI_Event;
		event.data3 = ModifierFlagsToGUIKeyModifiers(theEvent);

		switch (cocoaEventType)
		{
			case NSEventTypeLeftMouseDown:  event.subtype = EV_GUI_LButtonDown; break;
			case NSEventTypeRightMouseDown: event.subtype = EV_GUI_RButtonDown; break;
			case NSEventTypeOtherMouseDown: event.subtype = EV_GUI_MButtonDown; break;
			case NSEventTypeLeftMouseUp:    event.subtype = EV_GUI_LButtonUp;   break;
			case NSEventTypeRightMouseUp:   event.subtype = EV_GUI_RButtonUp;   break;
			case NSEventTypeOtherMouseUp:   event.subtype = EV_GUI_MButtonUp;   break;
			default: break;
		}

		NSEventToGameMousePosition(theEvent, &event);

		D_PostEvent(&event);
	}
	else
	{
		switch (cocoaEventType)
		{
			case NSEventTypeLeftMouseDown:
			case NSEventTypeRightMouseDown:
			case NSEventTypeOtherMouseDown:
				event.type = EV_KeyDown;
				break;

			case NSEventTypeLeftMouseUp:
			case NSEventTypeRightMouseUp:
			case NSEventTypeOtherMouseUp:
				event.type = EV_KeyUp;
				break;

			default:
				break;
		}

		event.data1 = min(KEY_MOUSE1 + [theEvent buttonNumber], NSInteger(KEY_MOUSE8));

		D_PostEvent(&event);
	}
}

void ProcessMouseWheelEvent(NSEvent* theEvent)
{
	if (!use_mouse)
	{
		return;
	}

	const int16_t modifiers = ModifierFlagsToGUIKeyModifiers(theEvent);
	const CGFloat delta   = (modifiers & GKM_SHIFT)
		? [theEvent deltaX]
		: [theEvent deltaY];
	const bool isZeroDelta = fabs(delta) < 1.0E-5;

	if (isZeroDelta && GUICapture)
	{
		return;
	}

	event_t event = {};

	if (GUICapture)
	{
		event.type    = EV_GUI_Event;
		event.subtype = delta > 0.0f ? EV_GUI_WheelUp : EV_GUI_WheelDown;
		event.data3   = modifiers;
	}
	else
	{
		event.type  = isZeroDelta  ? EV_KeyUp     : EV_KeyDown;
		event.data1 = delta > 0.0f ? KEY_MWHEELUP : KEY_MWHEELDOWN;
	}

	D_PostEvent(&event);
}

} // unnamed namespace


void I_ProcessEvent(NSEvent* event)
{
	const NSEventType eventType = [event type];

	switch (eventType)
	{
		case NSEventTypeMouseMoved:
			ProcessMouseMoveEvent(event);
			break;

		case NSEventTypeLeftMouseDown:
		case NSEventTypeLeftMouseUp:
		case NSEventTypeRightMouseDown:
		case NSEventTypeRightMouseUp:
		case NSEventTypeOtherMouseDown:
		case NSEventTypeOtherMouseUp:
			ProcessMouseButtonEvent(event);
			break;

		case NSEventTypeLeftMouseDragged:
		case NSEventTypeRightMouseDragged:
		case NSEventTypeOtherMouseDragged:
			ProcessMouseButtonEvent(event);
			ProcessMouseMoveEvent(event);
			break;

		case NSEventTypeScrollWheel:
			ProcessMouseWheelEvent(event);
			break;

		case NSEventTypeKeyDown:
		case NSEventTypeKeyUp:
			ProcessKeyboardEvent(event);
			break;

		case NSEventTypeFlagsChanged:
			ProcessKeyboardFlagsEvent(event);
			break;

		default:
			break;
	}
}
