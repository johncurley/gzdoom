# Linux session handoff

> **DONE — 2026-08-10. Both tasks passed.** Task 1 builds clean with no `gles_*`
> symbol errors; Task 2 is 11/11 OK on `gl,vulkan`. Getting there needed three
> fixes that were not renderer bugs: `HAVE_VULKAN=ON` did not compile, every GL
> screenshot was black (readback after the buffer swap), and the harness was
> macOS-only in its paths and could not identify the GL backend on Linux. Full
> results and the measurements are in `AGENTS.md` under Open items. Two new open
> items came out of it: Wayland windows not painting until an unrelated event,
> and an intermittently black GL game window. Kept for the record — do not
> re-run this from scratch.

Everything below is validation of work already committed, not new development.
Two things were done on macOS that **only Linux can check**, and both are cheap.

Start by reading `AGENTS.md` (current state and traps) and `CONTRIBUTING.md`
(how work is verified here — the standard is unusual and it is not optional).

---

## Context

`metal-audit` now contains the merge of `native-platform-expansion`: the native
POSIX platform layer, the ZWidget subtree, the CI smoke test, and the Metal
renderer work, in one tree. The merge was 214 files with only two conflicts, and
**it has only ever been built on macOS.**

You do **not** need to clone ZWidget. It is a git subtree — the files live in
this repository and arrive with the clone.

```bash
git clone <this repo> && cd gzdoom
git checkout metal-audit
```

---

## Task 1 — build, and check one specific edit

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPK3_QUIET_ZIPDIR=ON .
cmake --build build --parallel $(nproc)
./build/gzdoom -iwad /path/to/doom2.wad
```

**What is actually being tested.** Resolving `src/CMakeLists.txt` meant choosing
between two versions of the GLES block. The Metal side had:

```cmake
if (HAVE_GLES2)
    set (GLES_SOURCES ...)
    list (APPEND FASTMATH_SOURCES ${GLES_SOURCES})
endif()
```

while this branch lists the GLES sources unconditionally in the main source
list. Keeping both would compile them twice, so the conditional block was
**dropped**. That decision was made by reading the file, not by building it, and
it lands squarely in your build.

- Builds and runs → the call was right.
- Duplicate-symbol or missing-symbol errors around `gles_*` → it was wrong, and
  the fix is to restore the block or remove the unconditional entries, not both.

Also confirm the launcher still appears (run with no `-iwad`) and that GLES2
still builds if you use it: `-DHAVE_GLES2=ON`.

## Task 2 — run the cross-backend oracle with Vulkan

This is the first time Vulkan can be used here at all; the RX 550 makes it
possible.

```bash
python3 tools/matrix/crossbackend.py --selfcheck --backends gl,vulkan
python3 tools/matrix/crossbackend.py --backends gl,vulkan
```

**Run the self-check first and believe it.** It launches each backend twice on
the same config and requires both to reproduce themselves. If Vulkan does not
settle deterministically within `settle_frames`, every difference the sweep
reports afterwards is its own noise wearing a finding's clothes — that exact
mistake produced a phantom "2.6x tone difference between backends" during
bring-up, which was really OpenGL failing to settle.

**What this proves.** `CONTRIBUTING.md` requires shared-code changes to leave GL
and Vulkan bit-identical. Several were made for Metal parity —
`wadsrc/static/shaders/pp/lineardepth.fp`, `ssaocombine.fp`, and the SSAO
uniforms in `hw_postprocess.cpp` — and every one was verified **on GL only** and
assumed correct for Vulkan. This is that assumption's first test.

**How to read the output.** The verdict is about the *shape* of the difference,
not its size:

| shape | meaning |
|---|---|
| uniform low delta everywhere | expected backend noise — ignore |
| one horizontal band differs | a vertical offset or flip |
| half the frame differs | viewport, scissor or extent |
| delta only where an effect appears | that pass differs, the rest agrees |

A `SUSPECT` verdict is an invitation to run `tools/localize.py` on the two
captures, not a bug report. On macOS the `colormap` config reports a sparse note
under the coverage floor and is benign; whether Vulkan does the same is unknown.

**This code path has never been executed.** `vulkan: 1`, the `Vulkan device:`
proof marker and the `--backends` flag were written on a machine with no Vulkan
hardware. If the harness itself misbehaves — wrong backend confirmed, size
mismatch, crash — that is a bug in the tool, not a finding about the renderer.

---

## Traps that apply here

Most of `AGENTS.md`'s trap list is macOS-specific. These are not:

- **Archived CVARs leak between runs.** `vid_preferbackend` is
  `CVAR_ARCHIVE | CVAR_GLOBALCONFIG`, so one OpenGL launch silently turns every
  later run into an OpenGL run. Both harnesses now pass `-config` and use their
  own file, but any manual run you do by hand is exposed. Confirm the backend
  from the log, never from the CVAR you passed.
- **The pk3 must be repacked after any `wadsrc/` change**, including a revert:
  `build/tools/zipdir/zipdir -udf build/gzdoom.pk3 wadsrc/static`. On Linux the
  target is `build/gzdoom.pk3` — the bundle path in the macOS notes does not
  apply.
- **Discard the first launch after a build.** Cold shader compilation perturbs
  the settle, and GL is worse than Metal for this. Both harnesses now do a
  warmup launch automatically; manual A/Bs do not.

---

## Recording results

Add findings to `AGENTS.md` under the open items, and say what you measured
rather than what you concluded. If something fails, the numbers and the log
matter more than the diagnosis — this project has a long history of confident
diagnoses that were wrong, and `docs/history/agent-log.md` is largely a record
of them.

Nothing here should need a code change. If it does, it is a real finding.
