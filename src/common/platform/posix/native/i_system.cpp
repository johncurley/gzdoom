#include "i_system.h"
#include <cstdio>
#include <unistd.h>
#include <sys/utsname.h>
#include "m_argv.h"
#include "i_video.h"
#include "m_joy.h"
#include "d_eventbase.h"
#include <zwidget/window/window.h>

FArgs *Args;
IVideo *Video;

// External pointer from nativevideo.cpp
namespace {
    extern DisplayWindow* ActiveWindow;
}
// Wait, I can't use namespace extern like that easily if it's a class member.
// I'll just declare it.
#include "native_display.h" // Maybe I should put it here or just declare it

class DisplayWindow;
namespace NativePlatform {
    extern DisplayWindow* ActiveWindow;
}

// Actually, I'll just use a simple extern.
extern DisplayWindow* GetNativeActiveWindow(); // I'll add this helper to nativevideo.cpp
// Or just:
#include <zwidget/window/window.h>
extern DisplayWindow* GetActiveZWidgetWindow();

bool PerfAvailable;
double PerfToSec, PerfToMillisec;
FString queryiwad_key = "shift";

void I_ShowFatalError(const char *message) {
    fprintf(stderr, "Fatal Error: %s\n", message);
}

void I_PutInClipboard(const char *str) {
    auto window = GetActiveZWidgetWindow();
    if (window) window->SetClipboardText(str);
}

FString I_GetFromClipboard(bool use_primary_selection) {
    auto window = GetActiveZWidgetWindow();
    if (window) return window->GetClipboardText().c_str();
    return "";
}

FString I_GetCWD() {
    char temp[PATH_MAX];
    if (getcwd(temp, PATH_MAX)) return temp;
    return ".";
}

bool I_ChDir(const char* path) {
    return chdir(path) == 0;
}

unsigned int I_MakeRNGSeed() {
    return (unsigned int)time(NULL);
}

void I_OpenShellFolder(const char* infolder) {}
bool I_IsDarkMode() {
    // Check for GNOME
    char buffer[128];
    FILE* pipe = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
    if (pipe) {
        if (fgets(buffer, sizeof(buffer), pipe)) {
            if (strstr(buffer, "dark")) {
                pclose(pipe);
                return true;
            }
        }
        pclose(pipe);
    }

    // Check for KDE
    const char* home = getenv("HOME");
    if (home) {
        char kdepath[PATH_MAX];
        snprintf(kdepath, sizeof(kdepath), "%s/.config/kdeglobals", home);
        FILE* f = fopen(kdepath, "r");
        if (f) {
            while (fgets(buffer, sizeof(buffer), f)) {
                if (strstr(buffer, "ColorScheme=Dark") || strstr(buffer, "colorScheme=BreezeDark")) {
                    fclose(f);
                    return true;
                }
            }
            fclose(f);
        }

        // Check for GTK 3.0 (XFCE, MATE, etc.)
        char gtkpath[PATH_MAX];
        snprintf(gtkpath, sizeof(gtkpath), "%s/.config/gtk-3.0/settings.ini", home);
        f = fopen(gtkpath, "r");
        if (f) {
            while (fgets(buffer, sizeof(buffer), f)) {
                if (strstr(buffer, "gtk-application-prefer-dark-theme=true") || strstr(buffer, "gtk-application-prefer-dark-theme=1")) {
                    fclose(f);
                    return true;
                }
            }
            fclose(f);
        }
    }

    // Fallback to environment
    const char* colorterm = getenv("COLORTERM");
    if (colorterm && strstr(colorterm, "dark")) return true;

    return false;
}
void I_InitGraphics() {}
void I_ShutdownGraphics() {}
bool I_SetCursor(FGameTexture*) { return false; }

void I_SetWindowTitle(char const* title) {
    auto window = GetActiveZWidgetWindow();
    if (window) window->SetWindowTitle(title);
}

bool HoldingQueryKey(const char* key) { return false; }
bool I_PickIWad(bool showwin, FStartupSelectionInfo& info) { return true; }
void I_SetIWADInfo() {}

void I_DetectOS() {
    struct utsname name;
    if (uname(&name) == 0) {
        printf("OS: %s %s %s\n", name.sysname, name.release, name.machine);
    }
}

void CalculateCPUSpeed() {}
void I_PrintStr(const char *cp) { printf("%s", cp); }
