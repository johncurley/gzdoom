# Audit contract: GZDoom native POSIX platform layer (Linux/Wayland/X11)

You are being asked for an **independent, adversarial second opinion** on the
Linux/BSD platform layer of a hobby Doom source port. This is a
read-and-reason audit, not a build-and-test one.

**Do not read `AGENTS.md` until after you have written your findings.** It
contains the maintainer's own conclusions, and the entire value of this
exercise is that yours are formed independently. There is a step at the end
that asks you to read it and diff your findings against it.

This is the second contract of its kind. The first covered the Metal renderer,
and it exists because a previous audit returned 18 findings of which **6 were
false and the false ones were concentrated in the highest-severity slots** —
both CRITICALs were wrong, and one suggested fix would have introduced a real
bug. Sections 4, 5 and 6 are written to get more of the one genuinely valuable
finding that audit produced and less of the rest. Read them carefully.

A second reason to be careful here specifically: this subsystem is where the
project's *own* history of confident-and-wrong is thickest. Four separate
conclusions about a black GL frame were committed and then retracted over one
session; three separate "the launcher is fixed now" claims changed nothing
observable. The failure mode is always the same — a plausible mechanism,
asserted without a control run.

---

## 1. What this project is

GZDoom is a Doom-engine source port (C++17, GPLv3). This fork replaces SDL2
with a **native POSIX windowing and input backend** on Linux and BSD
(`GZDOOM_NATIVE_LINUX`, default **ON**): ZWidget for windowing, talking Wayland
and X11 directly, with the SDL path surviving only under
`-DGZDOOM_NATIVE_LINUX=OFF` as a control build.

Three code layers are involved and they are owned differently. **Which layer a
defect lives in is part of the finding**, because it decides how the fix is
published:

| Layer | Path | Ownership |
|---|---|---|
| Engine-side platform glue | `src/common/platform/posix/native/` | this fork, ordinary commits |
| Windowing/input backend | `libraries/ZWidget/` | **git subtree** of a fork of `dpjudas/ZWidget`; fixes here should go upstream |
| Inherited engine common code | `src/common/console/`, `src/common/rendering/`, `src/d_main.cpp` | upstream GZDoom, shared with every platform |

The same tree also carries a native Metal renderer for macOS. That is a
different audit (`docs/audits/audit-contract.md`) and is out of scope here.

---

## 2. Scope — read this narrowly

**In scope, in priority order:**

