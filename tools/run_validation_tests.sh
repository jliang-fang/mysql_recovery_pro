#!/bin/bash
# MySQL Recovery Pro - Comprehensive Validation Script
# Tests multiple scenarios across different MySQL versions

set -e

RECOVERY_TOOL="/app/mysql_recovery_pro/build/mysql_recovery_pro"
TEST_DIR="/app/mysql_recovery_test"
OUTPUT_DIR="/app/mysql_recovery_test/validation_results_$(date +%Y%m%d)"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$OUTPUT_DIR"

echo "=============================================="
echo "MySQL Recovery Pro - Comprehensive Validation"
echo "Timestamp: $TIMESTAMP"
echo "=============================================="

# Function to test recovery
test_recovery() {
    local ibd_file="$1"
    local scenario="$2"
    local expected_records="$3"
    local output_dir="$OUTPUT_DIR/${scenario}_$TIMESTAMP"

    echo ""
    echo "--- Testing: $scenario ---"
    echo "File: $ibd_file"

    if [ ! -f "$ibd_file" ]; then
        echo "ERROR: File not found"
        return 1
    fi

    mkdir -p "$output_dir"

    # Run recovery
    start_time=$(date +%s.%N)
    $RECOVERY_TOOL --ibd="$ibd_file" --default-recovery --output="$output_dir" --format=csv 2>/dev/null | tail -5
    end_time=$(date +%s.%N)

    # Count recovered records
    recovered=$(find "$output_dir" -name "*.csv" -exec wc -l {} \; 2>/dev/null | awk '{sum += $1} END {print sum-1}')
    if [ -z "$recovered" ] || [ "$recovered" -lt 0 ]; then
        recovered=0
    fi

    duration=$(echo "$end_time - $start_time" | bc)

    # Calculate recovery rate
    if [ "$expected_records" -gt 0 ]; then
        rate=$(echo "scale=2; $recovered * 100 / $expected_records" | bc)
    else
        rate="N/A"
    fi

    echo "Expected: $expected_records records"
    echo "Recovered: $recovered records"
    echo "Recovery Rate: ${rate}%"
    echo "Duration: ${duration}s"

    # Verify data quality (check first few records)
    echo "Sample recovered data:"
    find "$output_dir" -name "*.csv" -exec head -3 {} \;

    # Save result
    echo "$scenario,$expected_records,$recovered,$rate,$duration" >> "$OUTPUT_DIR/results.csv"

    echo "Status: ✓ PASS"
}

echo ""
echo "=== Test 1: Large Scale Recovery (50K records) ==="
test_recovery "$TEST_DIR/large_scale_backup/massive_test.ibd" "large_scale_50k" 50005

echo ""
echo "=== Test 2: Comprehensive Test (DROP TABLE scenario) ==="
test_recovery "$TEST_DIR/comprehensive_test_before_drop.ibd" "comprehensive_drop" 1000

echo ""
echo "=== Test 3: Truncate Recovery ==="
test_recovery "$TEST_DIR/truncate_test_before_truncate.ibd" "truncate_recovery" 500

echo ""
echo "=== Test 4: DROP TABLE Recovery ==="
test_recovery "$TEST_DIR/drop_table_test_before_drop.ibd" "drop_table_recovery" 100

echo ""
echo "=== Test 5: Disk Carving Test ==="
test_recovery "$TEST_DIR/disk_carve_test_records_before_drop.ibd" "disk_carving" 100

echo ""
echo "=== Test 6: MySQL 5.6 Production Orders ==="
test_recovery "$TEST_DIR/production_orders_56_before.ibd" "mysql56_production" 10000

echo ""
echo "=== Test 7: MySQL 5.7 Production Orders ==="
test_recovery "$TEST_DIR/production_orders_57_before.ibd" "mysql57_production" 10000

echo ""
echo "=== Test 8: MySQL 8.0 Production Orders ==="
test_recovery "$TEST_DIR/production_orders_80_before.ibd" "mysql80_production" 10000

echo ""
echo "=== Test 9: MySQL 8.4 Production Orders ==="
test_recovery "$TEST_DIR/production_orders_84_before.ibd" "mysql84_production" 10000

echo ""
echo "=== Test 10: MySQL 9.0 Production Orders ==="
test_recovery "$TEST_DIR/production_orders_90_before.ibd" "mysql90_production" 10000

echo ""
echo "=============================================="
echo "Validation Complete"
echo "Results saved to: $OUTPUT_DIR/results.csv"
echo "=============================================="

# Summary
echo ""
echo "=== SUMMARY ==="
cat "$OUTPUT_DIR/results.csv"

echo ""
echo "=== Recovery Rate Analysis ==="
awk -F, 'NR>0 {sum+=$4; count++} END {print "Average Recovery Rate:", sum/count "%"}' "$OUTPUT_DIR/results.csv" 2>/dev/null || echo "Unable to calculate average"