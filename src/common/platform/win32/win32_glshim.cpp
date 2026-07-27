/*
** win32_glshim.cpp
** Dynamically-loaded wgl entry points for the Windows OpenGL backend.
**
** These four wrappers used to live in gl_load.c, which was removed when the
** GL loader was replaced with glad. glad supplies the GL entry points but not
** the platform's wgl context-management calls, and win32glvideo.cpp and
** gl_sysfb.cpp still depend on them, so they are restored here unchanged in
** behaviour: OpenGL32.dll is resolved on first use rather than linked against,
** which keeps the executable loadable on a machine with no usable GL driver
** long enough to report the problem.
*/

#include <windows.h>
#include <stdlib.h>

#ifdef _MSC_VER
#pragma warning(disable: 4054)
#pragma warning(disable: 4996)
#endif

static HMODULE opengl32dll;
static HGLRC(WINAPI* createcontext)(HDC);
static BOOL(WINAPI* deletecontext)(HGLRC);
static BOOL(WINAPI* makecurrent)(HDC, HGLRC);
static PROC(WINAPI* getprocaddress)(LPCSTR name);

static void CheckOpenGL(void)
{
	if (opengl32dll == 0)
	{
		opengl32dll = LoadLibraryA("OpenGL32.DLL");
		if (opengl32dll != 0)
		{
			createcontext = (HGLRC(WINAPI*)(HDC)) GetProcAddress(opengl32dll, "wglCreateContext");
			deletecontext = (BOOL(WINAPI*)(HGLRC)) GetProcAddress(opengl32dll, "wglDeleteContext");
			makecurrent = (BOOL(WINAPI*)(HDC, HGLRC)) GetProcAddress(opengl32dll, "wglMakeCurrent");
			getprocaddress = (PROC(WINAPI*)(LPCSTR)) GetProcAddress(opengl32dll, "wglGetProcAddress");
		}
		else
		{
			// Should this ever happen we have no choice but to hard abort, there is no good way to recover.
			MessageBoxA(0, "OpenGL32.dll not found", "Fatal error", MB_OK | MB_ICONERROR | MB_TASKMODAL);
			exit(3);
		}
	}
}

extern "C" HGLRC zd_wglCreateContext(HDC dc)
{
	CheckOpenGL();
	return createcontext(dc);
}

extern "C" BOOL zd_wglDeleteContext(HGLRC context)
{
	CheckOpenGL();
	return deletecontext(context);
}

extern "C" BOOL zd_wglMakeCurrent(HDC dc, HGLRC context)
{
	CheckOpenGL();
	return makecurrent(dc, context);
}

extern "C" PROC zd_wglGetProcAddress(LPCSTR name)
{
	CheckOpenGL();
	return getprocaddress(name);
}
