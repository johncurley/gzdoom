#!/usr/bin/env python3
"""Golden-image regression matrix for the postprocess chain.

    python3 tools/matrix/run.py                     # check against the baseline
    python3 tools/matrix/run.py --update-baseline   # record a new baseline
    python3 tools/matrix/run.py --only ssao,lens    # a subset
    python3 tools/matrix/run.py --list

WHY THIS EXISTS
---------------
This branch is validated by "build it, load a WAD, look at it", so the tested
surface has always equalled exactly whatever the dev config happened to enable.
Passes behind a default-off cvar had never had a single pixel checked. Five were
broken: tonemap, lens and colormap rendered upside down, ssao composited
nothing, and custom postprocess shaders never executed at all. Each was found by
hand, months after it broke, and each took most of a session.

Every one of them would have been caught by this script on the day it broke.

WHAT MAKES IT MORE THAN A HASH COMPARISON
-----------------------------------------
A golden-image test alone cannot tell "the pass is correct" from "the pass never
ran" -- both reproduce the baseline byte for byte. That ambiguity IS the bug
class this branch keeps hitting, so it is encoded directly:

  must_differ_from   the config this one must NOT be pixel-identical to.
                     A collapse means the effect stopped taking effect, even if
                     it still matches its own stored baseline.

  must_match         for mathematical identities (gl_tonemap 4 is sqrt(c*c)),
                     the config this one must reproduce EXACTLY. Only meaningful
                     when some sibling config carries a must_differ_from proving
                     the same pass can change pixels at all.

**--update-baseline refuses to record if any of those relations fails.** You
cannot enshrine a broken pass as the golden image, which is the single most
valuable property here -- a baseline recorded while ssao was silently a no-op
would have made the regression permanent and invisible.

DETERMINISM, WHICH IS LOAD-BEARING
----------------------------------
Every launch gets `+cl_capfps 1`. Without it the reference viewpoint is NOT
reproducible: animated content advances per tic, and the tics elapsed by a given
rendered frame depend on frame rate, so two identical launches differed by a
mean 1.7 lum over 98% of the frame. With it they are byte-identical.

Captures are unattended via `+shotafter N quit`, and any config needing game
STATE rather than a cvar (the colormap palette flash) uses `+execafter`. Both
count only in-level frames, so the settle is identical in every launch.

`mt_caps` is run in every launch and its output kept in the log, so "which
config was this file?" is a grep rather than an inference.
"""

import argparse, hashlib, json, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from pngdiff import read_png, stats, diff  # noqa: E402

BASELINE = os.path.join(HERE, "baseline.json")
WORKDIR = "/tmp/gzdoom-matrix"
# The harness gets its OWN config file. Without this every launch reads and
# rewrites the user's gzdoom.ini, so an archived CVAR set by one config leaks
# into every later run -- and into their actual game. That silently invalidated
# a Metal-vs-OpenGL comparison and disabled the user's bloom during this
# branch's history. All settings that matter are pinned in configs.json
# "always", so this file starting empty is fine.
CONFIG = os.path.join(WORKDIR, "matrix.ini")
# Captures land here, not in the engine's default screenshot directory. That
# default is platform-specific (~/Documents/GZDoom/Screenshots on macOS, the XDG
# config dir on Linux), and hardcoding either means the harness reports
# NO CAPTURE for every config on the other platform. `screenshot_dir` is a CVAR,
# so the launch pins it and both platforms behave identically.
#
# This supersedes an earlier per-platform default plus a `launch.screenshots`
# override in configs.json. Pinning the CVAR is strictly better -- there is one
# path, on every platform, and it is inside WORKDIR with the rest of the run --
# so the override key is gone and nothing should reintroduce it.
SHOTS = os.path.join(WORKDIR, "shots")

# sys.platform -> the configs.json key suffix holding that platform's overrides.
# Still needed for the paths that genuinely do differ per machine: the binary
# and the mod files. Not the screenshot directory -- see SHOTS above.
PLATFORM_KEY = "darwin" if sys.platform == "darwin" else (
    "linux" if sys.platform.startswith("linux") else sys.platform)


def platform_launch(spec, base="launch"):
    """`launch` merged with any `launch_<platform>` block.

    The macOS values stay the defaults so nothing about that machine changes;
    a platform block overrides only the keys that genuinely differ (binary
    path, mod directory). The screenshot directory used to be one of these and
    is not any more -- the launch pins it as a CVAR, see SHOTS.
    """
    L = dict(spec.get(base, {}))
    L.update(spec.get(f"{base}_{PLATFORM_KEY}", {}))
    return L


