#!/usr/bin/env python3
"""Retired legacy validation entry point.

Use scripts/disaster_harness for disposable, sealed experiments. This module does
not mutate live datadirs or interpret file hashes, row counts, or exit codes as
row-level recovery evidence.
"""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


STATUS = "RETIRED_UNSAFE_VALIDATION_PIPELINE"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Legacy validation pipeline (retired; no experiments are executed)."
    )
    parser.add_argument("--setup", action="store_true")
    parser.add_argument("--truncate", action="store_true")
    parser.add_argument("--rmrf", action="store_true")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--rows", type=int)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    result = {
        "status": STATUS,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "requested_actions": {
            "setup": args.setup,
            "truncate": args.truncate,
            "rmrf": args.rmrf,
            "all": args.all,
        },
        "rows": args.rows,
        "scenarios": [],
        "message": (
            "No live-datadir mutation, destructive filesystem operation, copied-IBD "
            "recovery claim, row-count rate, file-hash rate, or threshold evaluation "
            "is performed. Use scripts/disaster_harness for sealed comparisons."
        ),
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(result["message"], file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
