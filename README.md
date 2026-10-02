# GZDoom-based engine with native Metal and POSIX backends

This project builds on the GZDoom codebase and develops its own renderer,
platform support, compatibility work, and user interface.

- **A native Metal renderer for macOS.** A direct Metal 2 backend, not MoltenVK
  — no Vulkan translation layer in the way. Written to mirror the Vulkan
  backend's structure so the two can be read side by side.
- **A native POSIX platform layer for Linux/BSD.** Wayland/X11 and libinput
  through ZWidget instead of SDL2, with the display libraries `dlopen`'d so one
  binary runs under either, or neither.

**Mod compatibility is an active development goal.** We test real mods and
implement compatibility feature by feature, based on demonstrated needs. We do
not claim complete compatibility with GZDoom, UZDoom, or every feature they
provide. A mod's declared engine or ZScript version is a useful signal, not a
compatibility guarantee; supported behavior is established through testing.

The project also has its own renderer and platform technology and its own UI
design direction. Compatibility work does not require matching another port's
feature set or presentation.

---

## Status

**macOS / Metal** — playable and in daily use on the Intel development machine.
Documented captures compare it with OpenGL; scene normals, fog, model normals,
and palette tonemapping match within one or two channel values on the tested
routes. One known residual is an SSAO contribution difference of about 0.4/255
in the final frame; see the
[Metal renderer guide](src/common/rendering/metal/README_METAL_RENDERER.md).

**Linux** — native Wayland and X11 backends with desktop theme detection. The
X11 raw-keyboard path has been interactively validated on the Linux test
machine with balanced press/release events and no stuck actions. That machine
has also passed the GL/Vulkan cross-backend suite and has a platform-specific
golden baseline. The native backend is tested on the maintainer's Linux
hardware, not on every compositor or GPU.

**BSD** — the native POSIX design is intended to cover BSD as well, but this
fork does not currently claim BSD runtime verification.

**Windows** — builds in CI, but is not runtime-tested by the maintainer.
Reports welcome.

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

The stock Metal shader stages are shipped as pre-translated MSL and compiled
into the native metallib where supported by the build. Runtime translation is
retained for custom and mod-provided shaders. The Metal backend still has
hardware-specific limitations; results apply to the tested hardware and
routes.

### Platform validation status

| Platform | Status |
|---|---|
| macOS Intel / Metal | Tested by the maintainer |
| Linux / native Wayland and X11 | Tested on documented hardware |
| Linux / OpenGL and Vulkan | Tested on documented hardware |
| Apple Silicon / Metal | Builds in CI; runtime testing wanted |
| Windows | Builds in CI; not run by the maintainer |
| BSD | Intended by the native POSIX design; not runtime-verified |

Apple Silicon support must not be inferred from a successful build. The first
useful report from an M-series Mac is a clean run of the matrix tools below,
followed by ordinary gameplay with any visual or stability problems recorded.

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

**Reproducible testing is especially useful.** Renderer comparisons only prove
behavior on hardware someone actually runs, and mod compatibility needs a
specific package and gameplay route to establish which features work.

Two high-value contributions are **Apple Silicon runtime testing** and
**reproducible mod compatibility reports**.

For Apple Silicon, run from a clean checkout:

```bash
python3 tools/matrix/run.py --update-baseline
python3 tools/matrix/crossbackend.py
```

The first records a golden-image baseline across the postprocess chain and
refuses to record if any pass is broken; the second compares Metal against
OpenGL and reports where they diverge. Both run windowed, use their own config
file, and will not touch your settings. Either failing on M-series hardware is a
real finding. For a mod report, include its name and version, exact package and
sidecars, engine commit, launch command, and the startup/menu/gameplay route
that failed or succeeded. Do not include copyrighted game data.

Bug reports are welcome with the same caveat that applies to everything here: a
report that says what you measured beats one that says what you think happened.

For a renderer or platform report, include the commit or release, machine
model, operating system version, GPU, selected backend, exact command, whether
the relevant matrix tools passed, and a short reproduction for any crash or
visual difference. Mod reports should include the package and route details
listed above. Do not attach copyrighted game data to issues or pull requests.

Targeted code contributions are welcome when the contributor can explain and
verify the change. Small, measured changes and reproducible compatibility
reports are especially useful.

---

## Documentation

- `CONTRIBUTING.md` — how work is verified here. Read before submitting.
- `docs/handoff-framegraph-2026-09-28.md` — framegraph status and next work.
- `docs/engine-modernization.md` — the durable roadmap.
- `docs/gpu-capture-protocol.md` — GPU frame capture runbook.
- `src/common/rendering/metal/README_METAL_RENDERER.md` and
  `.github/copilot-instructions.md` — Metal renderer field guides.

---

## Project relationship

This is an independently developed project based on GZDoom source code. It is
not affiliated with or endorsed by the GZDoom or UZDoom projects. Its
compatibility, renderer, platform, and UI priorities are set independently.

`libraries/ZWidget` is a **git subtree** tracking a fork of
[dpjudas/ZWidget](https://github.com/dpjudas/ZWidget). Fixes that are not
specific to this fork are sent upstream.

## License

GPL v3. Copyright (c) 1998-2025 ZDoom + GZDoom teams and
contributors; see the license files for individual contributor licenses. Doom
source (c) 1997 id Software, Raven Software, and contributors.

Special thanks to Coraline of the EDGE team for the original README template.

### Resources
- https://zdoom.org/ — home page
- https://forum.zdoom.org/ — forum
- https://zdoom.org/wiki/ — wiki
