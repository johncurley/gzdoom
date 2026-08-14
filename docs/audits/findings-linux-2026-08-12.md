# Native POSIX platform audit

Independent code-reading audit of the pruned `metal-audit` export. No runtime
tests were run, and `AGENTS.md`/`docs/` were not read because dispatch says to
skip the comparison pass in this export.

## Findings

### 1. Wayland globals are bound above the advertised server version

- **File and line range:** `libraries/ZWidget/src/window/wayland/wayland_display_backend.cpp:507-539`
- **Category:** `protocol`
- **Ownership:** `zwidget-subtree`
- **Backends affected:** `wayland`
- **Severity:** `medium`
- **Confidence:** `high`
- **Status:** `new` (documentation comparison unavailable in this export)
- **Failure scenario:** A compositor advertises a legal lower version, such as `wl_seat` 7 or `xdg_wm_base` 1. The callback ignores its `version` argument and requests versions 8 and 4. `wl_registry.bind` requires the requested version not to exceed the advertised version; a conformant compositor may raise a protocol error and disconnect the client before a window is created. The direction is client protocol failure / Wayland connection loss, not a stale or duplicated event.
- **Falsification test:** Run with `ZWIDGET_DISPLAY_BACKEND=Wayland` against a compositor or nested test compositor advertising those lower versions, with `WAYLAND_DEBUG=1`. The control is the current KWin run; success there does not falsify the portability defect. A clean run against a deliberately low-version compositor would.
- **Fix direction:** Bind each interface at `min(requested, advertised)` and only use version-supported listener entries/requests. This belongs in the ZWidget fork and needs its cherry-pick-and-publish workflow.

### 2. Registry ordering can permanently disable Wayland clipboard setup

- **File and line range:** `libraries/ZWidget/src/window/wayland/wayland_display_backend.cpp:507-526`
- **Category:** `correctness`
- **Ownership:** `zwidget-subtree`
- **Backends affected:** `wayland`
- **Severity:** `medium`
- **Confidence:** `high`
- **Status:** `new` (documentation comparison unavailable in this export)
- **Failure scenario:** The compositor announces `wl_data_device_manager` before `wl_seat`; global order is not client-controlled. The manager is bound, but line 522 sees no seat, so no data device or listener is created. When the later seat arrives, that branch does not retry. Clipboard reads remain empty and clipboard offers/selections are never observed. This is a lost capability/event path, not duplication or a crash.
- **Falsification test:** Use a minimal compositor/proxy that emits the two globals in reverse order, then exercise clipboard selection and retrieval. The current KWin run is the control for the normal order, but does not disprove the race. The defect is falsified if the later seat path demonstrably creates/registers the data device.
- **Fix direction:** Store the manager until both prerequisites exist and call an idempotent `EnsureDataDevice()` from both registry branches. Publish as a ZWidget subtree fix.

### 3. X11 fallback activation can focus an unmapped window

- **File and line range:** `libraries/ZWidget/src/window/x11/x11_display_window.cpp:307-328` (separate ICCCM handler at `:733-738`)
- **Category:** `protocol`
- **Ownership:** `zwidget-subtree`
- **Backends affected:** `x11`
- **Severity:** `medium`
- **Confidence:** `medium`
- **Status:** `new` (documentation comparison unavailable in this export)
- **Failure scenario:** On an X server without `_NET_ACTIVE_WINDOW`, `Activate()` calls `XSetInputFocus` after `XRaiseWindow` without ensuring `isMapped`/viewability. If activation precedes `Show()` or processed `MapNotify`, the server may return `BadMatch` for the non-viewable target. The direction is an X protocol error printed by Xlib and focus not being granted. This matches the reported startup error shape. Code alone cannot identify which site produced that report: the fallback is the direct no-WM startup candidate; the `WM_TAKE_FOCUS` site requires a WM message. The latter correctly uses the ICCCM message timestamp, but should not blindly focus an impossible non-viewable target.
- **Falsification test:** Run `ZWIDGET_DISPLAY_BACKEND=X11` under bare Xvfb with an X error handler or stderr capture, logging `isMapped` and event order at both call sites. XWayland/KWin is the control where EWMH normally handles activation; a clean control there does not falsify the bare-server case. If fallback activation is always after `MapNotify` and no `BadMatch` occurs, this startup finding is falsified.
- **Fix direction:** Map first and defer fallback focus until `MapNotify`/viewability, or guard with `XGetWindowAttributes` and `IsViewable`. Preserve `WM_TAKE_FOCUS` and its supplied timestamp; do not replace it with unconditional `CurrentTime`. This is a ZWidget subtree change.

## Q1 — Wayland first-paint fix

The unconditional `m_NeedsUpdate = true` at `wayland_display_window.cpp:22-41`
is a reasonable repair for the specific lost-update race, but it does not
guarantee a buffer is attached and committed after every configure.

For bitmap windows, `CheckNeedsUpdate()` clears the flag and calls the host;
the host eventually calls `Widget::Repaint()`. `Repaint()` returns without
painting when there is no `DispCanvas`. If it paints, `PresentBitmap()` creates
a SHM buffer only for positive dimensions and commits only once
`m_AppSurfaceBuffer` exists. Therefore a configure can still be followed by no
commit if the canvas is not constructed yet or the paint supplies zero sizes.
The zero-sized initial toplevel configure leaves `m_LogicalSize` unchanged,
because `OnXDGToplevelConfigureEvent()` accepts only positive dimensions;
`CreateBuffers()` also rejects non-positive sizes. A later positive configure
should recover because it sets the flag again.

