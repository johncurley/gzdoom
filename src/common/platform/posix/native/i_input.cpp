#include "i_input.h"
#include "i_interface.h"
#include <zwidget/window/window.h>
#include <cstdio>

// Prevent infinite event processing loops
static int event_recursion_depth = 0;
static const int MAX_EVENT_RECURSION = 2;
static bool event_processing_error_reported = false;

void I_GetEvent() {
    if (event_recursion_depth >= MAX_EVENT_RECURSION) {
        if (!event_processing_error_reported) {
            fprintf(stderr, "WARNING: Event recursion depth exceeded in I_GetEvent()\n");
            event_processing_error_reported = true;
        }
        return;  // Prevent recursive event processing
    }
    
    ++event_recursion_depth;
    
    try {
        DisplayWindow::ProcessEvents();
    } catch (const std::exception& e) {
        fprintf(stderr, "ERROR in DisplayWindow::ProcessEvents(): %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "ERROR: Unknown exception in DisplayWindow::ProcessEvents()\n");
    }
    
    --event_recursion_depth;
}

void I_StartTic() {
    I_GetEvent();
}

void I_StartFrame() {}
void I_SetMouseCapture() {}
void I_ReleaseMouseCapture() {}
void I_SetNativeMouse(bool wantNative) {}