def apply_scene(spec, scene):
    """Merge a named entry from configs.json "scenes" over spec["launch"].

    A scene is what the harness points the camera at. The default one uses a
    third-party mod, which is fine for the machines that have it and useless
    everywhere else, so the stock-IWAD scenes exist to give the suite something
    it can always run -- including on a machine that has only DOOM2.wad.
    """
    if not scene:
        return spec
    scenes = spec.get("scenes", {})
    if scene not in scenes:
        sys.exit(f"unknown scene {scene!r}; have: {', '.join(sorted(scenes))}")
    s = dict(scenes[scene])
    s.update(s.pop(PLATFORM_KEY, {}))     # per-platform paths inside the scene
    spec = dict(spec)
    L = dict(spec["launch"])
    # A scene replaces the whole way the level is reached, so clear both routes
    # before applying it -- otherwise the default savegame would survive into a
    # scene that wants +map and silently win.
    L.pop("savegame", None)
    L.pop("map", None)
    # "always_extra" appends; a scene setting "always" outright would replace the
    # pinned determinism cvars (cl_capfps above all) with whatever it listed, and
    # the loss would be silent. Scene-specific cvars are the common case anyway --
    # a stock scene needs screenblocks pinned, the mod scene does not.
    extra = s.pop("always_extra", [])
    L.update(s)
    L["always"] = list(L.get("always", [])) + list(extra)
    spec["launch"] = L
    return spec


def load_configs(scene=None):
    with open(os.path.join(HERE, "configs.json")) as f:
        spec = json.load(f)
    spec = dict(spec)
    spec["launch"] = platform_launch(spec)
    return apply_scene(spec, scene)


def launch(cfg, spec, verbose):
    """One configuration, one process, no operator. Returns (png_path, log_path)."""
    L = spec["launch"]
    # GZDOOM_MATRIX_BINARY overrides the configured path, so a second build
    # directory (a Vulkan-enabled one, say) can be swept without editing a
    # file that is shared with the other platforms.
    binary = os.path.join(ROOT, os.environ.get("GZDOOM_MATRIX_BINARY",
                                               L["binary"]))
    if not os.path.exists(binary):
        sys.exit(f"binary not found: {binary}\n"
                 f"Build first: cmake --build "
                 f"{os.path.dirname(L['binary']).split('/')[0] or 'build'} "
                 f"--target zdoom -j {os.cpu_count()}")
    os.makedirs(SHOTS, exist_ok=True)

    # -nolauncher because an unattended run must never stop at a window waiting
    # to be clicked. -iwad already suppresses the launcher today, so this is
    # belt and braces -- but it is the cheap kind: the launcher appears whenever
    # more than one IWAD is installed and the -iwad name fails to resolve, and
    # the failure mode is the harness hanging until its timeout with no output
    # that says why.
    argv = [binary, "-nolauncher", "-iwad", L["iwad"], "-config", CONFIG,
            "+screenshot_dir", SHOTS]
    for f in L.get("files", []) + cfg.get("extra_files", []):
        argv += ["-file", os.path.expanduser(f)]
    # A scene is reached either by loading a savegame or by warping to a map.
    # `map` exists so a scene can be built from a stock IWAD alone, with no
    # savegame file to carry between machines -- see the "scenes" block in
    # configs.json. savegame wins if a scene somehow sets both.
    if L.get("savegame"):
        argv += ["-loadgame", L["savegame"]]
    elif L.get("map"):
        argv += ["+map", L["map"]]

    for tok in L.get("always", []):
        argv += tok.split()
    for tok in cfg.get("cvars", []):
        argv += tok.split()

    settle = L.get("settle_frames", 120)
    # mt_caps well before the shot so the resolved path is in the log for the
    # frame that was actually captured.
    argv += ["+execafter", str(max(settle - 40, 10)), "mt_caps"]
    for frames, command in cfg.get("exec", []):
        argv += ["+execafter", str(frames)] + command.split()
    argv += ["+shotafter", str(settle), "quit"]

    log_path = os.path.join(WORKDIR, cfg["name"] + ".log")
    if verbose:
        print("   " + " ".join(argv[1:]))
    with open(log_path, "w") as log:
        subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)

    text = open(log_path, errors="replace").read()
    m = re.findall(r"^Captured (.+)$", text, re.M)
    if not m:
        return None, log_path
    src = os.path.join(SHOTS, m[-1].strip())
    dst = os.path.join(WORKDIR, cfg["name"] + ".png")
    if not os.path.exists(src):
        return None, log_path
    shutil.move(src, dst)
    return dst, log_path


