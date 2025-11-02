/*
 ** cocoa_native_handle.h
 **
 **---------------------------------------------------------------------------
 ** Copyright 2025 GZDoom Maintainers and Contributors
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
 ** Native Cocoa handle structure for Metal renderer integration.
 ** Provides access to underlying macOS objects for advanced rendering.
 **
 */

#pragma once

#ifdef __APPLE__

#ifdef __OBJC__
@class NSWindow;
@class NSView;
@class CAMetalLayer;
#else
typedef struct objc_object NSWindow;
typedef struct objc_object NSView;
typedef struct objc_object CAMetalLayer;
#endif

// Native handle structure for Cocoa window/Metal layer access
// Used for Metal renderer integration and advanced platform features
struct CocoaNativeHandle
{
	NSWindow*      nsWindow;     // Main window object
	NSView*        nsView;       // Content view for rendering
	CAMetalLayer*  metalLayer;   // Metal layer for Metal 2+ rendering (macOS 10.13+)
};

#endif // __APPLE__