For OpenGL/Vulkan, the SHM path is intentionally irrelevant. `Show()` makes the
initial empty commit required by the xdg-surface handshake, and `CreateBuffers()`
does not allocate SHM. Content must arrive from `eglSwapBuffers`; `DrawSurface()`
does nothing for these APIs. The fix cannot itself guarantee first GL pixels.
The pending `m_FrameCallback` matters only to bitmap scheduling: it suppresses
another callback request, but does not prevent the already committed buffer
from displaying. A never-delivered callback could delay later bitmap repaints,
but that separate runtime condition is not established by code reading.

The better repair is likely at the host paint/initialization boundary, with a
positive-size retry once the canvas exists, or a configure/`Show()` retry that
does not consume the update until it can paint. Moving the flag alone to
`Show()` would not fix a zero-size buffer or missing canvas. The distinguishing
evidence is a pre-fix control trace of configure serial, logical size, canvas
existence, `CheckNeedsUpdate`, `PresentBitmap`, buffer creation, and commit,
followed by the same post-fix trace. Failure to reproduce on the pre-fix commit
means the causal failure was not demonstrated and the change is not validated
by that machine; it does not prove the change wrong, but it is an unconfirmed
robustness fix rather than a proven bug fix.

## Q2 — X11 first paint and the reported launcher behavior

I find no X11 analogue requiring a Wayland-style configure/ack fix. X11 selects
`ExposureMask` and `StructureNotifyMask`, and `OnExpose()` calls
`OnWindowPaint()` directly. Mapping, expose, and resize can each trigger paint;
X11 has no ack-before-attach rule. For bitmap windows this is a sufficient
repaint trigger in principle. For OpenGL, first content depends on the later
EGL/GLX swap after mapping, not on `Expose`; that is normal X11 behavior, not
evidence of a blank-window defect here. A resize can expose an interim blank
until the next swap, but the event logic does not permanently lose the repaint.

I cannot account for the bare-Xvfb launcher exit with the required confidence.
`RunLoop()` exits when its window map becomes empty, so the cheapest discriminator
is an Xvfb run logging every window creation/destruction, `OnWindowClose`, and
the `windows.empty()` transition, compared with XWayland/KWin. Without that
trace, a no-WM artifact, an unhandled close path, and an X error are
indistinguishable.

## Looked at hard and concluded was fine

- The conditional `wl_pointer` listener version guards are correct; generated
  protocol files were not treated as hand-written findings.
- Raw/cooked routing, route latching, raw suppression, and native
  `ResetButtonTriggers()` match the stated semantics; no supported stuck-key or
  duplicate-event defect was found.
- Wayland held-key cleanup on window destruction and the manual held-key array
  walk are appropriate.
- The no-SHM OpenGL/Vulkan path and dynamic loading design are internally
  consistent and do not justify link-time dependencies.
- Backend probe order, root SDL force-disable, vacant enum value 2, POSIX theme
  derivation, and early theme initialization match the contract.

## §8 comparison with restored project notes

- **Finding 1 — new.** `AGENTS.md`'s Linux task list does not mention binding
  Wayland globals above the advertised version, and neither does
  `docs/handoff-linux.md`. The finding remains code-supported: the registry
  callback ignores its `version` parameter. It is separate from the documented
  first-paint issue.
- **Finding 2 — new.** No matching clipboard/global-order issue appears in
  either restored document. The ordering argument remains valid because the
  callback conditionally creates the data device only when the seat has already
  arrived.
- **Finding 3 — already documented, attribution refined.** `AGENTS.md` records
  the recurring X11 `BadMatch` and identifies the usual map/focus race
  (`AGENTS.md:179-189`, repeated at `:770-781`). My added code reading narrows
  the likely startup site to the no-EWMH fallback at `x11_display_window.cpp:324-327`,
  while keeping the ICCCM `WM_TAKE_FOCUS` site as a second possibility. The
  documents do not contain the required event-order measurement, so this is not
  a contradiction.
- **Q1 — already documented and deliberately unverified.** `AGENTS.md:96-111`
  and `docs/handoff-linux.md:9-13` describe the same configure/0x0/
  `m_NeedsUpdate` mechanism, the lack of a pre-fix control, and the required
  validation. My answer adds the limitation that the flag cannot guarantee a
  commit when `DispCanvas` or a positive size is absent; it does not claim the
  fix is disproven. The restored notes agree that failure to reproduce is an
  informative absence of proof.
- **Q2 — already documented for the open symptoms; analysis is new.** The
  separation between Wayland and X11, the recurring `BadMatch`, and the bare-Xvfb
  exit are all explicitly open and unchased in `AGENTS.md:179-189` and
  `docs/handoff-linux.md`'s archived handoff. My conclusion that X11's expose/
  resize delivery is the first-paint substitute, with no xdg-style ack rule, is
  an additional protocol/code analysis, not a measured resolution. I did not
  turn the Xvfb observation into a finding, consistent with the notes' explicit
  “not confirmed” status.

The restored documents do not contradict any original finding. They confirm
that Q1 and the X11 symptoms were pre-existing open work, while Findings 1–2
remain independent observations not covered by the project handoff.
