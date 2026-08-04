#!/bin/bash
# =============================================================================
# Phase 6: CI Report Pipeline
# =============================================================================
# Orchestrates the full validation pipeline:
#   1. Build with ASan/UBSan
#   2. Run P0 regression tests
#   3. Run P1 fuzz tests
#   4. Generate final validation report
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
REPORT_DIR="${PROJECT_DIR}/phase6_report"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

mkdir -p "$REPORT_DIR"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

# Step 1: Build with ASan/UBSan
log "========================================="
log "  Phase 6 CI Report Pipeline"
log "========================================="
log "Step 1/5: Build with ASan/UBSan..."

cd "$PROJECT_DIR"

if [ -f "${SCRIPT_DIR}/build_asan.sh" ]; then
    "${SCRIPT_DIR}/build_asan.sh" 2>&1 | tail -10
else
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        2>&1 | tail -3
    cmake --build build -j"$(nproc)" 2>&1 | tail -5
fi

log "Build complete."

# Step 2: Run P0 regression tests
log "Step 2/5: Running P0 regression tests..."
P0_LOG="${REPORT_DIR}/p0_regression_${TIMESTAMP}.log"
if [ -f "${BUILD_DIR}/tests/test_p0_regression" ]; then
    timeout 120 "${BUILD_DIR}/tests/test_p0_regression" 2>&1 | tee "$P0_LOG"
    P0_PASSED=$(grep -c "PASSED" "$P0_LOG" || true)
    P0_FAILED=$(grep -c "FAILED" "$P0_LOG" || true)
    log "P0: ${P0_PASSED} passed, ${P0_FAILED} failed"
else
    log "WARNING: test_p0_regression not found"
fi

# Step 3: Run P1 fuzz tests
log "Step 3/5: Running P1 salvage fuzz tests..."
P1_SALVAGE_LOG="${REPORT_DIR}/p1_salvage_${TIMESTAMP}.log"
if [ -f "${BUILD_DIR}/tests/test_p1_salvage_fuzz" ]; then
    timeout 120 "${BUILD_DIR}/tests/test_p1_salvage_fuzz" 2>&1 | tee "$P1_SALVAGE_LOG"
fi

log "Running P1 layout fuzz tests..."
P1_LAYOUT_LOG="${REPORT_DIR}/p1_layout_${TIMESTAMP}.log"
if [ -f "${BUILD_DIR}/tests/test_p1_layout_fuzz" ]; then
    timeout 30 "${BUILD_DIR}/tests/test_p1_layout_fuzz" 2>&1 | tee "$P1_LAYOUT_LOG"
fi

log "Running P1 type classifier fuzz tests..."
P1_TYPE_LOG="${REPORT_DIR}/p1_type_${TIMESTAMP}.log"
if [ -f "${BUILD_DIR}/tests/test_p1_type_classifier_fuzz" ]; then
    timeout 30 "${BUILD_DIR}/tests/test_p1_type_classifier_fuzz" 2>&1 | tee "$P1_TYPE_LOG"
fi

# Step 4: Generate report
log "Step 4/5: Generating validation report..."
REPORT_FILE="${REPORT_DIR}/validation_report_${TIMESTAMP}.html"
MATRIX_FILE="${PROJECT_DIR}/phase5_results/summary.json"

if [ -f "${SCRIPT_DIR}/phase6_generate_report.py" ]; then
    python3 "${SCRIPT_DIR}/phase6_generate_report.py" \
        --build-dir="$BUILD_DIR" \
        --matrix-results="$MATRIX_FILE" \
        --output="$REPORT_FILE" \
        2>&1
    log "Report generated: $REPORT_FILE"
else
    log "WARNING: phase6_generate_report.py not found"
fi

# Step 5: Summary
log "Step 5/5: Summary"
log "========================================="
log "  Phase 6 Pipeline Complete"
log "  Report: ${REPORT_FILE}"
log "  Logs:   ${REPORT_DIR}/"
log "========================================="

# Check for failures
TOTAL_FAIL=0
for log_file in "$P0_LOG" "$P1_SALVAGE_LOG" "$P1_LAYOUT_LOG" "$P1_TYPE_LOG"; do
    if [ -f "$log_file" ] && grep -q "FAILED" "$log_file" 2>/dev/null; then
        FAIL_COUNT=$(grep "FAILED" "$log_file" | grep -oP 'Test[s]?:\s+\d+' | grep -oP '\d+$' || echo "1")
        TOTAL_FAIL=$((TOTAL_FAIL + FAIL_COUNT))
    fi
done

if [ "$TOTAL_FAIL" -gt 0 ]; then
    log "WARNING: ${TOTAL_FAIL} test(s) failed"
    exit 1
fi

log "All tests passed."
exit 0