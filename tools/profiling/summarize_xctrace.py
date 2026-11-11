#!/usr/bin/env python3
"""Summarize exported Instruments time-profile XML without engine instrumentation.

Weights are inclusive sampled stack weights, never exclusive frame percentages.
Optional address/symbol files must come from this trace's matching image mapping.
"""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path, help="xctrace time-profile XML export")
    parser.add_argument("--addresses", type=Path, help="one engine address per line")
    parser.add_argument("--symbols", type=Path, help="matching atos output, one line per address")
    parser.add_argument("--start", type=float, default=0, help="inclusive trace-relative seconds")
    parser.add_argument("--end", type=float, default=float("inf"), help="exclusive trace-relative seconds")
    parser.add_argument("--thread", default="Main Thread", help="thread display-name prefix")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--json", type=Path, dest="output", help="write complete summary JSON")
    args = parser.parse_args()
    if bool(args.addresses) != bool(args.symbols):
        parser.error("--addresses and --symbols must be supplied together")
    if not (0 <= args.start < args.end) or args.top < 1:
        parser.error("require 0 <= start < end and top >= 1")
    return args


def load_symbols(address_path, symbol_path):
    if address_path is None:
        return {}
    addresses = address_path.read_text().splitlines()
    symbols = symbol_path.read_text().splitlines()
    if len(addresses) != len(symbols):
        raise ValueError("address and symbol files have different line counts")
    values = [int(address, 0) for address in addresses]
    if len(set(values)) != len(values):
        raise ValueError("duplicate addresses in symbol mapping")
    return dict(zip(values, symbols))


def summarize(args):
    root = ET.parse(args.profile).getroot()
    nodes = [node for node in root.findall("node")
             if node.find("schema") is not None
             and node.find("schema").get("name") == "time-profile"]
    if len(nodes) != 1:
        raise ValueError("export exactly one time-profile table")
    table = nodes[0]
    definitions = {}
    for element in table.iter():
        key = element.get("id")
        if key:
            if key in definitions:
                raise ValueError("duplicate XML definition id: " + key)
            definitions[key] = element

    def resolve(element):
        visited = set()
        while element.get("ref"):
            key = element.get("ref")
            if key in visited:
                raise ValueError("cyclic XML reference: " + key)
            visited.add(key)
            element = definitions[key]
        return element

    columns = [column.findtext("mnemonic") for column in table.find("schema")]
    required = {"time", "thread", "thread-state", "weight", "stack"}
    if not required.issubset(columns):
        raise ValueError("missing required time-profile columns")
    symbols = load_symbols(args.addresses, args.symbols)
    threads, states, leaves, inclusive = Counter(), Counter(), Counter(), Counter()
    seconds = defaultdict(Counter)
    samples = 0
    mapped_samples = 0
    first = last = None
    total_weight = 0

    for row in table.findall("row"):
        cells = list(row)
        if len(cells) != len(columns):
            raise ValueError("row column count differs from schema")
        cells = dict(zip(columns, map(resolve, cells)))
        timestamp = int(cells["time"].text) / 1e9
        if not args.start <= timestamp < args.end:
            continue
        thread = cells["thread"].get("fmt", "(unknown thread)")
        weight = int(cells["weight"].text) / 1e6
        if weight < 0:
            raise ValueError("negative sample weight")
        threads[thread] += weight
        if not thread.startswith(args.thread):
            continue
        samples += 1
        total_weight += weight
        first = timestamp if first is None else min(first, timestamp)
        last = timestamp if last is None else max(last, timestamp)
        states[cells["thread-state"].get("fmt", "(unknown)")] += weight
        stack = cells["stack"]
        leaf = stack.get("fmt", "(unlabeled stack)").split(" ←", 1)[0]
        leaves[leaf] += weight
        seconds[int(timestamp)]["sample_weight_ms"] += weight
        names = set()
        for fragment in stack:
            fragment = resolve(fragment)
            if fragment.tag != "text-addresses":
                continue
            for address in (fragment.text or "").split():
                symbol = symbols.get(int(address, 0))
                if symbol:
                    names.add(symbol.split(" (in ", 1)[0])
        if names:
            mapped_samples += 1
        for name in names:
            inclusive[name] += weight

    def ranking(counter):
        return [{"name": name, "sample_weight_ms": weight,
                 "share_of_selected_sample_weight_percent":
                 100 * weight / total_weight if total_weight else 0}
                for name, weight in sorted(counter.items(), key=lambda item: (-item[1], item[0]))]

    return {
        "input": str(args.profile.resolve()),
        "selection": {"thread_prefix": args.thread, "start_seconds": args.start,
                      "end_seconds_exclusive": args.end if args.end != float("inf") else None},
        "interpretation": "Inclusive sampled stack weights overlap. Shares are of selected thread sample weight, not frame or wall time. Missing samples do not measure waits.",
        "sample_count": samples, "sample_weight_ms": total_weight,
        "first_sample_seconds": first, "last_sample_seconds": last,
        "samples_with_mapped_engine_frame": mapped_samples,
        "thread_sample_weights_ms_in_window": dict(threads),
        "selected_sample_state_weights_ms": dict(states),
        "leaf_ranking": ranking(leaves), "inclusive_engine_ranking": ranking(inclusive),
        "per_second": {str(key): dict(value) for key, value in sorted(seconds.items())},
        "symbol_mapping": {"addresses": str(args.addresses) if args.addresses else None,
                           "symbols": str(args.symbols) if args.symbols else None},
    }


def main():
    args = arguments()
    try:
        report = summarize(args)
        if not report["sample_count"]:
            raise ValueError("selection contains no matching samples")
        if args.output:
            args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        print("error: " + str(error), file=sys.stderr)
        return 1
    print(report["interpretation"])
    print(f"Selected {report['sample_count']} samples, {report['sample_weight_ms']:.1f} ms sample weight")
    for key in ("leaf_ranking", "inclusive_engine_ranking"):
        print("\n" + key + ":")
        for item in report[key][:args.top]:
            print(f"{item['sample_weight_ms']:9.1f} ms  {item['share_of_selected_sample_weight_percent']:6.2f}%  {item['name']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
