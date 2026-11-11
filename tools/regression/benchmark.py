#!/usr/bin/env python3
"""Interleaved first-visit/warm reload timing using caller-owned assets and save."""
import argparse
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

from fixtures import room
from run import BACKENDS, ROOT, confirm_backend, require, sha256, validate_preparation, records

CONTROLS = {"sound-precache": "snd_precache_async", "sound-siblings": "snd_asyncsiblings",
            "png": "mt_async_png",
            "texture-batching": "mt_batch_texture_uploads"}
MARKERS = ("REG_FIRST", "REG_WARM", "REG_FINISH")


def timing_windows(text):
    """Reject missing/duplicate markers; return maxima in two distinct windows."""
    positions = []
    for marker in MARKERS:
        matches = list(re.finditer(r"^" + marker + r"\r?$", text, re.M))
        require(len(matches) == 1, "missing or duplicate timing marker: " + marker)
        positions.append(matches[0].end())
    require(positions == sorted(positions), "timing markers out of order")
    result = {}
    for key, start, end in zip(("first_ms", "warm_ms"), positions, positions[1:]):
        samples = [float(value) for value in re.findall(
            r"vid_frametrace .*?max=\s*([\d.]+)", text[start:end])]
        require(samples and all(value > 0 for value in samples), "missing timing windows: " + key)
        result[key] = max(samples)
        result[key + "_windows"] = samples
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--iwad", type=Path, required=True)
    parser.add_argument("--file", type=Path, action="append", default=[], help="repeat in exact mod load order")
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--map", required=True, help="expected map identifier in the save, e.g. MAP09")
    parser.add_argument("--config", type=Path, required=True, help="copied for each run; never overwritten")
    parser.add_argument("--backend", choices=BACKENDS, default="metal")
    parser.add_argument("--control", choices=CONTROLS, default="sound-precache")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--prediction", required=True, help="record numeric prediction before measuring")
    args = parser.parse_args()
    require(args.backend == "metal" or args.control == "sound-precache",
            "PNG and texture batching controls require Metal")
    for name in ("binary", "iwad", "save", "config"):
        setattr(args, name, getattr(args, name).expanduser().resolve(strict=True))
    args.file = [path.expanduser().resolve(strict=True) for path in args.file]
    require(re.fullmatch(r"[A-Za-z0-9_]+", args.map), "invalid map identifier")
    if args.output:
        root = args.output.expanduser().resolve()
        root.mkdir(parents=True, exist_ok=False)
    else:
        root = Path(tempfile.mkdtemp(prefix="gzdoom-benchmark-"))
    print("Artifacts: " + str(root), flush=True)
    manifest = {"prediction": args.prediction, "backend": args.backend, "control": args.control,
                "map": args.map, "order": [0, 1, 1, 0, 0, 1],
                "assets": [{"path": str(path), "sha256": sha256(path)}
                           for path in [args.binary, args.iwad, args.config, args.save] + args.file],
                "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "git_status": subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True)}
    import platform
    manifest["platform"] = platform.platform()
    manifest["packages"] = {path.name: sha256(path) for path in args.binary.parent.glob("*.pk3")}
    manifest["runner_sources"] = {name: sha256(Path(__file__).with_name(name))
                                  for name in ("benchmark.py", "run.py", "fixtures.py")}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2))
    stage = root / "stage.wad"
    stage.write_bytes(room("REGSTART"))
    rows = []
    for index, enabled in enumerate(manifest["order"]):
        directory = root / ("%d-mode%d" % (index + 1, enabled))
        (directory / "saves").mkdir(parents=True)
        shutil.copyfile(args.save, directory / "saves/start.zds")
        config = args.config.read_text()
        # Pin launch/background behavior in this private copy.
        pins = {"vid_preferbackend": str(BACKENDS[args.backend]), "i_pauseinbackground": "false",
                "i_soundinbackground": "true", "vid_activeinbackground": "true",
                "vid_lowerinbackground": "false"}
        for key, value in pins.items():
            config, count = re.subn(r"^" + key + r"=.*$", key + "=" + value, config, flags=re.M)
            if not count:
                require("[GlobalSettings]" in config, "config has no GlobalSettings")
                config = config.replace("[GlobalSettings]", "[GlobalSettings]\n" + key + "=" + value, 1)
        (directory / "engine.ini").write_text(config)
        command = [str(args.binary), "-config", str(directory / "engine.ini"),
                   "-savedir", str(directory / "saves"), "-iwad", str(args.iwad), "-noautoload", "-noautoexec"]
        command += ["-file"] + [str(path) for path in args.file] + [str(stage)]
        command += ["-window", "-width", "1024", "-height", "768",
                    "+map", "REGSTART", "+cl_capfps", "1", "+vid_dontdowait", "0",
                    "+vid_profilemarks", "0", "+vid_frametrace", "0", "+vid_stalltrace", "0",
                    "+snd_precache_audit", "0", "+con_notifylines", "0", "+con_notifytime", "0",
                    "+" + CONTROLS[args.control], str(enabled),
                    "+logfile", str(directory / "engine.log"),
                    "+execafter", "35", "vid_frametrace 1"]
        if args.control in ("sound-precache", "sound-siblings"):
            command += ["+snd_asyncprepare", "1"]
        if args.control == "sound-siblings":
            command += ["+snd_precache_async", "1", "+snd_retainedcache", "0"]
        for frame, marker, action in ((105, "REG_FIRST", "load start.zds"),
                                       (220, "REG_WARM", "load start.zds"),
                                       (335, "REG_FINISH", "quit")):
            script = directory / (marker.lower() + ".cfg")
            script.write_text("echo " + marker + "\nsnd_prepare_stats\n" + action + "\n")
            command += ["+execafter", str(frame), "exec " + str(script)]
        (directory / "argv.json").write_text(json.dumps(command, indent=2))
        with (directory / "stdio.log").open("w") as output:
            process = subprocess.Popen(command, cwd=directory, stdout=output, stderr=subprocess.STDOUT)
            try:
                require(process.wait(timeout=args.timeout) == 0, "engine failed")
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
        log = (directory / "engine.log").read_text(errors="replace")
        confirm_backend(log, args.backend)
        stdio = (directory / "stdio.log").read_text(errors="replace")
        require(len(re.findall(r"^" + re.escape(args.map) + r" -", stdio, re.M)) == 2,
                "save did not load the expected map twice")
        row = {"mode": enabled, **timing_windows(stdio)}
        preparation = records(log, "snd_prepare:")
        require(len(preparation) == 3, "missing audio preparation checkpoints")
        validate_preparation(preparation)
        if args.control == "sound-precache" and enabled:
            require(preparation[1].get("hits", 0) > preparation[0].get("hits", 0),
                    "sound precache control did not consume worker results")
        row["audio_checkpoints"] = preparation
        rows.append(row)
        print(json.dumps(row), flush=True)
    summary = {str(mode): {key: {
        "median": statistics.median(row[key] for row in rows if row["mode"] == mode),
        "range": [min(row[key] for row in rows if row["mode"] == mode),
                  max(row[key] for row in rows if row["mode"] == mode)]}
        for key in ("first_ms", "warm_ms")} for mode in (0, 1)}
    (root / "summary.json").write_text(json.dumps({"rows": rows, "summary": summary}, indent=2))
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print("FAIL: " + str(error), file=sys.stderr)
        sys.exit(1)
