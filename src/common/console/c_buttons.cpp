/*
** c_dispatch.cpp
** Functions for executing console commands and aliases
**
**---------------------------------------------------------------------------
** Copyright 1998-2007 Randy Heit
** Copyright 2019 Christoph Oelckers
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

#include "c_buttons.h"

#include "c_dispatch.h"
#include "printf.h"
#include "cmdlib.h"
#include "c_console.h"
#include "m_joy.h"
#include "c_bind.h"

ButtonMap buttonMap;


//=============================================================================
//
//
//
//=============================================================================

void ButtonMap::SetButtons(const char** names, int count)
{
	Buttons.Resize(count);
	NumToName.Resize(count);
	NameToNum.Clear();
	for(int i = 0; i < count; i++)
	{
		Buttons[i] = {};
		NameToNum.Insert(names[i], i);
		NumToName[i] = names[i];
	}
}

//=============================================================================
//
//
//
//=============================================================================

int ButtonMap::ListActionCommands (const char *pattern)
{
	char matcher[32];
	int count = 0;

	for (auto& btn : NumToName)
	{
		if (pattern == NULL || CheckWildcards (pattern,
			(mysnprintf (matcher, countof(matcher), "+%s", btn.GetChars()), matcher)))
		{
			Printf ("+%s\n", btn.GetChars());
			count++;
		}
		if (pattern == NULL || CheckWildcards (pattern,
			(mysnprintf (matcher, countof(matcher), "-%s", btn.GetChars()), matcher)))
		{
			Printf ("-%s\n", btn.GetChars());
			count++;
		}
	}
	return count;
}


//=============================================================================
//
//
//
//=============================================================================

int ButtonMap::FindButtonIndex (const char *key, int funclen) const
{
    if (!key) return -1;

	FName name = funclen == -1? FName(key, true) : FName(key, funclen, true);
	if (name == NAME_None) return -1;

	auto res = NameToNum.CheckKey(name);
	if (!res) return -1;

	return *res;
}


//=============================================================================
//
//
//
//=============================================================================

void ButtonMap::ResetButtonTriggers ()
{
	for (auto &button : Buttons)
	{
		button.ResetTriggers ();
	}
}

//=============================================================================
//
//
//
//=============================================================================

void ButtonMap::ResetButtonStates ()
{
	for (auto &btn : Buttons)
	{
		if (!btn.bReleaseLock) 
		{
			btn.ReleaseKey (0);
		}
		btn.ResetTriggers ();
	}
}

//=============================================================================
//
//
//
//=============================================================================

void ButtonMap::GetAxes ()
{
	float joyaxes[NUM_AXIS_CODES];
	I_GetAxes(joyaxes);

	for (int i = 0; i < (int)Buttons.Size(); i++)
	{
		FButtonStatus &btn = Buttons[i];
		FString &btn_name = NumToName[i];

		btn.AddAxes(btn_name, joyaxes);
	}
}

//=============================================================================
//
//
//
//=============================================================================

bool FButtonStatus::PressKey (int keynum)
{
	int i, open;

	keynum &= KEY_DBLCLICKED-1;

	if (keynum == 0)
	{ // Issued from console instead of a key, so force on
		Keys[0] = 0xffff;
		for (i = MAX_KEYS-1; i > 0; --i)
		{
			Keys[i] = 0;
		}
	}
	else
	{
		for (i = MAX_KEYS-1, open = -1; i >= 0; --i)
		{
			if (Keys[i] == 0)
			{
				open = i;
			}
			else if (Keys[i] == keynum)
			{ // Key is already down; do nothing
				return false;
			}
		}
		if (open < 0)
		{ // No free key slots, so do nothing
			Printf ("More than %u keys pressed for a single action!\n", MAX_KEYS);
			return false;
		}
		Keys[open] = keynum;
	}
	uint8_t wasdown = bDown;
	bDown = bWentDown = true;

	// Returns true if this key caused the button to go down.
	return !wasdown;
}

//=============================================================================
//
//
//
//=============================================================================

bool FButtonStatus::ReleaseKey (int keynum)
{
	int i, numdown, match;
	uint8_t wasdown = bDown;

	keynum &= KEY_DBLCLICKED-1;

	if (keynum == 0)
	{ // Issued from console instead of a key, so force off
		for (i = MAX_KEYS-1; i >= 0; --i)
		{
			Keys[i] = 0;
		}
		bWentUp = true;
		bDown = false;
	}
	else
	{
		for (i = MAX_KEYS-1, numdown = 0, match = -1; i >= 0; --i)
		{
			if (Keys[i] != 0)
			{
				++numdown;
				if (Keys[i] == keynum)
				{
					match = i;
				}
			}
		}
		if (match < 0)
		{ // Key was not down; do nothing
			return false;
		}
		Keys[match] = 0;
		bWentUp = true;
		if (--numdown == 0)
		{
			bDown = false;
		}
	}

	// Returns true if releasing this key caused the button to go up.
	return wasdown && !bDown;
}

//=============================================================================
//
//
//
//=============================================================================

void FButtonStatus::AddAxes (FString &btn_name, float joyaxes[NUM_AXIS_CODES])
{
	bIsAxis = false;
	Axis = 0.0f;

	for (int i = 0; i < NUM_AXIS_CODES; i++)
	{
		const float axis_value = joyaxes[i];
		if (axis_value > 0.0f)
		{
			// Only the binding of this axis can affect its contribution.
			// Keep live bindings and accumulation order; avoid scanning all
			// keys and allocating two command-key lists for every button.
			const char *binding = Bindings.GetBind(KeyAxisMapping[i]);
			if (!binding || (binding[0] != '+' && binding[0] != '-') ||
				stricmp(binding + 1, btn_name.GetChars()) != 0)
				continue;

			if (binding[0] == '+')
				Axis += axis_value;
			else
				Axis -= axis_value;
			bIsAxis = true;
		}
	}

	Axis = clamp<float>(Axis, 0.0f, 1.0f);
}

// Independent legacy membership oracle for the explicit native diagnostic.
static void ReferenceButtonAxes(FButtonStatus &button, FString &name,
	float axes[NUM_AXIS_CODES])
{
	button.bIsAxis = false;
	button.Axis = 0.0f;
	const auto positive = Bindings.GetKeysForCommand(FStringf("+%s", name.GetChars()).GetChars());
	const auto negative = Bindings.GetKeysForCommand(FStringf("-%s", name.GetChars()).GetChars());
	for (int i = 0; i < NUM_AXIS_CODES; ++i)
	{
		if (!(axes[i] > 0.0f)) continue;
		if (positive.Contains(KeyAxisMapping[i]))
		{
			button.Axis += axes[i];
			button.bIsAxis = true;
		}
		if (negative.Contains(KeyAxisMapping[i]))
		{
			button.Axis -= axes[i];
			button.bIsAxis = true;
		}
	}
	button.Axis = clamp<float>(button.Axis, 0.0f, 1.0f);
}

CCMD(input_axes_selftest)
{
	// The diagnostic never changes the live digital button state. Restore
	// every binding on all exits, including exception unwinding.
	unsigned comparisons = 0, failures = 0, negative = 0;
	{
		struct RestoreBindings
		{
			FKeyBindings Saved = Bindings;
			~RestoreBindings() { Bindings = Saved; }
		} restore;
		const char *commands[] = {"", "+forward", "-forward", "+FoRwArD",
			"+attack", "-attack", "+forward; echo ignored", " +forward", "+forward "};
		const char *names[] = {"forward", "attack", "altattack", "unbound"};
		uint32_t seed = 0x19831111;
		for (int bank = 0; bank < 16; ++bank)
		{
			Bindings.UnbindAll();
			for (int key = 0; key < NUM_KEYS; ++key)
			{
				seed = seed * 1664525u + 1013904223u;
				Bindings.SetBind(key, commands[(seed >> 16) % countof(commands)]);
			}
			for (int pattern = 0; pattern < NUM_AXIS_CODES + 5; ++pattern)
			{
				float axes[NUM_AXIS_CODES] = {};
				if (pattern < NUM_AXIS_CODES)
					axes[pattern] = 1.0f;
				else if (pattern != NUM_AXIS_CODES)
					for (int i = 0; i < NUM_AXIS_CODES; ++i)
						axes[i] = pattern == NUM_AXIS_CODES + 1 ? -1.0f :
							pattern == NUM_AXIS_CODES + 2 ? 1.0f :
							pattern == NUM_AXIS_CODES + 3 ? 0.25f :
							(i & 1) ? 0.75f : -0.5f;
				for (auto name : names)
				{
					FString command(name);
					FButtonStatus original = {};
					original.Keys[0] = 42;
					original.bDown = original.bWentDown = original.bReleaseLock = true;
					original.bIsAxis = true;
					original.Axis = 0.75f;
					auto expected = original, actual = original;
					ReferenceButtonAxes(expected, command, axes);
					actual.AddAxes(command, axes);
					const bool same = actual.Axis == expected.Axis &&
						actual.bIsAxis == expected.bIsAxis &&
						actual.bDown == original.bDown &&
						actual.bWentDown == original.bWentDown &&
						actual.bWentUp == original.bWentUp &&
						actual.bReleaseLock == original.bReleaseLock &&
						memcmp(actual.Keys, original.Keys, sizeof(actual.Keys)) == 0;
					++comparisons;
					failures += !same;
					actual.Axis = expected.Axis == 0.5f ? 0.25f : 0.5f;
					negative += actual.Axis != expected.Axis;
				}
			}
		}
	}
	Printf("input-axes-selftest: %s comparisons=%u failures=%u negative=%u\n",
		comparisons && !failures && negative == comparisons ? "PASS" : "FAIL",
		comparisons, failures, negative);
}

//=============================================================================
//
//
//
//=============================================================================

void ButtonMap::AddButtonTabCommands()
{
	// Add all the action commands for tab completion
	for (auto& btn : NumToName)
	{
		char tname[16];
		strcpy (&tname[1], btn.GetChars());
		tname[0] = '+';
		C_AddTabCommand (tname);
		tname[0] = '-';
		C_AddTabCommand (tname);
	}
}
