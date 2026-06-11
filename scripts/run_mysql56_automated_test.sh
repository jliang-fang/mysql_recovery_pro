#!/bin/bash
# =============================================================================
# Automated MySQL 5.6/5.7 Recovery Test Suite
# Author: jliang.fang (Global MySQL Kernel QA Expert)
# Purpose: Run comprehensive tests for 99.9%+ recovery rate
# =============================================================================

set -e

# ==============================================================================
# Configuration
# ==============================================================================
BASE_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_SCRIPT="$BASE_DIR/comprehensive_mysql56_test_suite.py"
BUILD_DIR="$BASE_DIR/../build"
OUTPUT_DIR="$BASE_DIR/../test_output/mysql56_automated_$(date +%Y%m%d_%H%M%S)"
PYTHON3=${PYTHON3:-python3}

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ==============================================================================
# Utility Functions
# ==============================================================================

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

check_prerequisites() {
    log_info "Checking prerequisites..."

    # Check Python3
    if ! command -v python3 &> /dev/null; then
        log_fail "Python3 is not installed"
        exit 1
    fi
    log_success "Python3 found: $(python3 --version)"

    # Check recovery tool
    if [ ! -f "$BUILD_DIR/mysql_recovery_pro" ]; then
        log_warning "Recovery tool not found, attempting to build..."
        cd "$BASE_DIR/.." && make -C build 2>/dev/null || {
            log_fail "Failed to build recovery tool"
            exit 1
        }
    fi

    if [ ! -f "$BUILD_DIR/mysql_recovery_pro" ]; then
        log_fail "Recovery tool build failed"
        exit 1
    fi
    log_success "Recovery tool found: $BUILD_DIR/mysql_recovery_pro"

    # Check test data
    if [ ! -d "$BASE_DIR/../test_data" ]; then
        log_warning "Test data directory not found, will generate test data..."
    else
        log_success "Test data directory found"
    fi

    # Create output directory
    mkdir -p "$OUTPUT_DIR"
    log_success "Output directory: $OUTPUT_DIR"
}

run_test_suite() {
    log_info "=========================================="
    log_info "Running MySQL 5.6/5.7 Comprehensive Test Suite"
    log_info "=========================================="

    # Run the Python test suite
    "$PYTHON3" "$TEST_SCRIPT" \
        --test-data-dir="$BASE_DIR/../test_data" \
        --output-dir="$OUTPUT_DIR" \
        --tool-path="$BUILD_DIR/mysql_recovery_pro" \
        --page-size=16384

    local exit_code=$?
    return $exit_code
}

