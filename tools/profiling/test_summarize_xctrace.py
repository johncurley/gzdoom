"""Controls for the offline Instruments summary, with no runtime capture needed."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from summarize_xctrace import summarize


class SummaryControls(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.profile = self.root / "profile.xml"
        columns = ["stack", "time", "weight", "thread", "thread-state"]
        schema = "".join(f"<col><mnemonic>{name}</mnemonic></col>" for name in columns)
        self.xml = f'''<trace-query-result><node><schema name="time-profile">{schema}</schema>
<row><backtrace id="1" fmt="Leaf ← (2 other frames)"><text-addresses id="2">16 32 16</text-addresses></backtrace>
<sample-time id="3">0</sample-time><weight id="4">1000000</weight>
<thread id="5" fmt="Main Thread 0x1"/><thread-state id="6" fmt="Running"/></row>
<row><backtrace ref="1"/><sample-time id="7">1000000000</sample-time>
<weight id="8">3000000</weight><thread ref="5"/><thread-state ref="6"/></row>
</node></trace-query-result>'''
        self.profile.write_text(self.xml)
        self.addresses = self.root / "addresses.txt"
        self.symbols = self.root / "symbols.txt"
        self.addresses.write_text("0x10\n0x20\n")
        self.symbols.write_text("FunctionA (in gzdoom)\nFunctionB (in gzdoom)\n")
        self.args = argparse.Namespace(profile=self.profile, addresses=self.addresses,
                                       symbols=self.symbols, start=0, end=float("inf"),
                                       thread="Main Thread")

    def test_references_schema_order_and_recursive_deduplication(self):
        report = summarize(self.args)
        self.assertEqual(report["sample_count"], 2)
        self.assertEqual(report["sample_weight_ms"], 4)
        self.assertEqual(report["leaf_ranking"][0]["sample_weight_ms"], 4)
        self.assertEqual([(x["name"], x["sample_weight_ms"])
                          for x in report["inclusive_engine_ranking"]],
                         [("FunctionA", 4), ("FunctionB", 4)])

    def test_exact_window_boundaries(self):
        self.args.end = 1
        self.assertEqual(summarize(self.args)["sample_weight_ms"], 1)
        self.args.start, self.args.end = 1, 2
        self.assertEqual(summarize(self.args)["sample_weight_ms"], 3)

    def test_archived_binary_names_and_source_lines_do_not_split_symbols(self):
        self.symbols.write_text("FunctionA (in archived-gzdoom) (a.cpp:3)\n"
                                "FunctionA (in archived-gzdoom) (a.cpp:7)\n")
        report = summarize(self.args)
        self.assertEqual([(x["name"], x["sample_weight_ms"])
                          for x in report["inclusive_engine_ranking"]], [("FunctionA", 4)])

    def test_leaf_summary_without_optional_symbol_mapping(self):
        self.args.addresses = self.args.symbols = None
        report = summarize(self.args)
        self.assertEqual(report["sample_weight_ms"], 4)
        self.assertEqual(report["inclusive_engine_ranking"], [])

    def test_broken_reference_is_rejected(self):
        self.profile.write_text(self.xml.replace('backtrace ref="1"', 'backtrace ref="999"'))
        with self.assertRaises(KeyError):
            summarize(self.args)

    def test_cyclic_reference_is_rejected(self):
        self.profile.write_text(self.xml.replace('backtrace id="1"', 'backtrace id="1" ref="1"'))
        with self.assertRaisesRegex(ValueError, "cyclic"):
            summarize(self.args)

    def test_mismatched_symbol_mapping_is_rejected(self):
        self.symbols.write_text("FunctionA\n")
        with self.assertRaisesRegex(ValueError, "line counts"):
            summarize(self.args)

    def test_empty_window_fails_cli(self):
        command = [sys.executable, str(Path(__file__).with_name("summarize_xctrace.py")),
                   str(self.profile), "--start", "2", "--end", "3"]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("no matching samples", result.stderr)


if __name__ == "__main__":
    unittest.main()
