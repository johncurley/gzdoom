# Audit contract: GZDoom native POSIX platform layer (Linux/Wayland/X11)

**Revised 2026-08-18.** This is the same contract, updated for what has
changed since the first pass on 2026-08-12 — see the revision note at the end
of §3 and §4 for exactly what moved. If you have a copy of the original
contract cached or memorized, do not trust it; several of its claims about
this codebase are now false, not just outdated in emphasis (raw input's X11
gap in particular — read §4 before assuming it).

You are being asked for an **independent, adversarial second opinion** on the
Linux/BSD platform layer of a hobby Doom source port. This is a
read-and-reason audit: read the code, form hypotheses, and where an existing
tool can check one cheaply (§5.4's table), run it — but do not implement or
commit a fix, and do not modify the build.

**Do not read `AGENTS.md` until after you have written your findings.** It
contains the maintainer's own conclusions, and the entire value of this
exercise is that yours are formed independently. There is a step at the end
that asks you to read it and diff your findings against it.

This is the second contract of its kind (a third pass, counting this
revision). The first covered the Metal renderer, and it exists because a
previous audit returned 18 findings of which **6 were false and the false
ones were concentrated in the highest-severity slots** — both CRITICALs were
wrong, and one suggested fix would have introduced a real bug. Sections 4, 5
and 6 are written to get more of the one genuinely valuable finding that audit
produced and less of the rest. Read them carefully.

A second reason to be careful here specifically: this subsystem is where the
project's *own* history of confident-and-wrong is thickest. Four separate
conclusions about a black GL frame were committed and then retracted over one
session; three separate "the launcher is fixed now" claims changed nothing
observable; and most recently, a full implement-rebuild-retest cycle went into
a plausible, well-reasoned fix for a whole-server X11 deadlock that turned out
to be caused by nothing in this codebase at all — a 30-line bare Xlib program
with no gzdoom code reproduced the identical hang (`AGENTS.md` Tasks — Linux
item 13). The failure mode is always the same — a plausible mechanism,
asserted without a control run, and in item 13's case, without first checking
whether the mechanism was even reachable from application code at all.

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

- **`HAVE_VULKAN` auto-detects, and it is ON by default on this machine
  now** (`CMakeLists.txt:259-285`, changed since the previous version of this
  contract). Non-Apple platforms probe `find_package(Vulkan QUIET)`; if found,
  `HAVE_VULKAN` defaults ON unless overridden. Confirmed here:
  `build/CMakeCache.txt` has `HAVE_VULKAN:UNINITIALIZED=ON`, and
  `libvulkan.so.1.4.357` is present. So an *ordinary* build on this box now
  compiles the Vulkan backend in — do not assume otherwise, and do not assume
  the reverse either without checking `build/CMakeCache.txt` first. One CI job
  (`Linux GCC 12 Vulkan`) now builds it explicitly and is `compile_only`
  (no GPU on the runner) — still no CI job *runs* it.
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
- **Raw keyboard input now exists on both backends — this is a change from
  the previous version of this contract, which called it Wayland-only.**
  X11 gained `XI_RawKeyPress`/`XI_RawKeyRelease` via XInput2 on 2026-08-13
  (`AGENTS.md` Tasks — Linux item 6), verified interactively but not through
  this kind of adversarial read. It is a legitimate audit target in its own
  right: check the `XIAllMasterDevices` subscription (an earlier draft of it
  used `MasterPointerID` and silently dropped every raw keyboard event — the
  bug class to look for is exactly that shape, a plausible-looking device/mask
  argument that compiles and runs but selects the wrong event stream), the
  keycode-minus-8 translation to evdev/`RawKeycode` numbering, and whether
  `LockKeyboard()`/`UnlockKeyboard()` are symmetric with the Wayland
  implementation.
- **Tooling absent on this machine:** no PIL, no ImageMagick, no `xdotool`,
  no `wmctrl`, no `ccache`, no `strace`, no `xtrace`. Any test you propose
  that needs to focus, move or raise a window from a script cannot be run
  here, and neither can protocol-level tracing — a finding that would need
  either is still worth filing, labelled as unverified.
  **`Xephyr` and `xorg-xinit` are now installed** (added 2026-08-18, see item
  13), so a nested or VT-attached real X server is reachable if a finding
  needs one — but see item 13 before trusting either as a clean test
  environment: both wedge under `twm` for reasons unrelated to this
  codebase, confirmed with a bare Xlib client.

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
- **`in_keytrace` and `ZWIDGET_TRACE_REPEAT` are gone, not merely
  deprecated** — removed 2026-08-13 once the X11 raw-input trace they existed
  to support passed (`AGENTS.md` item 7). The previous version of this
  contract listed `in_keytrace 1` as an available diagnostic; it is not
  anymore. Do not file its absence as a finding, and do not expect it to work
  if you try it.
- **A whole-server X11 deadlock on this machine is confirmed unrelated to
  this codebase — do not re-derive it as a finding.** Mapping a plain window
  under `twm` and then making almost any request that needs a server reply
  can hang the entire X connection, for every client on the display, not just
  the one that mapped the window. Reproduced with a bare ~30-line Xlib
  program containing no gzdoom or ZWidget code, run as the first client
  against a freshly started server (`AGENTS.md` item 13, 2026-08-18). If your
  own testing hits an X11 hang under a from-scratch `twm` session, check
  item 13 before writing it up as a defect in this tree — it very likely
  is not one. This does **not** mean every X11 hang report on this branch is
  automatically the same thing; it means the null hypothesis for a *fresh*
  one is "the host, not this code" and needs the same falsification (a bare
  Xlib client, as the first thing to touch a clean server) before being
  attributed here.

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
   | `python3 tools/matrix/run.py [--only C] [--scene doom2\|doom1]` | golden-image regression over 11 postprocess configs, keyed per platform (`platforms.linux`/`platforms.darwin` in `baseline.json` — do not compare against a `darwin` run), own config file, pinned window, warmup launch |
   | `python3 tools/matrix/crossbackend.py --backends gl,vulkan` | **two independent implementations compared against each other** — needs no baseline file and no determinism across time, and `HAVE_VULKAN` is ON in this machine's default build (see §3) so both backends are actually present. Run `--selfcheck` first and believe it. Has its own `DEGENERATE_MEAN` guard against a relation resting on a blank capture — don't propose re-adding one. |
   | `ZWIDGET_DISPLAY_BACKEND=...` | select the ZWidget backend on one binary; default probe order is Wayland → X11 → SDL2 |
   | `vid_stalltrace <ms>` / `vid_frametrace` | backend-agnostic instruments added since the previous version of this contract (`v_video.h`'s `VLoopPhase`/`VLoopContext`, `V_LoopTraceBoundary()` in `D_DoomLoop`) — a slow game-loop iteration broken into named phases, and the true frame interval, both to stderr, both silent unless armed. Not a substitute for `matrix.py`'s pixel-level checks, but the right instrument for a hitching/stall claim instead of a rendering-correctness one. |
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
(The previous version of this contract's two questions — the Wayland
first-paint fix and the X11 `BadMatch` — are both resolved: the first does
not reproduce under a real compositor, the second was fixed and published
upstream as `dd88a86b7`. Do not re-open either without new evidence.)

**Q1 — The new X11 raw-input path (XInput2) has not had an adversarial read.**

Landed 2026-08-13 (`x11_display_window.cpp`, `XI_RawKeyPress`/
`XI_RawKeyRelease`), verified only by interactive play, and it already had one
mistake caught informally rather than by review: an early draft subscribed
`MasterPointerID` instead of `XIAllMasterDevices` and silently dropped every
raw keyboard event — no crash, no error, just nothing arriving. That is the
bug shape worth hunting for here: a device/mask/scope argument that compiles,
runs, and produces *plausible* output while actually being wrong.

1. Check the actual `XISelectEvents` call and mask construction against what
   Wayland's raw-input path does for the same feature — do they select the
   same event scope, or could X11 be over- or under-selecting relative to it?
2. Check the keycode translation (X11 keycode minus 8, to land on evdev/
   `RawKeycode` numbering) at every call site it's used, not just the one that
   was tested interactively — is there a second path (a different event type,
   a different device class) that needs the same offset and doesn't have it?
3. Check `LockKeyboard()`/`UnlockKeyboard()` symmetry against the Wayland
   implementation: same enable/disable moments, same interaction with the
   raw/cooked switch-over path in `nativevideo.cpp` (§4's `ResetButtonStates`
   note applies here too — a keyboard lock that doesn't reset button state on
   a path switch reproduces the exact stuck-key shape that trap exists for).
4. Is this the fork's code or does it touch ZWidget subtree files needing
   the cherry-pick-and-publish procedure? Say which files.

**Q2 — Is the `WM_TAKE_FOCUS`-while-unviewable guard actually sufficient?**

This is a read-only question — the live harness for testing it (`twm` on this
machine) is confirmed broken for reasons outside this codebase (item 13), so
there is no test to run; reason from ICCCM and the code alone, and say
explicitly that you could not verify it dynamically.

The fix (`89d79bcbe`, published as `dd88a86b7`) makes both
`X11DisplayWindow::Activate()`'s fallback branch and the `WM_TAKE_FOCUS`
handler in `OnClientMessage` check `IsWindowViewable()`
(`XGetWindowAttributes` round trip) before calling `XSetInputFocus`, and defers
an `Activate()` call entirely via `pendingActivate` when `!isMapped`,
discharged by `Show()`.

1. Between `IsWindowViewable()` returning true and the subsequent
   `XSetInputFocus` call, is there a window where the server could have
   unmapped the window in between — and if so, does the guard's single
   round-trip check actually close the race, or only narrow it?
2. `pendingActivate` is discharged unconditionally in `Show()`, regardless of
   what caused `Show()` to run. Is there a sequence — hide/show cycling,
   multiple `Activate()` calls before a single `Show()` — where a stale
   pending activation fires for the wrong intent, or where two overlapping
   `Activate()` calls before mapping collapse into behavior different from
   what either caller expected?
3. ICCCM's own opinion on `WM_TAKE_FOCUS` includes what timestamp the client
   should use and when a client should prefer `WM_TAKE_FOCUS` over a bare
   `XSetInputFocus`/`_NET_ACTIVE_WINDOW`. Does this implementation's choice of
   which mechanism to use, and when, actually match the spec — independent of
   the viewability question?

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