1. `src/common/platform/posix/native/` — `nativevideo.cpp` (~780 lines:
   framebuffer, ZWidget window host, key/mouse routing), `i_input.cpp`
   (~280: event pump, capture policy, raw/cooked selection), `gl_sysfb.cpp`
   (~490: GLX and EGL context creation, both `dlopen`'d), `i_system.cpp`,
   `i_main.cpp`, `x11_compat.h`.
2. `libraries/ZWidget/src/window/wayland/` — `wayland_display_window.cpp`
   (~550) and `wayland_display_backend.cpp` (~1100). Protocol handshake,
   buffer lifetime, seat/keyboard/pointer handling, key repeat, clipboard.
   The generated `*-protocol.c` / `*-client-protocol.h` files are
   wayland-scanner output; do not audit them as hand-written code, but see
   §3 for the trap they carry.
3. `libraries/ZWidget/src/window/x11/` — `x11_display_window.cpp` (~1330),
   `x11_connection.cpp`, `x11_dynamic.h`, `x11_remap.h`.
4. `libraries/ZWidget/src/window/window.cpp` — backend selection and probe
   order.
5. Theme detection where it is platform-specific:
   `libraries/ZWidget/src/core/theme.cpp` (`POSIXNativeTheme` /
   `POSIXNativeThemeImpl` only, roughly lines 415–600) and
   `src/common/widgets/widgetresourcedata.cpp`.
6. `CMakeLists.txt` and `src/CMakeLists.txt` **only** for how this backend and
   its dependencies are configured, linked and gated.

**Out of scope** — do not spend report space here: the renderer backends
themselves (OpenGL, Vulkan, Metal internals — GL/EGL/GLX *context creation and
surface handoff* is in scope, everything downstream of a current context is
not), gameplay and playsim code, the ZScript compiler and VM, the filesystem
layer, the Cocoa and Win32 platform layers except as a *reference to compare
against*, and general C++ style.

Audit the **working tree as it currently stands** (branch `metal-audit`), not
the last tagged release.

---

## 3. Environment profile — this is a hard constraint, not context

The Linux machine is the only one that can run any of this, and its
configuration decides which claims are even checkable. It is:

```
Distro:      Arch / CachyOS            Compositor:  KDE Plasma on Wayland (kwin_wayland)
GPU:         AMD RX 550 (polaris12)    Driver:      Mesa 26.1.6 (radeonsi)
OpenGL:      4.6                       Vulkan:      1.4
```

X11 is reachable, but on this box it is normally **XWayland under kwin**, not a
bare X server with a traditional window manager. A finding that depends on
specific window-manager behaviour may be unverifiable here; say so rather than
assuming.

Consequences you must respect:

- **`HAVE_VULKAN` defaults OFF** (`CMakeLists.txt:259`). An ordinary build has
  OpenGL only. Vulkan exists on this hardware and is a real cross-check oracle,
  but only in a build configured for it. Do not assume a Vulkan code path is
  exercised by any default build, and note that **no CI job configures it
  either**.
- **Wayland, xkbcommon, X11, Xi, libGL and libEGL are `dlopen`'d, never linked.**
  ZWidget's CMake takes their *include* directories from `pkg-config` and links
  nothing (`libraries/ZWidget/CMakeLists.txt:15-31`); `gl_sysfb.cpp` resolves
  `libGL.so.1` and `libEGL.so.1` by hand. This is deliberate: one binary must
  run under Wayland, under X11, or on a machine with neither. **Do not propose
  adding a link-time dependency on any of them**, and do not report a
  `dlsym`-heavy loader as duplication.
- **The tree vendors wayland-scanner output but compiles against the *system*
  `libwayland` headers**, so the two can disagree at a version boundary. This
  has bitten twice: `WL_KEYBOARD_KEY_STATE_REPEATED` (wl_keyboard v10, absent
  on Ubuntu 22.04) and `wl_pointer_listener` gaining members — where appending
  handlers unconditionally overflows the listener struct. The established
  pattern is to guard version-dependent entries with the `*_SINCE_VERSION`
  macros the *system* header defines (see
  `wayland_display_backend.cpp:119-145`). **A missing guard on a newer protocol
  entry is a legitimate finding class and worth actively hunting.** "This
  `#ifdef` is unnecessary" is not.
- **X11 macro pollution is load-bearing.** `x11_compat.h` renames `GC` to
  `XGC` across the Xlib include; `x11_remap.h` `#undef`s `None`, `Success`,
  `Always`, `GrayScale` and then redefines ~50 Xlib entry points as macros
  pointing at the dynamic loader. That is why X11 code compares against `0L`
  rather than `None` and passes `2` rather than `Always`. **These are not
  magic numbers.** Do not report them as such, and if you propose touching
  include order, show that the replacement survives both `x11_remap.h` and
  ZWidget's own headers.
- **ZWidget's SDL backends are force-disabled at the root.**
  `ENABLE_SDL2` / `ENABLE_SDL3` default ON for non-Windows and are set
  `OFF CACHE INTERNAL FORCE` immediately before
  `add_subdirectory(libraries/ZWidget)` (`CMakeLists.txt:389-396`). Setting
  them from `src/CMakeLists.txt` would be too late — ZWidget is configured at
  root, `src` afterwards. `ZWIDGET_NO_SDL` is dead and does nothing.
- **The OpenGL ES backend was removed entirely** (commit `449c4fad1`,
  2026-08-12): sources, shaders, build option, menu entries, launcher radio
  button. Backend enum value **2 is deliberately left vacant** rather than
  reused, so an existing `gzdoom.ini` asking for Metal (3) still means Metal
  (`src/common/rendering/v_video.cpp:139-168`). Do not report the gap as a
  bug or propose renumbering.
- **Raw keyboard input exists on Wayland only.** X11 has no equivalent path;
  `X11DisplayWindow::LockKeyboard()` is an empty stub with a comment saying so.
  That is a known feature gap, not an oversight — see §4.
- **Tooling absent on this machine:** no PIL, no ImageMagick, no `xdotool`,
  no `wmctrl`, no `ccache`. Any test you propose that needs to focus, move or
  raise a window from a script cannot be run here.

---

## 4. Domain knowledge — intentional decisions, not bugs

Every item below was checked against the code while this contract was written.
Don't rediscover them. Do still scrutinize whether each is *applied correctly*
at every site — several of these are correct in principle and have more sites
than obvious ones.

- **Raw and cooked input are two paths, selected per context, and both are
  reported by the backend.** Cooked is keysym → `InputKey` → gzkey and is used
  for GUI (menus, console) because only it produces text. Raw (`in_rawkeyboard`,
  default off) passes the evdev scancode through unchanged — `RawKeycode`,
  `DIK_*` and evdev are the same PC scancode set, so **there is no mapping
  table and its absence is not a bug**. Raw drives gameplay only, when
  `in_rawkeyboard && !GUICapture`; while it is active,
  `nativevideo.cpp:535,570` suppress the *cooked* gameplay events so the two do
  not double-post, and `OnWindowRawKey` early-returns when it is not
  (`nativevideo.cpp:582-598`). Switching paths under a held key calls
  `buttonMap.ResetButtonStates()` (`i_input.cpp:163`) on purpose: without it the
  press and the release land on different paths and the key stays down forever.
- **`I_StartTic()` calls `buttonMap.ResetButtonTriggers()` first, and must.**
  `bWentDown`/`bWentUp` are set by `PressKey`/`ReleaseKey` and cleared nowhere
  else, and `G_BuildTiccmd` reads `ButtonPressed()` — which *is* `bWentDown` —
  for jump and attack. Without the reset a single tap latches the button on for
  every later tic. **The SDL path still omits it** (`posix/sdl/i_input.cpp:608`,
  upstream included); that asymmetry is real and known, and reporting the
  native side's call as redundant would be exactly backwards. Note the shape of
  the original symptom, because it generalises: it looked identical to a stuck
  key while every input trace showed balanced press/release pairs, because the
  latched flag was `bWentDown`, not `bDown`.
- **Key routing is latched at press time and repeats are dropped on a route
  change.** `keyRoutes` in `nativevideo.cpp:495-520` records GUI-vs-Game per
  key at press; a timer-generated repeat whose latched route no longer matches
  the current capture state is **discarded**, while the release still follows
  the latched route. That asymmetry is what keeps press/release pairs
  symmetric across a capture flip (Enter closing a menu is the case it was
  written for). It is not an inconsistency.
- **`I_GetEvent()` refuses to re-enter `DisplayWindow::ProcessEvents()`.**
  Callbacks can call back into `I_GetEvent` while a pump is still on the stack;
  nested requests set `deferred_event_pump` and the *outer* pump loops, capped
  at `kMaxDeferredPumps = 1024` (`i_input.cpp:18-58`). The cap is a deliberate
  bound, not a magic number, and the flattening is not dead code.
- **`ui_theme 0` derives most of the launcher palette from two colours.**
  `SimpleTheme::ThemeColors` has twelve entries; a desktop reports only a window
  background and foreground. `POSIXNativeThemeImpl::DetectColors()`
  (`theme.cpp:434-556`) reads KDE `kdeglobals`, then GTK `settings.ini`, then
  `.Xresources`/`.Xdefaults`, decides light-vs-dark from a Rec.709 luminance of
  the detected background, and *derives* the header/input, action, border and
  divider shades from it by a signed `Shade()` mix, copies the foreground to two
  more slots, and forces hover/active text to white. The hover and active
  *backgrounds* stay at their hard-coded KDE-blue accents by design. This is a
  heuristic and is known to be one — `ui_theme 3` exists precisely as the escape
  hatch when the inference looks wrong on a given desktop. "Only two colours are
  actually detected" is a description of the design, not a finding.
- **`InitWidgetResources()` runs once, early, so `ui_theme` needs a restart.**
  It is called from `D_DoomMain_Internal` (`d_main.cpp:3765`), which is *before*
  `D_InitGame` executes command-line `+` parameters (`d_main.cpp:3315`). The ini
  value applies because `M_LoadDefaults()` ran earlier still. ZWidget also only
  themes the launcher, error window and net-start window — in-game menus use
  GZDoom's own renderer and are unaffected.
- **`CUSTOM_CVAR` callbacks cannot be relied on for command-line `+set`.**
  `FBaseCVar::ForceSet` runs the callback only `if (m_UseCallback)`, and
  `m_UseCallback` is false between `FBaseCVar::DisableCallbacks()`
  (`d_main.cpp:3209`) and `FBaseCVar::EnableCallbacks()` (`d_main.cpp:3356`) —
  the window that contains `exec->ExecCommands()` at `d_main.cpp:3315`.
  `EnableCallbacks()` then fires each cvar's callback once, **except** for any
  carrying `CVAR_NOINITCALL` (`c_cvars.cpp:607-620`), whose callback never runs
  at all on that path. So a clamp written into a `CUSTOM_CVAR` body is not a
  guarantee: selection logic downstream needs its own range check. `V_GetBackend()`
  is the worked example of doing this correctly. This is inherited upstream
  behaviour; a finding here is only interesting if it names a *site* that
  trusts the clamp.
- **`libraries/ZWidget` is a git subtree and must stay one.** It tracks
  `johncurley/ZWidget`, branch `wayland-c-bindings`. Pulling down works
  normally; **`git subtree push` does not work for this repository and must not
  be used** — `subtree split` synthesises ZWidget history out of gzdoom's entire
  log and produced 22,906 commits of which 2 touched the published files, which
  `git merge-base --is-ancestor` then reports as a clean fast-forward. Changes
  are published by cherry-picking onto a branch off the fork instead; see
  `CONTRIBUTING.md`. Before it was a subtree it was a plain vendored copy, and
  three lineages drifted until **eight API families** had silently diverged.
  Practical consequence for you: **do not propose "just edit ZWidget"** as a fix
  shape without saying it needs publishing, and treat any hand-edit that has not
  been pushed back as a maintainability finding in its own right.
- **Fork-side ZWidget work that is deliberate, not accidental:** generated
  Wayland protocol bindings replacing waylandpp, the runtime `dlopen` loaders,
  `POSIXNativeTheme`, raw keyboard dispatch, and fixes for stuck keys
  (`wl_keyboard.enter`'s held-key array is now walked by hand, because
  `wl_array_for_each` relies on an implicit `void*` conversion that is invalid
  in C++ — `wayland_display_backend.cpp:623-631`), a window-destroy
  use-after-free (`OnWindowDestroyed` clears the cached focus/hover pointers and
  the held-scancode set, because the key *release* that follows the press which
  closed a window still arrives — `wayland_display_backend.cpp:437-455`),
  missing punctuation mappings, and `GetKeyState` on mouse buttons.
- **`in_keytrace` is a temporary diagnostic** and logs to **stderr**, not
  `Printf`, deliberately: once video is up `Printf` goes to the in-game console
  and cannot be captured from a piped run. It is scheduled for removal. Filing
  "debug output left in" is accurate but already known; filing it as anything
  above `info` is severity inflation.

---

## 5. Standard of evidence

Every finding must clear these bars. A finding that cannot is either
downgraded to an explicitly-labelled *observation* or dropped.

1. **Concrete failure scenario.** Name the input, protocol sequence or state,
   and the wrong output, hang, protocol error or crash it produces. "This looks
   fragile" is not a finding. For a compositor- or WM-dependent claim, name a
   *behaviour* a conformant implementation is allowed to have, not a product.
2. **Direction must be checked, separately from mechanism.** The mechanism
   being real does not make the predicted symptom real. For this subsystem,
   state which way the error goes: is an event **lost or duplicated**; is a key
   stuck **down or up**; does a window come up **blank or stale**; is the
   protocol error on the **client or the compositor** side. The canonical miss
   here was a latched-input bug whose symptom read as a stuck key while every
   trace showed balanced press/release pairs — the mechanism and the symptom
   were in different variables. If your finding predicts an observable symptom,
   say which one and why.
3. **Site must be checked, separately from mechanism.** There are three layers
   (§1) and a mechanism can be right with the site wrong. Say explicitly which
   layer you believe owns it. **Mis-sited-but-real is a legitimate finding
   class** — if you are confident in a mechanism but unsure whether it lives in
   the fork's glue, the ZWidget subtree or inherited upstream code, say exactly
   that; it is still useful.
4. **Falsification test — the single cheapest check that would prove you
   *wrong*.** It has to be runnable on the machine in §3. What exists:

   | Tool | What it gives you |
   |---|---|
   | `python3 tools/matrix/run.py [--only C] [--scene doom2\|doom1]` | golden-image regression over 11 postprocess configs, own config file, pinned window, warmup launch |
   | `python3 tools/matrix/crossbackend.py --backends gl,vulkan` | **two independent implementations compared against each other** — needs no baseline file and no determinism across time. Run `--selfcheck` first and believe it. |
   | `in_keytrace 1` | every key event to **stderr** as `down`/`up`/`rep`/`rawdn`/`rawup`/`mdn`/`mup`/`wheel`, plus the held-button set once per tic |
   | `ZWIDGET_DISPLAY_BACKEND=...` | select the ZWidget backend on one binary; default probe order is Wayland → X11 → SDL2 |
   | `MESA_DEBUG=1` | names the failing GL call on stderr, free. It is what cracked the GL black-frame bug (17,423 `GL_INVALID_OPERATION in glUniform(program not linked)` per run). **`gl_debug_level` produces nothing here** — the context is not a debug context; do not propose it. |
   | `MESA_GL_VERSION_OVERRIDE` / `MESA_GLSL_VERSION_OVERRIDE` | force an older GL/GLSL profile to exercise version-gated paths |
   | `spectacle -b -n -f -o out.png` | screenshot a Wayland-native window. X11 root grabs and ffmpeg `x11grab` only see XWayland and come back **black**. |
   | `-DGZDOOM_NATIVE_LINUX=OFF` | build the SDL path as a control |

   `crossbackend.py` is the strongest oracle available and the reason this
   machine matters: it is the only one with two non-Metal backends that both
   run. If your finding can be expressed as "GL and Vulkan should agree here and
   won't", say so.
5. **A test that has never been run in the failing configuration proves
   nothing.** Before claiming a defect, say what the *control* is — the run
   that must fail without the bug present. This is the house standard
   (`CONTRIBUTING.md`) and it is not decorative: at an ~8% flake rate, single
   samples produced two confident, committed, wrong findings in one session,
   one of which fingered a config key that **is not a cvar in the source at
   all**.
6. **Don't guess at runtime behavior you cannot observe.** You cannot run this.
   Say "I cannot verify this without running it" rather than asserting. For
   anything compositor-dependent, that is the *expected* answer, and pairing it
   with a precise protocol-level argument is worth more than a confident guess.
7. **No severity inflation.** CRITICAL means crash, hang, corruption, input
   that becomes unusable, or a window that never paints, in a default
   configuration on a mainstream compositor. The previous audit spent both of
   its CRITICAL slots on false positives, which is worse for the maintainer
   than having filed nothing.

An empty or short report is an acceptable outcome. Do not manufacture findings
to fill space. Three well-evidenced findings beat eighteen.

---

## 6. Two specific questions

Answer these explicitly, in addition to whatever you find on your own. Both are
genuinely open — neither has an answer waiting in `AGENTS.md` for you to match.

**Q1 — The Wayland first-paint fix has no control run.**

Symptom as reported: the launcher comes up blank and stays blank until an
unrelated event (moving the pointer over it) happens. The fix sets
`window->m_NeedsUpdate = true` unconditionally in
`xdg_surface_handle_configure`, immediately after `xdg_surface_ack_configure`
(`libraries/ZWidget/src/window/wayland/wayland_display_window.cpp:22-43`). The
stated reasoning is that `xdg_toplevel.configure` only requests a paint when it
is given a non-zero size (`OnXDGToplevelConfigureEvent`, same file, 384-394),
that a compositor's **first** configure is normally 0x0 — that being how it
tells the client to choose its own size — and that `m_NeedsUpdate` starts true
but the run loop's `CheckNeedsUpdate()` can consume it before the handshake
completes.

**This rests entirely on a reading of the xdg-shell protocol. It has no
measurement behind it, and it could not be reproduced on the machine that wrote
it** — the launcher painted on every attempt, before and after the change.

Derive from the protocol and the code:

1. **Is the fix correct** — does setting the flag at the ack point actually
   guarantee a buffer is attached and committed after every configure? Trace it
   through: `CheckNeedsUpdate()` (`wayland_display_backend.cpp:370-380`) →
   `windowHost->OnWindowPaint()` → `Widget::Repaint()`
   (`libraries/ZWidget/src/core/widget.cpp:435-445`) →
   `WaylandDisplayWindow::PresentBitmap()` (549-line file, ~349-370). Note
   `Repaint()` returns early if there is no `DispCanvas`, `PresentBitmap()`
   does nothing if `CreateBuffers()` declined, and `CreateBuffers()` returns
   early on a non-positive size and **returns before creating any SHM buffer at
   all** when `m_renderAPI` is `OpenGL` or `Vulkan`.
2. **Is it sufficient?** Name what a compositor would have to do for the
   launcher to still come up blank with this fix in place. Candidates worth
   ruling in or out explicitly: a first configure that arrives before
   `DispCanvas` exists; a window whose `m_LogicalSize` is still 0x0 at first
   paint; the `RenderAPI::OpenGL` path, where `Show()` commits without
   attaching a buffer (`wayland_display_window.cpp:241-260`) and content is
   supposed to arrive via `eglSwapBuffers` instead; and the interaction with
   `m_FrameCallback`, which `DrawSurface()` will not re-arm while one is
   pending.
3. **Is it in the right place?** If a better fix exists — for example driving
   the paint from `Show()` or from the toplevel configure with a size fallback
   — say which, and what evidence would distinguish it from this one.

The maintainer's planned test is to reproduce the blank launcher on the pre-fix
commit (that is the control; without it the fix proves nothing) and then show
it painting after. Tell them what a *failure to reproduce on the pre-fix
commit* would imply about the fix, so that outcome is informative rather than
merely inconclusive. This matters beyond the fork: it is an upstream ZWidget
bug affecting every ZWidget Wayland application, and it is queued for a PR to
dpjudas.

**Q2 — X11 was left untouched, and it prints a protocol error on every start.**

xdg-shell has no X11 equivalent, so the Q1 fix does not cover the X11 backend
and the X11 side was not changed. Two loose ends, neither chased:

- **`BadMatch` (opcode 42, `X_SetInputFocus`) prints on every X11 backend
  start.** Non-fatal — Xlib's default handler prints and continues — but it is
  a real protocol error. There are two call sites:
  `X11DisplayWindow::Activate()` (`x11_display_window.cpp:327`, the fallback
  branch taken when `_NET_ACTIVE_WINDOW` is unavailable) and the ICCCM
  `WM_TAKE_FOCUS` handler (`:737`). The window advertises `WM_TAKE_FOCUS` in
  its `WM_PROTOCOLS` at creation (`:100-105`) and is created unmapped, mapped
  later by `Show()` (`:250-257`). The usual cause of this error is
  `XSetInputFocus` on a window that is not yet viewable — i.e. a map/focus
  race. Identify which site fires, what state makes it fire, and what the
  correct guard is; ICCCM has an opinion about which of these two mechanisms a
  client should use and when, and about the timestamp each should carry.
- **Does X11 need a first-paint fix at all?** `ExposureMask` and
  `StructureNotifyMask` are selected (`:40-43`) and `OnExpose()` calls
  `OnWindowPaint()` directly (`:747-750`). Argue from the protocol whether that
  is a complete substitute for the Wayland configure→ack→commit handshake, or
  whether there is an analogous window — first map, resize, or the
  `RenderAPI::OpenGL` path — where the X11 backend can also present nothing.

A related, unexplained observation you may or may not be able to account for:
**the launcher exits on its own under bare Xvfb, between 12s and 25s, with
nothing in the log.** Under XWayland with a window manager it stayed up, so
this may be a no-WM artifact rather than a bug; it is not confirmed either way.
CI would not catch it — the smoke test passes `-iwad`, so it never opens the
launcher. If your reading of the X11 event loop suggests a mechanism, say so
and name the cheapest test; if it does not, say that instead.

---

## 7. Reporting format

Write findings to `FINDINGS.md` in the repo root. Rank most-severe-first.
Per finding:

- **File and line range**
- **Category** — exactly one of:
  - `correctness` — wrong behavior, crash, hang, use-after-free, lost or
    duplicated input, stuck button state, resource leak with a named owner
  - `protocol` — a violation of the Wayland/xdg-shell, ICCCM, EWMH, XInput2 or
    EGL/GLX contract that a conformant implementation is permitted to punish,
    even where KWin currently tolerates it
  - `portability` — correct here, dependent on something this machine happens
    to have: a compositor, a window manager, a Mesa version, a distro's
    `libwayland` version, a locale. The vendored-bindings-vs-system-headers
    class in §3 lives here.
  - `maintainability` — dead code, a hand-edit to the ZWidget subtree that has
    not been published upstream, an undocumented workaround, diagnostics left
    in, a build dependency that is declared but unused
- **Ownership** — **required for every finding**, one of `fork` /
  `zwidget-subtree` / `upstream-inherited`, per the table in §1. This is not
  bookkeeping: it decides whether the fix is an ordinary commit, a
  cherry-pick-and-publish through the subtree procedure in §4, or a patch with
  nowhere to send it (upstream GZDoom is effectively frozen). If you cannot
  tell, say so — that is a legitimate answer and more useful than a guess.
- **Backends affected** — `wayland` / `x11` / `both` / `neither` (build- or
  config-level). Several real defects here exist on exactly one of the two, and
  a finding that does not say which invites a wasted test on the wrong one.
- **Severity**: critical / high / medium / low / info, per §5.7
- **Confidence**: high / medium / low — low-confidence entries are welcome as
  long as they are labelled
- **Failure scenario**: input/protocol sequence/state → wrong behavior, with
  direction of error per §5.2
- **Falsification test** per §5.4, naming the control run per §5.5
- **Fix direction**: the shape of the fix, not necessarily a patch

Then a short section listing anything you *looked at hard and concluded was
fine* — the negative space is genuinely useful and cheap for you to produce.

---

## 8. Only after you've written your findings

Read `AGENTS.md` — the "Tasks — Linux" section near the top and the "Traps that
have cost real sessions" section at the bottom are what matter — and
`docs/handoff-linux.md`, whose two validation tasks are **done** and must not be
re-proposed. Mark each of your findings **new** vs. **already documented**.

That comparison is the actual deliverable. This is a second opinion, not a race
to find the most bugs. If `AGENTS.md` contradicts one of your findings, don't
automatically defer to it: say which of you you think is right and why. It has
been wrong before, and several of its entries are explicitly recorded as
unverified readings rather than measurements.
