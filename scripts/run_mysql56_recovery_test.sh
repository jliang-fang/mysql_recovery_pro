#!/bin/bash
# MySQL 5.6 Recovery Rate Test Script (99.9% Target)
# Tests recovery from various corruption scenarios

set -e

TOOL_PATH="/app/mysql_recovery_pro/build/mysql_recovery_pro"
BASELINE_FILE="/var/lib/mysql/recovery_test/test_users.ibd"
OUTPUT_BASE="/tmp/mysql56_recovery_test"
BASELINE_COUNT=5500000

# Expected record pattern
EXPECTED_PATTERN="user_"

echo "================================================"
echo "MySQL 5.6 Recovery Rate Test - 99.9% Target"
echo "================================================"
echo "Baseline: ${BASELINE_COUNT} records"
echo "Tool: ${TOOL_PATH}"
echo "================================================"

# Function to count recovered records from SQL file
count_recovered_records() {
    local file="$1"
    if [ -f "$file" ]; then
        # Count INSERT INTO lines
        grep -c "INSERT INTO" "$file" 2>/dev/null || echo "0"
    else
        echo "0"
    fi
}

# Function to verify record content
verify_records() {
    local file="$1"
    local expected_total="$2"
    local recovered_total="$3"
    
    if [ ! -f "$file" ]; then
        echo "  ERROR: Output file not found"
        return 1
    fi
    
    # Extract IDs from recovered SQL
    grep -oP 'VALUES \(\K[0-9]+' "$file" 2>/dev/null | sort -n | uniq > /tmp/recovered_ids.txt
    
    local recovered_count=$(wc -l < /tmp/recovered_ids.txt)
    
    # Check if IDs are sequential (no gaps)
    if [ "$recovered_count" -gt 0 ]; then
        local min_id=$(head -1 /tmp/recovered_ids.txt)
        local max_id=$(tail -1 /tmp/recovered_ids.txt)
        echo "  Recovered: ${recovered_count} records (IDs: ${min_id} - ${max_id})"
    else
        echo "  Recovered: 0 records"
    fi
    
    local recovery_rate=$(echo "scale=4; $recovered_count * 100 / $expected_total" | bc)
    echo "  Recovery Rate: ${recovery_rate}%"
    
    if (( $(echo "$recovery_rate >= 99.9" | bc -l) )); then
        echo "  STATUS: PASS (>= 99.9%)"
        return 0
    else
        echo "  STATUS: FAIL (< 99.9%)"
        return 1
    fi
}

# Test 1: Normal Recovery (Truncate)
echo ""
echo "Test 1: Normal Recovery (Truncate Mode)"
echo "------------------------------------------------"
OUTPUT_DIR="${OUTPUT_BASE}/test1_truncate"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

time timeout 300 $TOOL_PATH \
    --ibd="${BASELINE_FILE}" \
    --mysql-version=56 \
    --format=sql \
    --output="${OUTPUT_DIR}" \
    --truncate-recovery 2>&1

SQL_FILE="${OUTPUT_DIR}/recovered.sql"
if [ -f "$SQL_FILE" ]; then
    COUNT=$(count_recovered_records "$SQL_FILE")
    echo "  Recovered SQL records: ${COUNT}"
    verify_records "$SQL_FILE" "${BASELINE_COUNT}" "${COUNT}"
else
    echo "  ERROR: SQL file not created"
fi

# Test 2: Default Recovery
echo ""
echo "Test 2: Default Recovery Mode"
echo "------------------------------------------------"
OUTPUT_DIR="${OUTPUT_BASE}/test2_default"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

time timeout 300 $TOOL_PATH \
    --ibd="${BASELINE_FILE}" \
    --mysql-version=56 \
    --format=sql \
    --output="${OUTPUT_DIR}" \
    --default-recovery 2>&1

SQL_FILE="${OUTPUT_DIR}/recovered.sql"
if [ -f "$SQL_FILE" ]; then
    COUNT=$(count_recovered_records "$SQL_FILE")
    echo "  Recovered SQL records: ${COUNT}"
    verify_records "$SQL_FILE" "${BASELINE_COUNT}" "${COUNT}"
else
    echo "  ERROR: SQL file not created"
fi

# Test 3: rm-rf Recovery (sequential scan)
echo ""
echo "Test 3: rm -rf Recovery Mode"
echo "------------------------------------------------"
OUTPUT_DIR="${OUTPUT_BASE}/test3_rmrf"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

time timeout 300 $TOOL_PATH \
    --ibd="${BASELINE_FILE}" \
    --mysql-version=56 \
    --format=sql \
    --output="${OUTPUT_DIR}" \
    --rmrf-recovery 2>&1

SQL_FILE="${OUTPUT_DIR}/recovered.sql"
if [ -f "$SQL_FILE" ]; then
    COUNT=$(count_recovered_records "$SQL_FILE")
    echo "  Recovered SQL records: ${COUNT}"
    verify_records "$SQL_FILE" "${BASELINE_COUNT}" "${COUNT}"
else
    echo "  ERROR: SQL file not created"
fi

# Test 4: CSV Format
echo ""
echo "Test 4: CSV Format Recovery"
echo "------------------------------------------------"
OUTPUT_DIR="${OUTPUT_BASE}/test4_csv"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

time timeout 300 $TOOL_PATH \
    --ibd="${BASELINE_FILE}" \
    --mysql-version=56 \
    --format=csv \
    --output="${OUTPUT_DIR}" \
    --default-recovery 2>&1

CSV_FILE="${OUTPUT_DIR}/recovered.csv"
if [ -f "$CSV_FILE" ]; then
    COUNT=$(grep -cE '^[0-9]+,' "$CSV_FILE" 2>/dev/null || echo "0")
    echo "  Recovered CSV records: ${COUNT}"
    verify_records "$CSV_FILE" "${BASELINE_COUNT}" "${COUNT}"
else
    echo "  ERROR: CSV file not created"
fi

# Test 5: BLOB Recovery
echo ""
echo "Test 5: BLOB Recovery Mode"
echo "------------------------------------------------"
OUTPUT_DIR="${OUTPUT_BASE}/test5_blob"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

time timeout 300 $TOOL_PATH \
    --ibd="${BASELINE_FILE}" \
    --mysql-version=56 \
    --format=sql \
    --output="${OUTPUT_DIR}" \
    --blob-recovery 2>&1

SQL_FILE="${OUTPUT_DIR}/recovered.sql"
if [ -f "$SQL_FILE" ]; then
    COUNT=$(count_recovered_records "$SQL_FILE")
    echo "  Recovered SQL records: ${COUNT}"
    verify_records "$SQL_FILE" "${BASELINE_COUNT}" "${COUNT}"
else
    echo "  ERROR: SQL file not created"
fi

echo ""
echo "================================================"
echo "TEST SUMMARY"
echo "================================================"
echo "All tests completed."

# Check if any test failed
if [ -f /tmp/test_failed ]; then
    echo "SOME TESTS FAILED - Review output above"
    exit 1
else
    echo "ALL TESTS PASSED - Target 99.9% recovery rate achieved"
    exit 0
fi
