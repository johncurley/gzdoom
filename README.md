# GZDoom — native Metal / native POSIX fork

A fork of [GZDoom](https://zdoom.org/) with two changes upstream does not have:

- **A native Metal renderer for macOS.** A direct Metal 2 backend, not MoltenVK
  — no Vulkan translation layer in the way. Written to mirror the Vulkan
  backend's structure so the two can be read side by side.
- **A native POSIX platform layer for Linux/BSD.** Wayland/X11 and libinput
  through ZWidget instead of SDL2, with the display libraries `dlopen`'d so one
  binary runs under either, or neither.

Everything else is GZDoom: same ZScript VM, same playsim, same PK3 assets, same
mods. If a mod runs on GZDoom it should run here, and upstream's
[wiki](https://zdoom.org/wiki/) still applies for engine-level questions.

---

## Status

**macOS / Metal** — playable and in daily use on the development machine. The
renderer is checked against the OpenGL backend frame-by-frame; scene normals,
fog, model normals and the palette tonemap all match the reference within a
pixel value or two. One known residual: SSAO differs from OpenGL by ~0.4/255 in
its contribution to the final frame, bounded and documented in `AGENTS.md`.

**Linux** — native Wayland and X11 backends with desktop theme detection. The
X11 raw-keyboard path has been interactively validated on the Linux test
machine with balanced press/release events and no stuck actions. That machine
has also passed the GL/Vulkan cross-backend suite; its selected MAP06 scene is
not launch-to-launch deterministic, so no Linux golden baseline has been
recorded.

**BSD** — the native POSIX design is intended to cover BSD as well, but this
fork does not currently claim BSD runtime verification.

**Windows** — unchanged from upstream and covered by CI, but not yet run by the
maintainer. Reports welcome.

**Apple Silicon — untested.** Development is on an Intel Mac (HD 6000, Metal
2.0). Nothing here has ever run on an M-series part, and the compute AO and
bloom paths, which are gated off on Intel, would be **on** by default there.
This is the single most useful thing an outside tester could change; see
"Helping out".

**OpenGL / Vulkan** — OpenGL remains the broadly compatible reference backend;
the Linux Vulkan path is enabled and has passed the current 11-configuration
cross-backend suite against OpenGL. The exact OpenGL feature/profile level is
driver-dependent, so claims about older compatibility profiles or newer core
features still need to be tied to a measured machine.

The Metal renderer currently translates most known engine shaders at runtime
and keeps native MSL for Metal-specific compute work. A future build-time
`metallib` for the stock shader permutations should reduce startup and first
frame compilation cost, while retaining runtime translation for custom and
mod-provided shaders.

---

## Building

Requires CMake 3.16+ and a C++17 compiler.

### macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo .
cmake --build build -j 8
./build/gzdoom.app/Contents/MacOS/gzdoom
```

`HAVE_METAL` defaults ON for Apple. Note that `--target zdoom` builds only the
executable — you need the default target for the `.pk3` assets beside it.

### Linux / BSD

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPK3_QUIET_ZIPDIR=ON .
cmake --build build --parallel $(nproc)
./build/gzdoom -iwad /path/to/doom2.wad
```

Needs `libinput` and `libudev`; Wayland, xkbcommon, X11 and Xi are loaded at
runtime and are not link-time dependencies. Override the backend with
`ZWIDGET_DISPLAY_BACKEND=Wayland|X11|SDL2` (probe order is Wayland → X11 → SDL2).
Set `-DGZDOOM_NATIVE_LINUX=OFF` for the legacy SDL2 path.

### Windows

As upstream — see the wiki's Programmer's Corner.

---

## Running a downloaded build on macOS

Builds are not code-signed or notarized, so Gatekeeper will refuse them:

```bash
xattr -dr com.apple.quarantine /path/to/gzdoom.app
```

You need your own IWAD (`doom2.wad`, `freedoom2.wad`, …) as with any source
port. Run with no `-iwad` argument to get the launcher.

---

## Helping out

**Testing is worth more than code here.** The renderer work is verified by
comparing captures against the OpenGL backend, and that only proves things about
hardware someone actually runs.

The most valuable contribution right now is **anyone with an Apple Silicon Mac**
running:

```bash
python3 tools/matrix/run.py --update-baseline
python3 tools/matrix/crossbackend.py
```

The first records a golden-image baseline across the postprocess chain and
refuses to record if any pass is broken; the second compares Metal against
OpenGL and reports where they diverge. Both run windowed, use their own config
file, and will not touch your settings. Either failing on M-series hardware is a
real finding.

Bug reports are welcome with the same caveat that applies to everything here: a
report that says what you measured beats one that says what you think happened.

**AI-assisted contributions are welcome.** See `CONTRIBUTING.md` for how that
works and what you are certifying when you submit.

---

## Documentation

- `CONTRIBUTING.md` — how work is verified here. Read before submitting.
- `AGENTS.md` — current state, open items, and the traps that have cost real
  time.
- `docs/history/agent-log.md` — historical log, 2026-06 onward. An archive,
  kept because it records what was **disproved**, not a current task guide.
- `docs/engine-modernization.md` — the durable roadmap.
- `docs/gpu-capture-protocol.md` — GPU frame capture runbook.
- `src/common/rendering/metal/README_METAL_RENDERER.md` and
  `.github/copilot-instructions.md` — Metal renderer field guides.

---

## Relationship to upstream

Based on GZDoom, and intended to stay that way — mods and the community are
there. This fork is independent of UZDoom.

`libraries/ZWidget` is a **git subtree** tracking a fork of
[dpjudas/ZWidget](https://github.com/dpjudas/ZWidget). Fixes that are not
specific to this fork are sent upstream.

## License

GPL v3, as upstream. Copyright (c) 1998-2025 ZDoom + GZDoom teams and
contributors; see the license files for individual contributor licenses. Doom
source (c) 1997 id Software, Raven Software, and contributors.

Special thanks to Coraline of the EDGE team for the original README template.

### Resources
- https://zdoom.org/ — home page
- https://forum.zdoom.org/ — forum
- https://zdoom.org/wiki/ — wiki
