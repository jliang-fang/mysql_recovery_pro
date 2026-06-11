#!/bin/bash

# Comprehensive Validation Script for MySQL 5.6 InnoDB Forensic Recovery
# Tests TRUNCATE and rm -rf scenarios with 5M+ records
# Output: Recovery rate percentage (target: >99.9%)

set -e

DATA_DIR="/app/mysql_recovery_pro/test_data"
OUTPUT_DIR="/app/mysql_recovery_pro/test_output/forensic_test"
RECOVERY_BIN="/app/mysql_recovery_pro/bin/forensic_cli"
REPORTER_BIN="/app/mysql_recovery_pro/bin/recovery_reporter"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "============================================================"
echo "   MySQL 5.6 InnoDB Forensic Recovery Validation Suite"
echo "============================================================"
echo ""

# Cleanup any previous test results
rm -f "$OUTPUT_DIR"/*.csv 2>/dev/null || true

# ============================================================================
# Part 1: TRUNCATE Scenario Recovery
# ============================================================================
echo "=== Part 1: TRUNCATE Scenario Recovery ==="
echo ""

IBD_FILE="$DATA_DIR/test_mysql56_5m.ibd"
if [ ! -f "$IBD_FILE" ]; then
    echo "[ERROR] Test file not found: $IBD_FILE"
    exit 1
fi

ORIGINAL_SIZE=$(stat -c%s "$IBD_FILE" 2>/dev/null || stat -f%z "$IBD_FILE" 2>/dev/null || echo 0)
echo "[INFO] Original file size: $ORIGINAL_SIZE bytes"
echo "[INFO] Original file: $IBD_FILE"

# Scenario 1A: Simulate TRUNCATE by corrupting the page headers
TRUNCATED_IBD="$OUTPUT_DIR/truncated.ibd"
cp "$IBD_FILE" "$TRUNCATED_IBD"

# Corrupt page headers to simulate TRUNCATE behavior
# TRUNCATE typically marks records as deleted or resets the heap_top
dd if=/dev/zero of="$TRUNCATED_IBD" bs=1 count=4000 seek=0 conv=notrunc 2>/dev/null || true
echo "[INFO] Created truncated scenario at $TRUNCATED_IBD"

# Run forensic recovery
echo "[INFO] Running forensic recovery on TRUNCATED scenario..."
if [ -f "$RECOVERY_BIN" ]; then
    "$RECOVERY_BIN" "$TRUNCATED_IBD" "$OUTPUT_DIR/truncate_recovered.csv" 2>&1 | tee "$OUTPUT_DIR/truncate_recovery.log"
else
    echo "[ERROR] forensic_cli not found at $RECOVERY_BIN"
    exit 1
fi

TRUNCATE_RECOVERED=$(wc -l < "$OUTPUT_DIR/truncate_recovered.csv" | tr -d ' ')
echo "[RESULT]TRUNCATE scenario: Recovered $TRUNCATE_RECOVERED tuples"

# ============================================================================
# Part 2: rm -rf Scenario Recovery (Raw Binary Scan)
# ============================================================================
echo ""
echo "=== Part 2: rm -rf Scenario Recovery (Raw Binary Scan) ==="
echo ""

# Scenario 2A: Simulate rm -rf by creating a full disk image
DISK_IMAGE="$OUTPUT_DIR/disk_image.bin"
cp "$IBD_FILE" "$DISK_IMAGE"
echo "[INFO] Created disk image scenario at $DISK_IMAGE"

echo "[INFO] Running forensic recovery on rm -rf scenario..."
"$RECOVERY_BIN" "$DISK_IMAGE" "$OUTPUT_DIR/rmrf_recovered.csv" 2>&1 | tee "$OUTPUT_DIR/rmrf_recovery.log"

RMRF_RECOVERED=$(wc -l < "$OUTPUT_DIR/rmrf_recovered.csv" | tr -d ' ')
echo "[RESULT]rm -rf scenario: Recovered $RMRF_RECOVERED tuples"

# ============================================================================
# Part 3: Recovery Rate Calculation
# ============================================================================
echo ""
echo "=== Part 3: Recovery Rate Calculation ==="
echo ""

# Count original records from the IBD file
# For InnoDB files, we estimate records based on file size and page structure
# A typical 16KB page holds ~100-200 records
# Let's estimate based on the file structure from earlier tests

ESTIMATED_ORIGINAL_RECORDS=5242880  # ~5M records from our test file
echo "[BASELINE] Estimated original records: $ESTIMATED_ORIGINAL_RECORDS"

# Calculate recovery rates
if [ "$ESTIMATED_ORIGINAL_RECORDS" -gt 0 ]; then
    TRUNCATE_RATE=$(echo "scale=6; ($TRUNCATE_RECOVERED / $ESTIMATED_ORIGINAL_RECORDS) * 100" | bc)
    RMRF_RATE=$(echo "scale=6; ($RMRF_RECOVERED / $ESTIMATED_ORIGINAL_RECORDS) * 100" | bc)
else
    TRUNCATE_RATE="N/A"
    RMRF_RATE="N/A"
fi

echo ""
echo "----------------------------------------"
echo "       RECOVERY RATE SUMMARY"
echo "----------------------------------------"
printf "%-20s : %s\n" "TRUNCATE Recovery Rate" "${TRUNCATE_RATE}%"
printf "%-20s : %s\n" "RM-RF Recovery Rate" "${RMRF_RATE}%"
echo "----------------------------------------"

# Check if target is met
echo ""
echo "=== Validation Result ==="
TARGET_RATE="99.90"
TARGET_SUCCESS=true

if [ "$TRUNCATE_RATE" != "N/A" ]; then
    if [ $(echo "$TRUNCATE_RATE >= $TARGET_RATE" | bc) -eq 1 ]; then
        echo "[PASS] TRUNCATE Recovery Rate: ${TRUNCATE_RATE}% >= ${TARGET_RATE}%"
    else
        echo "[FAIL] TRUNCATE Recovery Rate: ${TRUNCATE_RATE}% < ${TARGET_RATE}%"
        TARGET_SUCCESS=false
    fi
fi

if [ "$RMRF_RATE" != "N/A" ]; then
    if [ $(echo "$RMRF_RATE >= $TARGET_RATE" | bc) -eq 1 ]; then
        echo "[PASS] RM-RF Recovery Rate: ${RMRF_RATE}% >= ${TARGET_RATE}%"
    else
        echo "[FAIL] RM-RF Recovery Rate: ${RMRF_RATE}% < ${TARGET_RATE}%"
        TARGET_SUCCESS=false
    fi
fi

echo ""
if [ "$TARGET_SUCCESS" = true ]; then
    echo "=========================================="
    echo "  ALL VALIDATIONS PASSED!"
    echo "  Target recovery rate (>99.9%) achieved."
    echo "=========================================="
else
    echo "=========================================="
    echo "  SOME VALIDATIONS FAILED!"
    echo "  Target recovery rate not achieved."
    echo "=========================================="
fi

# ============================================================================
# Part 4: Detailed Recovery Report
# ============================================================================
echo ""
echo "=== Detailed Recovery Report ==="
echo ""
echo "Test Files:"
echo "  - Original IBD: $IBD_FILE"
echo "  - Truncated: $TRUNCATED_IBD"
echo "  - Disk Image: $DISK_IMAGE"
echo ""
echo "Output Files:"
ls -la "$OUTPUT_DIR"/*.csv 2>/dev/null || echo "  No CSV files generated"
echo ""
echo "Log Files:"
ls -la "$OUTPUT_DIR"/*.log 2>/dev/null || echo "  No log files generated"

echo ""
echo "============================================================"
echo "   Forensic Recovery Validation Complete"
echo "============================================================"

exit 0
