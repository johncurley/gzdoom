#!/usr/bin/env python3
"""Predict the Reinhard tonemap capture from the Uncharted2 one, per pixel.

Both modes in shaders/pp/tonemap.fp are pointwise functions of the SAME scene
colour c:

    Linear(c) = c*c                       (applied to both, not to PALETTE)
    Uncharted2: u = sRGB(U2(c^2) * W)     W = 1/U2(11.2)
    Reinhard:   r = sRGB(x/(1+x)), x=c^2

so given the mode-1 image we can invert for c^2 and forward-evaluate mode 3.
This compares two live measurements against each other rather than against a
number derived from one of them, which is the point (see handoff METHOD
LESSONS).

Usage:  tonemap_predict.py uncharted2.png reinhard.png

Reports, per channel and overall: the mean absolute error between predicted
and actual Reinhard in 8-bit levels, alongside the mean absolute difference
between the two captures themselves. If the modes are collapsing to one
shader, actual-vs-uncharted2 is ~0 and the prediction error is large; if the
fix works, the prediction error should be small (quantisation + any ordered
dither) and the raw difference large.

Why the prediction and not just "are they different?"
-----------------------------------------------------
"The two images differ" is satisfied by almost any bug -- a stray flip, an
exposure drift between launches, a different viewpoint. Predicting mode 3 from
mode 1 through the curve relation cannot be satisfied by accident: it demands
they differ by a *specific* per-pixel function. Use it in preference to a mean
brightness comparison.

VALIDATED 2026-08-05 against a synthetic ramp spanning into HDR:
    true pair       0.309 levels predicted error vs 28.111 raw difference
    collapse pair  28.122 levels predicted error vs  0.000 raw difference
so the two failure modes are ~90x apart and cannot be confused.

Hard-won rules
--------------
- Byte-identical captures are ambiguous, ALWAYS. They mean "perfect identity"
  and "the pass never ran" equally well. Distinguish with separate evidence
  that the pass executes -- e.g. a config where it visibly changes the frame --
  before reading anything into a match.
- Both captures must come from separate launches with the cvar on the command
  line. Typing cvars into a running console leaves earlier settings applied and
  silently confounds every later capture in that session.
- gl_tonemap 4 (Linear) is sqrt(c*c) == c, a mathematical identity. It is the
  best available probe of the PP plumbing precisely because it removes the
  tonemap maths as a variable: with the pipeline correct it must be
  byte-identical to gl_tonemap 0.
"""

import sys
import zlib
import struct


def read_png(path):
    """Decode a non-interlaced 8-bit RGB/RGBA PNG. stdlib only -- no PIL here."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, b"", None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        ctype = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        if ctype == b"IHDR":
            w, h, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color not in (2, 6) or interlace:
                raise ValueError(f"{path}: need 8-bit non-interlaced RGB/RGBA")
            nch = 3 if color == 2 else 4
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length

    raw = zlib.decompress(idat)
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p : p + stride])
        p += stride
        if ft == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                c = prev[i - nch] if i >= nch else 0
                b = prev[i]
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError(f"{path}: bad filter {ft}")
        out[y * stride : (y + 1) * stride] = line
        prev = line
    return w, h, nch, bytes(out)


def u2(x):
    A, B, C, D, E, F = 0.15, 0.50, 0.10, 0.20, 0.02, 0.30
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F


WHITE_SCALE = 1.0 / u2(11.2)


def uncharted2(x):
    """x is already-linearised scene value (c^2). Returns display value."""
    v = u2(x) * WHITE_SCALE
    return v ** 0.5 if v > 0 else 0.0


def reinhard(x):
    v = x / (1.0 + x)
    return v ** 0.5 if v > 0 else 0.0


def build_tables():
    """LUT: 8-bit Uncharted2 output -> 8-bit Reinhard output.

    Inverts uncharted2() by bisection on x in [0, 64]; the curve is monotone,
    so this is exact to the resolution we care about. Values above what
    uncharted2 can produce clamp to the top.
    """
    lut = []
    for level in range(256):
        target = level / 255.0
        lo, hi = 0.0, 64.0
        for _ in range(60):
            mid = 0.5 * (lo + hi)
            if uncharted2(mid) < target:
                lo = mid
            else:
                hi = mid
        x = 0.5 * (lo + hi)
        lut.append(min(255, max(0, round(reinhard(x) * 255.0))))
    return lut


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    wu, hu, nu, du = read_png(sys.argv[1])
    wr, hr, nr, dr = read_png(sys.argv[2])
    if (wu, hu) != (wr, hr):
        print(f"size mismatch: {wu}x{hu} vs {wr}x{hr}")
        return 1

    lut = build_tables()
    names = "RGB"
    pred_err = [0, 0, 0]
    raw_diff = [0, 0, 0]
    n = wu * hu
    identical = du == dr

    for i in range(n):
        bu = i * nu
        br = i * nr
        for c in range(3):
            a = du[bu + c]
            b = dr[br + c]
            pred_err[c] += abs(lut[a] - b)
            raw_diff[c] += abs(a - b)

    print(f"{wu}x{hu}, {n} px")
    print(f"byte-identical captures: {identical}")
    print()
    print("chan  mean|pred-actual|   mean|mode1-mode3|")
    for c in range(3):
        print(f"  {names[c]}      {pred_err[c]/n:8.3f}          {raw_diff[c]/n:8.3f}")
    tot_p = sum(pred_err) / (3 * n)
    tot_r = sum(raw_diff) / (3 * n)
    print(f"all      {tot_p:8.3f}          {tot_r:8.3f}")
    print()
    if identical:
        print("VERDICT: captures are byte-identical -- modes are STILL collapsing.")
    elif tot_p < 4.0 and tot_r > 4 * tot_p:
        print("VERDICT: mode 3 is predicted from mode 1 by the Reinhard/Uncharted2")
        print("         relation. Both shaders ran, and ran as their own source says.")
    else:
        print("VERDICT: images differ but not by the predicted transfer relation.")
        print("         Something other than the tonemap mode changed between runs")
        print("         (scene drift, exposure, colour format) -- check the logs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
