#!/usr/bin/env python3
"""Retired legacy evaluation entry point.

Use scripts/disaster_harness for disposable, sealed experiments. This module does
not mutate live containers or interpret counts, hashes, or exit codes as recovery
results.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


STATUS = "RETIRED_UNSAFE_EVALUATION_PLATFORM"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Legacy evaluation platform (retired; no experiments are executed)."
    )
    parser.add_argument("--recovery-tool")
    parser.add_argument("--output-dir")
    parser.add_argument("--versions", nargs="+")
    parser.add_argument("--row-counts", nargs="+", type=int)
    parser.add_argument("--corruptions", nargs="+")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    result = {
        "status": STATUS,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "requested": {
            "recovery_tool": args.recovery_tool,
            "output_dir": args.output_dir,
            "versions": args.versions,
            "row_counts": args.row_counts,
            "corruptions": args.corruptions,
        },
        "message": (
            "No live-container mutation, mounted-datadir access, destructive SQL, "
            "recovery-rate calculation, hash verdict, or matrix evaluation is "
            "performed. Use scripts/disaster_harness for sealed comparisons."
        ),
    }
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(result["message"], file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
