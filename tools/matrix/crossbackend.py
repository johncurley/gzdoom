#!/usr/bin/env python3
"""Metal-vs-OpenGL cross-backend sweep, using the matrix runner's launch path.

    python3 tools/matrix/crossbackend.py                 # every config
    python3 tools/matrix/crossbackend.py --only ssao      # a subset
    python3 tools/matrix/crossbackend.py --screenblocks 8,10,11
    python3 tools/matrix/crossbackend.py --keep           # keep the PNGs

WHY THIS EXISTS, SEPARATELY FROM run.py
---------------------------------------
run.py answers "did Metal change since the last baseline". It cannot answer "is
Metal correct", because a baseline recorded from a wrong frame enshrines the
wrong frame -- and on this branch that has happened. OpenGL is the reference
implementation the Metal backend was written against, renders the same scene
from the same engine state, and has no Y-flip patching, no reverse-Z, and no
top-left viewport convention. It is the cheapest available oracle for exactly
the bug class this backend keeps producing.

Concretely: the 2026-08-07 SSAO bug (GetSceneRect returned a GL bottom-left rect
that every Metal consumer read as top-left, so AO landed vertically displaced
whenever the scene viewport was inset) was invisible to run.py -- Metal matched
its own baseline perfectly. Against GL it would have been a single obvious
result. It cost a session to find via GPU capture instead.

CROSS-BACKEND IS NOT PIXEL-EXACT -- READ THIS BEFORE INTERPRETING OUTPUT
------------------------------------------------------------------------
Two backends never agree byte for byte. Different rasterisation fill rules,
texture filtering and mipmap selection, dithering, and float precision all
produce small, *spatially uniform* noise. A nonzero delta is the normal state.

So the signal is not "is there a difference" but "what SHAPE is the difference":

  uniform low delta everywhere        expected backend noise -- ignore
  one horizontal band differs         a vertical offset/flip bug (TODAY'S BUG)
  half the frame differs              a viewport, scissor or extent bug
  delta only where an effect appears  that pass differs; the rest agrees

That is why this reports a per-band breakdown instead of a single number. A
whole-frame mean hides a displaced effect: the pixels that gained AO and the
pixels that lost it average out. Bands do not average out.

DISCARD THE FIRST LAUNCH AFTER A BUILD
--------------------------------------
Bring-up, 2026-08-07: the first GL capture after a rebuild came back at
mean_lum 17.6 (max 183) and every capture after it at ~47.7 (max 255), same
config. Cold shader/PSO compilation on the first launch perturbs the settle, so
the captured frame is not the one `settle_frames` was tuned for. It was briefly
read as a 2.6x tone difference between backends -- a finding that did not exist.

Metal was byte-identical from its first run, so this is not symmetrical and you
cannot infer it from one backend looking stable. Always do a throwaway warm-up
launch after building, and run --selfcheck before believing a sweep.

WHAT IT CANNOT TELL YOU
-----------------------
A config where BOTH backends are broken the same way looks clean here. This
finds divergence, not correctness. Pair it with the must_differ_from relations
in run.py, which catch a pass that stopped running at all.
"""

import argparse, hashlib, os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import run as matrix                                    # noqa: E402
from pngdiff import read_png, diff                      # noqa: E402

BACKENDS = {"gl": 0, "metal": 3}    # vid_preferbackend; see V_GetBackend()

# Bands run top -> bottom of the frame. Enough to localise a half-frame or
# status-bar-sized displacement without drowning the output.
NBANDS = 6

# Above this max_channel_delta in a band, the two backends are not merely noisy.
# Chosen to sit well above filtering/dither noise and well below a displaced
# effect. Tune against a known-good config, not against the config under test.
BAND_MAX_DELTA_FLAG = 24

# A band whose mean delta is this multiple of the frame's median band mean is
# structurally different from the rest of the frame even if its absolute delta
# is modest. This is what catches a displaced pass.
BAND_OUTLIER_RATIO = 4.0


def launch_backend(cfg, spec, backend, verbose):
    """Run one config on one backend. Returns (png_path, log_path)."""
    forced = dict(cfg)
    forced["name"] = f"{cfg['name']}__{backend}"
    # Backend selection goes LAST so it wins over anything in the config or in
    # launch.always. vid_preferbackend is CVAR_NOINITCALL, so it must be set on
    # the command line -- setting it after startup does not re-create the
    # framebuffer.
    forced["cvars"] = list(cfg.get("cvars", [])) + [
        f"+vid_preferbackend {BACKENDS[backend]}"
    ]
    return matrix.launch(forced, spec, verbose)


