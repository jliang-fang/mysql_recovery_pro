#!/bin/bash
# =============================================================================
# Comprehensive MySQL Recovery Test Suite
# Tests TRUNCATE and rm -rf recovery scenarios for MySQL 5.6/5.7
# =============================================================================

set -e

# Configuration
TEST_DIR="/app/mysql_recovery_pro"
IBD_FILE="$TEST_DIR/test_data/test_users_5m.ibd"
OUTPUT_DIR="$TEST_DIR/test_output"
CONTAINER_NAME="mysql_recovery_56"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=================================================="
echo "MySQL Recovery Pro - Comprehensive Test Suite"
echo "=================================================="
echo ""

# Check if IBD file exists
if [ ! -f "$IBD_FILE" ]; then
    echo -e "${RED}Error: IBD file not found at $IBD_FILE${NC}"
    exit 1
fi

echo -e "${GREEN}✓${NC} IBD file found: $IBD_FILE"
echo "Size: $(du -h $IBD_FILE | cut -f1)"

# Check if container is running
if ! podman ps | grep -q "$CONTAINER_NAME"; then
    echo -e "${YELLOW}Warning: MySQL container not running. Starting...${NC}"
    $TEST_DIR/scripts/start_mysql56_env_podman.sh
else
    echo -e "${GREEN}✓${NC} MySQL 5.6 container is running"
fi

# Function to run recovery test
run_recovery_test() {
    local test_name=$1
    local mode=$2
    local expected_records=$3
    
    echo ""
    echo "=================================================="
    echo "Test: $test_name"
    echo "=================================================="
    
    local test_output="$OUTPUT_DIR/${test_name}_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$test_output"
    
    # Run recovery
    echo "Running recovery..."

    if [ -n "$mode" ]; then
        $TEST_DIR/build/mysql_recovery_pro \
            --ibd="$IBD_FILE" \
            --output="$test_output" \
            --format=csv \
            --$mode \
            --verbose 2>&1 | tee "$test_output/recovery.log"
    else
        $TEST_DIR/build/mysql_recovery_pro \
            --ibd="$IBD_FILE" \
            --output="$test_output" \
            --format=csv \
            --verbose 2>&1 | tee "$test_output/recovery.log"
    fi

    # Count recovered records
    if [ -f "$test_output/*.csv" ]; then
        local recovered_count=$(grep -c "^[0-9]" "$test_output"/*.csv 2>/dev/null || echo 0)
        echo ""
        echo "Recovered records: $recovered_count"
        
        if [ "$recovered_count" -ge "$expected_records" ]; then
            echo -e "${GREEN}✓${NC} Test PASSED: $recovered_count >= $expected_records records"
            return 0
        else
            echo -e "${YELLOW}⚠${NC} Test WARNING: $recovered_count < $expected_records records"
            return 1
        fi
    else
        echo -e "${RED}✗${NC} Test FAILED: No CSV output generated"
        return 1
    fi
}

# Function to verify MySQL container
verify_mysql() {
    echo ""
    echo "=================================================="
    echo "Verifying MySQL Container"
    echo "=================================================="
    
    local mysql_ready=$(podman exec "$CONTAINER_NAME" mysqladmin ping -h localhost -proot123 --silent 2>&1)
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓${NC} MySQL is running and accepting connections"
        
        # Verify test data
        local count=$(podman exec "$CONTAINER_NAME" mysql -uroot -proot123 recovery_test -N -e "SELECT COUNT(*) FROM test_users 2>/dev/null;" 2>/dev/null || echo "0")
        echo "Database record count: $count"
        
        return 0
    else
        echo -e "${RED}✗${NC} MySQL is not running"
        return 1
    fi
}

# Test 1: Basic Recovery
echo ""
echo -e "${YELLOW}Starting Test Suite...${NC}"
echo ""

# Verify MySQL first
verify_mysql

# Test 1: Basic Recovery (no special flags)
run_recovery_test "basic_recovery" "" "1"

# Test 2: TRUNCATE Recovery Mode
run_recovery_test "truncate_recovery" "truncate-recovery" "1"

# Test 3: rm -rf Recovery Mode
run_recovery_test "rmrf_recovery" "rmrf-recovery" "1"

# Test 4: Schemaless Heuristic Recovery
run_recovery_test "schemaless_recovery" "schemaless" "1"

# Test 5: Corruption Detection
run_recovery_test "corruption_check" "check-corruption" "1"

# Summary
echo ""
echo "=================================================="
echo "Test Summary"
echo "=================================================="
echo ""
echo "Tests completed. Check output in:"
echo "  $OUTPUT_DIR/"
echo ""
echo "Key Files:"
echo "  - test_users_5m.ibd: Source IBD file (5.5M records)"
echo "  - basic_recovery_*/: Basic recovery output"
echo "  - truncate_recovery_*/: TRUNCATE recovery output"
echo "  - rmrf_recovery_*/: rm -rf recovery output"
echo "  - recovered_table.csv: Recovered data"
echo ""

# Generate statistics
echo "Recovery Statistics:"
echo "  IBD file size: $(du -h $IBD_FILE | cut -f1)"
echo "  Page count: $(($(( $(stat -c%s $IBD_FILE) )) / 16384)) pages"
echo "  Page size: 16384 bytes"
echo ""

echo "Test suite completed."
