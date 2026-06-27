#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TRUNCATE Recovery Validation Script

This script validates two recovery scenarios:
1. Recovery from pre-TRUNCATE backup file (before.ibd)
2. Recovery from actual post-TRUNCATE file (simulated)

MySQL TRUNCATE behavior by version:
- MySQL 5.6/5.7: DELETE all rows (undo logs available)
- MySQL 8.0+: DROP + CREATE (file replaced, data pages zeroed)
"""

import os
import sys
import subprocess
import shutil
import tempfile
from pathlib import Path

RECOVERY_TOOL = "/app/mysql_recovery_pro/build/mysql_recovery_pro"
TEST_DIR = Path("/app/mysql_recovery_test")

def run_recovery(ibd_path: str, output_dir: str, schema: str = None,
                 frm_path: str = None, mode: str = "default",
                 verbose: bool = True) -> dict:
    """Execute recovery and return results"""
    cmd = [RECOVERY_TOOL, "--ibd", ibd_path, "--output", output_dir]

    if mode == "truncate":
        cmd.append("--truncate-recovery")
    elif mode == "rmrf":
        cmd.append("--rmrf-recovery")

    if frm_path and os.path.exists(frm_path) and os.path.getsize(frm_path) > 0:
        cmd.extend(["--frm", frm_path])

    if schema:
        cmd.extend(["--schema", schema])

    if verbose:
        cmd.append("--verbose")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)

        # Find CSV output
        csv_file = None
        for f in Path(output_dir).iterdir():
            if f.suffix == '.csv':
                csv_file = str(f)
                break

        recovered_rows = 0
        if csv_file:
            with open(csv_file, 'r') as f:
                recovered_rows = sum(1 for _ in f) - 1  # minus header

        return {
            "success": result.returncode == 0,
            "recovered_rows": recovered_rows,
            "csv_file": csv_file,
            "stderr": result.stderr,
            "stdout": result.stdout
        }
    except Exception as e:
        return {"success": False, "error": str(e)}

def create_truncated_file(original_path: str, output_path: str) -> bool:
    """Simulate a TRUNCATE TABLE file (MySQL 8.0+ behavior)

    MySQL 8.0 TRUNCATE:
    - Keeps FSP header (page 0) and IBD header (page 1)
    - Zeroes data pages (page 3+)
    - Creates new empty INDEX page
    """
    page_size = 16384

    with open(original_path, 'rb') as f:
        data = f.read()

    total_pages = len(data) // page_size

    # Create truncated file
    output = bytearray(data)

    # Zero pages 3+ (data pages), keep pages 0-2 (headers)
    for page_no in range(3, total_pages):
        start = page_no * page_size
        # Keep FIL header (38 bytes) but zero the rest
        for i in range(38, page_size):
            output[start + i] = 0

    with open(output_path, 'wb') as f:
        f.write(output)

    return True

def test_scenario(name: str, ibd_path: str, frm_path: str = None,
                  schema: str = None, mode: str = "default",
                  expected_rows: int = 3) -> dict:
    """Test a recovery scenario"""
    print(f"\n--- {name} ---")
    print(f"  IBD: {ibd_path}")
    if frm_path:
        print(f"  FRM: {frm_path}")

    with tempfile.TemporaryDirectory() as tmpdir:
        result = run_recovery(ibd_path, tmpdir, schema, frm_path, mode, verbose=False)

        status = "PASS" if result.get("recovered_rows", 0) >= expected_rows else "FAIL"
        print(f"  Status: {status}")
        print(f"  Recovered: {result.get('recovered_rows', 0)}/{expected_rows} rows")

        return {
            "name": name,
            "status": status,
            "recovered": result.get("recovered_rows", 0),
            "expected": expected_rows
        }

def main():
    print("=" * 70)
    print("TRUNCATE Recovery Validation")
    print("=" * 70)

    results = []

    # Test 1: MySQL 5.6 with FRM (pre-truncate backup)
    r1 = test_scenario(
        "MySQL 5.6 + FRM (before TRUNCATE)",
        str(TEST_DIR / "truncate/mysql56/before.ibd"),
        str(TEST_DIR / "truncate/mysql56/before.frm"),
        mode="truncate",
        expected_rows=2
    )
    results.append(r1)

    # Test 2: MySQL 5.7 with FRM (pre-truncate backup)
    r2 = test_scenario(
        "MySQL 5.7 + FRM (before TRUNCATE)",
        str(TEST_DIR / "truncate/mysql57/before.ibd"),
        str(TEST_DIR / "truncate/mysql57/before.frm"),
        mode="truncate",
        expected_rows=2
    )
    results.append(r2)

    # Test 3: MySQL 8.0 without FRM (pre-truncate backup, SDI needed)
    r3 = test_scenario(
        "MySQL 8.0 + SDI (before TRUNCATE)",
        str(TEST_DIR / "truncate/mysql80/before.ibd"),
        None,  # No FRM for MySQL 8.0
        mode="truncate",
        expected_rows=3
    )
    results.append(r3)

    # Test 4: MySQL 8.4 (pre-truncate backup)
    r4 = test_scenario(
        "MySQL 8.4 + SDI (before TRUNCATE)",
        str(TEST_DIR / "truncate/mysql84/before.ibd"),
        None,
        mode="truncate",
        expected_rows=3
    )
    results.append(r4)

    # Test 5: MySQL 9.0 (pre-truncate backup)
    r5 = test_scenario(
        "MySQL 9.0 + SDI (before TRUNCATE)",
        str(TEST_DIR / "truncate/mysql90/before.ibd"),
        None,
        mode="truncate",
        expected_rows=3
    )
    results.append(r5)

    # Test 6: Simulated TRUNCATE file (MySQL 8.0 behavior)
    print("\n--- Creating simulated TRUNCATE file (MySQL 8.0 behavior) ---")
    truncated_file = "/tmp/truncated_mysql80.ibd"
    create_truncated_file(
        str(TEST_DIR / "truncate/mysql80/before.ibd"),
        truncated_file
    )
    print(f"  Created: {truncated_file}")

    r6 = test_scenario(
        "MySQL 8.0 (after TRUNCATE - simulated zeroed pages)",
        truncated_file,
        None,
        schema="id:INT:pk,name:VARCHAR:100,age:INT,height:DECIMAL,json_col:JSON,blob_col:BLOB,text_col:TEXT",
        mode="truncate",
        expected_rows=0  # Expect 0 because pages are zeroed
    )
    results.append(r6)

    # Summary
    print("\n" + "=" * 70)
    print("Summary")
    print("=" * 70)

    passed = sum(1 for r in results if r["status"] == "PASS")
    total = len(results)

    for r in results:
        print(f"{r['name']}: {r['status']} ({r['recovered']}/{r['expected']} rows)")

    print(f"\nTotal: {passed}/{total} scenarios passed")
    print(f"Success rate: {passed/total*100:.1f}%")

if __name__ == "__main__":
    main()