def confirm_backend(log_path, backend):
    """Prove which backend actually initialised. Never infer it from the cvar.

    A failed Vulkan/Metal init silently falls back to OpenGL (i_video.mm), so a
    'metal' run that quietly became a GL run would otherwise compare GL to GL
    and report a clean pass -- the exact false-negative shape that cost a
    session on 2026-08-07.
    """
    try:
        text = open(log_path, errors="replace").read()
    except OSError:
        return None
    if "Metal renderer initialized successfully" in text:
        return "metal"
    if "Initializing OpenGL backend" in text:
        return "gl"
    if "Initializing OpenGLES2 backend" in text:
        return "gles"
    return None


def gain_match(apx, bpx):
    """Scale b's channels so its mean matches a's, and return the new buffer.

    The two backends differ by a large GLOBAL transfer-function factor -- as
    measured 2026-08-07, Metal renders the same frame ~2.6x brighter than GL
    (mean_lum 46.4 vs 17.6) with clipped highlights. That is a real finding and
    is reported separately as `tone`, but if it is left in the pixel data it
    swamps everything: every band exceeds every threshold and a genuinely
    displaced pass is indistinguishable from the background offset.

    Removing a single global gain leaves the STRUCTURE intact, which is what the
    band analysis is actually asking about. It cannot hide a displaced pass --
    that is a spatial difference, and no scalar multiply removes it.
    """
    asum = sum(apx); bsum = sum(bpx)
    if bsum == 0:
        return bpx, 1.0
    g = asum / bsum
    return bytes(min(255, int(v * g + 0.5)) for v in bpx), g


def band_report(a, b):
    """Per-band delta between two decoded frames, top to bottom."""
    (aw, ah, anch, apx), (bw, bh, bnch, bpx) = a, b
    if (aw, ah, anch) != (bw, bh, bnch):
        return None, f"size mismatch {aw}x{ah} vs {bw}x{bh}"

    # Exclude window chrome and the HUD strip, matching pngdiff's convention --
    # the HUD is 2D and identical across backends, and including it dilutes the
    # scene signal we care about.
    x0, x1 = int(aw * 0.05), int(aw * 0.95)
    ytop, ybot = int(ah * 0.09), int(ah * 0.80)

    bands = []
    step = (ybot - ytop) / NBANDS
    for i in range(NBANDS):
        by0 = int(ytop + i * step)
        by1 = int(ytop + (i + 1) * step)
        d = diff(apx, bpx, aw, ah, anch, (x0, by0, x1, by1))
        bands.append(d)
    return bands, None


