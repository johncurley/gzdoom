import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile
import zlib

from fixtures import make_fixture, make_png
from benchmark import timing_windows
from run import cache_passes, confirm_backend, records, validate_preparation, image_pixels, validate_paired_rng


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
                self.assertEqual(len(archive.namelist()), 15)
                self.assertIn(b"reg.long", archive.read("SNDINFO"))
                with self.assertRaises((zipfile.BadZipFile, zlib.error)):
                    archive.read("sounds/REGCORR.ogg")
                self.assertGreater(len(archive.read("sounds/REGHUGE.ogg")), 128 * 1024)
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

    def test_paired_rng_rejects_choices_pcm_and_draw_count(self):
        choices = "".join("snd-choice group=2 selected=%d\n" % (3 + i % 2) for i in range(8))
        pcm = "snd-cache-buffer hash=a bytes=12 start=0 end=6\n" + "snd-cache-buffer hash=b bytes=12 start=0 end=6\n"
        first = "snd-siblings-rng-begin mode=0\n" + choices + pcm + "snd-siblings-rng-end mode=0\n"
        second = "snd-siblings-rng-begin mode=1\n" + choices + pcm + "snd-siblings-rng-end mode=1\n"
        status = ("snd-siblings-rng-stream draws=8/8 equal=1\n"
                  "snd-siblings-rng-selftest: PASS comparisons=8 failures=0 negative=1 restored=1\n")
        validate_paired_rng(first + second + status)
        for invalid in (first + second.replace("selected=3", "selected=4", 1) + status,
                        first + second.replace("hash=a", "hash=c") + status,
                        first + second + status.replace("draws=8/8", "draws=8/9"),
                        first + second + status.replace("restored=1", "restored=0"),
                        first + status):
            with self.assertRaises(RuntimeError):
                validate_paired_rng(invalid)

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
