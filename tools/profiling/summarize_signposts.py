#!/usr/bin/env python3
"""Pair native engine signposts and list observed Update gaps in trace seconds."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


def numeric_context(message):
    # Instruments formats public integer arguments with grouping commas on
    # the reference host. Keep the original message as well as parsed fields.
    return {match[1]: int(match[2].replace(",", ""))
            for match in re.finditer(r"\b([A-Za-z_]\w*)=(-?\d[\d,]*)(?=\s|$)", message)}


def summarize(path, subsystem="org.gzdoom.engine", start=0, end=float("inf")):
    if not 0 <= start < end:
        raise ValueError("require 0 <= start < end")
    root = ET.parse(path).getroot()
    nodes = [node for node in root.findall("node")
             if node.find("schema") is not None and node.find("schema").get("name") == "os-signpost"]
    if len(nodes) != 1:
        raise ValueError("export exactly one os-signpost table")
    table = nodes[0]
    definitions = {}
    for element in table.iter():
        key = element.get("id")
        if key:
            if key in definitions:
                raise ValueError("duplicate XML id: " + key)
            definitions[key] = element

    def resolve(element):
        seen = set()
        while element.get("ref"):
            key = element.get("ref")
            if key in seen:
                raise ValueError("cyclic XML reference")
            seen.add(key)
            element = definitions[key]
        return element

    def display(element):
        return element.get("fmt", element.text or "")

    columns = [column.findtext("mnemonic") for column in table.find("schema")]
    if not {"time", "process", "event-type", "identifier", "name", "subsystem"}.issubset(columns):
        raise ValueError("missing signpost schema columns")
    events = []
    for row in table.findall("row"):
        if len(row) != len(columns):
            raise ValueError("row column count differs from schema")
        cells = dict(zip(columns, map(resolve, row)))
        if display(cells["subsystem"]) != subsystem:
            continue
        timestamp = int(cells["time"].text) / 1e9
        if not start <= timestamp < end:
            continue
        events.append({"time_seconds": timestamp,
                       "name": display(cells["name"]), "type": display(cells["event-type"]),
                       "identifier": display(cells["identifier"]),
                       "process": display(cells["process"]),
                       "message": display(cells["message"]) if "message" in cells else ""})
    events.sort(key=lambda event: event["time_seconds"])
    opened = {}
    intervals, unmatched_ends, duplicate_begins, unknown_types = [], [], [], []
    updates = []
    counts = Counter()
    for event in events:
        kind = event["type"].lower()
        counts[event["name"]] += 1
        key = (event["process"], event["name"], event["identifier"])
        if "begin" in kind:
            if key in opened:
                duplicate_begins.append(opened[key])
            opened[key] = event
        elif "end" in kind:
            begin = opened.pop(key, None)
            if begin is None:
                unmatched_ends.append(event)
            else:
                intervals.append({"name": event["name"], "process": event["process"],
                                  "start_seconds": begin["time_seconds"], "end_seconds": event["time_seconds"],
                                  "duration_ms": (event["time_seconds"] - begin["time_seconds"]) * 1000,
                                  "begin_message": begin["message"],
                                  "begin_context": numeric_context(begin["message"])})
        elif "event" in kind:
            if event["name"] == "EngineUpdate":
                event["context"] = numeric_context(event["message"])
                updates.append(event)
        else:
            unknown_types.append(event)
    gaps = []
    previous = {}
    for event in updates:
        before = previous.get(event["process"])
        if before:
            gaps.append({"process": event["process"],
                         "start_seconds": before["time_seconds"], "end_seconds": event["time_seconds"],
                         "gap_ms": (event["time_seconds"] - before["time_seconds"]) * 1000,
                         "start_message": before["message"], "end_message": event["message"]})
        previous[event["process"]] = event
    return {"input": str(Path(path).resolve()), "subsystem": subsystem,
            "selection": {"start_seconds": start,
                          "end_seconds_exclusive": end if end != float("inf") else None},
            "interpretation": "Native trace-clock intervals; observed Update gaps need continuous enabled collection and context checks. They are not GPU-present intervals. Missing/disabled markers can enlarge gaps.",
            "event_record_counts": dict(counts), "intervals": intervals, "update_events": updates,
            "observed_update_gaps": sorted(gaps, key=lambda gap: -gap["gap_ms"]),
            "unmatched_begins": list(opened.values()), "unmatched_ends": unmatched_ends,
            "duplicate_begins": duplicate_begins, "unknown_event_types": unknown_types}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("signposts", type=Path)
    parser.add_argument("--json", type=Path, dest="output")
    parser.add_argument("--start", type=float, default=0, help="inclusive trace-relative seconds")
    parser.add_argument("--end", type=float, default=float("inf"), help="exclusive trace-relative seconds")
    args = parser.parse_args()
    try:
        report = summarize(args.signposts, start=args.start, end=args.end)
        if args.output:
            args.output.write_text(json.dumps(report, indent=2) + "\n")
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        print("error: " + str(error), file=sys.stderr)
        return 1
    print(report["interpretation"])
    print("Event records:", report["event_record_counts"])
    print("Complete intervals:", len(report["intervals"]))
    for field in ["unmatched_begins", "unmatched_ends", "duplicate_begins", "unknown_event_types"]:
        print(field + ":", len(report[field]))
    for gap in report["observed_update_gaps"][:10]:
        print(f"Update gap {gap['gap_ms']:.3f} ms at {gap['start_seconds']:.6f}–{gap['end_seconds']:.6f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
