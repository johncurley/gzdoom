/*
 ** i_system.mm
 **
 **---------------------------------------------------------------------------
 ** Copyright 2012-2018 Alexey Lysiuk
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

// Carbon's MacTypes.h (via Cocoa) typedefs Point, Size and Rect; ZWidget
// declares structs with those names in zwidget/core/rect.h, so the two cannot
// share a translation unit -- whichever lands second loses, and no include
// ordering fixes it. This is the macOS counterpart of the X11 GC/None
// pollution documented in CLAUDE.md.
//
// Same treatment ZWidget applies internally in src/window/cocoa/AppKitWrapper.h:
// rename the Carbon types across the Cocoa include, then drop the macros so the
// ZWidget names are free below. That header is private to ZWidget (only
// libraries/ZWidget/include is on our include path), hence the local copy.
// Nothing in this file uses the Carbon Point/Size/Rect.
#define Point MacPoint
#define Rect  MacRect
#define Size  MacSize
#include <Cocoa/Cocoa.h>
#undef Point
#undef Rect
#undef Size

#include "launcherwindow.h"
#include "i_common.h"
#include "c_cvars.h"
#include "i_interface.h"

#include <fnmatch.h>
#include <sys/sysctl.h>

#include "i_system.h"
#include "st_console.h"
#include "v_text.h"

CVAR(String, queryiwad_key, "", CVAR_GLOBALCONFIG | CVAR_ARCHIVE); // Currently unimplemented.

EXTERN_CVAR(Bool, longsavemessages)
double PerfToSec, PerfToMillisec;

void CalculateCPUSpeed()
{
	long long frequency;
	size_t size = sizeof frequency;

	if (0 == sysctlbyname("machdep.tsc.frequency", &frequency, &size, nullptr, 0) && 0 != frequency)
	{
		PerfToSec = 1.0 / frequency;
		PerfToMillisec = 1000.0 / frequency;

		if (!batchrun)
		{
			Printf("CPU speed: %.0f MHz\n", 0.001 / PerfToMillisec);
		}
	}
}


void I_SetIWADInfo()
{
	FConsoleWindow::GetInstance().SetTitleText();
}


void I_PrintStr(const char* const message)
{
	FConsoleWindow::GetInstance().AddText(message);

	// Strip out any color escape sequences before writing to output
	char* const copy = new char[strlen(message) + 1];
	const char* srcp = message;
	char* dstp = copy;

	while ('\0' != *srcp)
	{
		if (TEXTCOLOR_ESCAPE == *srcp)
		{
			if ('\0' != srcp[1])
			{
				srcp += 2;
			}
			else
			{
				break;
			}
		}
		else if (0x1d == *srcp || 0x1f == *srcp) // Opening and closing bar character
		{
			*dstp++ = '-';
			++srcp;
		}
		else if (0x1e == *srcp) // Middle bar character
		{
			*dstp++ = '=';
			++srcp;
		}
		else
		{
			*dstp++ = *srcp++;
		}
	}

	*dstp = '\0';

	fputs(copy, stdout);
	delete[] copy;
	fflush(stdout);
}


void Mac_I_FatalError(const char* const message);

void I_ShowFatalError(const char *message)
{
	Mac_I_FatalError(message);
}

bool HoldingQueryKey(const char* key)
{
	// TODO: Implement
	return false;
}

bool I_PickIWad(bool showwin, FStartupSelectionInfo& info)
{
	if (!showwin)
	{
		return true;
	}

	I_SetMainWindowVisible(false);

	// The same ZWidget launcher every other platform uses -- win32
	// (i_system.cpp:376), native POSIX (native/i_system.cpp:137) and SDL POSIX
	// (sdl/i_system.cpp:360). macOS was the last holdout on its own Cocoa
	// picker, so the launcher's settings, autoload and network pages did not
	// exist here.
	//
	// This needs CocoaDisplayBackend::RunLoop's non-nesting path: GZDoom owns
	// [NSApp run] and defers startup onto a timer, so ExecModal runs inside an
	// already-running NSApp. Without that fix the launcher draws and ignores
	// every event.
	//
	// I_PickIWad_Cocoa (iwadpicker_cocoa.mm) is deliberately left in the tree
	// rather than deleted: it still carries the relaunch fixes (argument
	// concatenation, dropped arguments, the crash on re-entry) and is the
	// fallback if the ZWidget path regresses on macOS.
	// GZDoom's own Cocoa pump must stand down while the launcher is modal --
	// see I_SuspendCocoaEventPump. Without this the launcher window appears but
	// receives no input at all, because processEvents: drains the queue first.
	extern void I_SuspendCocoaEventPump();
	extern void I_ResumeCocoaEventPump();

	I_SuspendCocoaEventPump();
	const bool result = LauncherWindow::ExecModal(info);
	I_ResumeCocoaEventPump();

	I_SetMainWindowVisible(true);

	return result;
}


void I_PutInClipboard(const char* const string)
{
	NSPasteboard* pasteBoard = [NSPasteboard generalPasteboard];
	NSPasteboardType stringType = NSPasteboardTypeString;
	NSArray* types = @[stringType];  // Modern array literal
	NSString* content = [NSString stringWithUTF8String:string];

	[pasteBoard declareTypes:types owner:nil];
	[pasteBoard setString:content forType:stringType];
}

FString I_GetFromClipboard(bool returnNothing)
{
	if (returnNothing)
	{
		return FString();
	}

	NSPasteboard* const pasteBoard = [NSPasteboard generalPasteboard];
	NSString* const value = [pasteBoard stringForType:NSPasteboardTypeString];  // Modern API

	return FString([value UTF8String]);
}


unsigned int I_MakeRNGSeed()
{
	return static_cast<unsigned int>(arc4random());
}

FString I_GetCWD()
{
	NSString *currentpath = [[NSFileManager defaultManager] currentDirectoryPath];
	return currentpath.UTF8String;
}

bool I_ChDir(const char* path)
{
	return [[NSFileManager defaultManager] changeCurrentDirectoryPath:[NSString stringWithUTF8String:path]];
}

void I_OpenShellFolder(const char* folder)
{
	NSFileManager *filemgr = [NSFileManager defaultManager];
	NSString *currentpath = [filemgr currentDirectoryPath];

	[filemgr changeCurrentDirectoryPath:[NSString stringWithUTF8String:folder]];
	if (longsavemessages)
		Printf("Opening folder: %s\n", folder);
	std::system("open .");
	[filemgr changeCurrentDirectoryPath:currentpath];
}

bool I_IsDarkMode()
{
	#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 101400
		// currently the new startup popup on Mac is not active, so this won't get used
	    if (@available(macOS 10.14, *))
        {
            NSAppearance *appearance = [NSApp effectiveAppearance];
            NSAppearanceName name = [appearance bestMatchFromAppearancesWithNames:@ [
                (NSAppearanceName)@"NSAppearanceNameAqua",
                (NSAppearanceName)@"NSAppearanceNameDarkAqua"
            ]];
            return [name isEqualToString:(NSAppearanceName)@"NSAppearanceNameDarkAqua"];
        }
        else
        {
            return NO;
        }
	#else
		// Fallback for older macOS versions
		return NO;
	#endif
}