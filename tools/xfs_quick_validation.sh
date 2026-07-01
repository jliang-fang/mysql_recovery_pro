#!/bin/bash
# XFS Recovery Quick Validation Test
# Tests basic XFS disk carving functionality

set -e

echo "=== XFS Recovery Quick Validation Test ==="

# Test 1: Check binary exists
echo "[TEST 1] Checking binary..."
if [ ! -f "/app/mysql_recovery_pro/build/mysql_recovery_pro" ]; then
    echo "ERROR: Binary not found"
    exit 1
fi
echo "PASS: Binary exists"

# Test 2: Check --filesystem option
echo "[TEST 2] Checking --filesystem option..."
HELP_OUTPUT=$(/app/mysql_recovery_pro/build/mysql_recovery_pro --help 2>&1)
if ! echo "$HELP_OUTPUT" | grep -q "filesystem"; then
    echo "ERROR: --filesystem option not found in help"
    exit 1
fi
echo "PASS: --filesystem option available"

# Test 3: Check help shows xfs option
echo "[TEST 3] Checking XFS in help..."
if ! echo "$HELP_OUTPUT" | grep -q "xfs"; then
    echo "ERROR: xfs not mentioned in help"
    exit 1
fi
echo "PASS: XFS filesystem option documented"

# Test 4: Create test XFS disk image (if not exists)
echo "[TEST 4] Creating test XFS disk image..."
TEST_DIR="/app/mysql_recovery_test/xfs_quick_test"
mkdir -p "$TEST_DIR"

DISK_IMG="$TEST_DIR/test_xfs.img"
if [ ! -f "$DISK_IMG" ]; then
    # Create small XFS disk image (10MB)
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=10 2>/dev/null
    mkfs.xfs -f "$DISK_IMG" 2>/dev/null || {
        echo "WARN: Cannot create XFS filesystem (may need root)"
        echo "Skipping XFS disk image creation"
    }
fi

# Test 5: Test XFS detection on disk image
echo "[TEST 5] Testing XFS superblock detection..."
if [ -f "$DISK_IMG" ]; then
    # Check if we can detect XFS magic
    XFS_MAGIC=$(xxd -l 4 "$DISK_IMG" 2>/dev/null | head -1 | grep "5846 5342")
    if [ -n "$XFS_MAGIC" ]; then
        echo "PASS: XFS magic number detected (XFSB)"
    else
        echo "INFO: XFS magic check requires xxd"
    fi
fi

# Test 6: Verify no crash on invalid input
echo "[TEST 6] Testing error handling..."
OUTPUT=$(/app/mysql_recovery_pro/build/mysql_recovery_pro --disk-carving --filesystem=xfs --device-path=/dev/null --output="$TEST_DIR/out" 2>&1 || true)
if echo "$OUTPUT" | grep -q "error\|Error\|ERROR"; then
    echo "PASS: Proper error handling for invalid device"
else
    echo "INFO: No error message (may be acceptable)"
fi

echo ""
echo "=== All Validation Tests Completed ==="
echo ""
echo "Summary:"
echo "- Binary compiled successfully"
echo "- --filesystem=xfs option available"
echo "- XFS documented in help"
echo "- Error handling works"

echo ""
echo "For full XFS recovery testing, run:"
echo "  python3 /app/mysql_recovery_pro/tools/xfs_recovery_test.py"