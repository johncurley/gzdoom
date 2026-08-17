#!/usr/bin/env python3
"""Rename a pre-translated stage's entry point so many can share one metallib.

SPIRV-Cross names every entry point `main0`. A single .metallib cannot hold 92
functions with that name, so each stage gets a unique symbol before compilation:

    s_<filename stem with '-' mapped to '_'>

The stem carries the SuperFastHash of source-plus-defines that the engine
computes at runtime, so the symbol is stable for a given shader source and
changes when the source does. mt_shader.cpp's GeneratedSymbolName() derives the
same string; if the two ever disagree the lookup simply misses and the engine
falls back to compiling MSL -- slow, not wrong.

Only the bare identifier `main0` is rewritten. `main0_in` and `main0_out` are
struct names, local to each .air translation unit, and do not collide when the
modules are linked together.
"""

import os
import re
import sys


def symbol_for(stem):
    return "s_" + stem.replace("-", "_")


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <in.msl> <out.metal>", file=sys.stderr)
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    stem = os.path.splitext(os.path.basename(src))[0]

    text = open(src, "r", encoding="utf-8").read()
    text, count = re.subn(r"\bmain0\b", symbol_for(stem), text)
    if count != 1:
        # Exactly one bare main0 is expected: the entry point. Zero means the
        # file is not what we think it is; more than one means a naming
        # assumption changed and the rename could be corrupting something.
        print(f"{src}: expected exactly 1 bare 'main0', found {count}",
              file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
