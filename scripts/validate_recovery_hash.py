#!/usr/bin/env python3
"""Retired legacy hash validator.

Use scripts/disaster_harness for typed, sealed comparisons. File hashes, row
counts, and threshold percentages are not recovery evidence.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


STATUS = "RETIRED_UNSAFE_HASH_VALIDATION"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Legacy hash validator (retired; no validation is executed)."
    )
    parser.add_argument("--gen-hashes")
    parser.add_argument("--golden")
    parser.add_argument("--recovered")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--threshold", type=float)
    args = parser.parse_args()

    result = {
        "status": STATUS,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "requested": {
            "gen_hashes": args.gen_hashes,
            "golden": args.golden,
            "recovered": args.recovered,
            "threshold": args.threshold,
        },
        "message": (
            "No CSV hash generation, row-count recovery rate, threshold verdict, "
            "or Markdown validation report is produced. Use "
            "scripts/disaster_harness for typed canonical comparisons."
        ),
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(result["message"], file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