def signature(path):
    """Hash the DECODED PIXELS, never the file bytes.

    GZDoom's PNG writer embeds a timestamp, so two captures of an identical
    frame have different file md5s. Hashing the file would report a spurious
    DIFF on every single run and train the reader to ignore the output -- the
    worst possible failure mode for a regression suite. Verified: captures with
    differing file md5s compare at max_channel_delta 0.
    """
    w, h, nch, px = read_png(path)
    s = stats(w, h, nch, px)
    return {
        "pixels_md5": hashlib.md5(px).hexdigest(),
        "w": w, "h": h,
        "mean_lum": round(s["mean"], 4),
    }, (w, h, nch, px)


def compare(a, b):
    """Pixel comparison between two decoded images."""
    (aw, ah, anch, apx), (bw, bh, bnch, bpx) = a, b
    if (aw, ah, anch) != (bw, bh, bnch):
        return None
    return diff(apx, bpx, aw, ah, anch)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--only", help="comma-separated config names")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep captures in " + WORKDIR)
    ap.add_argument("--scene", help="named entry from configs.json \"scenes\"; "
                                    "omit for the default (mod-based) scene")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    spec = load_configs(args.scene)
    configs = spec["configs"]
    # Signatures are only comparable within one scene, so every baseline is
    # stamped with the scene that produced it and SCENE is what that stamp is
    # checked against. The default scene predates the stamp, hence its name
    # doubling as the value assumed for an unstamped baseline.
    scene = args.scene or "default"

    if args.list:
        for c in configs:
            rel = ""
            if c.get("must_differ_from"):
                rel = f"  (must differ from {c['must_differ_from']})"
            elif c.get("must_match"):
                rel = f"  (must MATCH {c['must_match']})"
            opt = "  [optional]" if c.get("optional") else ""
            print(f"  {c['name']:<22}{rel}{opt}")
            print(f"      {c.get('note','')}")
        return 0

    wanted = set(args.only.split(",")) if args.only else None
    if wanted:
        # Relations need their partner captured too, even if not requested.
        need = set(wanted)
        for c in configs:
            if c["name"] in wanted:
                for k in ("must_differ_from", "must_match"):
                    if c.get(k):
                        need.add(c[k])
        configs = [c for c in configs if c["name"] in need]
    else:
        configs = [c for c in configs
                   if not c.get("optional")
                   or all(os.path.exists(os.path.expanduser(f))
                          for f in c.get("extra_files", []))]
        skipped = [c["name"] for c in spec["configs"]
                   if c.get("optional") and c not in configs]
        if skipped:
            print(f"skipping optional configs (missing -file): {', '.join(skipped)}")
            print("  (run tools/pptest/make.py to enable the custom-PP ones)\n")

    os.makedirs(WORKDIR, exist_ok=True)

    images, sigs = {}, {}

    # One throwaway launch before the real ones. It settles two things that
    # otherwise corrupt the FIRST config only, which is the hardest kind of
    # failure to read: the harness config file (created fresh, so the window
    # is sized before the pinned win_w/win_h are applied -- this showed up as
    # "image geometry differs" on baseline alone), and cold shader compilation,
    # which perturbs the exposure settle for one launch after a rebuild.
    print("warmup launch (discarded)")
    launch(configs[0], spec, args.verbose)

    print(f"running {len(configs)} configurations, one launch each\n")
    for i, cfg in enumerate(configs, 1):
        name = cfg["name"]
        t0 = time.time()
        print(f"[{i}/{len(configs)}] {name} ...", end="", flush=True)
        png, log = launch(cfg, spec, args.verbose)
        if not png:
            print(f" NO CAPTURE  (see {log})")
            sigs[name] = None
            continue
        sig, img = signature(png)
        sigs[name], images[name] = sig, img
        print(f" {sig['pixels_md5'][:8]}  mean_lum={sig['mean_lum']:7.3f}  ({time.time()-t0:.0f}s)")

    # ---- relations first. These are what make the baseline trustworthy. ----
    print("\nrelations (the check a hash comparison cannot make):")
    rel_fail = []
    for cfg in configs:
        name = cfg["name"]
        for kind in ("must_differ_from", "must_match"):
            other = cfg.get(kind)
            if not other:
                continue
            if name not in images or other not in images:
                print(f"  ?? {name}: {kind} {other} -- missing capture")
                rel_fail.append(name)
                continue
            d = compare(images[name], images[other])
            if d is None:
                print(f"  !! {name}: {kind} {other} -- image geometry differs")
                rel_fail.append(name)
                continue
            identical = d["max_channel_delta"] == 0
            if kind == "must_differ_from":
                if identical or d["px_differing_gt2"] == 0:
                    print(f"  FAIL {name} is PIXEL-IDENTICAL to {other}. "
                          f"The effect is not taking effect.")
                    rel_fail.append(name)
                else:
                    print(f"  ok   {name} differs from {other}: "
                          f"{d['frac_differing']*100:.2f}% of px, "
                          f"max delta {d['max_channel_delta']}")
            else:
                if identical:
                    print(f"  ok   {name} reproduces {other} exactly (identity holds)")
                else:
                    print(f"  FAIL {name} should equal {other} but differs: "
                          f"{d['frac_differing']*100:.2f}% of px, "
                          f"max delta {d['max_channel_delta']}")
                    rel_fail.append(name)

    # ---- baseline ----
    if args.update_baseline:
        if rel_fail:
            print("\nREFUSING to record a baseline: relations failed for "
                  + ", ".join(sorted(set(rel_fail))))
            print("A baseline recorded now would enshrine a broken pass as correct,")
            print("and every future run would agree with it. Fix, then re-record.")
            return 2
        old = {}
        if os.path.exists(BASELINE):
            prev = json.load(open(BASELINE))
            # Merging across scenes would leave one file holding signatures of
            # two different viewpoints, indistinguishable from each other and
            # both wrong for whichever scene ran next. A scene change replaces.
            if prev.get("scene", "default") == scene:
                old = prev.get("configs", {})
            else:
                print(f"\nscene changed ({prev.get('scene', 'default')} -> {scene}); "
                      "replacing the baseline rather than merging into it")
        old.update({k: v for k, v in sigs.items() if v})
        with open(BASELINE, "w") as f:
            json.dump({"note": "Golden-image signatures. See tools/matrix/run.py.",
                       "recorded": time.strftime("%Y-%m-%d %H:%M"),
                       "scene": scene,
                       "configs": old}, f, indent=2, sort_keys=True)
        print(f"\nbaseline recorded for {len(sigs)} configurations -> {BASELINE}")
        return 0

    if not os.path.exists(BASELINE):
        print("\nno baseline yet. Record one with --update-baseline.")
        return 1 if rel_fail else 0

    stored = json.load(open(BASELINE))
    base_scene = stored.get("scene", "default")
    if base_scene != scene:
        # Comparing anyway would report a DIFF for every config, which reads as
        # a catastrophic regression and is really just a different viewpoint.
        # The relations above are self-contained and still carry their signal.
        print(f"\nbaseline was recorded on scene {base_scene!r}, this run is "
              f"{scene!r} -- skipping the baseline comparison.")
        print("  Signatures from two scenes are not comparable. Relations above "
              "still apply.")
        print("\n" + ("PASS (relations only)" if not rel_fail else "FAIL"))
        if rel_fail:
            print("  broken relations:    " + ", ".join(sorted(set(rel_fail))))
        return 1 if rel_fail else 0

    base = stored["configs"]
    print("\nagainst baseline:")
    drift = []
    for cfg in configs:
        name = cfg["name"]
        if name not in sigs or not sigs[name]:
            continue
        if name not in base:
            print(f"  --   {name}: no baseline entry (new config)")
            continue
        if sigs[name]["pixels_md5"] == base[name]["pixels_md5"]:
            print(f"  ok   {name}")
            continue
        # Report magnitude, not just inequality -- a driver update moving one
        # channel by 1 and a pass vanishing are both "hash differs".
        b_png = os.path.join(WORKDIR, name + ".png")
        note = (f"mean_lum {base[name]['mean_lum']:.3f} -> "
                f"{sigs[name]['mean_lum']:.3f}")
        print(f"  DIFF {name}: {note}   (capture: {b_png})")
        drift.append(name)

    ok = not rel_fail and not drift
    print("\n" + ("PASS" if ok else "FAIL"))
    if drift:
        print("  changed vs baseline: " + ", ".join(drift))
        print("  If the change is intended, re-record with --update-baseline.")
    if rel_fail:
        print("  broken relations:    " + ", ".join(sorted(set(rel_fail))))
    if not args.keep:
        print(f"\ncaptures kept in {WORKDIR} for inspection "
              f"(tools/localize.py shows where two differ)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
