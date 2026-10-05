# Handoff — Linux evdev gamepad — 2026-10-05

## Scope and current state

The native Linux backend now reads gamepads through evdev without adding an
SDL dependency. The implementation is in
`src/common/platform/posix/native/i_joystick.cpp`; the native input loop in
`src/common/platform/posix/native/i_input.cpp` polls devices each tic and
advances the shared haptics manager.

Implemented:

- Maps standard Linux gamepad buttons to the engine's `KEY_PAD_*` controls,
  including face buttons, shoulders, digital triggers, guide/menu buttons,
  paddles, and D-pad buttons. Generic joystick buttons retain joystick-button
  key codes.
- Normalizes six standard axes (two sticks and two triggers), applies the
  engine's per-axis dead zones, curves, sensitivity, and digital thresholds,
  and exposes those axes to gameplay.
- Merges `ABS_HAT0X/Y` with button-based D-pad input so either evdev encoding
  works without duplicate presses or premature releases.
- Polls `/dev/input` inotify notifications for hot-plug and removal, releases
  held controls on removal/disable, and resynchronizes keys and axes after
  `SYN_DROPPED`.
- Supports Linux `FF_RUMBLE` when the event node is writable and advertises
  the effect. Low-frequency strength drives the strong motor; high-frequency
  strength drives the weak motor. Trigger-specific haptics remain unsupported.
- Loads/saves the engine's standard joystick configuration by evdev identity,
  preferring the controller's unique ID or physical path and falling back to
  its event-node path.
- The shared single-axis and thumbstick helpers now treat a dead zone of
  `1.0` as full suppression, avoiding division by zero at the accepted upper
  bound. If an evdev state query fails while recovering from `SYN_DROPPED`,
  the affected old button/axis state is cleared rather than left latched.

## Verification

- `cmake --build build -j$(nproc)` passed on 2026-10-05.
- `git diff --check` passed.
- No evdev runtime or haptics result is claimed. This account cannot open the
  current `/dev/input/event*` nodes (`root:input`, mode `0660`) or
  `/dev/uinput` (`root:root`, mode `0660`). No physical controller is
  available yet. A synthetic `/dev/uinput` run was attempted with the
  escalated command, but `sudo -n` requires a password, so no virtual device
  was created and no device permissions were changed.

## Next Linux checks

1. Run a synthetic controller through `/dev/uinput` after arranging suitable
   device access. Verify face buttons, all six axes, `ABS_HAT0X/Y`, hot-plug,
   and `SYN_DROPPED` recovery against live engine input.
2. With a physical controller, verify USB input and `FF_RUMBLE` on at least
   one Xbox-style pad and one DualSense. Record the controller model, kernel
   device capabilities, permissions, exact test route, and observed motor
   behavior. Test Bluetooth separately afterward.
3. Compare the same mod/content setup against the Doom-CE baseline using
   UZDoom 5.0.0 or newer; record versions, content hashes, launch settings,
   behavior, and logs.

## Windows and Metal parity

Windows XInput already has input and rumble code, and CI builds Windows with
Visual Studio 2022, but this checkout has no Windows runtime result. Use the
Windows machine for the next platform pass: build with its installed Visual
Studio, then verify OpenGL/Vulkan startup and framegraph smoke output, followed
by XInput button/axis and rumble checks when a controller is available. Keep
build evidence separate from runtime evidence.

The current Cocoa GameController backend also contains controller-dependent
haptics code for macOS 11 and newer; the older IOKit joystick implementation
reports no haptics. Windows XInput has rumble, while the DirectInput joystick
implementation reports no haptics. So the hardware matrix must record which
backend and capabilities were actually exercised on each OS, rather than
assuming one controller exposes the same features everywhere.

Intel Metal work continues independently while Apple Silicon is unavailable.
The next Mac validation is the wall-fan batch check in
`docs/handoff-macos-2026-10-04.md`. Apple Silicon is still needed for TBDR
policy and performance conclusions; it is not a prerequisite for Intel Metal
correctness work or measured Intel GPU tuning.
