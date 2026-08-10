#!/usr/bin/env python3
"""Build the custom-postprocess-shader test mod, /tmp/pptest.pk3.

GZDoom's custom PP shaders (`HardwareShader postprocess ...` in GLDEFS) were the
last never-tested pass on the Metal backend, and testing them needed a mod. No
download is required: this builds a minimal one designed as a TWO-SIDED test,
which a real mod would not be.

    python3 tools/pptest/make.py
    ./build/gzdoom.app/Contents/MacOS/gzdoom -iwad DOOM2.wad \
      -file ~/Documents/GZDoom/Ashes2063Enriched2_23.pk3 -file /tmp/pptest.pk3 \
      -loadgame capspot.zds +cl_capfps 1 \
      +pptest_scale 1.0 +execafter 60 shaderenable pptest 1 +shotafter 120 quit

The design, and why each part is there:

  * `pptest_scale` is a CVARINFO cvar bound with `cvar_uniform`, so the shader's
    behaviour is set from the COMMAND LINE. The whole matrix runs unattended,
    with no console typing and therefore no reordered-toggle confound.

  * At `pptest_scale 1.0` the shader is a mathematical IDENTITY and the frame
    must be byte-identical to running no shader at all. That one result proves
    three things at once: the pass runs, its maths is exact, and it is UPRIGHT
    (a V flip would show as an enormous delta, not a null).

  * At any other value it must differ. This half is what makes the identity
    result mean something -- byte-identical is EQUALLY consistent with "the pass
    never ran", which is precisely how this path stayed broken.

  * `PPTESTTX` is pure red. If any channel-order bug reaches the custom-PP
    texture path, the frame renders BLUE. A null result cannot masquerade as a
    pass.

Change the `postprocess` target in GLDEFS to test each of `beforebloom`,
`scene` and `screen` -- they are three separate insertion points and, on Metal,
`screen` was wired up by a different fix than the other two.
"""

import os, shutil, struct, sys, zipfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = "/tmp/pptest.pk3"


def red_png(w=64, h=64):
    raw = b"".join(b"\x00" + bytes([255, 0, 0]) * w for _ in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    build = "/tmp/pptest"
    shutil.rmtree(build, ignore_errors=True)
    os.makedirs(os.path.join(build, "shaders"))
    os.makedirs(os.path.join(build, "textures"))
    for name in ("GLDEFS", "CVARINFO"):
        with open(os.path.join(HERE, name)) as f:
            open(os.path.join(build, name), "w").write(f.read())
    with open(os.path.join(HERE, "shaders", "pptest.fp")) as f:
        open(os.path.join(build, "shaders", "pptest.fp"), "w").write(f.read())
    open(os.path.join(build, "textures", "pptesttx.png"), "wb").write(red_png())

    if os.path.exists(OUT):
        os.remove(OUT)
    # stdlib zipfile rather than shelling out to `zip`, which is not installed
    # on the Linux box and is not part of a base install on any of the three
    # platforms. Same reason tools/pngdiff.py decodes PNGs by hand.
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(build):
            for name in sorted(files):
                path = os.path.join(root, name)
                z.write(path, os.path.relpath(path, build))
    print("wrote", OUT)


if __name__ == "__main__":
    sys.exit(main())
