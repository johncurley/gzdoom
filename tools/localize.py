#!/usr/bin/env python3
"""Where do two shots differ, and in which direction?

    python3 tools/localize.py before.png after.png

Prints an ASCII block map of the mean signed luminance delta plus global
stats (mean signed delta, brighter/darker pixel counts, max |delta|).

Why this exists: "the images differ" is not a finding -- the *shape* of the
difference identifies its cause, and this repeatedly told real effects apart
from confounds on the Metal bloom/AO work:

  - compact blob at a light source, one-directional  -> a bloom change
  - broad wash over the lower frame, brighter over time
        -> camera exposure adaptation drift, NOT the config under test
        (gl_exposure_speed defaults to 0.05; set it to 1 before any A/B)
  - scattered and bidirectional -> animation/flicker between captures; the
        pair is worthless, retake it

Read the signed mean together with the brighter/darker counts: a real effect
is overwhelmingly one-directional (e.g. 34519 darker vs 3 brighter), while
noise is mixed. Always take a same-config control pair -- on this hardware
drift has exceeded the measured effect by 26x.

Crop box and stdlib-only caveats as in cluster.py.
"""
import sys
from pngdiff import read_png

a_p, b_p = sys.argv[1], sys.argv[2]
w, h, nch, A = read_png(a_p)
_, _, _, B = read_png(b_p)
x0, y0 = int(w*0.05), int(h*0.09)
x1, y1 = int(w*0.95), int(h*0.80)

COLS, ROWS = 40, 18
bw, bh = (x1-x0)//COLS, (y1-y0)//ROWS
print(f"grid {COLS}x{ROWS}, block {bw}x{bh}px, region ({x0},{y0})-({x1},{y1})")
print("symbol: '.' none  '-' <1  '+' 1-4  '#' 4-12  '@' >12  (mean signed lum delta, B-A)\n")

worst = (0, None)
for r in range(ROWS):
    line = ""
    for c in range(COLS):
        sx, sy = x0 + c*bw, y0 + r*bh
        tot = 0; n = 0
        for y in range(sy, min(sy+bh, y1), 2):
            for x in range(sx, min(sx+bw, x1), 2):
                i = (y*w + x)*nch
                la = (A[i]*299 + A[i+1]*587 + A[i+2]*114)//1000
                lb = (B[i]*299 + B[i+1]*587 + B[i+2]*114)//1000
                tot += lb - la; n += 1
        d = tot/max(n, 1)
        if abs(d) > abs(worst[0]):
            worst = (d, (sx, sy))
        ad = abs(d)
        line += '.' if ad < 0.05 else ('-' if ad < 1 else ('+' if ad < 4 else ('#' if ad < 12 else '@')))
    print("  " + line)

print(f"\nlargest block delta: {worst[0]:+.3f} lum at pixel {worst[1]}")

# Global signed stats over the region
tot = 0; n = 0; pos = 0; neg = 0; mx = 0
for y in range(y0, y1):
    for x in range(x0, x1):
        i = (y*w + x)*nch
        la = (A[i]*299 + A[i+1]*587 + A[i+2]*114)//1000
        lb = (B[i]*299 + B[i+1]*587 + B[i+2]*114)//1000
        d = lb - la
        tot += d; n += 1
        if d > 0: pos += 1
        elif d < 0: neg += 1
        mx = max(mx, abs(d))
print(f"mean signed delta (B-A): {tot/n:+.4f}   brighter px: {pos} ({100*pos/n:.3f}%)   "
      f"darker px: {neg} ({100*neg/n:.3f}%)   max |delta|: {mx}")
