#!/bin/bash
# Comprehensive Test Suite for MySQL Recovery Pro
# Tests various corruption scenarios and recovery capabilities

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
TEST_DATA_DIR="$PROJECT_DIR/test_data"
OUTPUT_DIR="/tmp/recovery_tests_$(date +%Y%m%d_%H%M%S)"

echo "=========================================="
echo "MySQL Recovery Pro - Comprehensive Tests"
echo "=========================================="
echo "Output directory: $OUTPUT_DIR"
echo ""

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counter
TESTS_PASSED=0
TESTS_FAILED=0

# Function to run a test
run_test() {
    local test_name="$1"
    local test_cmd="$2"
    local expected_output="$3"
    
    echo -n "Testing: $test_name ... "
    
    if eval "$test_cmd" > "$OUTPUT_DIR/${test_name}.log" 2>&1; then
        if [ -z "$expected_output" ] || grep -q "$expected_output" "$OUTPUT_DIR/${test_name}.log"; then
            echo -e "${GREEN}PASSED${NC}"
            ((TESTS_PASSED++))
            return 0
        else
            echo -e "${RED}FAILED (unexpected output)${NC}"
            echo "Expected: $expected_output"
            echo "Got:"
            cat "$OUTPUT_DIR/${test_name}.log"
            ((TESTS_FAILED++))
            return 1
        fi
    else
        echo -e "${RED}FAILED (command failed)${NC}"
        echo "Command: $test_cmd"
        echo "Output:"
        cat "$OUTPUT_DIR/${test_name}.log"
        ((TESTS_FAILED++))
        return 1
    fi
}

# Function to run a test that is expected to fail
run_test_expected_fail() {
    local test_name="$1"
    local test_cmd="$2"
    
    echo -n "Testing (expected fail): $test_name ... "
    
    if eval "$test_cmd" > "$OUTPUT_DIR/${test_name}.log" 2>&1; then
        echo -e "${YELLOW}PASSED (unexpected success)${NC}"
        ((TESTS_FAILED++))
        return 1
    else
        echo -e "${GREEN}PASSED (expected failure)${NC}"
        ((TESTS_PASSED++))
        return 0
    fi
}

echo "=== Basic Recovery Tests ==="

# Test 1: Compact format recovery
run_test "compact_recovery" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_compact.ibd --output=$OUTPUT_DIR/compact --format=csv" \
    "Recovery completed"

# Test 2: Redundant format recovery
run_test "redundant_recovery" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_redundant.ibd --output=$OUTPUT_DIR/redundant --format=csv" \
    "Recovery completed"

# Test 3: MySQL 5.6 compatibility
run_test "mysql56_compatibility" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_mysql56.ibd --output=$OUTPUT_DIR/mysql56 --format=csv" \
    "Recovery completed"

# Test 4: MySQL 8.0 compatibility
run_test "mysql80_compatibility" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_mysql80.ibd --output=$OUTPUT_DIR/mysql80 --format=csv" \
    "Recovery completed"

echo ""
echo "=== Corruption Detection Tests ==="

# Test 5: Header corruption detection
run_test "header_corruption_detection" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_header_corrupt.ibd --output=$OUTPUT_DIR/corrupt --check-corruption --format=csv" \
    "Corruption check completed"

# Test 6: Truncated file recovery
run_test "truncated_recovery" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_truncated.ibd --output=$OUTPUT_DIR/trunc --truncate-recovery --format=csv" \
    "Recovery completed"

# Test 7: SDI page recovery
run_test "sdi_recovery" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_sdi.ibd --output=$OUTPUT_DIR/sdi --format=csv" \
    "Recovery completed"

echo ""
echo "=== Advanced Recovery Tests ==="

# Test 8: Complete file simulation
run_test "complete_file_recovery" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_complete.ibd --output=$OUTPUT_DIR/complete --format=csv" \
    "Recovery completed"

# Test 9: Header repair mode
run_test "header_repair" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_header_corrupt.ibd --output=$OUTPUT_DIR/repair --repair --format=csv" \
    "Recovery completed"

# Test 10: Validate mode
run_test "validate_mode" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$TEST_DATA_DIR/test_compact.ibd --output=$OUTPUT_DIR/validate --validate --format=csv" \
    "Recovery completed"

echo ""
echo "=== Edge Case Tests ==="

# Test 11: Empty file handling
run_test_expected_fail "empty_file" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=/tmp/empty.ibd --output=$OUTPUT_DIR/empty --format=csv"

# Test 12: Non-existent file handling
run_test_expected_fail "nonexistent_file" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=/tmp/nonexistent.ibd --output=$OUTPUT_DIR/nonexistent --format=csv"

# Test 13: Very small file handling
dd if=/dev/zero of=$OUTPUT_DIR/small.ibd bs=100 count=1 2>/dev/null
run_test_expected_fail "small_file" \
    "$BUILD_DIR/mysql_recovery_pro --ibd=$OUTPUT_DIR/small.ibd --output=$OUTPUT_DIR/small --format=csv"

echo ""
echo "=== Summary ==="
echo "Tests passed: $TESTS_PASSED"
echo "Tests failed: $TESTS_FAILED"
echo ""

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed. Check logs in $OUTPUT_DIR${NC}"
    exit 1
fi
