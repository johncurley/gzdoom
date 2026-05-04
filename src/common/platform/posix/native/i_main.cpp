#include <csignal>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <limits.h>
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#ifdef __HAIKU__
#include <OS.h>
#include <image.h>
#endif
#include "i_system.h"
#include "m_argv.h"
#include "i_video.h"

extern IVideo *Video;
extern IVideo *gl_CreateVideo();
extern FString progdir;
int GameMain();

int main(int argc, char **argv)
{
    Args = new FArgs(argc, argv);
    Video = gl_CreateVideo();

    char program[PATH_MAX];
    bool found = false;

#if defined(__FreeBSD__) || defined(__DragonFly__)
    int mib[4];
    size_t len;
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PATHNAME;
    mib[3] = -1;
    len = sizeof(program);
    if (sysctl(mib, 4, program, &len, NULL, 0) != -1) {
        found = true;
    }
#elif defined(__NetBSD__)
    if (readlink("/proc/curproc/exe", program, sizeof(program)) != -1) {
        found = true;
    }
#elif defined(__OpenBSD__)
    // OpenBSD doesn't have a reliable KERN_PROC_PATHNAME in sysctl for all versions.
    // realpath on argv[0] is the common fallback if not using KERN_PROC_ARGS.
    if (realpath(argv[0], program) != NULL) {
        found = true;
    }
#elif defined(__HAIKU__)
    image_info info;
    int32 cookie = 0;
    while (get_next_image_info(0, &cookie, &info) == B_OK) {
        if (info.type == B_APP_IMAGE) {
            strncpy(program, info.name, sizeof(program));
            found = true;
            break;
        }
    }
#else // Linux
    if (readlink("/proc/self/exe", program, sizeof(program)) != -1) {
        found = true;
    }
#endif

    if (found) {
        char* slash = strrchr(program, '/');
        if (slash != NULL) {
            *(slash + 1) = '\0';
            progdir = program;
        } else {
            progdir = "./";
        }
    } else {
        // Fallback to realpath on argv[0] if everything else fails
        if (realpath(argv[0], program) != NULL) {
            char* slash = strrchr(program, '/');
            if (slash != NULL) {
                *(slash + 1) = '\0';
                progdir = program;
            } else {
                progdir = "./";
            }
        } else {
            progdir = "./";
        }
    }

    printf("Native Linux backend initialized.\n");
    
    extern void I_StartupJoysticks();
    I_StartupJoysticks();
    
    return GameMain();
}
