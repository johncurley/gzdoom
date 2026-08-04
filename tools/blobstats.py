#!/usr/bin/env python3
"""Five-number signature of a localized additive effect (bloom, glow, flare).

    python3 tools/blobstats.py baseline.png effect.png [--thresh 2]

Prints bbox, aspect, delta-weighted centroid, peak, energy and the count of
pixels above threshold, for the positive luminance delta (effect - baseline).

Why this exists, and why it is not localize.py: localize answers "where and in
which direction", at 40x18 block resolution, which is the right tool for "did
anything happen and is it one-directional". It cannot answer "is this blob the
same SHAPE as that blob", because a wide-flat and a square blob of equal
energy produce similar block maps. The Metal-vs-reference bloom work needs
shape, so it needs per-pixel moments.

DEFINITIONS -- these matter, because a signature is only comparable to another
signature computed the same way:

  delta   per-pixel Rec.601 luminance of `effect` minus that of `baseline`,
          POSITIVE PART ONLY. Bloom is additive; negative deltas are drift or
          noise and are reported separately as a sanity check, not folded in.
  thresh  default 2 luminance units, matching localize.py's '+' band, chosen
          because 8-bit screenshot quantization and dither put roughly 1 unit
          of floor under everything.
  bbox    tight bounds of pixels with delta > thresh.
  centroid  delta-weighted, over pixels above threshold, in FULL-FRAME pixel
          coordinates with y measured DOWNWARD from the top of the image.
          Reflection-axis arithmetic must use the same convention -- get the
          scene viewport from `mt_caps` and remember it is not the window.
  peak    max delta anywhere in the frame.
  energy  sum of delta over pixels above threshold.
  px>N    count of pixels above threshold.

A pure vertical reflection of the effect preserves bbox size, peak, energy and
the count, and moves ONLY the centroid's y. That is the whole reason these
five are reported together: any candidate mechanism has to be checked against
all of them, and one that explains the peak but predicts the wrong pixel count
is contradicted, not partially right.

CROP: none. Unlike localize.py this does not crop the HUD, because a bloom
blob near the frame edge would be clipped by a crop and silently change shape.
Use a scene where the HUD is not itself blooming, and use the title bar as the
control that the two captures are the same window.

stdlib only -- see pngdiff.py. Both images must be the same dimensions; a
mismatch aborts rather than rescaling, because a resized capture pair cannot
produce a comparable signature.
"""
import sys
from pngdiff import read_png


def luma_plane(path):
    w, h, nch, D = read_png(path)
    lum = [0] * (w * h)
    for p in range(w * h):
        i = p * nch
        lum[p] = (D[i] * 299 + D[i + 1] * 587 + D[i + 2] * 114) // 1000
    return w, h, lum


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    thresh = 2.0
    for a in sys.argv[1:]:
        if a.startswith("--thresh"):
            thresh = float(a.split("=", 1)[1]) if "=" in a else thresh
    if len(args) < 2:
        print(__doc__)
        return 2
    if "--thresh" in sys.argv:
        thresh = float(sys.argv[sys.argv.index("--thresh") + 1])

    base_p, eff_p = args[0], args[1]
    w0, h0, base = luma_plane(base_p)
    w1, h1, eff = luma_plane(eff_p)
    if (w0, h0) != (w1, h1):
        print(f"ABORT: dimensions differ, {w0}x{h0} vs {w1}x{h1}. "
              "A rescaled pair cannot produce a comparable signature.")
        return 1

    minx, miny, maxx, maxy = w0, h0, -1, -1
    peak = 0.0
    energy = 0.0
    count = 0
    sx = sy = 0.0
    neg_count = 0
    neg_sum = 0.0

    for y in range(h0):
        row = y * w0
        for x in range(w0):
            d = eff[row + x] - base[row + x]
            if d > peak:
                peak = d
            if d < 0:
                neg_count += 1
                neg_sum -= d
                continue
            if d > thresh:
                count += 1
                energy += d
                sx += x * d
                sy += y * d
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y

    print(f"baseline : {base_p}")
    print(f"effect   : {eff_p}")
    print(f"frame    : {w0}x{h0}   threshold: delta > {thresh:g}")
    if count == 0:
        print("\nNO PIXELS ABOVE THRESHOLD.")
        print("Either the effect did not apply, or it genuinely does nothing "
              "here. Check the MD5s of the two captures before interpreting: "
              "byte-identical means the toggle did not take.")
        return 0

    bw, bh = maxx - minx + 1, maxy - miny + 1
    print(f"\n  bbox      {bw} x {bh}   at ({minx},{miny})-({maxx},{maxy})")
    print(f"  aspect    {bw / bh:.2f}:1")
    print(f"  centroid  ({sx / energy:.1f}, {sy / energy:.1f})")
    print(f"  peak      {peak:.1f}")
    print(f"  energy    {energy:.0f}")
    print(f"  px>{thresh:g}      {count}")
    print(f"\n  (negative-delta pixels: {neg_count}, total {neg_sum:.0f} -- "
          "should be near zero for a purely additive effect; a large value "
          "means drift or animation between captures, so retake the pair)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
