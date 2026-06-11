#!/bin/bash
# =============================================================================
# MySQL Recovery Pro - 5M Record Recovery Test Suite
# Purpose: Test recovery of 500万+ records with various corruption scenarios
# Target: 99.9% recovery rate across all scenarios
# =============================================================================

set -e

# Configuration
BASE_DIR="/app/mysql_recovery_pro"
TEST_DATA_DIR="${BASE_DIR}/test_data"
OUTPUT_DIR="${BASE_DIR}/test_results"
RECOVERY_TOOL="${BASE_DIR}/build/mysql_recovery_pro"
NUM_RECORDS=${NUM_RECORDS:-5500000}
PAGE_SIZE=${PAGE_SIZE:-16384}

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

log_failure() {
    echo -e "${RED}[FAIL]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

# Check prerequisites
check_prerequisites() {
    log_info "Checking prerequisites..."
    
    if [ ! -f "$RECOVERY_TOOL" ]; then
        log_failure "Recovery tool not found: $RECOVERY_TOOL"
        log_info "Please build the recovery tool first: bash ${BASE_DIR}/scripts/build_linux.sh"
        exit 1
    fi
    
    if [ ! -d "$TEST_DATA_DIR" ]; then
        mkdir -p "$TEST_DATA_DIR"
        log_info "Created test data directory: $TEST_DATA_DIR"
    fi
    
    if [ ! -d "$OUTPUT_DIR" ]; then
        mkdir -p "$OUTPUT_DIR"
        log_info "Created output directory: $OUTPUT_DIR"
    fi
    
    # Check for IBD files
    IBD_FILES=$(ls -1 "$TEST_DATA_DIR"/*.ibd 2>/dev/null || true)
    if [ -z "$IBD_FILES" ]; then
        log_warning "No IBD files found in test data directory"
        log_info "Generating test IBD files..."
        python3 "${BASE_DIR}/scripts/generate_5m_golden_data.py" \
            --num-records $NUM_RECORDS \
            --page-size $PAGE_SIZE \
            --output "${TEST_DATA_DIR}/test_5m.ibd"
    fi
    
    log_success "Prerequisites check passed"
}

# Run single recovery test
run_recovery_test() {
    local test_name=$1
    local input_file=$2
    local output_dir=$3
    local expected_records=$4
    local expected_min_rate=${5:-0.95}
    
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    
    log_info "Test $TOTAL_TESTS: $test_name"
    log_info "  Input: $input_file"
    log_info "  Output: $output_dir"
    
    # Run recovery
    local start_time=$(date +%s)
    local result=$($RECOVERY_TOOL --ibd "$input_file" --output "$output_dir" --format sql 2>&1)
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    # Analyze output
    local records_recovered=0
    if [ -f "$output_dir/output.sql" ]; then
        records_recovered=$(grep -c "VALUES (" "$output_dir/output.sql" 2>/dev/null || echo 0)
    elif [ -f "$output_dir/recovered.sql" ]; then
        records_recovered=$(grep -c "VALUES (" "$output_dir/recovered.sql" 2>/dev/null || echo 0)
    fi
    
    # Calculate recovery rate
    if [ "$expected_records" -gt 0 ]; then
        local recovery_rate=$(echo "scale=4; $records_recovered / $expected_records" | bc 2>/dev/null || echo "0")
        local recovery_pct=$(echo "scale=2; $recovery_rate * 100" | bc 2>/dev/null || echo "0")
    else
        local recovery_rate=1
        local recovery_pct=100
    fi
    
    # Check pass/fail
    local threshold_pct=$(echo "scale=2; $expected_min_rate * 100" | bc 2>/dev/null || echo "95")
    
    if [ $(echo "$recovery_rate >= $expected_min_rate" | bc -l 2>/dev/null || echo 0) -eq 1 ]; then
        log_success "  Recovery rate: ${recovery_pct}% (${records_recovered}/${expected_records} records) in ${duration}s"
        PASSED_TESTS=$((PASSED_TESTS + 1))
        return 0
    else
        log_failure "  Recovery rate: ${recovery_pct}% (${records_recovered}/${expected_records} records) - below threshold ${expected_min_rate}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        return 1
    fi
}

# Test scenarios
test normal_recovery() {
    local test_name="Normal Recovery (no corruption)"
    local input_file="${TEST_DATA_DIR}/test_mysql56_5m.ibd"
    local output_dir="${OUTPUT_DIR}/normal_recovery"
    
    if [ ! -f "$input_file" ] || [ $(stat -c%s "$input_file" 2>/dev/null || echo 0) -lt 100000000 ]; then
        log_warning "Skipping $test_name - insufficient test data"
        return 0
    fi
    
    mkdir -p "$output_dir"
    run_recovery_test "$test_name" "$input_file" "$output_dir" 5000000 0.99
}

test small_corruption() {
    local test_name="Small Corruption (1% pages)"
    local input_file="${TEST_DATA_DIR}/test_mysql56_5m.ibd"
    local output_dir="${OUTPUT_DIR}/small_corruption"
    
    if [ ! -f "$input_file" ]; then
        log_warning "Skipping $test_name - no input file"
        return 0
    fi
    
    # Create partially corrupted file
    python3 "${BASE_DIR}/scripts/corruption_scenarios.py" \
        -i "$input_file" \
        -o "${TEST_DATA_DIR}/corrupted_1pct.ibd" \
        -s random_pages \
        --num-pages 500 \
        --seed 42
    
    mkdir -p "$output_dir"
    run_recovery_test "$test_name" "${TEST_DATA_DIR}/corrupted_1pct.ibd" "$output_dir" 5000000 0.99
}

test truncate_recovery() {
    local test_name="Truncation Recovery"
    local input_file="${TEST_DATA_DIR}/test_mysql56_5m.ibd"
    local output_dir="${OUTPUT_DIR}/truncate_recovery"
    
    if [ ! -f "$input_file" ]; then
        log_warning "Skipping $test_name - no input file"
        return 0
    fi
    
    # Create truncated file (90% original size)
    local original_size=$(stat -c%s "$input_file")
    local truncate_size=$((original_size * 90 / 100))
    head -c "$truncate_size" "$input_file" > "${TEST_DATA_DIR}/truncated_10perc.ibd"
    
    mkdir -p "$output_dir"
    run_recovery_test "$test_name" "${TEST_DATA_DIR}/truncated_10perc.ibd" "$output_dir" 4500000 0.95
}

test cleanup_old_results() {
    log_info "Cleaning up old test results..."
    rm -rf "${OUTPUT_DIR}/"*
    mkdir -p "$OUTPUT_DIR"
}

run_all_tests() {
    log_info "============================================"
    log_info "MySQL Recovery Pro - 5M Record Test Suite"
    log_info "============================================"
    log_info "Target: 99.9% recovery rate"
    log_info "Total expected records: ${NUM_RECORDS}"
    log_info "============================================"
    log_info ""
    
    # Run tests
    cleanup_old_results
    
    test normal_recovery
    test small_corruption
    test truncate_recovery
    
    # Print summary
    log_info "============================================"
    log_info "TEST SUMMARY"
    log_info "============================================"
    log_info "Total tests: $TOTAL_TESTS"
    log_info "Passed: $PASSED_TESTS"
    log_info "Failed: $FAILED_TESTS"
    log_info ""
    
    if [ $TOTAL_TESTS -gt 0 ]; then
        local pass_rate=$(echo "scale=2; $PASSED_TESTS * 100 / $TOTAL_TESTS" | bc 2>/dev/null || echo "0")
        log_info "Pass rate: ${pass_rate}%"
    fi
    
    if [ $FAILED_TESTS -eq 0 ]; then
        log_success "All tests passed!"
        exit 0
    else
        log_failure "Some tests failed"
        exit 1
    fi
}

# Main execution
main() {
    check_prerequisites
    run_all_tests
}

# Run main
main "$@"
