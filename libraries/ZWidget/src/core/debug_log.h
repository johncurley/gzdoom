#pragma once

#ifdef __APPLE__
extern "C" void DebugLog(const char* format, ...);
#else
#include <cstdio>
#include <cstdarg>
inline void DebugLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    fflush(stdout);
}
#endif
