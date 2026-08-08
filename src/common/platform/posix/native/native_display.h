#ifndef __NATIVE_DISPLAY_H__
#define __NATIVE_DISPLAY_H__

class DisplayWindow;

// Helper to get the active ZWidget window from system services
DisplayWindow* GetActiveZWidgetWindow();

class NativeHandle {
public:
    virtual ~NativeHandle() = default;
};

#endif
