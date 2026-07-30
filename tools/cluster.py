#!/usr/bin/env python3
"""Group screenshots by whether their game viewport is pixel-identical.

    python3 tools/cluster.py shot1.png shot2.png shot3.png

Prints a per-shot fingerprint (viewport SHA1, mean/max luminance, bright-pixel
fraction) then groups shots whose viewports are byte-identical.

Why this exists: when A/B-ing a rendering change by screenshot, the first
question is always "did anything change at all?", and the eye is bad at it in
dark scenes. On 2026-07-27 seven shots taken across `gl_bloom 0/1` and every
`mt_compute_bloom_composite` mode came back as **one** distinct image --
proving the test location was invalid (nothing in the scene exceeded the bloom
threshold) rather than that the configs agreed. Run this before drawing any
conclusion from a screenshot A/B, and always include a control pair that is
*supposed* to differ.

The crop box excludes macOS window chrome and the HUD strip, tuned for
windowed captures of this game -- adjust BOX_FRAC for other capture setups.
Stdlib only (see pngdiff.py for the PNG decoder); no PIL or ImageMagick needed.
"""
import hashlib, sys
from pngdiff import read_png

BOX_FRAC = (0.05, 0.09, 0.95, 0.80)  # exclude window chrome and HUD strip

def viewport_bytes(w, h, nch, px):
    x0, y0 = int(w*BOX_FRAC[0]), int(h*BOX_FRAC[1])
    x1, y1 = int(w*BOX_FRAC[2]), int(h*BOX_FRAC[3])
    out = bytearray()
    for y in range(y0, y1):
        out += px[(y*w + x0)*nch:(y*w + x1)*nch]
    return bytes(out), (x0, y0, x1, y1)

rows = []
for p in sys.argv[1:]:
    w, h, nch, px = read_png(p)
    vb, box = viewport_bytes(w, h, nch, px)
    tot = 0; mx = 0; bright = 0; n = 0
    for i in range(0, len(vb), nch):
        lum = (vb[i]*299 + vb[i+1]*587 + vb[i+2]*114)//1000
        tot += lum; n += 1
        if lum > mx: mx = lum
        if lum > 128: bright += 1
    rows.append((p.split('/')[-1], hashlib.sha1(vb).hexdigest()[:10],
                 tot/n, mx, 100.0*bright/n))

groups = {}
for name, h_, mean, mx, bf in rows:
    groups.setdefault(h_, []).append(name)

print(f"{'file':42s} {'hash':11s} {'mean_lum':>9s} {'max':>4s} {'bright%':>8s}")
for name, h_, mean, mx, bf in rows:
    print(f"{name:42s} {h_:11s} {mean:9.4f} {mx:4d} {bf:8.4f}")
print(f"\n{len(groups)} distinct image(s):")
for h_, names in groups.items():
    print(f"  {h_}: {len(names)} shot(s) -> {', '.join(n[-15:] for n in names)}")
