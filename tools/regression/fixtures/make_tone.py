#!/usr/bin/env python3
"""Regenerate the synthetic fixture using an installed libsndfile encoder."""
import argparse
import ctypes
import ctypes.util
import math
from pathlib import Path


class SoundInfo(ctypes.Structure):
    _fields_ = [("frames", ctypes.c_int64), ("samplerate", ctypes.c_int),
                ("channels", ctypes.c_int), ("format", ctypes.c_int),
                ("sections", ctypes.c_int), ("seekable", ctypes.c_int)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", default=ctypes.util.find_library("sndfile"))
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("tone.ogg"))
    parser.add_argument("--frequency", type=int, default=500)
    parser.add_argument("--frames", type=int, default=6000)
    args = parser.parse_args()
    if not args.library:
        parser.error("libsndfile is required only to regenerate the committed tone")
    library = ctypes.CDLL(args.library)
    library.sf_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(SoundInfo)]
    library.sf_open.restype = ctypes.c_void_p
    library.sf_writef_short.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16),
                                       ctypes.c_int64]
    library.sf_writef_short.restype = ctypes.c_int64
    library.sf_close.argtypes = [ctypes.c_void_p]
    rate, frames = 24000, args.frames
    samples = (ctypes.c_int16 * frames)(
        *(round(12000 * math.sin(2 * math.pi * args.frequency * i / rate)) for i in range(frames)))
    info = SoundInfo(frames=frames, samplerate=rate, channels=1, format=0x200000 | 0x0060)
    handle = library.sf_open(bytes(args.output), 0x20, ctypes.byref(info))
    if not handle:
        raise RuntimeError("libsndfile could not open the Ogg/Vorbis output")
    try:
        if library.sf_writef_short(handle, samples, frames) != frames:
            raise RuntimeError("short fixture write")
    finally:
        if library.sf_close(handle):
            raise RuntimeError("fixture close failed")


if __name__ == "__main__":
    main()
