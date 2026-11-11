"""Small deterministic maps, PNGs and audio definitions for native regression runs."""
import io
from pathlib import Path
import struct
import wave
import zipfile
import zlib

TONE_DIR = Path(__file__).with_name("fixtures")
TEXTURES = ("REGRGB", "REGRGBA", "REGPAL")
SOUNDS = ("reg.a", "reg.b")


def png_chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload)))


def paeth(left, up, diagonal):
    prediction = left + up - diagonal
    distances = (abs(prediction - left), abs(prediction - up), abs(prediction - diagonal))
    return (left, up, diagonal)[distances.index(min(distances))]


def make_png(width, height, channels):
    """Encode rows with all five PNG filters, including non-power-of-two sizes."""
    stride = width * channels
    previous = bytes(stride)
    filtered = bytearray()
    pixels = bytearray()
    for y in range(height):
        row = bytearray((x * 7 + y * 13 + c * 61) & 255
                        for x in range(width) for c in range(channels))
        if channels == 4:
            for x in range(width):
                row[x * 4 + 3] = (0, 128, 255)[(x // 5 + y // 5) % 3]
        pixels.extend(row)
        kind = y % 5
        filtered.append(kind)
        for i, value in enumerate(row):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            diagonal = previous[i - channels] if i >= channels else 0
            predictor = (0, left, up, (left + up) // 2, paeth(left, up, diagonal))[kind]
            filtered.append((value - predictor) & 255)
        previous = row
    color = {3: 2, 4: 6}[channels]
    header = struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) +
            png_chunk(b"IDAT", zlib.compress(filtered)) + png_chunk(b"IEND", b""),
            bytes(pixels))


def make_paletted_png():
    width, height = 64, 32
    header = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
    rows = b"".join(b"\0" + bytes((x // 8 + y // 8) % 4 for x in range(width))
                    for y in range(height))
    palette = bytes((255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255))
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) +
            png_chunk(b"PLTE", palette) + png_chunk(b"tRNS", bytes((0, 128, 255, 255))) +
            png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b""))


def room(map_name, textured=False):
    floor = "REGPAL" if textured else "FLOOR0_1"
    walls = ["REGRGB", "REGRGB", "REGRGB", "REGRGB", "REGRGBA"] if textured else ["STARTAN3"] * 5
    parts = ['namespace="ZDoom";']
    for x, y in [(-128, -128), (-128, 128), (128, 128), (128, -128), (0, 128)]:
        parts.append("vertex { x=%d; y=%d; }" % (x, y))
    parts.append('sector { heightfloor=0; heightceiling=128; texturefloor="%s"; '
                 'textureceiling="CEIL1_1"; lightlevel=192; }' % floor)
    for texture in walls:
        parts.append('sidedef { sector=0; texturemiddle="%s"; }' % texture)
    for first, last, side in [(0, 1, 0), (1, 4, 1), (2, 3, 2), (3, 0, 3), (4, 2, 4)]:
        parts.append("linedef { v1=%d; v2=%d; sidefront=%d; blocking=true; }" %
                     (first, last, side))
    parts.append("thing { x=0; y=0; angle=90; type=1; skill1=true; skill2=true; "
                 "skill3=true; skill4=true; skill5=true; single=true; }")
    text = "\n".join(parts).encode()
    offset = 12 + len(text)
    directory = b"".join(struct.pack("<ii8s", pos, size, name) for pos, size, name in
                         [(12, 0, map_name.encode()), (12, len(text), b"TEXTMAP"),
                          (offset, 0, b"ENDMAP")])
    return struct.pack("<4sii", b"PWAD", 3, offset) + text + directory


def make_fixture(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    stage = directory / "stage.wad"
    stage.write_bytes(room("REGSTART"))
    fixture = directory / "regression.pk3"
    sound_names = ["reg.a", "reg.b", "reg.c", "reg.d", "reg.alias", "reg.random",
                   "reg.long", "reg.bad", "reg.wave"]
    sndinfo = ("reg.a REGA\nreg.b REGB\nreg.c REGC\nreg.d REGD\n"
               "$alias reg.alias reg.a\n$random reg.random { reg.b reg.c }\n"
               "reg.long REGLONG\nreg.bad REGBAD\nreg.wave REGWAVE\n"
               "reg.corrupt REGCORR\nreg.huge REGHUGE\n")
    mapinfo = ('map REGTEST "Regression room"\n{\nPrecacheSounds = ' +
               ", ".join('"%s"' % name for name in sound_names) + "\n}\n")
    wav = io.BytesIO()
    with wave.open(wav, "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(1)
        stream.setframerate(8000)
        stream.writeframes(bytes(range(256)) * 8)
    entries = {
        "maps/REGTEST.wad": room("REGTEST", True),
        "textures/REGRGB.png": make_png(64, 64, 3)[0],
        "textures/REGRGBA.png": make_png(37, 29, 4)[0],
        "textures/REGPAL.png": make_paletted_png(),
        "sounds/REGA.ogg": (TONE_DIR / "tone.ogg").read_bytes(),
        "sounds/REGB.ogg": (TONE_DIR / "tone-alt.ogg").read_bytes(),
        "sounds/REGC.ogg": (TONE_DIR / "tone.ogg").read_bytes(),
        "sounds/REGD.ogg": (TONE_DIR / "tone.ogg").read_bytes(),
        "sounds/REGLONG.ogg": (TONE_DIR / "tone-long.ogg").read_bytes(),
        "sounds/REGBAD.ogg": b"OggS\0\2" + bytes(20) + b"\1\7\1vorbis",
        "sounds/REGWAVE.wav": wav.getvalue(),
        "sounds/REGCORR.ogg": b"OggS" + bytes(60),
        "sounds/REGHUGE.ogg": b"OggS" + bytes(128 * 1024 - 3),
        "SNDINFO": sndinfo.encode(),
        "MAPINFO": mapinfo.encode(),
    }
    with zipfile.ZipFile(fixture, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(entries.items()):
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    # Deliberately corrupt one unused sound's raw-deflate block. Its directory
    # remains valid, so the advisory-reader test exercises a real read failure.
    with zipfile.ZipFile(fixture) as archive:
        offset = archive.getinfo("sounds/REGCORR.ogg").header_offset
    payload = bytearray(fixture.read_bytes())
    name_size, extra_size = struct.unpack_from("<HH", payload, offset + 26)
    payload[offset + 30 + name_size + extra_size] = 7  # reserved deflate BTYPE
    fixture.write_bytes(payload)
    return fixture, stage
