#include "i_input.h"
#include "i_interface.h"
#include <zwidget/window/window.h>
#include <cstdio>

// Prevent re-entry of event processing from ZWidget callbacks
// Events should be processed linearly, not recursively
static bool event_processing_in_progress = false;

void I_GetEvent() {
    // If we're already processing events, don't re-enter
    // This prevents infinite loops when ZWidget callbacks trigger more events
    if (event_processing_in_progress) {
        return;
    }
    
    event_processing_in_progress = true;
    
    try {
        DisplayWindow::ProcessEvents();
    } catch (const std::exception& e) {
        fprintf(stderr, "ERROR in DisplayWindow::ProcessEvents(): %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "ERROR: Unknown exception in DisplayWindow::ProcessEvents()\n");
    }
    
    event_processing_in_progress = false;
}

void I_StartTic() {
    I_GetEvent();
}

void I_StartFrame() {}
void I_SetMouseCapture() {}
void I_ReleaseMouseCapture() {}
void I_SetNativeMouse(bool wantNative) {}
