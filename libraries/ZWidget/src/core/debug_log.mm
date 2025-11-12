#import <Foundation/Foundation.h>
#include <cstdarg>

extern "C" void DebugLog(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);

    va_end(args);

    NSLog(@"%s", buffer);
}
