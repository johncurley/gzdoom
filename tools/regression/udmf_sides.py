#!/usr/bin/env python3
"""Exercise UDMF source-side bounds through real map loading and ZScript inspection."""
import argparse
import json
from pathlib import Path
import platform
import re
import struct
import subprocess
import tempfile
import zipfile

from fixtures import room
from run import BACKENDS, confirm_backend, sha256


CASES = {
    "sparse": (8, 7, -1),
    "shared": (1, 0, -1),
    "oversized": (1, 0, 1),
    "negative": (1, 0, -2),
    "int-min": (1, 0, -2147483648),
    "int-max": (1, 0, 2147483647),
    "absent": (5, 0, -1),
}


def make_fixture(path, case):
    count, front, back = CASES[case]
    data = room("SIDECHK")
    _, size, _ = struct.unpack_from("<ii8s", data, struct.unpack_from("<i", data, 8)[0] + 16)
    text = data[12:12 + size].decode()
    text = re.sub(r'sidedef \{[^}]*\}', '', text)
    sides = '\n'.join('sidedef { sector=0; texturemiddle="STARTAN3"; offsetx=%d; }' %
                      (100 + i) for i in range(count))
    text = text.replace('linedef {', sides + '\nlinedef {', 1)
    text = re.sub(r'sidefront=\d+;', 'sidefront=%d;' % front, text)
    # Only the first line has an explicit back reference. Other backs stay absent.
    text = text.replace('blocking=true;', 'sideback=%d; blocking=true;' % back, 1)
    payload = text.encode()
    offset = 12 + len(payload)
    wad = struct.pack("<4sii", b"PWAD", 3, offset) + payload
    wad += b''.join(struct.pack("<ii8s", start, length, name) for start, length, name in
                    [(12, 0, b"SIDECHK"), (12, len(payload), b"TEXTMAP"), (offset, 0, b"ENDMAP")])
    # Inspect the loaded geometry, not a duplicate implementation of the guard.
    script = '''version "4.10"
class SideBoundsCheck : EventHandler
{
    override void WorldLoaded(WorldEvent e)
    {
        int fronts = 0, backs = 0, offsets = 0, links = 0;
        for (int i = 0; i < level.lines.Size(); ++i)
        {
            let line = level.lines[i];
            if (line.sidedef[0] != null)
            {
                ++fronts;
                if (line.sidedef[0].GetTextureXOffset(Side.mid) == EXPECTED_OFFSET) ++offsets;
                if (line.sidedef[0].linedef == line) ++links;
            }
            if (line.sidedef[1] != null) ++backs;
        }
        Console.Printf("udmf-sides: lines=%d sides=%d fronts=%d backs=%d offsets=%d links=%d",
                       level.lines.Size(), level.sides.Size(), fronts, backs, offsets, links);
    }
}
'''.replace('EXPECTED_OFFSET', str(100 + front))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, content in (("maps/SIDECHK.wad", wad), ("ZSCRIPT", script),
                              ("MAPINFO", 'GameInfo { AddEventHandlers = "SideBoundsCheck" }\n')):
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, content)


def run_case(args, root, case):
    directory = root / case
    directory.mkdir()
    fixture = directory / "fixture.pk3"
    make_fixture(fixture, case)
    (directory / "saves").mkdir()
    config = directory / "engine.ini"
    config.write_text("[GlobalSettings]\nvid_preferbackend=%d\nvid_fullscreen=false\n"
                      "snd_musicvolume=0\n" % BACKENDS[args.backend])
    command = [str(args.binary), "-iwad", str(args.iwad), "-file", str(fixture),
               "-config", str(config), "-savedir", str(directory / "saves"),
               "-noautoload", "-noautoexec", "-window", "+disableautosave", "1",
               "+vid_preferbackend", str(BACKENDS[args.backend]),
               "+logfile", str(directory / "engine.log"), "+map", "SIDECHK",
               "+execafter", "35", "quit"]
    (directory / "argv.json").write_text(json.dumps(command, indent=2))
    timed_out = False
    with (directory / "stdio.log").open("w") as output:
        process = subprocess.Popen(command, cwd=directory, stdout=output, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.terminate()
            try:
                code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                code = process.wait()
    log = (directory / "engine.log").read_text(errors="replace") if (directory / "engine.log").exists() else ""
    backend_error = ""
    try:
        confirm_backend(log, args.backend)
    except RuntimeError as error:
        backend_error = str(error)
    matches = re.findall(r"udmf-sides: lines=(\d+) sides=(\d+) fronts=(\d+) backs=(\d+) offsets=(\d+) links=(\d+)", log)
    expected = ("5", "5", "5", "0", "5", "5")
    input_confirmed = "adding %s," % args.iwad in log and log.count("SIDECHK -") == 1
    passed = code == 0 and not timed_out and not backend_error and input_confirmed and matches == [expected]
    return {"passed": passed, "returncode": code, "timeout": timed_out,
            "backend_error": backend_error, "input_confirmed": input_confirmed,
            "geometry": matches, "fixture_sha256": sha256(fixture)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--iwad", type=Path, required=True)
    parser.add_argument("--backend", choices=BACKENDS, default="gl")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    args = parser.parse_args()
    args.binary = args.binary.resolve(strict=True)
    args.iwad = args.iwad.resolve(strict=True)
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="gzdoom-udmf-sides-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print("Artifacts:", root, flush=True)
    (root / "manifest.json").write_text(json.dumps({
        "binary": str(args.binary), "binary_sha256": sha256(args.binary),
        "package_sha256": sha256(args.binary.with_name("gzdoom.pk3")),
        "support_sha256": sha256(args.binary.with_name("game_support.pk3")),
        "iwad": str(args.iwad), "iwad_sha256": sha256(args.iwad), "backend": args.backend,
        "platform": platform.platform(), "runner_sha256": sha256(__file__),
    }, indent=2))
    results = {}
    for case in args.cases:
        results[case] = run_case(args, root, case)
        print(case, "PASS" if results[case]["passed"] else "FAIL", flush=True)
        (root / "summary.json").write_text(json.dumps(results, indent=2))
    return 0 if all(row["passed"] for row in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
