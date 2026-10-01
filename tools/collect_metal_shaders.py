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

    A live `mt_shader_report` can also identify engine stages that already
    exist in the keyed runtime cache but are not in the source generated set.
    Promote only those exact keys (printed before the arrow):

         python3 tools/collect_metal_shaders.py --from-cache \\
             shaders/pp/lineardepth.fp_2bfc2051_frag \\
             shaders/pp/ssao.fp_8027bbc1_frag

    The normal build recompiles the metallib after new stages are added and
    repacks gzdoom.pk3. Cache keys include the source and defines, so these
    entries remain safe if their shader source changes later.

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
CACHE = os.path.dirname(SRC)


def filter_shader_key(key):
    """Match MtShaderManager::FilterShaderKey for the ASCII engine keys."""
    return "".join(c for c in key if c.isascii() and (c.isalnum() or c in "_-"))


def collect_cached_keys(keys):
    if not os.path.isdir(CACHE):
        print(f"no shader cache at {CACHE}")
        return 1

    entries = []
    for key in keys:
        stem = filter_shader_key(key)
        if not stem:
            print(f"invalid empty shader key: {key!r}")
            return 1
        src = os.path.join(CACHE, f"mt_{stem}.msl")
        if not os.path.isfile(src):
            print(f"no cached MSL for {key}: {src}")
            return 1
        with open(src, "rb") as infile:
            data = infile.read()
        if not data:
            print(f"cached MSL is empty for {key}: {src}")
            return 1
        entries.append((stem, data))

    os.makedirs(DEST, exist_ok=True)
    added = updated = same = 0
    for stem, data in entries:
        dst = os.path.join(DEST, f"{stem}.msl")
        if os.path.isfile(dst):
            with open(dst, "rb") as infile:
                if infile.read() == data:
                    same += 1
                    continue
            updated += 1
        else:
            added += 1
        with open(dst, "wb") as outfile:
            outfile.write(data)

    print(f"promoted {len(entries)} exact runtime cache keys -> {DEST}")
    print(f"  added {added}, updated {updated}, unchanged {same}")
    print("Reconfigure and build so CMake links the new metallib stages and repacks gzdoom.pk3.")
    return 0


def main():
    if len(sys.argv) > 1:
        if sys.argv[1] == "--from-cache" and len(sys.argv) > 2:
            return collect_cached_keys(sys.argv[2:])
        print("usage: collect_metal_shaders.py [--from-cache <exact-key> ...]")
        return 2

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
