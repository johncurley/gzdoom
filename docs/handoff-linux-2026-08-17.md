# Handoff to the Linux box — 2026-08-17

Written at the end of a macOS session. `origin/metal-audit` is current; nothing
is sitting on a local branch. Start with `git pull`.

## Do this first: verify the Linux matrix baseline

`tools/matrix/baseline.json` changed format this session. It is now **keyed by
platform** (`platforms.linux`, `platforms.darwin`) rather than holding one shared
set of signatures.

Why it changed: the old flat file held the Linux baseline, and a macOS run
compared against it reported every config changed with every relation passing —
which reads as a catastrophic regression and was nothing of the kind. `AGENTS.md`
had warned in prose that a baseline must not be shared between machines. Prose is
not a mechanism.

The existing Linux data was migrated under `platforms.linux` **on its documented
provenance, not on a verified run**. So the first thing to do here is confirm it:

```bash
python3 tools/matrix/run.py --scene doom2
```

- **PASS** — the migration was correct, carry on.
- **"no baseline for platform 'linux' yet"** — the migration did not take; record
  one with `--update-baseline`.
- **FAIL with every config changed and relations passing** — that is the
  cross-machine signature again, meaning the migrated data is not actually from
  this machine. Re-record.
- **FAIL with one config changed** — re-run twice before investigating. This
  suite has a known wandering-victim nondeterminism; a single config moving is a
  re-run, a *global* shift with relations intact is a finding.

## Highest-value item here: the fatal X11 `BadMatch`

Item 5 in **Tasks — Linux**. `BadMatch` (opcode 42, `X_SetInputFocus`) on every
X11 backend start, and it is **fatal** — Xlib's default error handler prints and
then calls `exit(1)`. Traced to two candidate sites by two independent readings,
one of them an outside audit. Unfixed.

This is a user-facing "the X11 backend does not launch" bug and it outranks
everything else on either platform's list.

## Also open on Linux

- **Item 6: X11 raw input via XInput2.** Wayland is done; X11 has no equivalent.
  Respect the `I_StartTic()` / `ResetButtonTriggers()` trap in `CLAUDE.md`.
- **Item 3: `crossbackend.py` drops a config's own map.** `crossbackend.py:158`
  calls `matrix.launch()` with no `scene`, so `effective_map()` resolves a
  scene-keyed map dict against `"default"` and gets nothing — the three bloom
  configs silently run on the scene's map instead of the one chosen for them.
  Worth fixing early: this is the oracle you would lean on to validate other
  work, and it has been partly lying.
- **Item 4: `run.py` has no degenerate-frame guard.** `crossbackend.py` got
  `DEGENERATE_MEAN` in 2026-08-10; `run.py` never did.
- **Item 7: remove the input diagnostics** (`in_keytrace`,
  `ZWIDGET_TRACE_REPEAT`) once input is settled.
- **Items 10/11: publish the two ZWidget fixes upstream** to dpjudas. Fixed in
  `c3474d697`, never published. Use the cherry-pick route in `CLAUDE.md` —
  `git subtree push` does not work for this repository.

## What changed on macOS this session, that Linux inherits

Backend-agnostic, so all of it is live on Linux too:

- **`vid_stalltrace <ms>`** — splits a slow game-loop iteration into named phases
  (playsim, display, renderview, levelload, precache, savegame, …) and reports at
  the `D_DoomLoop` boundary. Works on GL and Vulkan unchanged.
- **`vid_frametrace`** is the true frame interval; the Metal-only `mt_frametrace`
  is the renderer bracket only. The names are nearly identical and the reports
  look alike — read the header line, not the name. Misreading one for the other
  cost a session.
- **`V_LoopTraceBoundary()`** in `D_DoomLoop`, and `VLoopPhase`/`VLoopContext`
  in `v_video.h`. If you add a phase on the Linux side, put it in the same
  system.
- **`AppActive` is now polled from `[NSApp isActive]` on macOS** because its
  notifications never fire there. The Linux/SDL and native paths set it from
  real focus events and are unaffected — but note `d_main.cpp:926`,
  `d_net.cpp:2197` and `m_haptics.cpp:454` all read it, so if focus tracking is
  wrong on a Wayland/X11 backend the same three behaviours break: background
  rendering, `vid_lowerinbackground`, and haptics.

## Landed after this document was first written

Two macOS-only changes that a Linux checkout will nevertheless notice:

- **`wadsrc/static/shaders/metal/generated/` is ~2.3MB of pre-translated MSL,
  92 files.** Shipped in `gzdoom.pk3`, and compiled into
  `native_shaders.metallib` at build time so a macOS cold start does no shader
  work at all. Inert on Linux — the whole block is inside
  `if (HAVE_METAL AND APPLE)` — but it is why the pk3 grew and why `wadsrc`
  gained a directory that has nothing to do with GL or Vulkan.
- **The CMake glob for those files is configure-time.** If anyone regenerates
  the MSL set, `cmake` must be re-run or the metallib keeps the old stages. Only
  matters on Apple.

Nothing here changes the GL or Vulkan paths, and no Linux task depends on it.

## Traps this session added to `AGENTS.md`, worth reading before measuring

- An **unfocused window** is throttled to a locked ~268ms cadence, and the
  **start screen** has its own `minwaittime` throttle that doubles each time it
  trips. Both produce intervals that look like catastrophic stalls and are not.
  Check the `active=` and `context=` stamps.
- The **screen wipe** takes ~1.1s by design when `cl_capfps` is set
  (`wipe.cpp`'s `I_WaitVBL(2)` branch, 25ms floor per tic). Not a stall.
- **Quit through the game**, not `kill`, when measuring anything cached at
  shutdown. The Metal PSO archive is written in a destructor; a killed run never
  persists it, and the next "warm" run is really a second cold one. That artifact
  sent an entire investigation down a false path.
