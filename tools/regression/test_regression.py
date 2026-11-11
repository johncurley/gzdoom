import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from fixtures import make_fixture, make_png
from benchmark import timing_windows
from run import cache_passes, confirm_backend, records, validate_preparation, image_pixels


class RegressionTools(unittest.TestCase):
    def test_all_png_filters(self):
        for channels in (3, 4):
            with self.subTest(channels=channels), tempfile.TemporaryDirectory() as directory:
                encoded, expected = make_png(37, 29, channels)
                path = Path(directory) / "test.png"
                path.write_bytes(encoded)
                self.assertEqual(image_pixels_small(path), (37, 29, channels, expected))

    def test_fixture_determinism(self):
        with tempfile.TemporaryDirectory() as directory:
            a, stage_a = make_fixture(Path(directory) / "a")
            b, stage_b = make_fixture(Path(directory) / "b")
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(stage_a.read_bytes(), stage_b.read_bytes())
            with zipfile.ZipFile(a) as archive:
                self.assertEqual(len(archive.namelist()), 13)
                self.assertIn(b"reg.long", archive.read("SNDINFO"))
                data = archive.read("maps/REGTEST.wad")
                self.assertEqual(struct.unpack_from("<4s", data)[0], b"PWAD")

    def test_backend_rejection(self):
        confirm_backend("GL_RENDERER: Intel", "gl")
        with self.assertRaises(RuntimeError):
            confirm_backend("GL_RENDERER: Intel", "metal")

    def test_budget_and_disabled_control(self):
        validate_preparation(records("snd_prepare: enabled=0 queued=0 hits=0 (no worker)", "snd_prepare:"))
        validate_preparation(records("snd_prepare: enabled=1 queued=0 hits=0 (no worker)", "snd_prepare:"))
        validate_preparation([{"enabled": 1, "peak": 64, "budget": 64}])
        for bad in ([], [{"enabled": 1}], [{"enabled": 1, "peak": 65, "budget": 64}]):
            with self.assertRaises(RuntimeError):
                validate_preparation(bad)

    def test_audio_audit_requires_complete_sequence(self):
        audit = "snd-cache-begin\nsnd-cache-resident buffers=1 bytes=12\nsnd-cache-end\n"
        self.assertEqual(len(cache_passes(audit * 3)), 3)
        for bad in ("", audit, audit * 3 + "snd-cache-begin\n", "snd-cache-end\n"):
            with self.assertRaises(RuntimeError):
                cache_passes(bad)

    def test_timing_markers(self):
        valid = ("REG_FIRST\nvid_frametrace count=2 max= 120.5\n"
                 "REG_WARM\nvid_frametrace count=2 max= 20.5\nREG_FINISH\n")
        self.assertEqual(timing_windows(valid)["first_ms"], 120.5)
        for invalid in ("", valid.replace("REG_WARM", "OTHER"),
                        valid + "REG_FIRST\n", valid.replace("max= 20.5", "max= n/a")):
            with self.assertRaises(RuntimeError):
                timing_windows(invalid)

    def test_pixel_negative_control(self):
        encoded, expected = make_png(320, 200, 3)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frame.png"
            path.write_bytes(encoded)
            pixels = image_pixels(path)[3]
            self.assertEqual(pixels, expected)
            changed = bytearray(pixels)
            changed[len(changed) // 2] ^= 1
            self.assertNotEqual(hashlib.sha256(changed).digest(), hashlib.sha256(pixels).digest())


def image_pixels_small(path):
    from pngdiff import read_png
    return read_png(path)


if __name__ == "__main__":
    unittest.main()
