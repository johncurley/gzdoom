#include "i_system.h"
#include <cstdio>
#include <ctime>
#include <limits.h>
#include <strings.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <zwidget/window/window.h>

#include "launcherwindow.h"
#include "m_argv.h"
#include "i_video.h"
#include "m_joy.h"

extern DisplayWindow *GetActiveZWidgetWindow();

FArgs *Args;
IVideo *Video;

bool PerfAvailable;
double PerfToSec, PerfToMillisec;
FString queryiwad_key = "shift";

void I_ShowFatalError(const char *message) {
	fprintf(stderr, "Fatal Error: %s\n", message);
}

void I_PutInClipboard(const char *str) {
	auto window = GetActiveZWidgetWindow();
	if (window)
		window->SetClipboardText(str);
}

FString I_GetFromClipboard(bool use_primary_selection) {
	auto window = GetActiveZWidgetWindow();
	if (window)
		return window->GetClipboardText().c_str();
	return "";
}

FString I_GetCWD() {
	char temp[PATH_MAX];
	if (getcwd(temp, PATH_MAX))
		return temp;
	return ".";
}

bool I_ChDir(const char *path) {
	return chdir(path) == 0;
}

unsigned int I_MakeRNGSeed() {
	return (unsigned int)time(NULL);
}

void I_OpenShellFolder(const char *infolder) {}

bool I_IsDarkMode() {
	char buffer[128];
	FILE *pipe = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
	if (pipe) {
		if (fgets(buffer, sizeof(buffer), pipe)) {
			if (strstr(buffer, "dark")) {
				pclose(pipe);
				return true;
			}
		}
		pclose(pipe);
	}

	const char *home = getenv("HOME");
	if (home) {
		char kdepath[PATH_MAX];
		snprintf(kdepath, sizeof(kdepath), "%s/.config/kdeglobals", home);
		FILE *f = fopen(kdepath, "r");
		if (f) {
			while (fgets(buffer, sizeof(buffer), f)) {
				if (strstr(buffer, "ColorScheme=Dark") || strstr(buffer, "colorScheme=BreezeDark")) {
					fclose(f);
					return true;
				}
			}
			fclose(f);
		}

		char gtkpath[PATH_MAX];
		snprintf(gtkpath, sizeof(gtkpath), "%s/.config/gtk-3.0/settings.ini", home);
		f = fopen(gtkpath, "r");
		if (f) {
			while (fgets(buffer, sizeof(buffer), f)) {
				if (strstr(buffer, "gtk-application-prefer-dark-theme=true")
					|| strstr(buffer, "gtk-application-prefer-dark-theme=1")) {
					fclose(f);
					return true;
				}
			}
			fclose(f);
		}
	}

	const char *colorterm = getenv("COLORTERM");
	if (colorterm && strstr(colorterm, "dark"))
		return true;

	return false;
}

extern void I_InitNativeWindow();

void I_InitGraphics() {
    I_InitNativeWindow();
}
void I_ShutdownGraphics() {}

void I_SetWindowTitle(char const *title) {
	auto window = GetActiveZWidgetWindow();
	if (window)
		window->SetWindowTitle(title);
}

bool HoldingQueryKey(const char *key) {
	auto *w = GetActiveZWidgetWindow();
	if (!w || !key)
		return false;
	if (!strcasecmp(key, "shift"))
		return w->GetKeyState(InputKey::LShift) || w->GetKeyState(InputKey::RShift);
	if (!strcasecmp(key, "ctrl"))
		return w->GetKeyState(InputKey::LControl) || w->GetKeyState(InputKey::RControl);
	return false;
}

bool I_PickIWad(bool showwin, FStartupSelectionInfo &info) {
	if (!showwin)
		return true;
	// Same ZWidget launcher as the SDL Linux path; replace LauncherWindow / ExecModal for a custom IWAD UI.
	return LauncherWindow::ExecModal(info);
}

void I_SetIWADInfo() {}

void I_DetectOS() {
	struct utsname name;
	if (uname(&name) == 0) {
		printf("OS: %s %s %s\n", name.sysname, name.release, name.machine);
	}
}

void CalculateCPUSpeed() {}

void I_PrintStr(const char *cp) {
	printf("%s", cp);
}