run_individual_tests() {
    log_info "=========================================="
    log_info "Running Individual Recovery Tests"
    log_info "=========================================="

    local IBD_FILE="$BASE_DIR/../test_data/test_mysql56_100k.ibd"

    # Test 1: Basic recovery
    log_info "Test 1: Basic Recovery"
    if [ -f "$IBD_FILE" ]; then
        timeout 120 "$BUILD_DIR/mysql_recovery_pro" \
            --ibd="$IBD_FILE" \
            --mysql-version=56 \
            --output="$OUTPUT_DIR/test_basic" \
            --format=csv 2>&1 | tee "$OUTPUT_DIR/test_basic.log"
        log_success "Test 1 completed"
    fi

    # Test 2: Truncate recovery
    log_info "Test 2: Truncate Recovery"
    if [ -f "$IBD_FILE" ]; then
        local orig_size=$(stat -c%s "$IBD_FILE")
        local trunc_size=$((orig_size * 80 / 100))
        dd if="$IBD_FILE" of="$OUTPUT_DIR/test_trunc.ibd" bs=1 count=$trunc_size 2>/dev/null
        timeout 120 "$BUILD_DIR/mysql_recovery_pro" \
            --ibd="$OUTPUT_DIR/test_trunc.ibd" \
            --truncate-recovery \
            --mysql-version=56 \
            --output="$OUTPUT_DIR/test_trunc" \
            --format=csv 2>&1 | tee "$OUTPUT_DIR/test_trunc.log"
        rm -f "$OUTPUT_DIR/test_trunc.ibd"
        log_success "Test 2 completed"
    fi

    # Test 3: RMRF recovery
    log_info "Test 3: RMRF Recovery"
    if [ -f "$IBD_FILE" ]; then
        cp "$IBD_FILE" "$OUTPUT_DIR/test_rmrf.ibd"
        # Zero out first 10%
        dd if=/dev/zero of="$OUTPUT_DIR/test_rmrf.ibd" bs=1 count=100000 seek=0 conv=notrunc 2>/dev/null
        timeout 120 "$BUILD_DIR/mysql_recovery_pro" \
            --ibd="$OUTPUT_DIR/test_rmrf.ibd" \
            --rmrf-recovery \
            --mysql-version=56 \
            --output="$OUTPUT_DIR/test_rmrf" \
            --format=csv 2>&1 | tee "$OUTPUT_DIR/test_rmrf.log"
        rm -f "$OUTPUT_DIR/test_rmrf.ibd"
        log_success "Test 3 completed"
    fi

    # Test 4: No checksum verification
    log_info "Test 4: No Checksum Recovery"
    timeout 120 "$BUILD_DIR/mysql_recovery_pro" \
        --ibd="$IBD_FILE" \
        --no-checksum \
        --mysql-version=56 \
        --output="$OUTPUT_DIR/test_no_cs" \
        --format=csv 2>&1 | tee "$OUTPUT_DIR/test_no_cs.log"
    log_success "Test 4 completed"
}

generate_report() {
    log_info "=========================================="
    log_info "Generating Test Report"
    log_info "=========================================="

    local report_file="$OUTPUT_DIR/test_report.md"
    local json_file="$OUTPUT_DIR/test_summary.json"

    cat > "$report_file" << REPORT_EOF_HEADER
# MySQL 5.6/5.7 Recovery Test Report

## Executive Summary

## Test Results

| Test | Status | Records Recovered | Recovery Rate | Notes |
|------|--------|-------------------|---------------|-------|
REPORT_EOF_HEADER

    # Add results from JSON if available
    if [ -f "$json_file" ]; then
        # Parse JSON and add to report
        $PYTHON3 -c "
import json
import sys
with open('$json_file', 'r') as f:
    data = json.load(f)
    for result in data.get('results', []):
        print(f\"| {result.get('test_name', 'N/A')} | {result.get('status', 'N/A')} | {result.get('records_recovered', 0)} | {result.get('recovery_rate', 0):.2f}% | {result.get('notes', '')} |\")
" >> "$report_file"
    fi

    cat >> "$report_file" << REPORT_EOF_FOOTER

## Performance Metrics

- Pages scanned: --
- Records recovered: --
- Duration: --
- Memory usage: --

## Key Findings

1. MySQL 5.6 compatibility: --
2. MySQL 5.7 compatibility: --
3. Truncate recovery: --
4. rm -rf recovery: --
5. Torn page recovery: --
6. Checksum bypass: --

## Recommendations

1. Ensure proper checksum handling
2. Optimize truncate recovery algorithm
3. Implement better rm -rf recovery

## Conclusion

Report generated at: $(date)
REPORT_EOF_FOOTER

    log_success "Report generated: $report_file"
}

# ==============================================================================
# Main
# ==============================================================================

main() {
    log_info "=========================================="
    log_info "MySQL 5.6/5.7 Automated Recovery Test Suite"
    log_info "=========================================="
    log_info "Started at: $(date)"
    log_info ""

    # Check prerequisites
    check_prerequisites

    # Run test suite
    if run_test_suite; then
        log_success "Comprehensive test suite completed"
    else
        log_warning "Comprehensive test suite had some failures"
    fi

    # Run individual tests
    run_individual_tests

    # Generate report
    generate_report

    log_info ""
    log_info "=========================================="
    log_info "Test Suite Completed at: $(date)"
    log_info "Output directory: $OUTPUT_DIR"
    log_info "=========================================="
}

main "$@"
