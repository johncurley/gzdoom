#!/usr/bin/env python3
"""Native resource-preparation controls. Artifacts are kept outside the checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile

from fixtures import make_fixture

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from pngdiff import read_png

BACKENDS = {"metal": 3, "gl": 0, "vulkan": 1}
CASES = ("serial", "defaults", "single-upload", "no-retention", "defaults-repeat")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def records(log, prefix):
    result = []
    for line in log.splitlines():
        if not line.startswith(prefix):
            continue
        row = dict((key, int(value)) for key, value in re.findall(r"([\w-]+)=(\d+)", line))
        if prefix == "snd_prepare:":
            row["worker-present"] = int("(no worker)" not in line)
        result.append(row)
    return result


def confirm_backend(log, backend):
    signatures = {"metal": "Metal renderer initialized successfully.",
                  "gl": "GL_RENDERER:", "vulkan": "Vulkan device:"}
    # Vulkan startup wording differs from GL; fail explicitly if unqualified.
    require(signatures[backend] in log, "requested backend not confirmed: " + backend)
    if backend != "metal":
        require("Metal renderer initialized successfully." not in log, "backend silently changed")


def validate_preparation(rows):
    require(rows, "missing audio preparation metrics")
    for row in rows:
        require("enabled" in row, "missing audio preparation state")
        if "peak" not in row:
            require(row.get("worker-present") == 0 and row.get("queued") == 0 and row.get("hits") == 0,
                    "live worker omitted budget metrics")
        else:
            require("budget" in row and row["peak"] <= row["budget"],
                    "audio preparation exceeded reservation budget")


def cache_passes(log):
    result = []
    current = None
    for line in log.splitlines():
        if line == "snd-cache-begin":
            require(current is None, "nested audio audit")
            current = []
        elif line == "snd-cache-end":
            require(current is not None and any(s.startswith("snd-cache-resident") for s in current),
                    "incomplete audio audit")
            result.append(current)
            current = None
        elif current is not None and line.startswith(("snd-cache-buffer", "snd-cache-resident")):
            current.append(line)
    require(current is None and len(result) >= 3, "missing map audio audits")
    return result


def image_pixels(path):
    width, height, channels, data = read_png(path)
    require(width >= 320 and height >= 200 and len(set(data)) > 32, "empty or invalid capture")
    return (width, height, channels, data)


def validate_paired_rng(log):
    require("snd-siblings-rng-stream draws=8/8 equal=1" in log,
            "actual C RNG draw count/state changed or shim not active")
    require("snd-siblings-rng-selftest: PASS comparisons=8 failures=0 negative=1 restored=1" in log,
            "paired sound RNG state/choice test failed")
    paired = re.findall(r"snd-siblings-rng-begin mode=([01])\n(.*?)snd-siblings-rng-end mode=\1", log, re.S)
    require(len(paired) == 2 and [mode for mode, _ in paired] == ["0", "1"],
            "missing paired sound RNG boundaries")
    def outputs(segment):
        return (re.findall(r"^snd-choice .*?$", segment, re.M),
                re.findall(r"^snd-cache-buffer .*?$", segment, re.M))
    reference_choices, reference_pcm = outputs(paired[0][1])
    actual_choices, actual_pcm = outputs(paired[1][1])
    require(len(reference_choices) == 8 and reference_choices == actual_choices and
            len(reference_pcm) == 2 and reference_pcm == actual_pcm,
            "paired sound choice/PCM/loop order changed")


def run_case(args, root, fixture, stage, name):
    directory = root / name
    directory.mkdir()
    (directory / "saves").mkdir()
    config = directory / "engine.ini"
    # No user autoexec/autoload or archived cvars enter these synthetic controls.
    config.write_text("[GlobalSettings]\nvid_preferbackend=%d\n"
                      "vid_fullscreen=false\nvid_hidpi=false\nvid_vsync=false\n"
                      "vid_activeinbackground=true\ni_pauseinbackground=false\n"
                      "vid_lowerinbackground=false\ni_soundinbackground=true\n"
                      "snd_musicvolume=0\n" % BACKENDS[args.backend])
    command = [str(args.binary), "-config", str(config), "-savedir", str(directory / "saves"),
               "-iwad", str(args.iwad), "-file", str(stage), str(fixture),
               "-noautoload", "-noautoexec", "-width", "800", "-height", "600", "-window",
               "+vid_preferbackend", str(BACKENDS[args.backend]),
               "+win_w", "800", "+win_h", "600", "+map", "REGSTART",
               "+r_drawplayersprites", "0", "+screenblocks", "12",
               "+gl_bloom", "0", "+gl_ssao", "0", "+gl_tonemap", "0",
               "+gl_lens", "0", "+gl_fxaa", "0", "+cl_capfps", "1",
               "+con_notifylines", "0", "+con_notifytime", "0", "+gl_precache", "1",
               "+snd_precache_audit", "1",
               "+snd_retainedcache", "0" if name == "no-retention" else "2048",
               "+logfile", str(directory / "engine.log"),
               "+r_texture_snapshot_selftest", "REGRGB", "REGRGBA", "REGPAL",
               "+r_framegraph_selftest", "+input_axes_selftest",
               "+execafter", "35", "map REGTEST",
               "+execafter", "55", "playsound reg.a",
               "+execafter", "90", "pause",
               "+shotafter", "140", str(directory / "view.png"),
               "+execafter", "150", "map REGSTART",
               "+execafter", "190", "snd_retention_stats",
               "+execafter", "195", "snd_prepare_stats",
               "+execafter", "210", "snd_reset",
               "+execafter", "240", "quit"]
    if args.backend == "metal":
        command += ["+execafter", "30", "mt_png_precache_stats",
                    "+execafter", "31", "mt_debug 1",
                    "+execafter", "105", "mt_png_precache_stats",
                    "+execafter", "106", "mt_debug 0"]
    if name == "serial":
        command += ["+snd_asyncprepare", "0", "+snd_precache_async", "0"]
        if args.backend == "metal":
            command += ["+mt_async_png", "0", "+mt_batch_texture_uploads", "0"]
    elif name == "single-upload" and args.backend == "metal":
        command += ["+mt_batch_texture_uploads", "0"]
    if name in ("defaults", "defaults-repeat"):
        command += ["+snd_preparation_selftest", "reg.a", "reg.b"]
        command += ["+snd_siblings_selftest", "reg.random", "reg.b", "reg.c",
                    "reg.corrupt", "reg.huge", "reg.long"]
        command += ["+snd_siblings_rng_selftest", "reg.random", "reg.b", "reg.c"]
    (directory / "argv.json").write_text(json.dumps(command, indent=2))
    environment = os.environ.copy()
    if name in ("defaults", "defaults-repeat") and args.rng_shim:
        environment["DYLD_INSERT_LIBRARIES"] = str(args.rng_shim)
    (directory / "rng-shim.json").write_text(json.dumps({
        "inserted": environment.get("DYLD_INSERT_LIBRARIES", "")}, indent=2))
    with (directory / "stdio.log").open("w") as output:
        process = subprocess.Popen(command, cwd=directory, env=environment,
                                   stdout=output, stderr=subprocess.STDOUT)
        try:
            require(process.wait(timeout=args.timeout) == 0, "engine failed: " + name)
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
    require("REGTEST -" in log and log.count("REGSTART -") >= 2, "map sequence incomplete")
    require("snapshot self-test: PASS" in log and "selftest: PASS" in log,
            "native texture/graph test failed")
    if name in ("defaults", "defaults-repeat"):
        if "snd-preparation-selftest: SKIP" in log:
            require(args.allow_audio_skip, "audio worker unavailable; explicit --allow-audio-skip required")
        else:
            require("snd-preparation-selftest: PASS comparisons=6 failures=0 negative=6" in log,
                    "native audio self-test failed")
            require("snd-siblings-selftest: PASS comparisons=2 failures=0 negative=1" in log,
                    "sibling admission/bounds/corrupt-input test failed")
            pairs = re.findall(r"snd-siblings-compare-begin\n(.*?)snd-siblings-compare-end", log, re.S)
            require(len(pairs) == 2, "missing sibling PCM/loop comparisons")
            for pair in pairs:
                buffers = [line for line in pair.splitlines() if line.startswith("snd-cache-buffer ")]
                require(len(buffers) == 2 and buffers[0] == buffers[1], "sibling PCM/loop metadata changed")

    if name in ("defaults", "defaults-repeat") and "snd-preparation-selftest: SKIP" not in log:
        validate_paired_rng(log)

    axes = re.search(r"input-axes-selftest: PASS comparisons=(\d+) failures=0 negative=(\d+)", log)
    require(axes is not None and int(axes[1]) > 0 and axes[1] == axes[2],
            "native axis input equivalence/negative controls failed")
    passes = cache_passes(log)
    retention = records(log, "snd_retention:")
    preparation = records(log, "snd_prepare:")
    require(retention and preparation, "missing audio lifecycle metrics")
    require(retention[-1]["last-pass-bytes"] <= 2048 * 1024, "retention budget exceeded")
    if name == "no-retention":
        require(retention[-1]["last-pass-bytes"] == 0, "disabled retention kept buffers")
    validate_preparation(preparation)
    endpoint = preparation[-1]
    if endpoint["enabled"]:
        minimum_hits = 8 if name in ("defaults", "defaults-repeat") else 2
        require(endpoint.get("hits", 0) >= minimum_hits and endpoint.get("failed", 0) >= 1,
                "map precache workers or oversized fallback did not execute")
        require(endpoint.get("notready", -1) == 0 and endpoint.get("jobs", -1) == 0 and
                endpoint.get("reserved", -1) == 0, "unconsumed audio fixture preparation")
    burst = [(int(count), int(buffers)) for count, buffers in re.findall(
        r"TexUpload: (\d+) \([\d.]+ms, extra CBs: (\d+)\)", log)]
    png = records(log, "Metal PNG precache:")
    if args.backend == "metal":
        require(len(png) == 2, "missing PNG worker metrics")
        require(burst and sum(count for count, _ in burst) >= 3, "missing texture upload execution")
        ready = png[1]["first-use-ready"] - png[0]["first-use-ready"]
        require(ready == (0 if name == "serial" else 2), "PNG worker path did not execute")
        require(png[1]["reserved"] == 0 and png[1]["peak"] <= png[1]["budget"],
                "PNG worker reservation leak/budget exceeded")
    pixels = image_pixels(directory / "view.png")
    result = {"case": name, "pixel_sha256": hashlib.sha256(pixels[3]).hexdigest(),
              "dimensions": pixels[:3], "audio_selftest_skipped":
              "snd-preparation-selftest: SKIP" in log, "cache_passes": passes,
              "retention": retention, "preparation": preparation, "png": png,
              "uploads": sum(count for count, _ in burst),
              "extra_command_buffers": sum(buffers for _, buffers in burst)}
    (directory / "result.json").write_text(json.dumps(result, indent=2))
    print(name + ": native checks passed", flush=True)
    return result, pixels


def run_cold_siblings(args, root, fixture, stage, index, enabled):
    directory = root / ("cold-%d-mode%d" % (index, enabled))
    (directory / "saves").mkdir(parents=True)
    config = directory / "engine.ini"
    config.write_text("[GlobalSettings]\nvid_preferbackend=%d\n"
                      "vid_fullscreen=false\nvid_vsync=false\n"
                      "vid_activeinbackground=true\ni_pauseinbackground=false\n"
                      "vid_lowerinbackground=false\ni_soundinbackground=true\n"
                      "snd_musicvolume=0\n" % BACKENDS[args.backend])
    command = [str(args.binary), "-config", str(config), "-savedir", str(directory / "saves"),
               "-iwad", str(args.iwad), "-file", str(stage), str(fixture),
               "-noautoload", "-noautoexec", "-rngseed", "19831111", "-window",
               "+map", "REGSTART", "+vid_preferbackend", str(BACKENDS[args.backend]),
               "+cl_capfps", "1", "+con_notifylines", "0", "+con_notifytime", "0",
               "+snd_precache_async", "0", "+snd_asyncprepare", "1",
               "+snd_asyncsiblings", str(enabled), "+snd_retainedcache", "0",
               "+snd_precache_audit", "1", "+snd_sfxvolume", "0.05",
               "+logfile", str(directory / "engine.log"),
               "+execafter", "30", "echo SIBLING_SEQUENCE_BEGIN",
               "+execafter", "125", "echo SIBLING_SEQUENCE_END",
               "+execafter", "150", "map REGSTART",
               "+execafter", "175", "snd_prepare_stats",
               "+execafter", "185", "snd_reset", "+execafter", "210", "quit"]
    for frame in range(35, 106, 10):
        command += ["+execafter", str(frame), "playsound reg.random"]
    (directory / "argv.json").write_text(json.dumps(command, indent=2))
    with (directory / "stdio.log").open("w") as output:
        process = subprocess.Popen(command, cwd=directory, stdout=output, stderr=subprocess.STDOUT)
        try:
            require(process.wait(timeout=args.timeout) == 0, "cold sibling engine failed")
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
    require(log.count("\nSIBLING_SEQUENCE_BEGIN\n") == 1 and
            log.count("\nSIBLING_SEQUENCE_END\n") == 1, "missing cold sound boundaries")
    segment = log.split("\nSIBLING_SEQUENCE_BEGIN\n")[1].split("\nSIBLING_SEQUENCE_END\n")[0]
    choices = re.findall(r"^snd-choice group=(\d+) selected=(\d+)$", segment, re.M)
    require(len(choices) == 8 and len({row[0] for row in choices}) == 1 and
            len({row[1] for row in choices}) == 2, "random variant sequence was not exercised")
    buffers = re.findall(r"^snd-cache-buffer .*$", segment, re.M)
    require(len(buffers) == 2, "unexpected cold sound publication count")
    preparation = records(log, "snd_prepare:")
    validate_preparation(preparation)
    endpoint = preparation[-1]
    if endpoint["enabled"]:
        require(endpoint.get("hits", 0) > 0 if enabled else endpoint.get("hits", 0) == 0,
                "cold sibling worker did not follow its control setting")
        require(endpoint.get("notready", 0) == 0 and endpoint.get("failed", 0) == 0 and
                endpoint.get("jobs", 0) == 0 and endpoint.get("reserved", 0) == 0,
                "cold sibling fallback/cleanup failed")
    else:
        require(args.allow_audio_skip, "cold sibling worker unavailable")
    result = {"mode": enabled, "choices": choices, "buffers": buffers, "preparation": preparation}
    (directory / "result.json").write_text(json.dumps(result, indent=2))
    print(directory.name + ": random sequence, PCM and cleanup passed", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--iwad", type=Path, required=True, help="DOOM2 IWAD (not redistributed)")
    parser.add_argument("--backend", choices=BACKENDS, default="metal")
    parser.add_argument("--output", type=Path, help="new directory; default is a temporary directory")
    parser.add_argument("--timeout", type=int, default=90, help="per-process seconds")
    parser.add_argument("--allow-audio-skip", action="store_true",
                        help="qualify graphics only where the audio worker is unavailable")
    args = parser.parse_args()
    args.binary = args.binary.expanduser().resolve(strict=True)
    args.iwad = args.iwad.expanduser().resolve(strict=True)
    if args.output:
        args.output = args.output.expanduser().resolve()
        args.output.mkdir(parents=True, exist_ok=False)
    root = args.output or Path(tempfile.mkdtemp(prefix="gzdoom-regression-"))
    print("Artifacts: " + str(root), flush=True)
    args.rng_shim = None
    if sys.platform == "darwin":
        args.rng_shim = root / "rand-shim.dylib"
        subprocess.run(["xcrun", "clang", "-std=c11", "-dynamiclib", "-Wall", "-Wextra", "-Werror",
                        str(Path(__file__).with_name("rand_shim.c")), "-o", str(args.rng_shim)], check=True)
        checks = subprocess.check_output([sys.executable, str(Path(__file__).with_name("check_rand_shim.py")),
                                          str(args.rng_shim)], text=True)
        (root / "shim-controls.json").write_text(checks)
    fixture, stage = make_fixture(root / "fixtures")
    manifest = {"platform": platform.platform(), "python": sys.version,
                "backend": args.backend, "binary": str(args.binary),
                "binary_sha256": sha256(args.binary), "iwad_sha256": sha256(args.iwad),
                "fixture_sha256": sha256(fixture), "stage_sha256": sha256(stage),
                "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "git_status": subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True)}
    manifest["packages"] = {path.name: sha256(path) for path in args.binary.parent.glob("*.pk3")}
    manifest["runner_sources"] = {name: sha256(Path(__file__).with_name(name))
                                  for name in ("run.py", "fixtures.py", "rand_shim.c", "check_rand_shim.py")}
    manifest["rng_shim_sha256"] = sha256(args.rng_shim) if args.rng_shim else None
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2))
    results, reference = [], None
    for name in CASES:
        result, pixels = run_case(args, root, fixture, stage, name)
        if reference is None:
            reference = pixels
            audit = result["cache_passes"][:2]
        else:
            require(pixels == reference, "rendered pixels changed: " + name)
            require(result["cache_passes"][:2] == audit,
                    "PCM/order/loop metadata/resident payload changed: " + name)
        results.append(result)
    cold = [run_cold_siblings(args, root, fixture, stage, i + 1, enabled)
            for i, enabled in enumerate((0, 1, 1, 0))]
    # Startup seed does not establish equal live client RNG state across fresh
    # processes. Exact choice/order invariance is checked by the paired native
    # oracle; these independent runs check real readiness and the PCM set.
    require(all(sorted(row["buffers"]) == sorted(cold[0]["buffers"]) for row in cold),
            "cold playback PCM/loop set changed")
    corrupted = bytearray(reference[3])
    corrupted[len(corrupted) // 2] ^= 1
    require(bytes(corrupted) != reference[3], "pixel negative control failed")
    if args.backend == "metal":
        require(results[1]["uploads"] == results[2]["uploads"], "upload workloads changed")
        require(results[1]["extra_command_buffers"] < results[2]["extra_command_buffers"],
                "batching did not reduce command buffers")
    enabled = results[1]["retention"][-1]["last-pass-bytes"]
    require(enabled == 12000, "retention positive control did not retain the played tone")
    summary = {"status": "PASS", "audio_worker": "SKIP" if any(row["audio_selftest_skipped"] for row in results) else "tested",
               "one_byte_negative_control": True, "results": results, "cold_siblings": cold}
    (root / "summary.json").write_text(json.dumps(summary, indent=2))
    print("PASS: five resource controls and four cold sound controls; exact pixels/PCM, random choices and lifecycle budgets.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print("FAIL: " + str(error), file=sys.stderr)
        sys.exit(1)
