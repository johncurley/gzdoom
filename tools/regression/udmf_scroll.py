#!/usr/bin/env python3
"""Check live UDMF floor/ceiling scrolling and shared-source wall scrolling."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

from run import BACKENDS, confirm_backend, sha256


def fixture(path):
    modes = [('both', 15), ('visual', 1), ('physical', 14), ('unknown', 0),
             ('BoTh', 15), ('ViSuAl', 1), ('PhYsIcAl', 14)]
    parts = ['namespace="ZDoom";']
    checks = []
    expected = []
    index = 0
    for plane in ('floor', 'ceiling'):
        short = 'floor' if plane == 'floor' else 'ceil'
        cases = [(f'scroll_{short}_type="{name}";', mask) for name, mask in modes]
        cases += [(f'scroll{plane}mode={mask};', mask) for mask in (15, 1, 14, 0)]
        cases += [(f'scroll_{short}_type="physical"; scroll_{short}_type="visual";', 1),
                  (f'scroll{plane}mode=15; scroll_{short}_type="unknown";', 0),
                  (f'scroll_{short}_type="unknown"; scroll{plane}mode=15;', 15)]
        for properties, mask in cases:
            x = index * 512
            for vx, vy in ((x-128, -128), (x-128, 128), (x+128, 128), (x+128, -128)):
                parts.append(f'vertex {{ x={vx}; y={vy}; }}')
            parts.append('sector { heightfloor=0; heightceiling=128; '
                         'texturefloor="FLOOR0_1"; textureceiling="CEIL1_1"; '
                         f'xscroll{plane}=0.5; yscroll{plane}=0.25; {properties} }}')
            for edge in range(4):
                parts.append(f'sidedef {{ sector={index}; texturemiddle="STARTAN3"; }}')
                parts.append(f'linedef {{ v1={index*4+edge}; v2={index*4+(edge+1)%4}; '
                             f'sidefront={index*4+edge}; blocking=true; }}')
            actor = 'CeilingProbe' if plane == 'ceiling' else 'FloorProbe'
            z = 120 if plane == 'ceiling' else 0
            checks.append(f'probes.Push(Actor.Spawn("{actor}", ({x},0,{z}))); '
                          f'origins.Push({x}); masks.Push({mask}); planes.Push(Sector.{plane});')
            expected.append((str(index), str(mask), str(int(bool(mask & 1))), str(int(bool(mask & 14)))))
            index += 1
    # A separate room has all four lines sharing one source side. Per-part
    # rates deliberately differ from the global fallback.
    wall = index
    x = index * 512
    for vx, vy in ((x-128, -128), (x-128, 128), (x+128, 128), (x+128, -128)):
        parts.append(f'vertex {{ x={vx}; y={vy}; }}')
    parts.append('sector { heightfloor=0; heightceiling=128; '
                 'texturefloor="FLOOR0_1"; textureceiling="CEIL1_1"; }')
    parts.append(f'sidedef {{ sector={wall}; texturemiddle="STARTAN3"; '
                 'xscroll=0.25; xscrolltop=0.5; yscrollmid=0.75; }')
    for edge in range(4):
        parts.append(f'linedef {{ v1={wall*4+edge}; v2={wall*4+(edge+1)%4}; '
                     f'sidefront={wall*4}; blocking=true; }}')
    parts.append(f'thing {{ x={x}; y=0; type=1; skill1=true; skill2=true; '
                 'skill3=true; skill4=true; skill5=true; single=true; }')
    payload = '\n'.join(parts).encode()
    end = 12 + len(payload)
    wad = struct.pack('<4sii', b'PWAD', 3, end) + payload
    wad += b''.join(struct.pack('<ii8s', start, size, name) for start, size, name in
                    ((12, 0, b'SCROLLCK'), (12, len(payload), b'TEXTMAP'), (end, 0, b'ENDMAP')))
    script = '''version "4.10"
class FloorProbe : Actor { Default { Radius 4; Height 8; } }
class CeilingProbe : FloorProbe { Default { +SPAWNCEILING +NOGRAVITY } }
class ScrollCheck : EventHandler
{
    Array<Actor> probes;
    Array<double> origins;
    Array<int> masks, planes;
    int ticks;
    override void WorldLoaded(WorldEvent e)
    {
        SETUP
    }
    override void WorldTick()
    {
        if (++ticks != 20) return;
        for (int i=0; i<probes.Size(); ++i)
        {
            if (probes[i] == null)
            {
                Console.Printf("udmf-scroll: missing probe %d", i);
                continue;
            }
            let sec = level.sectors[i];
            bool texture = abs(sec.GetXOffset(planes[i])) > 0.01 &&
                           abs(sec.GetYOffset(planes[i])) > 0.01;
            bool carry = probes[i] != null && abs(probes[i].Pos.X-origins[i]) > 0.01 &&
                         abs(probes[i].Pos.Y) > 0.01;
            Console.Printf("udmf-scroll: case=%d mask=%d texture=%d carry=%d", i, masks[i], texture, carry);
        }
        for (int i=WALL*4; i<level.sides.Size(); ++i)
        {
            let side = level.sides[i];
            Console.Printf("udmf-wall: side=%d topx=%.3f topy=%.3f midx=%.3f midy=%.3f botx=%.3f boty=%.3f",
                i, side.GetTextureXOffset(Side.top), side.GetTextureYOffset(Side.top),
                side.GetTextureXOffset(Side.mid), side.GetTextureYOffset(Side.mid),
                side.GetTextureXOffset(Side.bottom), side.GetTextureYOffset(Side.bottom));
        }
    }
}
'''.replace('SETUP', '\n'.join(checks)).replace('WALL', str(wall))
    with zipfile.ZipFile(path, 'w') as archive:
        for name, data in (('maps/SCROLLCK.wad', wad), ('ZSCRIPT', script),
                           ('MAPINFO', 'GameInfo { AddEventHandlers = "ScrollCheck" }')):
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--iwad', type=Path, required=True)
    parser.add_argument('--backend', choices=BACKENDS, default='gl')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binary, iwad = args.binary.resolve(strict=True), args.iwad.resolve(strict=True)
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='gzdoom-udmf-scroll-'))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print('Artifacts:', root, flush=True)
    package = root / 'fixture.pk3'
    expected = fixture(package)
    config = root / 'engine.ini'
    config.write_text('[GlobalSettings]\nsnd_musicvolume=0\nvid_fullscreen=false\n')
    (root / 'saves').mkdir()
    command = [str(binary), '-iwad', str(iwad), '-file', str(package), '-config', str(config),
               '-savedir', str(root/'saves'), '-noautoload', '-noautoexec', '-window',
               '+disableautosave', '1', '+cl_capfps', '1', '+vid_preferbackend', str(BACKENDS[args.backend]),
               '+logfile', str(root/'engine.log'), '+map', 'SCROLLCK', '+execafter', '140', 'quit']
    (root/'manifest.json').write_text(json.dumps(dict(argv=command, binary_sha256=sha256(binary),
        package_sha256=sha256(binary.with_name('gzdoom.pk3')), support_sha256=sha256(binary.with_name('game_support.pk3')),
        iwad_sha256=sha256(iwad), fixture_sha256=sha256(package), runner_sha256=sha256(__file__)), indent=2))
    with (root/'stdio.log').open('w') as output:
        try:
            code = subprocess.run(command, cwd=root, stdout=output, stderr=subprocess.STDOUT, timeout=90).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    log = (root/'engine.log').read_text(errors='replace') if (root/'engine.log').exists() else ''
    error = ''
    try:
        confirm_backend(log, args.backend)
    except RuntimeError as exc:
        error = str(exc)
    cases = re.findall(r'udmf-scroll: case=(\d+) mask=(\d+) texture=(\d) carry=(\d)', log)
    failures = [dict(expected=want, actual=got) for want, got in zip(expected, cases) if want != got]
    sectors_ok = cases == expected
    walls = re.findall(r'udmf-wall: side=(\d+) topx=([-\d.]+) topy=([-\d.]+) midx=([-\d.]+) midy=([-\d.]+) botx=([-\d.]+) boty=([-\d.]+)', log)
    # Expected rate ratios, independent of whether the event runs before or
    # after the scroller tick. Every decompressed copy must move identically.
    wall_ok = ([row[0] for row in walls] == [str(len(expected)*4+i) for i in range(4)]
               and len({row[1:] for row in walls}) == 1)
    for row in walls:
        tx, ty, mx, my, bx, by = map(float, row[1:])
        wall_ok &= tx > 0 and my > 0 and bx > 0 and abs(tx-2*bx) < 0.001 and ty == mx == by == 0
    passed = (code == 0 and not error and sectors_ok and wall_ok
              and 'adding %s,' % iwad in log and log.count('SCROLLCK -') == 1)
    result = dict(passed=passed, returncode=code, backend_error=error, sectors_ok=sectors_ok, cases=cases,
                  failures=failures, wall_ok=wall_ok, walls=walls)
    (root/'summary.json').write_text(json.dumps(result, indent=2))
    print('sectors: %s (%d/%d results, %d mismatches); walls: %s; exit: %s' %
          ('PASS' if sectors_ok else 'FAIL', len(cases), len(expected), len(failures),
           'PASS' if wall_ok else 'FAIL', code), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