def classify(bands):
    """Turn per-band numbers into the shape names from the module docstring."""
    means = sorted(b["mean_delta"] for b in bands)
    median = means[len(means) // 2]
    worst = max(bands, key=lambda b: b["mean_delta"])
    worst_i = bands.index(worst)
    hot = [i for i, b in enumerate(bands)
           if b["max_channel_delta"] > BAND_MAX_DELTA_FLAG]

    ratio = (worst["mean_delta"] / median) if median > 1e-9 else (
        float("inf") if worst["mean_delta"] > 1e-9 else 1.0)

    if not hot and ratio < BAND_OUTLIER_RATIO:
        return "OK", f"uniform backend noise (median band mean {median:.3f})"
    if ratio >= BAND_OUTLIER_RATIO:
        return "SUSPECT", (f"band {worst_i} mean {worst['mean_delta']:.3f} is "
                           f"{ratio:.1f}x the median band ({median:.3f}) -- "
                           f"localised difference, check for a vertical offset")
    return "SUSPECT", (f"bands {hot} exceed max_delta {BAND_MAX_DELTA_FLAG} "
                       f"(worst {worst['max_channel_delta']})")


def selfcheck(configs, spec, verbose):
    """Does each backend reproduce ITSELF? Run this before trusting a sweep.

    Measured 2026-08-07 during bring-up: Metal reproduced byte for byte across
    runs, GL did not -- one capture came back at mean_lum 17.6 (max 183) and the
    next at 47.7 (max 255), same config. The first cross-backend result was
    therefore read as a 2.6x tone difference between backends when it was GL
    failing to settle, most likely auto-exposure not converged within
    settle_frames on that path.

    A cross-backend oracle whose own captures are not reproducible reports its
    noise as findings. That is worse than no oracle, because the findings look
    specific. So this is a gate, not a diagnostic: if a backend fails here,
    raise launch.settle_frames (or pin exposure) until it passes, and only then
    run a sweep.
    """
    print("Self-check: two identical runs per backend. Both must be identical.\n")
    bad = 0
    for cfg in configs:
        for backend in ("gl", "metal"):
            sigs, means = [], []
            for i in (1, 2):
                c = dict(cfg)
                c["name"] = f"{cfg['name']}__selfcheck{i}"
                png, log = launch_backend(c, spec, backend, verbose)
                actual = confirm_backend(log, backend)
                if actual != backend or not png:
                    print(f"  {cfg['name']:20s} {backend:5s} run {i}: launch "
                          f"failed (log says {actual}) -- {log}")
                    bad += 1
                    sigs = None
                    break
                w, h, nch, px = read_png(png)
                sigs.append(hashlib.md5(px).hexdigest())
                means.append(sum(px) / len(px))
            if not sigs:
                continue
            if sigs[0] == sigs[1]:
                print(f"  {cfg['name']:20s} {backend:5s} REPRODUCIBLE "
                      f"(mean {means[0]:.3f})")
            else:
                bad += 1
                print(f"  {cfg['name']:20s} {backend:5s} *** NOT REPRODUCIBLE ***"
                      f"  mean {means[0]:.3f} then {means[1]:.3f}"
                      f"  (delta {abs(means[0]-means[1]):.3f})")
    print()
    if bad:
        print("Self-check FAILED. Do not run a sweep until this passes -- the\n"
              "differences it reports would be indistinguishable from capture\n"
              "noise. Raise launch.settle_frames in configs.json, or pin\n"
              "exposure, then re-run.")
    else:
        print("Self-check passed. Cross-backend results can be trusted to the\n"
              "extent that both backends reproduce themselves.")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="comma-separated config names")
    ap.add_argument("--screenblocks",
                    help="comma-separated screenblocks values to sweep each "
                         "config across (e.g. 8,10,11). The inset scene "
                         "viewport at <11 is what exposed the 2026-08-07 bug.")
    ap.add_argument("--selfcheck", action="store_true",
                    help="Run each backend TWICE on the same config and report "
                         "whether it reproduces itself. Do this before trusting "
                         "any cross-backend result -- see the determinism note.")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    spec = matrix.load_configs()

    configs = spec["configs"]
    if args.only:
        wanted = set(args.only.split(","))
        configs = [c for c in configs if c["name"] in wanted]
        if not configs:
            sys.exit("no configs matched --only")

    os.makedirs(matrix.WORKDIR, exist_ok=True)
    if args.selfcheck:
        rc = selfcheck(configs, spec, args.verbose)
        if not args.keep:
            shutil.rmtree(matrix.WORKDIR, ignore_errors=True)
        return rc

    blocks = args.screenblocks.split(",") if args.screenblocks else [None]

    os.makedirs(matrix.WORKDIR, exist_ok=True)
    rows, failures = [], 0

    for cfg in configs:
        for sb in blocks:
            c = dict(cfg)
            if sb is not None:
                c = dict(c)
                c["name"] = f"{cfg['name']}_sb{sb}"
                c["cvars"] = list(cfg.get("cvars", [])) + [f"+screenblocks {sb}"]

            imgs, ok = {}, True
            for backend in ("gl", "metal"):
                png, log = launch_backend(c, spec, backend, args.verbose)
                actual = confirm_backend(log, backend)
                if actual != backend:
                    print(f"  {c['name']:28s} {backend:5s} FAILED to initialise "
                          f"(log says: {actual}) -- {log}")
                    ok = False
                    break
                if not png:
                    print(f"  {c['name']:28s} {backend:5s} no capture -- {log}")
                    ok = False
                    break
                imgs[backend] = read_png(png)

            if not ok:
                failures += 1
                continue

            gw, gh, gnch, gpx = imgs["gl"]
            mw, mh, mnch, mpx = imgs["metal"]
            if (gw, gh, gnch) != (mw, mh, mnch):
                print(f"  {c['name']:28s} ERROR size mismatch "
                      f"{gw}x{gh} vs {mw}x{mh}")
                failures += 1
                continue

            # Report the global tone difference, then remove it so the band
            # analysis can see structure. These are two separate questions and
            # conflating them makes both unanswerable.
            matched, gain = gain_match(gpx, mpx)
            bands, err = band_report(imgs["gl"], (mw, mh, mnch, matched))
            if err:
                print(f"  {c['name']:28s} ERROR {err}")
                failures += 1
                continue

            verdict, why = classify(bands)
            tone = f"tone x{1.0/gain:.2f}" if gain else "tone n/a"
            if verdict != "OK":
                failures += 1
            rows.append((c["name"], verdict, f"{why} [{tone}]", bands))
            print(f"  {c['name']:28s} {verdict:8s} {tone:12s} {why}")

    print()
    for name, verdict, why, bands in rows:
        if verdict == "OK":
            continue
        print(f"{name}: per-band GL vs Metal (top -> bottom)")
        for i, b in enumerate(bands):
            print(f"  band {i}: mean {b['mean_delta']:8.4f}  "
                  f"max {b['max_channel_delta']:3d}  "
                  f"differing>2 {b['frac_differing']*100:6.2f}%")
        print()

    if not args.keep:
        shutil.rmtree(matrix.WORKDIR, ignore_errors=True)
    else:
        print(f"captures kept in {matrix.WORKDIR}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
