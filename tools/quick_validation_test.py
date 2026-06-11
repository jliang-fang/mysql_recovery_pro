#!/usr/bin/env python3
"""
Quick validation test for mysql_recovery_pro extreme recovery.
Tests core functionality without requiring MySQL container.
"""

import subprocess
import sys
import os
import hashlib
import tempfile
import struct
from pathlib import Path


def create_test_ibd_file(output_path: str, num_pages: int = 100) -> bool:
    """Create a test .ibd file with valid InnoDB INDEX pages."""
    print(f"Creating test IBD file: {output_path}")

    page_size = 16384

    with open(output_path, 'wb') as f:
        for page_no in range(num_pages):
            page = bytearray(page_size)

            # FIL header
            # Page number (bytes 4-7)
            struct.pack_into('<I', page, 4, page_no)

            # Prev/Next pages (bytes 8-15) = 0xFFFFFFFF
            for i in range(8, 16):
                page[i] = 0xFF

            # LSN (bytes 16-23)
            lsn = 1000 + page_no * 100
            struct.pack_into('<Q', page, 16, lsn)

            # Page type (bytes 24-25) = FIL_PAGE_INDEX (0x45BF)
            page[24] = 0x45
            page[25] = 0xBF

            # Space ID (bytes 34-37)
            struct.pack_into('<I', page, 34, 1)

            # PAGE header (offset 38)
            # N_DIR_SLOTS
            page[38] = 0
            page[39] = 2

            # HEAP_TOP
            page[40] = 0
            page[41] = 200

            # N_HEAP
            page[42] = 0
            page[43] = 5

            # LEVEL = 0 (leaf)
            page[50] = 0
            page[51] = 0

            # INDEX_ID
            struct.pack_into('<Q', page, 52, 0x12345678ABCDEF00)

            # INFIMUM at offset 99
            page[99] = 0x02
            page[100] = 0x00
            page[101] = 120  # Next offset
            page[102] = 0

            # SUPREMUM at offset 160
            page[160] = 0x03
            page[161] = 0x00
            page[162] = 0xFF
            page[163] = 0xFF

            # Page directory at end
            page[16382] = 160
            page[16383] = 0
            page[16380] = 99
            page[16381] = 0

            f.write(page)

    print(f"  Created {num_pages} pages ({num_pages * page_size} bytes)")
    return True


def run_recovery_tool(tool_path: str, input_path: str, output_dir: str) -> dict:
    """Run the recovery tool and return statistics."""
    print(f"\nRunning recovery tool...")

    # Check if tool exists
    if not os.path.exists(tool_path):
        print(f"  ERROR: Tool not found: {tool_path}")
        return {"success": False, "error": "Tool not found"}

    # Create output directory
    os.makedirs(output_dir, exist_ok=True)

    # Run recovery
    cmd = [
        tool_path,
        "scan",
        "-i", input_path,
        "-o", output_dir,
        "-f", "sql"
    ]

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=300
        )

        stats = {
            "success": result.returncode == 0,
            "returncode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr
        }

        # Parse output for statistics
        for line in result.stdout.split('\n'):
            if 'pages' in line.lower():
                print(f"  {line}")

        return stats

    except subprocess.TimeoutExpired:
        return {"success": False, "error": "Timeout"}
    except Exception as e:
        return {"success": False, "error": str(e)}


def run_unit_tests(test_dir: str) -> bool:
    """Run C++ unit tests."""
    print("\n" + "=" * 60)
    print("Running C++ Unit Tests")
    print("=" * 60)

    test_executables = [
        "test_extreme_scenarios",
        "test_page_scanner",
        "test_heuristic_scanner",
        "test_btree_traversal",
    ]

    passed = 0
    failed = 0

    for test_name in test_executables:
        test_path = os.path.join(test_dir, test_name)
        if not os.path.exists(test_path):
            print(f"  SKIP: {test_name} (not found)")
            continue

        print(f"\nRunning {test_name}...")
        result = subprocess.run(
            [test_path, "--gtest_brief=1"],
            capture_output=True,
            text=True,
            timeout=60
        )

        if result.returncode == 0:
            print(f"  PASS: {test_name}")
            passed += 1
        else:
            print(f"  FAIL: {test_name}")
            if result.stderr:
                print(f"    {result.stderr[:200]}")
            failed += 1

    print(f"\n{'=' * 60}")
    print(f"Unit Tests: {passed} passed, {failed} failed")
    print(f"{'=' * 60}")

    return failed == 0


def main():
    print("=" * 60)
    print("MySQL Recovery Pro - Quick Validation Test")
    print("=" * 60)

    # Paths
    script_dir = Path(__file__).parent
    project_dir = script_dir.parent
    build_dir = project_dir / "build"
    tool_path = build_dir / "mysql_recovery_pro"

    # Create temp directory for test
    test_dir = Path(tempfile.mkdtemp(prefix="recovery_validation_"))
    print(f"\nTest directory: {test_dir}")

    try:
        # Step 1: Create test IBD file
        print("\n" + "-" * 40)
        print("Step 1: Create Test Data")
        print("-" * 40)

        test_ibd = test_dir / "test_table.ibd"
        create_test_ibd_file(str(test_ibd), num_pages=100)

        # Compute hash of test file
        with open(test_ibd, 'rb') as f:
            test_hash = hashlib.sha256(f.read()).hexdigest()
        print(f"  Test file SHA-256: {test_hash[:16]}...")

        # Step 2: Run unit tests
        print("\n" + "-" * 40)
        print("Step 2: Run Unit Tests")
        print("-" * 40)

        unit_tests_passed = run_unit_tests(str(build_dir))

        # Step 3: Test recovery tool
        print("\n" + "-" * 40)
        print("Step 3: Test Recovery Tool")
        print("-" * 40)

        output_dir = test_dir / "recovered"
        stats = run_recovery_tool(str(tool_path), str(test_ibd), str(output_dir))

        if stats["success"]:
            print("  Recovery tool executed successfully")
        else:
            print(f"  Recovery tool failed: {stats.get('error', 'Unknown error')}")

        # Step 4: Summary
        print("\n" + "=" * 60)
        print("VALIDATION SUMMARY")
        print("=" * 60)

        print(f"\nTest IBD file: {test_ibd.stat().st_size} bytes")
        print(f"Unit tests: {'PASSED' if unit_tests_passed else 'FAILED'}")
        print(f"Recovery tool: {'PASSED' if stats['success'] else 'FAILED'}")

        # Overall result
        all_passed = unit_tests_passed and stats["success"]

        print(f"\n{'=' * 60}")
        if all_passed:
            print("ALL VALIDATION TESTS PASSED")
        else:
            print("SOME TESTS FAILED - See details above")
        print("=" * 60)

        return 0 if all_passed else 1

    finally:
        # Cleanup
        import shutil
        if test_dir.exists():
            shutil.rmtree(test_dir)
            print(f"\nCleaned up test directory")


if __name__ == "__main__":
    sys.exit(main())
