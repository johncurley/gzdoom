#!/usr/bin/env python3
"""Collect pre-translated Metal shaders into wadsrc for shipping in gzdoom.pk3.

WHY THIS EXISTS
    GLSL -> SPIR-V -> MSL translation costs ~1.9s across 92 stages on a cold
    cache (measured 2026-08-17, Intel HD 6000). That is paid after every shader
    edit and after every cache wipe, which during shader work is several times a
    day. Shipping the translated MSL in gzdoom.pk3 removes it: mt_shader.cpp
    checks `shaders/metal/generated/<key>.msl` before the on-disk cache and
    before invoking glslang at all.

WHY STALENESS IS SAFE
    The `<key>` in each filename carries the same SuperFastHash of source and
    defines that the engine computes at runtime. Edit a .fp and the hash changes,
    so the shipped file no longer matches and the engine falls through to
    translating. Forgetting to re-run this script costs you 1.9s of startup; it
    does NOT run stale shader code. That property is the whole reason this ships
    MSL text rather than a prebuilt metallib.

USAGE
    1. Clear the caches so every stage is actually translated (a cache hit
       produces no dump -- only fresh translations are written):

         rm -f ~/Library/Application\\ Support/zdoom/cache/*.msl

    2. Launch with dumping on, reach gameplay so the whole compile state
       machine runs (material -> NAT -> user -> effect shaders), then quit:

         ./build/gzdoom.app/Contents/MacOS/gzdoom -iwad DOOM2.wad \\
             +mt_dumpshaders 1 +map MAP01

    3. Run this script, then repack the pk3:

         python3 tools/collect_metal_shaders.py
         build/tools/zipdir/zipdir -udf \\
             build/gzdoom.app/Contents/MacOS/gzdoom.pk3 wadsrc/static

    Step 3's repack is not optional. Editing wadsrc without repacking leaves the
    running engine on the old contents with no error and no visible sign -- see
    CLAUDE.md.

SCOPE
    Only the engine's own programs are worth shipping. Mod shaders come from
    PK3s that this repository does not control, so they are skipped: a mod
    shader's MSL would be dead weight for every user who does not run that mod,
    and the runtime path handles them correctly already.
"""

import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEST = os.path.join(REPO, "wadsrc", "static", "shaders", "metal", "generated")
SRC = os.path.expanduser("~/Library/Application Support/zdoom/cache/generated")


def main():
    if not os.path.isdir(SRC):
        print(f"no dump directory at {SRC}")
        print("Launch with +mt_dumpshaders 1 after clearing the .msl cache.")
        return 1

    names = sorted(n for n in os.listdir(SRC) if n.endswith(".msl"))
    if not names:
        print(f"{SRC} is empty -- nothing was translated.")
        print("A warm .msl cache produces no dumps; clear it and re-run.")
        return 1

    os.makedirs(DEST, exist_ok=True)
    existing = set(n for n in os.listdir(DEST) if n.endswith(".msl"))

    added = updated = same = 0
    total = 0
    for n in names:
        src = os.path.join(SRC, n)
        dst = os.path.join(DEST, n)
        data = open(src, "rb").read()
        total += len(data)
        if n in existing:
            if open(dst, "rb").read() == data:
                same += 1
                continue
            updated += 1
        else:
            added += 1
        shutil.copyfile(src, dst)

    # Stale entries are reported, never deleted automatically. A partial dump
    # run -- quitting before the compile state machine finishes -- would
    # otherwise silently delete good entries, and the cost of a stale file is
    # only that it never matches.
    stale = sorted(existing - set(names))

    print(f"collected {len(names)} stages ({total / 1024.0:.0f} KB) -> {DEST}")
    print(f"  added {added}, updated {updated}, unchanged {same}")
    if stale:
        print(f"  {len(stale)} file(s) in wadsrc were not in this dump:")
        for n in stale[:10]:
            print(f"    {n}")
        if len(stale) > 10:
            print(f"    ... and {len(stale) - 10} more")
        print("  Left in place deliberately. They are either from another")
        print("  configuration or superseded by a source change; a superseded")
        print("  file simply never matches its hash again. Delete by hand once")
        print("  you are sure the dump was complete.")
    print("\nNow repack the pk3, or none of this takes effect:")
    print("  build/tools/zipdir/zipdir -udf "
          "build/gzdoom.app/Contents/MacOS/gzdoom.pk3 wadsrc/static")
    return 0


if __name__ == "__main__":
    sys.exit(main())
