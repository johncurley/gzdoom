# GZDoom Native POSIX Backend (Linux/BSD)

## Overview
This codebase uses a custom native POSIX backend to bypass SDL-based abstraction layers for windowing and input on Linux and BSD systems (FreeBSD, OpenBSD, etc.). This approach grants full control over the engine stack and allows for direct integration with system toolkits (X11/Wayland).

## Architectural Strategy
- **Backend:** Native implementation in `src/common/platform/posix/native/`.
- **UI Integration:** Fully integrated with `ZWidget` for windowing, input event routing, and system services (clipboard, etc.).
- **Concurrency:** Leverages `libdispatch` (GCD) for shared concurrency logic across macOS and POSIX systems.
- **Renderer Tiers:**
    - **High-Tier:** Metal/Compute shader pipeline (macOS) / Vulkan (Linux/BSD).
    - **Mid-Tier:** OpenGL 4.1.
    - **Legacy-Tier:** OpenGL 3.3.

## Build Configuration
The native backend is controlled via the `GZDOOM_NATIVE_LINUX` CMake option (renamed description to include BSD). 
- Enable with `-DGZDOOM_NATIVE_LINUX=ON`.

## Workflow
- **Development:** All Linux/BSD-specific platform work should be directed to the `native/` backend.
- **Debugging:** The native stack provides direct visibility into the windowing and input event pipeline through ZWidget, eliminating the "mystery box" effect of SDL.
