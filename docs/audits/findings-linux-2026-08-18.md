# Audit findings: native POSIX platform layer (Linux/Wayland/X11)

Audited against `docs/audits/audit-contract-linux.md` (revision 2026-08-18), branch
`metal-audit`, working tree as checked out. `AGENTS.md` was **not** read until
after the findings below were written; §8's diff follows at the end of this
file.

Environment used for verification: Arch/CachyOS, KDE Plasma on Wayland
(kwin_wayland), Mesa 26.1.6, AMD RX 550. X11 tested via XWayland (not a bare
X server with a traditional WM) unless noted. Tools actually run:
`python3 tools/matrix/crossbackend.py --backends gl,vulkan --selfcheck`, and a
direct `ZWIDGET_DISPLAY_BACKEND=X11 ./build/gzdoom ... +quit` smoke launch.
`xdotool`/`wmctrl` are absent on this machine, so no finding below that needs
scripted focus/window changes could be driven to completion; each such case
says so explicitly.

---

## Finding 1 — X11 does not synthesize key-up on focus loss; Wayland does, deliberately

- **File and line range**: `libraries/ZWidget/src/window/x11/x11_display_window.cpp:910-917` (`OnFocusOut`), contrasted with `libraries/ZWidget/src/window/wayland/wayland_display_backend.cpp:654-683` (`keyboard_handle_leave`). Symptom manifests in `src/common/platform/posix/native/nativevideo.cpp:474-556` (`GZDoomWindowHost::keyRoutes`, `OnWindowKeyDown`/`OnWindowKeyUp`).
- **Category**: `correctness` (lost input; also an `X11DisplayWindow::keyState` staleness bug at `x11_display_window.cpp:436-440`).
- **Ownership**: `zwidget-subtree` for the root cause (`x11_display_window.cpp`'s `OnFocusOut`); the symptom-carrying `keyRoutes` latch is `fork` (`nativevideo.cpp`). Both sites would need touching to fix this cleanly, and the subtree half needs the cherry-pick-and-publish procedure (§4 of the contract).
- **Backends affected**: `x11` only (both raw and cooked keyboard delivery paths are affected identically, since the bug is upstream of the raw/cooked split — the missing event is a *cooked* `KeyRelease`/`OnWindowKeyUp` that never arrives at all).
- **Severity**: high
- **Confidence**: high for the mechanism (read directly, and the asymmetry is explained in Wayland's own comment, which states exactly the failure this omission on X11 reproduces); medium for how often it is actually triggered by ordinary play (needs a hold-key-across-focus-loss sequence).

**Mechanism.** Wayland's `keyboard_handle_leave` has an explicit comment
explaining why it walks `m_PressedScancodes` and synthesizes `OnWindowKeyUp`
for every currently-held key on focus loss: *"Per protocol, focus loss
releases every held key -- the compositor will not send the individual
key-up events. Synthesize them, or the client keeps keys latched down until
something else resets state."* X11's `OnFocusOut` does the equivalent-looking
thing for `RawInput.Focused` (clears the raw-input focus flag) but does
**not** walk its own per-key state (`keyState`, a `std::map<InputKey, bool>`
at `x11_display_window.h:110`) and does not call `windowHost->OnWindowKeyUp()`
for any key that is still logically down. This is the same failure mode
Wayland's fix exists to prevent, on the other backend, unfixed.

X11 has the identical protocol shape: `KeyPress`/`KeyRelease` are delivered
only to whichever window currently has server-side input focus. If focus
moves to a different X client while a key is held, and the key is physically
released while that *other* client has focus, this window's `OnKeyRelease`
never fires — there is no synthesized event and no protocol event to
synthesize it from.

**Direction of error and site of the actual symptom.** The engine already
guards against "gameplay button appears stuck" via
`I_SetWindowFocus(false)` → `buttonMap.ResetButtonStates()`
(`src/common/platform/posix/native/i_input.cpp:230-241`), which resets the
*bound-action* state (jump/fire/etc.) unconditionally on focus loss — so this
is **not** the classic "character keeps moving forever" symptom, and does not
reproduce the `bWentDown`-vs-`bDown` trap the contract warns about.

What is *not* reset is `GZDoomWindowHost::keyRoutes`
(`nativevideo.cpp:323`), a `std::map<InputKey, EventRoute>` that latches
whether a key's press/release pair is routed to the GUI or to gameplay, and
`X11DisplayWindow::keyState`, which backs `GetKeyState()`. Both are
per-key maps with no per-tic reconciliation (contrast
`I_ReconcileMouseButtons()` in `i_input.cpp:152-187`, which exists
specifically to catch a missed *mouse*-button release the same way — "If we
miss a wl_pointer button release ... the game can get a stuck KEY_MOUSE*
down state. As a safety net, reconcile ... once per tic" — but was never
extended to keyboard). Trace the sequence:

1. Player holds a key (say `W`) in gameplay. `OnWindowKeyDown` records
   `keyRoutes[W] = Game` (`nativevideo.cpp:495`).
2. Focus moves away (alt-tab, click another window) — X11 delivers
   `FocusOut`; `OnFocusOut` clears `RawInput.Focused` only.
   `buttonMap.ResetButtonStates()` runs via `I_SetWindowFocus(false)`, so the
   character stops moving — correctly, from the player's point of view.
   `keyRoutes[W]` and `X11DisplayWindow::keyState[W]` remain `true`/`Game`.
3. Player releases `W` while a *different* window has focus. This window
   never receives `KeyRelease` for `W` — X11 delivered it to the focused
   client. `keyRoutes[W]` is never erased.
4. Player refocuses this window and presses `W` again. `OnWindowKeyDown`
   finds a live entry in `keyRoutes` and — per the comment at
   `nativevideo.cpp:477-491` — treats this as an auto-repeat of an
   already-held key (`isRepeat = true`). For the gameplay branch
   (`nativevideo.cpp:514`, `else if (!isRepeat && !RawKeyboardActive)`),
   `isRepeat == true` skips the block entirely: **no `EV_KeyDown` is
   posted**, i.e. the fresh keypress is silently eaten. (The same
   misclassification also degrades the GUI route: `EV_GUI_KeyRepeat` is sent
   in place of `EV_GUI_KeyDown` at `nativevideo.cpp:507`, which some widgets
   — anything gating on Down rather than Down-or-Repeat — will not act on.)
5. The stale entry self-heals on the very next full press/release of that
   same key: `OnWindowKeyUp` posts a spurious `EV_KeyUp` (harmless — bindings
   treat an up-without-down as a no-op) and erases `keyRoutes[W]`, so the
   press after that behaves normally.

So the concrete, checkable claim is: **one keypress is silently dropped**,
exactly once per affected key per focus-loss-while-held episode, self-healing
on the next cycle. Not a permanently stuck key, not a crash — hence `high`
rather than `critical` — but a real, reproducible, correctly-directioned
input-loss bug in the default configuration (`in_rawkeyboard 0`), with a very
ordinary trigger (alt-tabbing while holding a movement key, common in
practice).

**Falsification test.** Could not be driven to completion here: reproducing
it needs a second window to hold focus while the key is released, and moving
focus programmatically needs `xdotool`/`wmctrl`, both absent per §3. The
control that would disprove this: hold a key, focus a second X11 window,
release the key there, refocus GZDoom, press the same key once — if
`EV_KeyDown`/`EV_GUI_KeyDown` is posted normally, the mechanism above is
wrong. This is stated as unverified dynamically, per the contract's evidence
standard §5.6; the code-level trace (steps 1-5 above) is what stands in its
place.

**Fix direction.** Give `X11DisplayWindow::OnFocusOut` the same treatment as
Wayland's `keyboard_handle_leave`: walk `keyState`, and for every key
currently `true`, call `windowHost->OnWindowKeyUp(key)` before delivering
`OnWindowDeactivated()`, then clear `keyState`. That closes the ZWidget-side
staleness and, transitively, the `keyRoutes` misclassification in
`nativevideo.cpp` without touching the fork-side file at all.

---

## Finding 2 (Q2.1) — the `WM_TAKE_FOCUS`/viewability guard narrows the BadMatch race but does not close it, and nothing outside a narrow scope replaces Xlib's fatal default error handler

- **File and line range**: `libraries/ZWidget/src/window/x11/x11_display_window.cpp:342-348` (`IsWindowViewable`), `:860-891` (`OnClientMessage`, `WM_TAKE_FOCUS` handler), `:350-386` (`Activate()`'s fallback branch).
- **Category**: `protocol` (an X request the server is entitled to answer with `BadMatch` if the window state changes between our round trip and our next request; also `correctness` for the resulting crash).
- **Ownership**: `zwidget-subtree`.
- **Backends affected**: `x11` only.
- **Severity**: medium (the failure mode, if hit, is a hard process crash — Xlib's default error handler calls `exit(1)` on any unhandled protocol error, confirmed by grep: the only `XSetErrorHandler` call anywhere in the X11 backend is the narrowly-scoped one around `XShmAttach`, `x11_display_window.cpp:609-612`, restored immediately after; nothing else changes the default handler — but the race window is a single round trip with no intervening event processing, so the probability of actually hitting it is low. This is a genuine, still-open gap, just a narrow one, which is why it isn't `critical`.)
- **Confidence**: high on the mechanism; explicitly **not dynamically verified** — the `twm` harness that would exercise this is confirmed broken for unrelated reasons (AGENTS.md item 13, cross-referenced in the contract itself), so this is reasoned from ICCCM and the code only, as instructed by the contract for this exact question.

**Analysis.** `XGetWindowAttributes` (inside `IsWindowViewable()`) is a
request-reply round trip: it blocks until the server answers, and *while
answering*, the server's view of the window is authoritative and current
(this is exactly the synchronization property the comment at
`x11_display_window.cpp:339-341` claims). What the guard does **not**
establish is anything about the window's state *after* the reply arrives.
Between the `XGetWindowAttributes` reply landing in this client and the next
request (`XSetInputFocus`) being sent, the only work this thread does is a
few local branches — no `select()`, no event pump, nothing that hands control
back to the X server or the window manager. So the window for a genuine race
is real but tiny: it needs the WM to unmap the window and have that
request's effects land at the server in the handful of instructions between
the two client requests. That is narrow, not zero. X requests from different
clients are serialized by the server in arrival order, not synchronized with
any one client's round trips, so there is no formal guarantee here, only a
practical one from how little code runs in between.

The `WM_TAKE_FOCUS` handler in `OnClientMessage` has the identical shape:
`IsWindowViewable()` then immediately `XSetInputFocus`, no intervening event
processing.

**Conclusion on Q2.1**: the guard narrows the race from "always crashes when
a WM asks for pre-map focus" (the bug `dd88a86b7` actually fixed, which was
wide open before) to "crashes only if the WM's unmap request is processed by
the X server in the sub-millisecond gap between our round trip returning and
our next request going out." It does **not** formally close it, and nothing
downstream would catch the resulting `BadMatch` — there is no general error
handler installed. Given how narrow the window is, this is not something I
would block a release on, but it is not "sufficient" in the strict sense
Q2.1 asks about.

**Falsification test**: cannot be run here (needs `twm`, which item 13
confirms is unusable for unrelated reasons on this machine — a bare Xlib
program hangs before this scenario could even be staged). The control that
would prove the race closed: install a fresh custom `XSetErrorHandler` for
the whole process (not just around Shm), stress-cycle map/unmap from a
second client at high frequency while calling `Activate()`/delivering
synthetic `WM_TAKE_FOCUS`, and confirm zero `BadMatch` reports over many
thousands of iterations. Not run.

**Fix direction**: install a process-wide error handler that logs and
survives `BadMatch`/`BadWindow` instead of the Xlib default (`exit(1)`)
around all focus-related requests, or accept the race as it stands and treat
any future `BadMatch` report here as this same class rather than a new bug.

---

## Finding 3 (Q2.2) — `pendingActivate` is a sticky, intent-less boolean; an unrelated `Show()` can discharge a stale activation

- **File and line range**: `libraries/ZWidget/src/window/x11/x11_display_window.cpp:270-284` (`Show()`), `:350-363` (`Activate()`'s pending-activate branch), `x11_display_window.h:112` (`bool pendingActivate = false;`).
- **Category**: `correctness`.
- **Ownership**: `zwidget-subtree`.
- **Backends affected**: `x11` only (Wayland's `Activate()` at `wayland_display_window.cpp:282-295` has no equivalent deferred-intent state — it is idempotent per call and does not interact with `Show()`/`Hide()` at all).
- **Severity**: medium
- **Confidence**: medium — the mechanism is a straightforward read of the state machine, but I could not verify dynamically whether any current call site actually produces the ordering this depends on (see below), so I can't say how live a risk this is today versus latent.

**Analysis.** `pendingActivate` is a single `bool`, not a queue and not
tagged with which `Activate()` call (or caller) set it. Two behaviors follow
directly from that:

1. **Multiple `Activate()` calls before one `Show()` collapse harmlessly.**
   Setting an already-`true` boolean to `true` again is a no-op, so two
   callers both wanting activation-on-map get exactly one discharge. This
   part is fine and is very likely the case the field exists for (the
   `LauncherWindow` constructor-before-`Show()` case the code comments cite).
2. **`Show()` discharges unconditionally, regardless of why *this* `Show()`
   call happened.** There is no cancellation path: `Hide()` does not clear
   `pendingActivate`, and there is no timeout. So: window is shown normally
   (`pendingActivate` false). Later, `Hide()` is called. While hidden,
   something calls `Activate()` — `pendingActivate` becomes `true`. Before
   that intent is ever fulfilled, something else — for a reason unrelated to
   the original `Activate()` call, e.g. just wanting to redisplay the window
   — calls `Show()`. `Show()` has no way to know the pending flag belongs to
   a different, possibly stale, request; it discharges it and calls
   `Activate()`, stealing input focus as a side effect of a `Show()` call
   whose caller did not ask for that.

This directly answers Q2.2's question: yes, there is a sequence where a
stale pending activation fires for an intent that no longer matches — the
flag has no expiry and no association with a specific `Show()` call.

**Whether this is reachable today**: I did not find, in the files in scope,
a call site that actually does `Activate()` while hidden followed later by
an unrelated `Show()` — the one documented use (`LauncherWindow`) creates,
activates, then shows once, which is exactly the safe case. This may be
latent (correct for every current caller, wrong for a caller that doesn't
exist yet) rather than actively wrong today. I could not verify this
dynamically (would need to drive `Hide()`/`Show()`/`Activate()` interleavings
interactively, which needs `wmctrl`/`xdotool`).

**Falsification test**: instrument (temporarily, not proposed as a
committed change) `Show()` to log whenever it discharges `pendingActivate`,
run the launcher and any dialog/net-start window through ordinary use, and
confirm the log only ever fires immediately after the `Activate()` that was
clearly intended for that exact `Show()`. Not run here — no scripted-focus
tool available to drive dialog open/close cycles.

**Fix direction**: either tag `pendingActivate` with the requesting call
(e.g. store nothing but clear it in `Hide()` too, so a hide before the show
cancels the intent), or make `Activate()` re-check some liveness condition
before firing from `Show()`'s discharge path.

---

## Finding 4 (Q2.3) — `_NET_ACTIVE_WINDOW` uses `CurrentTime` rather than an event timestamp; `WM_TAKE_FOCUS` itself is handled correctly

- **File and line range**: `libraries/ZWidget/src/window/x11/x11_display_window.cpp:369-379` (`_NET_ACTIVE_WINDOW` `ClientMessage`), `:875-884` (`WM_TAKE_FOCUS` handler), `:380-385` (bare `XSetInputFocus` fallback).
- **Category**: `protocol`
- **Ownership**: `zwidget-subtree`
- **Backends affected**: `x11` only
- **Severity**: low / info
- **Confidence**: medium (ICCCM/EWMH reading; not something a capture or a log can adjudicate, and KWin is known to be lenient here, so this is not observable as broken on this machine specifically)

**WM_TAKE_FOCUS is handled correctly.** ICCCM 4.1.7 requires the client to
use the timestamp supplied in the `WM_TAKE_FOCUS` `ClientMessage`, not
`CurrentTime`, when calling `XSetInputFocus` in response. The code does
exactly this (`Time timestamp = event->xclient.data.l[1]; ...
XSetInputFocus(display, window, RevertToParent, timestamp);`), and the
comment at `:878-880` shows this was a deliberate choice, not an oversight.
This is correct and is not a finding.

**`_NET_ACTIVE_WINDOW` uses `CurrentTime`.** EWMH's `_NET_ACTIVE_WINDOW`
spec says `data.l[1]` should carry the timestamp of the user event that
caused the activation request (0 if unknown), specifically so focus-stealing
prevention in the WM can judge how "fresh" the request is. This code always
sends `CurrentTime` (`x11_display_window.cpp:375`). This is extremely common
in practice (most non-EWMH-pedantic toolkits do the same) and KWin does not
visibly reject it — this is not something observable as broken on the
reference machine — but it is not what the spec recommends, and a
strict/older WM with active focus-stealing prevention is permitted to
deprioritize a `CurrentTime` request. Low severity because the practical
blast radius (on the WMs this fork is likely to be run under) is small, and
because it would show up as "window doesn't grab focus on launch," not a
crash or lost input.

**The bare `XSetInputFocus` fallback** (`:380-385`, used only when the WM
does not advertise `_NET_ACTIVE_WINDOW` — e.g. `twm`, per the contract's own
environment notes) is a client unilaterally seizing focus outside any
WM-mediated protocol. ICCCM does not forbid this outright, but it is
explicitly the less-preferred mechanism versus letting the WM decide via
`WM_TAKE_FOCUS`; on a WM enforcing its own focus policy (focus-follows-mouse,
click-to-focus with strict ordering) this can produce a focus grab the WM's
policy would not otherwise have granted. Given `twm` is the only readily
available WM lacking `_NET_ACTIVE_WINDOW` on this box, and `twm` itself is
confirmed broken here for unrelated reasons (item 13), this path could not be
exercised end-to-end.

**Falsification test**: none run; this is a spec-reading finding. A
falsification would be: find or configure a WM with strict focus-stealing
prevention (e.g. an older Metacity/Mutter config, or a WM that specifically
distinguishes timestamped vs `CurrentTime` `_NET_ACTIVE_WINDOW` requests),
launch under it, and see whether the window actually receives focus on
`Activate()`. Not available on this machine.

**Fix direction**: thread the originating event's timestamp through to
`Activate()` where one is available (e.g. from a user-initiated action);
fall back to `CurrentTime` only when there genuinely is none, matching EWMH's
own suggested degradation.

---

## Finding 5 — debug diagnostics left in unconditionally, on both the ZWidget X11 backend and the fork's GL/EGL bring-up code

- **File and line range**: `libraries/ZWidget/src/window/x11/x11_display_window.cpp:900,912` (`OnFocusIn`/`OnFocusOut`); `src/common/platform/posix/native/gl_sysfb.cpp:165-170` (`LogToDisk`, appends to a hardcoded `/tmp/gzdoom_debug.log` on every call site that uses it) and its call sites at `:215,225,411,413`; assorted unconditional `fprintf(stderr, ...)` at `gl_sysfb.cpp:320-321,333-334,337,347-348,358,405,424`.
- **Category**: `maintainability`
- **Ownership**: `x11_display_window.cpp` lines are `zwidget-subtree`; `gl_sysfb.cpp` is `fork`.
- **Backends affected**: `x11` for the focus prints; `both` for the GL/EGL bring-up prints (they run regardless of Wayland vs X11, since `SystemGLFrameBuffer`'s constructors are shared).
- **Severity**: low
- **Confidence**: high — directly reproduced, not just read. A plain
  `ZWIDGET_DISPLAY_BACKEND=X11 ./build/gzdoom ... +quit` run prints
  `[X11] OnFocusIn called: mode=0, detail=3` and the matching `OnFocusOut`
  line to stderr on every focus transition, unconditionally, with no cvar or
  env var gating it off.

**Analysis.** The contract explicitly documents that `in_keytrace` and
`ZWIDGET_TRACE_REPEAT` — the *intentional* diagnostics for exactly this kind
of input tracing — were removed once their job was done (2026-08-13). These
`fprintf` calls in `OnFocusIn`/`OnFocusOut` are the same class of leftover
instrumentation, just never registered as a named, opt-in diagnostic and
therefore never flagged for removal. They cost nothing functionally (stderr
in a game process is usually not observed by a player) but they are
unconditional, per-event, and would spam any log capture of a normal play
session. `LogToDisk` is a step further: it opens and appends to a fixed
`/tmp/gzdoom_debug.log` path every time `InitEGL` is invoked, on every
platform this file's macros apply to, growing without bound across runs and
sessions, with no rotation and no opt-out.

**Falsification test**: already run — see above; empirically confirmed
present, not hypothetical.

**Fix direction**: delete the two `fprintf`s in `OnFocusIn`/`OnFocusOut`
(cherry-pick-and-publish through the subtree procedure), and either delete
`LogToDisk` entirely or gate it behind an existing debug cvar so it does not
write to disk on every ordinary launch.

---

## Finding 6 — `POSIXNativeThemeImpl::DetectColors()` has no positive "light" signal; an unconfigured GTK-light desktop gets the hardcoded dark palette

- **File and line range**: `libraries/ZWidget/src/core/theme.cpp:434-556`, specifically the GTK branch at `:487-503` and the no-detection fallthrough at `:531` (`if (detected)` — false means every value stays at the hardcoded dark defaults declared at `:439-450`).
- **Category**: `correctness` (a description of a specific gap in the heuristic's fallback direction, not the same claim the contract pre-clears — see below)
- **Ownership**: `zwidget-subtree` (fork-authored file within the subtree; the contract's own domain-knowledge section lists `POSIXNativeTheme` under "fork-side ZWidget work that is deliberate, not accidental").
- **Backends affected**: `neither` (build/config-level — this is launcher theming, independent of Wayland vs X11).
- **Severity**: medium (cosmetic — wrong launcher colors, not a functional break), low-medium blended given uncertainty
- **Confidence**: low-medium. Reasoned from the code; **not dynamically verified**, because this reference machine is KDE and always takes the `kdeglobals` branch (`detected = true` before the GTK check ever runs), so the gap cannot be exercised here without removing `~/.config/kdeglobals`, which risks disturbing the actual desktop environment and was not done.

**Analysis.** The contract pre-clears one specific claim about this function:
*"'Only two colours are actually detected' is a description of the design,
not a finding."* That is a claim about *granularity* (only bg/fg are read,
the rest are derived) and is correctly out of bounds. The claim here is
different and more specific: it is about which *direction* the fallback
defaults to when detection finds nothing at all.

Walking the three detection branches in order:

1. **KDE** (`kdeglobals`): sets `detected = true` only if `BackgroundNormal=`
   is found under `[Colors:Window]`.
2. **GTK** (`settings.ini`): sets `detected = true` **only** if
   `gtk-application-prefer-dark-theme=1` (or `=true`) is found. There is no
   corresponding check for a *light* signal — no read of GTK's actual
   background color, no check for the theme name, nothing. A GTK desktop
   running its own default (Adwaita, light) with no explicit dark-theme
   override present in `settings.ini` leaves `detected` false here.
3. **Xresources**: only overrides `bgMain`/`fgMain` if `background:` /
   `foreground:` lines are present in `.Xresources`/`.Xdefaults`; does not
   set `detected` at all.

If none of the three produce a hit — which describes an out-of-the-box
GNOME/GTK session with no `kdeglobals`, no dark-theme override, and no
`.Xresources` — `detected` stays `false`, the `if (detected)` block at `:531`
is skipped, and every one of the twelve theme colors keeps its hardcoded
value from `:439-450`, which is the **dark** palette. So the practical
result of "auto" (`ui_theme 0`) on a vanilla light-themed GTK desktop with no
manual desktop-file tweaking is a dark launcher on a light desktop — the
opposite of what "auto, matches the desktop" is supposed to do, and
specifically because the heuristic can only detect "definitely dark," never
"definitely light" or "definitely GTK's own colors."

**Falsification test**: rename `~/.config/kdeglobals` out of the way (if
present) and `~/.config/gtk-3.0/settings.ini` (if present) and `~/.Xresources`
so all three probes miss, launch with `ui_theme 0`, and check whether the
launcher renders with the dark palette regardless of the actual desktop's
light/dark setting. Not run — this machine's KDE session makes `kdeglobals`
authoritative, and deliberately breaking that file to test a GTK code path
felt like the wrong kind of experiment to run against the working desktop
environment for a low/medium-severity, cosmetic-only finding.

**Fix direction**: read GTK's actual background/foreground where available
(e.g. via `gsettings get org.gnome.desktop.interface` shelling out, or
parsing a GTK CSS theme's declared colors) rather than only a boolean
dark-theme flag; failing that, default the no-detection fallthrough to the
built-in light/dark split via `I_IsDarkMode()` (already used by `ui_theme 3`,
`widgetresourcedata.cpp:77`) instead of unconditionally dark.

---

## Finding 7 — silent, undiagnosed GLX context-creation failure path in `gl_sysfb.cpp`

- **File and line range**: `src/common/platform/posix/native/gl_sysfb.cpp:361-373` and `:416-428` (the two `SystemGLFrameBuffer` constructors' GLX fallback blocks).
- **Category**: `correctness`
- **Ownership**: `fork`
- **Backends affected**: `x11` only (this path only runs when EGL is either not compiled in or has already failed and X11 is in use — Wayland always goes through the EGL path when `HAVE_WAYLAND_EGL` is defined).
- **Severity**: medium (real gap, but its literal precondition — both `InitEGL` failing or not being compiled in, *and* GLX itself failing — is unlikely on this hardware/driver combination; flagged because of the contract's own history with exactly this failure shape)
- **Confidence**: medium — the code path is real and unambiguous; whether it is *currently reachable* on any machine this fork ships to was not established, since EGL succeeds on this box and the GLX branch is never taken in practice here.

**Analysis.** Both constructors do, in the GLX fallback:

```cpp
if (InitGLX()) {
  ...
  XVisualInfo *vi = zd_glXChooseVisual(display, screen, visual_attribs);
  if (vi) {
    GLXContext context = zd_glXCreateContext(display, vi, NULL, GL_TRUE);
    zd_glXMakeCurrent(display, window, context);
    ...
  }
}
```

If `InitGLX()` returns `false` (libGL.so.1 not found, or the four `dlsym`
lookups don't all resolve — `gl_sysfb.cpp:66-91`), there is no `fprintf`, no
log line, nothing — the constructor simply returns having done nothing. If
`InitGLX()` succeeds but `glXChooseVisual` returns `nullptr` (no matching
visual — plausible on an unusual display/depth combination or a software-only
GLX implementation), the `if (vi)` block is skipped, again completely
silently. If `glXCreateContext` itself returns a null/failure context, its
result is passed to `glXMakeCurrent` with **no failure check at all** — the
call proceeds regardless, and downstream GL calls would then run against
whatever context (or lack of one) `glXMakeCurrent` leaves bound.

The reason this is worth flagging despite the narrow precondition: the
contract's own history names *this exact failure shape* — a black/blank GL
frame — as the subsystem's single most expensive category of past mistake
("Four separate conclusions about a black GL frame were committed and then
retracted over one session"), and the actual historical cause there
(`GL_INVALID_OPERATION` from `glUniform` on a program that never linked,
diagnosed via `MESA_DEBUG=1`) was a downstream symptom of a GL context that
existed but wasn't fully valid. A GLX context that fails to bind at all,
silently, is upstream of and consistent with that same observable: a window
that never paints, with nothing in stderr to explain why. This is not a
claim that it *has* caused a past incident — the EGL path is preferred and
succeeds first on every machine this was likely tested on — only that the
code, as it stands, would reproduce that exact silent-failure shape if ever
exercised.

**Falsification test**: the cheap version — temporarily block `libGL.so.1`
from resolving (e.g. via a wrapper `LD_LIBRARY_PATH`/`dlopen` shim, not
attempted here to avoid destabilizing the GL install other falsification
tests in this audit depend on) and confirm the window comes up blank with
zero stderr output, versus the EGL path's explicit `fprintf` failure
messages at `gl_sysfb.cpp:265,297,306,315`. Not run.

**Fix direction**: add the same `fprintf`-on-failure treatment the EGL path
already has to each silent branch (`InitGLX()` returning false,
`glXChooseVisual` returning null, `glXCreateContext` returning a null
context) so a future black-window report at least has something in the log
to start from.

---

## Answers to the contract's two specific questions (§6)

**Q1 — the new X11 raw-input path (XInput2).** No correctness bug found in
the raw-input path itself:

1. **Event scope**: `x11_connection.cpp:64-81` selects `XI_RawMotion`,
   `XI_RawKeyPress`, `XI_RawKeyRelease` on the root window with
   `eventmask.deviceid = XIAllMasterDevices` — the code comment even
   documents the exact bug class the contract asked me to hunt for
   (`MasterPointerID` silently dropping keyboard events) and explains why
   `XIAllMasterDevices` is used instead. This is already correct and matches
   Wayland's effectively-focus-scoped delivery once gated by
   `RawInput.Focused` in `OnXInputEvent` (`x11_display_window.cpp:1189-1190`).
   I could not verify under XWayland whether raw key delivery is scoped
   identically to a genuine Wayland-focused surface in every edge case (e.g.
   focus ownership disputes between XWayland and native Wayland clients) —
   flagging as unverified rather than asserting it is fine, per §5.6.
2. **Keycode-minus-8 translation**: exactly one site does this
   (`x11_display_window.cpp:1198`), guarded by `if (rawEvent->detail < 8)
   return false;` against underflow. Grepped for every other `detail`/
   `keycode` use in the file (`:928`, `:1085-1086` commented out, `:1209`);
   none needs the same offset — `:928` uses `XkbKeycodeToKeysym`, which does
   its own translation, and `:1209` is a mouse-button number, not a keycode.
   No missing second site found.
3. **`LockKeyboard`/`UnlockKeyboard` symmetry**: both backends implement
   these as pure boolean flags with no protocol side effects
   (`x11_display_window.cpp:397-405`, `wayland_display_window.cpp:299-307`),
   called from the same single call site in `i_input.cpp:110-115`
   (`I_CheckRawKeyboard`), which also calls `buttonMap.ResetButtonStates()`
   unconditionally on every raw/cooked transition (`i_input.cpp:120`) — this
   is symmetric and correct on both backends. **However**, see Finding 1
   above: X11 has a *related but distinct* focus-loss gap in the **cooked**
   key-tracking path that Wayland's `keyboard_handle_leave` explicitly
   guards against and X11's `OnFocusOut` does not. It is not the raw-input
   lock/unlock symmetry itself, but it is exactly the "stuck-key shape" the
   contract's own framing for Q1.3 points at, on the adjacent path.
4. **Ownership**: entirely `zwidget-subtree`. `x11_connection.cpp`'s raw
   event selection and `x11_display_window.cpp`'s `OnXInputEvent` are both
   inside `libraries/ZWidget/src/window/x11/`; none of this touches the
   fork's own `nativevideo.cpp`/`i_input.cpp` beyond the pre-existing
   `LockKeyboard`/`UnlockKeyboard`/`ResetButtonStates()` call sites, which
   were already there for Wayland. A fix to Finding 1 (the focus-loss gap)
   would also be entirely inside the ZWidget subtree.

**Q2 — is the `WM_TAKE_FOCUS`-while-unviewable guard sufficient?** No, not
in the strict sense, though it is a large practical improvement over what it
replaced. See Findings 2-4 above for the three sub-answers: (1) the
viewability check narrows the `BadMatch`/crash race to a single round-trip
gap rather than closing it, and nothing outside a narrow scope replaces
Xlib's fatal default error handler if that gap is ever hit; (2)
`pendingActivate` is a sticky, un-cancelable boolean that a later, unrelated
`Show()` can discharge, misattributing an old activation intent to a new
call — not proven live against any current call site, but a real gap in the
state machine; (3) `WM_TAKE_FOCUS` itself correctly uses the WM-supplied
timestamp per ICCCM, but the client-initiated `_NET_ACTIVE_WINDOW` path uses
`CurrentTime` rather than an event timestamp, which is common practice but
not what EWMH recommends, and the non-EWMH bare-`XSetInputFocus` fallback is
the less-preferred mechanism under ICCCM's own input-focus model. None of
this was dynamically verified — the `twm` harness that would exercise it is
confirmed broken for reasons outside this codebase (item 13) — so all three
are reasoned from the code and the relevant specs alone, as the question
itself asks for.

---

## Looked at hard, concluded fine (negative space)

- **`XIAllMasterDevices` raw-key subscription** (`x11_connection.cpp:76`) —
  correct, and the code's own comment shows the maintainers already knew
  about and avoided the `MasterPointerID` trap the contract describes.
- **The single keycode-minus-8 site** and its guard — correct, and I could
  not find a second site needing the same treatment.
- **`LockKeyboard`/`UnlockKeyboard` symmetry** across backends — both are
  inert flags with matching call sites and matching `ResetButtonStates()`
  discipline in `i_input.cpp`.
- **`WM_TAKE_FOCUS`'s own timestamp handling** — correctly preserves the
  WM-supplied `Time` rather than substituting `CurrentTime`, matching ICCCM.
- **The SHM-attach narrow error handler** (`x11_display_window.cpp:608-612`)
  — correctly scoped, correctly restored, and its rationale (avoiding the
  item-13-class deadlock from a giant synchronous `XPutImage`) matches what
  `AGENTS.md`/the contract describe elsewhere; no bug found here.
- **`ui_theme`'s out-of-range fallback** (`widgetresourcedata.cpp:24-28,
  67-112`) — even if the `CUSTOM_CVARD` clamp callback is skipped for a
  command-line `+set ui_theme 99` (per the documented `CVAR_NOINITCALL`/
  `EnableCallbacks()` trap), the selection logic is a boolean cascade with no
  array indexing, so an out-of-range value degrades to the light theme
  branch rather than reading out of bounds. Safe by construction; no site
  here trusts the clamp in a way that matters.
- **`DisplayBackend::TryCreateBackend()` probe order**
  (`libraries/ZWidget/src/window/window.cpp:90-137`) — matches the
  documented Wayland → X11 → SDL2 order once SDL2/SDL3 are accounted as
  force-disabled; `ZWIDGET_DISPLAY_BACKEND` override handled correctly for
  all named backends.
- **`x11_remap.h`'s macro table** — every Xlib entry point used anywhere in
  the X11 backend files I read appears to have a corresponding `#undef`/
  `#define` pair pointing at the dynamic loader; did not find a call site
  using an un-remapped (directly linked) Xlib symbol that should have been
  dynamic.
- **`crossbackend.py --backends gl,vulkan --selfcheck`** — ran successfully;
  all but one of 11 configs reproduced identically across two runs per
  backend (the one exception, `bloom_ref` on GL, showed a 1.275/25.318 ≈ 5%
  delta, consistent with the contract's documented ~8% flake rate and not
  pursued further — renderer timing is out of this audit's scope regardless).
  This confirms the harness itself is usable on this machine as described,
  even though it wasn't the right oracle for this audit's actual questions
  (input/focus handling, not rendering).
- **Debug-print finding (5) empirically confirmed**, not merely inferred —
  see the reproduction command in that finding.

---

## §8 — diff against `AGENTS.md` and `docs/handoff-linux.md`

Read after the findings above were written, per the contract.

- **Item 6** (`AGENTS.md` Tasks — Linux) records the X11 XInput2 raw-keyboard
  work landing 2026-08-13, "verified interactively" — consistent with what
  the contract's Q1 preamble says, and consistent with what I found: the
  landed code is correct on the points I checked (device mask, keycode
  offset, lock/unlock symmetry). AGENTS.md does not mention the
  `OnFocusOut` key-up-synthesis gap (Finding 1) at all — that finding is
  **new**, not a re-derivation of anything already logged.
- **Item 13** is the whole-server X11 deadlock under `twm`, confirmed
  unrelated to this codebase with a bare Xlib reproduction. The contract
  told me not to re-derive this, and I didn't — I used it as the reason
  Findings 2-4 (the Q2 sub-answers) and Finding 3's falsification test
  could not be run dynamically, exactly as the contract anticipates.
- I did not find any `AGENTS.md` entry discussing `pendingActivate`'s
  discharge semantics (Finding 3), the `_NET_ACTIVE_WINDOW` timestamp choice
  (Finding 4), the GTK-light-theme detection gap (Finding 6), or the silent
  GLX-failure path (Finding 7). All four are **new**.
- The debug `fprintf`s in `OnFocusIn`/`OnFocusOut` (Finding 5) are not
  mentioned either; `AGENTS.md`'s "Traps that have cost real sessions"
  section calls out `in_keytrace`/`ZWIDGET_TRACE_REPEAT` as diagnostics that
  *were* tracked and deliberately removed once done, which makes the
  untracked `OnFocusIn`/`OnFocusOut` prints and `LogToDisk` look like the
  same class of leftover that simply never got the same treatment — **new**,
  not a contradiction of anything already recorded.
- `docs/handoff-linux.md`'s two validation tasks are recorded as done and
  are not re-proposed here; nothing in this file overlaps with them.
- Nothing in `AGENTS.md` or `docs/handoff-linux.md` contradicts any finding
  above outright. Where AGENTS.md is silent on a topic I found something in
  (all seven findings, to varying degrees), I'm treating that silence as
  "not previously found" rather than "considered and dismissed," per the
  contract's instruction to say which of us is right rather than defer
  automatically — in every case here, AGENTS.md simply doesn't address the
  specific claim, so there is no direct disagreement to adjudicate.
