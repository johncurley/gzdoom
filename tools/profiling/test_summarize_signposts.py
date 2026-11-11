import tempfile
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

from summarize_signposts import numeric_context, summarize


class SignpostControls(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "signposts.xml"

    def write(self, events):
        root = ET.Element("trace-query-result")
        node = ET.SubElement(root, "node")
        schema = ET.SubElement(node, "schema", name="os-signpost")
        for name in ["time", "process", "event-type", "identifier", "name", "subsystem", "message"]:
            ET.SubElement(ET.SubElement(schema, "col"), "mnemonic").text = name
        for index, (time, kind, identifier, name) in enumerate(events):
            row = ET.SubElement(node, "row")
            ET.SubElement(row, "event-time").text = str(round(time * 1e9))
            ET.SubElement(row, "process", **({"id": "1", "fmt": "gzdoom (1)"} if index == 0 else {"ref": "1"}))
            ET.SubElement(row, "event-type").text = kind
            ET.SubElement(row, "identifier").text = str(identifier)
            ET.SubElement(row, "name").text = name
            ET.SubElement(row, "subsystem").text = "org.gzdoom.engine"
            ET.SubElement(row, "message").text = "numeric=1"
        ET.ElementTree(root).write(self.path)

    def test_pairs_distinct_scopes_and_update_gap(self):
        self.write([(0, "Event", 0, "EngineUpdate"), (0.01, "Begin", 1, "EngineLoop"),
                    (0.02, "Begin", 2, "EngineTick"), (0.03, "End", 2, "EngineTick"),
                    (0.12, "End", 1, "EngineLoop"), (0.13, "Event", 0, "EngineUpdate")])
        report = summarize(self.path)
        durations = {item["name"]: item["duration_ms"] for item in report["intervals"]}
        self.assertAlmostEqual(durations["EngineLoop"], 110)
        self.assertAlmostEqual(durations["EngineTick"], 10)
        self.assertAlmostEqual(report["observed_update_gaps"][0]["gap_ms"], 130)
        self.assertEqual(report["unmatched_begins"], [])
        self.assertEqual(report["unmatched_ends"], [])

    def test_lost_boundaries_and_duplicate_begin_are_reported(self):
        self.write([(0, "End", 3, "EngineLoop"), (0.1, "Begin", 1, "EngineLoop"),
                    (0.2, "Begin", 1, "EngineLoop")])
        report = summarize(self.path)
        self.assertEqual(len(report["unmatched_ends"]), 1)
        self.assertEqual(len(report["unmatched_begins"]), 1)
        self.assertEqual(len(report["duplicate_begins"]), 1)
        self.assertEqual(report["intervals"], [])

    def test_unknown_event_and_empty_collector(self):
        self.write([(0, "UnexpectedType", 1, "EngineUpdate")])
        self.assertEqual(len(summarize(self.path)["unknown_event_types"]), 1)
        self.write([])
        self.assertEqual(summarize(self.path)["event_record_counts"], {})

    def test_grouped_integer_context(self):
        self.assertEqual(numeric_context("update=2,105 loop=3,465 tic=2,123 state=-1 active=1"),
                         {"update": 2105, "loop": 3465, "tic": 2123, "state": -1, "active": 1})

    def test_capture_bounds_exclude_post_capture_gaps(self):
        self.write([(0, "Event", 0, "EngineUpdate"), (0.13, "Event", 0, "EngineUpdate"),
                    (0.14, "Begin", 1, "EngineLoop"), (0.4, "End", 1, "EngineLoop"),
                    (0.5, "Event", 0, "EngineUpdate")])
        report = summarize(self.path, start=0, end=0.2)
        self.assertEqual(len(report["observed_update_gaps"]), 1)
        self.assertAlmostEqual(report["observed_update_gaps"][0]["gap_ms"], 130)
        self.assertEqual(len(report["unmatched_begins"]), 1)
        self.assertEqual(report["intervals"], [])


if __name__ == "__main__":
    unittest.main()
