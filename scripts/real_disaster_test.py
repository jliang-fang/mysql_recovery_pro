#!/usr/bin/env python3
"""Retired legacy disaster runner.

Use scripts/disaster_harness for disposable, sealed experiments. This entry point
refuses to mutate live containers or interpret row counts as recovery rates.
"""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


STATUS = "RETIRED_UNSAFE_RUNNER"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Legacy disaster runner (retired; no experiments are executed)."
    )
    parser.add_argument("--version")
    parser.add_argument("--rows", type=int)
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    result = {
        "status": STATUS,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "version": args.version,
        "rows": args.rows,
        "all": args.all,
        "scenarios": [],
        "message": (
            "No live-container deletion, raw-device access, disk carving, or "
            "row-count recovery-rate evaluation is performed. Use the disposable "
            "scripts/disaster_harness workflow instead."
        ),
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(result["message"], file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
