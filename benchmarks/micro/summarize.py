#!/usr/bin/env python3
"""Summarize paired Linux benchmark CSVs without mixing allocation modes."""

import argparse
import csv
from collections import defaultdict
from pathlib import Path
import statistics
import sys


FIELDS = {
    "scenario", "subjects", "repetition", "operations", "wall_ns", "cpu_ns",
    "allocation_tracking", "allocations", "allocated_bytes", "voluntary_switches",
    "involuntary_switches", "transport_reads", "checksum",
}
SCENARIOS = {
    "snapshot_copy", "snapshot_view", "mutation", "mutation_snapshot",
    "idle_unrouted", "idle_routed",
}


def read_samples(directory):
    groups = defaultdict(list)
    paths = sorted(directory.glob("*.csv"))
    if not paths:
        raise ValueError(f"No CSV files found in {directory}")
    for path in paths:
        stage = path.name.split("-", 1)[0]
        if stage not in ("before", "after"):
            raise ValueError(f"{path}: filename must start with before- or after-")
        with path.open(newline="") as stream:
            reader = csv.DictReader(stream)
            if set(reader.fieldnames or ()) != FIELDS:
                raise ValueError(f"{path}: unexpected CSV columns")
            file_mode = None
            for line, raw in enumerate(reader, 2):
                try:
                    if None in raw:
                        raise ValueError("too many CSV values")
                    scenario = raw["scenario"]
                    if scenario not in SCENARIOS:
                        raise ValueError(f"unknown scenario {scenario!r}")
                    sample = {
                        field: int(raw[field]) for field in FIELDS
                        if field not in ("scenario", "allocations", "allocated_bytes")
                    }
                    if any(value < 0 for value in sample.values()):
                        raise ValueError("negative measurement")
                    mode = sample["allocation_tracking"]
                    if mode not in (0, 1):
                        raise ValueError("allocation_tracking must be 0 or 1")
                    if file_mode is not None and file_mode != mode:
                        raise ValueError("one CSV file must not mix allocation modes")
                    file_mode = mode
                    for field in ("allocations", "allocated_bytes"):
                        if mode == 0:
                            if raw[field] != "NA":
                                raise ValueError("uninstrumented allocations must be NA")
                        else:
                            sample[field] = int(raw[field])
                            if sample[field] < 0:
                                raise ValueError("negative allocation measurement")
                    if sample["wall_ns"] == 0:
                        raise ValueError("wall_ns must be positive")
                    operations = sample["operations"]
                    subjects = sample["subjects"]
                    if scenario.startswith("idle_"):
                        if operations != 0 or sample["checksum"] != subjects:
                            raise ValueError("invalid idle checksum or operation count")
                    else:
                        if operations == 0:
                            raise ValueError("non-idle operations must be positive")
                        expected = 0
                        if scenario.startswith("snapshot_"):
                            expected = operations * subjects
                        elif scenario == "mutation_snapshot":
                            expected = operations * (2 * subjects + 1)
                        if sample["checksum"] != expected:
                            raise ValueError("callback or membership checksum mismatch")
                    normalized = "snapshot" if scenario.startswith("snapshot_") else scenario
                    groups[mode, normalized, subjects, stage].append(sample)
                except (KeyError, TypeError, ValueError) as error:
                    raise ValueError(f"{path}:{line}: {error}") from error
    keys = {(mode, scenario, subjects) for mode, scenario, subjects, _ in groups}
    for key in keys:
        for stage in ("before", "after"):
            if (*key, stage) not in groups:
                raise ValueError(f"Missing {stage} samples for {key}")
        before_operations = {row["operations"] for row in groups[(*key, "before")]}
        after_operations = {row["operations"] for row in groups[(*key, "after")]}
        if before_operations != after_operations:
            raise ValueError(f"Mismatched operation counts for {key}")
    if not any(key[0] == 0 for key in keys) or not any(key[0] == 1 for key in keys):
        raise ValueError("Both uninstrumented timing and instrumented allocation data are required")
    scenarios = {(scenario, subjects) for mode, scenario, subjects in keys if mode == 0}
    if scenarios != {(scenario, subjects) for mode, scenario, subjects in keys if mode == 1}:
        raise ValueError("Timing and allocation runs must cover the same scenarios")
    return groups


def summary(values):
    return f"{statistics.median(values):.3f} [{min(values):.3f}, {max(values):.3f}]"


def change(before, after):
    baseline = statistics.median(before)
    if baseline == 0:
        return "n/a"
    return f"{(statistics.median(after) / baseline - 1) * 100:+.2f}%"


def compare(groups, mode, idle, metrics):
    keys = sorted({(scenario, subjects) for tracking, scenario, subjects, _ in groups
                   if tracking == mode and scenario.startswith("idle_") == idle})
    difference = "observed_median_delta" if idle else "median_change_percent"
    print("scenario,subjects,metric,before_median_[min_max],after_median_[min_max],"
          f"{difference},before_samples,after_samples")
    for scenario, subjects in keys:
        before = groups[mode, scenario, subjects, "before"]
        after = groups[mode, scenario, subjects, "after"]
        for label, measure in metrics:
            old_values = [measure(row) for row in before]
            new_values = [measure(row) for row in after]
            # Use a CSV writer because range strings contain commas.
            delta = (f"{statistics.median(new_values) - statistics.median(old_values):+.3f}"
                     if idle else change(old_values, new_values))
            csv.writer(sys.stdout).writerow([
                scenario, subjects, label, summary(old_values), summary(new_values),
                delta, len(before), len(after),
            ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "directory", nargs="?", type=Path,
        default=Path(__file__).resolve().parents[1] / "results" / "arm64-linux-2026-11-03",
    )
    args = parser.parse_args()
    try:
        groups = read_samples(args.directory)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print("Timing: allocation_tracking=0 only; units are ns per operation.")
    compare(groups, 0, False, [
        ("wall_ns/op", lambda row: row["wall_ns"] / row["operations"]),
        ("cpu_ns/op", lambda row: row["cpu_ns"] / row["operations"]),
    ])
    print("\nAllocations: allocation_tracking=1 only; instrumented times are excluded.")
    compare(groups, 1, False, [
        ("allocations/op", lambda row: row["allocations"] / row["operations"]),
        ("requested_bytes/op", lambda row: row["allocated_bytes"] / row["operations"]),
    ])
    print("\nIdle: allocation_tracking=0 only, normalized to 500 ms measured wall time.")
    print("Idle differences are observations, not evidence of an idle-path optimization.")
    print("Context switches include all process threads; transport reads are not writer wakeups.")
    compare(groups, 0, True, [
        ("cpu_ms/500ms", lambda row: row["cpu_ns"] / row["wall_ns"] * 500),
        ("voluntary_switches/500ms", lambda row: row["voluntary_switches"] * 5e8 / row["wall_ns"]),
        ("involuntary_switches/500ms", lambda row: row["involuntary_switches"] * 5e8 / row["wall_ns"]),
        ("transport_reads/500ms", lambda row: row["transport_reads"] * 5e8 / row["wall_ns"]),
    ])


if __name__ == "__main__":
    main()
