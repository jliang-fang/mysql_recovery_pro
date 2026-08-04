#!/usr/bin/env python3
"""
Phase 5: Analyze Disaster Recovery Matrix Results

Parses the summary.json produced by phase5_disaster_matrix.sh and
generates a formatted report of recovery rates across all tested
configurations.
"""

import json
import sys
import os
from pathlib import Path


def load_results(results_file: str) -> list:
    if not os.path.exists(results_file):
        print(f"ERROR: Results file not found: {results_file}")
        sys.exit(1)
    with open(results_file) as f:
        return json.load(f)


def compute_stats(results: list) -> dict:
    stats = {
        "total_tests": len(results),
        "by_version": {},
        "by_scenario": {},
        "by_filesystem": {},
        "overall": {"total_rows": 0, "total_ms": 0, "test_count": 0},
    }

    for r in results:
        ver = r.get("mysql_version", "unknown")
        sce = r.get("scenario", "unknown")
        fs = r.get("filesystem", "unknown")
        rows = r.get("recovered_rows", 0)
        ms = r.get("elapsed_ms", 0)

        stats["overall"]["total_rows"] += rows
        stats["overall"]["total_ms"] += ms
        stats["overall"]["test_count"] += 1

        # By version
        stats["by_version"].setdefault(ver, {"tests": 0, "total_rows": 0, "total_ms": 0})
        stats["by_version"][ver]["tests"] += 1
        stats["by_version"][ver]["total_rows"] += rows
        stats["by_version"][ver]["total_ms"] += ms

        # By scenario
        stats["by_scenario"].setdefault(sce, {"tests": 0, "total_rows": 0, "total_ms": 0})
        stats["by_scenario"][sce]["tests"] += 1
        stats["by_scenario"][sce]["total_rows"] += rows
        stats["by_scenario"][sce]["total_ms"] += ms

        # By filesystem
        stats["by_filesystem"].setdefault(fs, {"tests": 0, "total_rows": 0, "total_ms": 0})
        stats["by_filesystem"][fs]["tests"] += 1
        stats["by_filesystem"][fs]["total_rows"] += rows
        stats["by_filesystem"][fs]["total_ms"] += ms

    return stats


def print_report(stats: dict) -> None:
    print("=" * 70)
    print("  DISASTER RECOVERY MATRIX — ANALYSIS REPORT")
    print("=" * 70)

    o = stats["overall"]
    avg_ms = o["total_ms"] / max(o["test_count"], 1)
    print(f"\nTotal tests: {o['test_count']}")
    print(f"Total rows recovered: {o['total_rows']}")
    print(f"Average time per test: {avg_ms:.0f} ms")
    print()

    print("-" * 70)
    print("  By MySQL Version")
    print("-" * 70)
    print(f"{'Version':<12} {'Tests':<8} {'Rows':<12} {'Avg ms':<10}")
    for ver in sorted(stats["by_version"].keys()):
        v = stats["by_version"][ver]
        print(f"{ver:<12} {v['tests']:<8} {v['total_rows']:<12} "
              f"{v['total_ms'] / max(v['tests'], 1):<10.0f}")

    print()
    print("-" * 70)
    print("  By Scenario")
    print("-" * 70)
    print(f"{'Scenario':<15} {'Tests':<8} {'Rows':<12} {'Avg ms':<10}")
    for sce in sorted(stats["by_scenario"].keys()):
        s = stats["by_scenario"][sce]
        print(f"{sce:<15} {s['tests']:<8} {s['total_rows']:<12} "
              f"{s['total_ms'] / max(s['tests'], 1):<10.0f}")

    print()
    print("-" * 70)
    print("  By Filesystem")
    print("-" * 70)
    print(f"{'Filesystem':<12} {'Tests':<8} {'Rows':<12} {'Avg ms':<10}")
    for fs in sorted(stats["by_filesystem"].keys()):
        f = stats["by_filesystem"][fs]
        print(f"{fs:<12} {f['tests']:<8} {f['total_rows']:<12} "
              f"{f['total_ms'] / max(f['tests'], 1):<10.0f}")

    print()
    print("=" * 70)
    print("  END OF REPORT")
    print("=" * 70)


def main():
    results_file = sys.argv[1] if len(sys.argv) > 1 else "phase5_results/summary.json"
    results = load_results(results_file)
    stats = compute_stats(results)
    stats["overall"]["total_rows"] = sum(
        r.get("recovered_rows", 0) for r in results
    )
    print_report(stats)


if __name__ == "__main__":
    main()