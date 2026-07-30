#!/usr/bin/env python3
"""Minimal stdlib PNG reader + region statistics for screenshot A/B work.

    python3 tools/pngdiff.py a.png b.png [c.png ...]

Also the shared library for cluster.py and localize.py, which import
read_png() from here.

The dev machine has neither PIL nor ImageMagick, so this decodes PNG directly
(zlib + the five scanline filters, 8-bit non-interlaced only -- which is what
macOS screenshots and GZDoom's own captures produce). If Pillow is ever
available, this can be replaced wholesale; the callers only need read_png()
returning (w, h, channels, bytes).
"""
import struct, zlib, sys

def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', "not a PNG"
    pos, idat, ihdr = 8, [], None
    while pos < len(data):
        (length,) = struct.unpack('>I', data[pos:pos+4])
        ctype = data[pos+4:pos+8]
        chunk = data[pos+8:pos+8+length]
        if ctype == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', chunk)
        elif ctype == b'IDAT':
            idat.append(chunk)
        elif ctype == b'IEND':
            break
        pos += 12 + length
    w, h, depth, color, comp, filt, interlace = ihdr
    assert depth == 8 and interlace == 0, f"unsupported depth/interlace {depth}/{interlace}"
    nch = {0:1, 2:3, 3:1, 4:2, 6:4}[color]
    raw = zlib.decompress(b''.join(idat))
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i-nch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i-nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i-nch] if i >= nch else 0
                b = prev[i]
                c = prev[i-nch] if i >= nch else 0
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, nch, bytes(out)

def stats(w, h, nch, px, box=None):
    x0, y0, x1, y1 = box or (0, 0, w, h)
    total = 0; n = 0; mx = 0; hist = [0]*4
    for y in range(y0, y1):
        row = y * w * nch
        for x in range(x0, x1):
            i = row + x*nch
            lum = (px[i]*299 + px[i+1]*587 + px[i+2]*114) // 1000
            total += lum; n += 1
            mx = max(mx, lum)
            hist[min(3, lum // 64)] += 1
    return {'mean': total/n, 'max': mx, 'n': n,
            'bright_frac': (hist[2]+hist[3])/n}

def diff(a, b, w, h, nch, box=None):
    x0, y0, x1, y1 = box or (0, 0, w, h)
    worst = 0; ndiff = 0; total = 0; n = 0
    for y in range(y0, y1):
        row = y * w * nch
        for x in range(x0, x1):
            i = row + x*nch
            d = max(abs(a[i]-b[i]), abs(a[i+1]-b[i+1]), abs(a[i+2]-b[i+2]))
            worst = max(worst, d); total += d; n += 1
            if d > 2: ndiff += 1
    return {'max_channel_delta': worst, 'mean_delta': total/n,
            'px_differing_gt2': ndiff, 'frac_differing': ndiff/n}

if __name__ == '__main__':
    paths = sys.argv[1:]
    imgs = [read_png(p) for p in paths]
    w, h, nch = imgs[0][0], imgs[0][1], imgs[0][2]
    # Game viewport only, excluding window chrome and the HUD strip.
    box = (int(w*0.05), int(h*0.09), int(w*0.95), int(h*0.80))
    print(f"image {w}x{h} nch={nch}   analysis box={box}")
    for p, im in zip(paths, imgs):
        s = stats(im[0], im[1], im[2], im[3], box)
        print(f"  {p.split('/')[-1]:22s} mean_lum={s['mean']:7.3f} max={s['max']:3d} bright_frac={s['bright_frac']*100:6.3f}%")
    print()
    for i in range(len(imgs)-1):
        for j in range(i+1, len(imgs)):
            d = diff(imgs[i][3], imgs[j][3], w, h, nch, box)
            print(f"  {paths[i].split('/')[-1]} vs {paths[j].split('/')[-1]}: "
                  f"max_delta={d['max_channel_delta']} mean_delta={d['mean_delta']:.4f} "
                  f"differing>2: {d['px_differing_gt2']} ({d['frac_differing']*100:.4f}%)")
