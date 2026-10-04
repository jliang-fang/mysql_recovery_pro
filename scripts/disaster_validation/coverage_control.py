#!/usr/bin/env python3
"""Classify a Phase B row-loss as block-coverage boundary vs a real tool defect.

After TRUNCATE / rm -rf the old pages may be overwritten by later writes. That
is a property of the media, not of the recovery tool, and the prompt keeps it
outside the tool's acceptance criteria ("已落盘、未被覆盖").

For every clustered-index page of the preserved pre-disaster tablespace the
control asks:

  1. does that page still appear verbatim in the post-disaster image?
  2. if the missing ones are zeroed out, does direct .ibd recovery produce
     exactly the row count the disaster case recovered?

If both hold, every surviving page contributed its rows and the shortfall is
explained by overwritten blocks.

Usage:
  python3 -m scripts.disaster_validation.coverage_control --case <case-dir>
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

sys.path.insert(0, "/app/mysql_recovery_pro")

from scripts.disaster_validation.csv_to_canonical import csv_to_canonical_dataset  # noqa: E402
from scripts.disaster_validation.phase_a_offline_orchestrator import (  # noqa: E402
    build_schema_argument,
)
from scripts.disaster_harness.comparator import compare  # noqa: E402

BINARY = "/app/mysql_recovery_pro/build/mysql_recovery_pro"


def clustered_pages(ibd: Path, page_size: int, index_id: int) -> list[int]:
    pages = []
    data = ibd.read_bytes()
    for offset in range(0, len(data), page_size):
        if offset + page_size > len(data):
            break
        page_type = struct.unpack_from(">H", data, offset + 24)[0]
        page_index = struct.unpack_from(">Q", data, offset + 66)[0]
        if page_type == 0x45BF and page_index == index_id:
            pages.append(offset // page_size)
    return pages


def surviving(page_numbers: list[int], ibd: Path, image: Path, page_size: int) -> list[int]:
    data = ibd.read_bytes()
    digest_to_page = {hashlib.sha256(data[i * page_size:(i + 1) * page_size]).hexdigest(): i
                      for i in page_numbers}
    blob = image.read_bytes()
    found = set()
    step = 4096 if page_size % 4096 == 0 else page_size
    for offset in range(0, max(0, len(blob) - page_size + 1), step):
        digest = hashlib.sha256(blob[offset:offset + page_size]).hexdigest()
        if digest in digest_to_page:
            found.add(digest_to_page[digest])
    return sorted(found)


def main() -> int:
    parser = argparse.ArgumentParser(description="Block-coverage control for a Phase B case")
    parser.add_argument("--case", required=True, type=Path)
    args = parser.parse_args()
    case = args.case

    record = json.loads((case / "reports" / "scenario.json").read_text(encoding="utf-8"))
    truth = json.loads((case / "truth" / "ground_truth.json").read_text(encoding="utf-8"))
    verification = record.get("verification", {})
    runs = verification.get("runs", {})
    primary = runs.get("disk_carving") or {}
    comparison = primary.get("comparison", {}) or {}
    false_negatives = comparison.get("false_negatives") or 0

    ibd_files = sorted((case / "persistent-input").glob("*.ibd"))
    images = sorted(Path(record["recovery"]["image"]).parent.glob(case.name + ".img"))
    if not ibd_files or not images:
        print("coverage_control: missing preserved input or media image")
        return 2

    index_id = record.get("clustered_index_id")
    if index_id is None:
        print("coverage_control: no clustered index id recorded")
        return 2

    page_size = 16384
    ibd, image = ibd_files[0], images[0]
    pages = clustered_pages(ibd, page_size, int(index_id))
    keep = set(surviving(pages, ibd, image, page_size))
    missing_pages = sorted(set(pages) - keep)

    data = bytearray(ibd.read_bytes())
    for number in missing_pages:
        start = number * page_size
        data[start:start + page_size] = b"\x00" * page_size

    with tempfile.TemporaryDirectory(prefix="coverage-") as tmp:
        masked = Path(tmp) / "masked.ibd"
        masked.write_bytes(bytes(data))
        out = Path(tmp) / "out"
        out.mkdir()
        schema_arg = build_schema_argument(truth["schema"])
        proc = subprocess.run(
            [BINARY, f"--ibd={masked}", f"--output={out}", "--format=csv",
             f"--page-size={page_size}", f"--schema={schema_arg}"],
            capture_output=True, text=True, timeout=1800, check=False)
        csv_path = out / "recovered.csv"
        if not csv_path.is_file():
            print(f"coverage_control: masked recovery produced no CSV (rc={proc.returncode})")
            return 2
        masked_doc = csv_to_canonical_dataset(
            csv_path, truth["database"], truth["table"], truth["schema"],
            truth.get("mysql_version", ""))
        masked_comparison = compare(truth, masked_doc, f"{case.name}_coverage_control")

    recovered_rows = comparison.get("actual_rows") or 0
    masked_rows = masked_comparison.get("actual_rows") or 0
    masked_correct = masked_comparison.get("correct_rows") or 0
    truth_rows = comparison.get("expected_rows") or truth.get("row_count")

    explained = (masked_rows == recovered_rows == masked_correct
                 and (truth_rows - masked_rows) == (false_negatives or 0))
    verdict = "PASS_BLOCK_COVERAGE" if explained else "FAIL"

    report = {
        "case": case.name,
        "verdict": verdict,
        "clustered_index_id": index_id,
        "clustered_pages": len(pages),
        "surviving_pages": len(keep),
        "overwritten_pages": missing_pages,
        "overwritten_page_count": len(missing_pages),
        "rows_expected": truth_rows,
        "rows_recovered": recovered_rows,
        "rows_missing": (truth_rows - recovered_rows),
        "rows_on_surviving_pages": masked_rows,
        "rows_correct_on_surviving_pages": masked_correct,
        "false_negatives": false_negatives,
        "exactly_explained_by_overwritten_blocks": explained,
        "reason": ("every row on a page that survived verbatim was recovered; the "
                   "shortfall sits only on blocks TRUNCATE/overwrite consumed"
                   if explained else
                   "shortfall not fully explained by overwritten pages"),
        "recovery_binary": BINARY,
        "masked_control_stderr": proc.stderr.strip()[-400:],
    }
    (case / "reports" / "coverage_control.json").write_text(
        json.dumps(report, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"{report['verdict']:>20}  {case.name}  "
          f"recovered={recovered_rows}/{truth_rows} "
          f"overwritten_pages={len(missing_pages)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
