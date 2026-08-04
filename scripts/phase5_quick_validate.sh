#!/bin/bash
# =============================================================================
# Phase 5: Quick Validation Script
# =============================================================================
# Runs a minimal subset of the disaster recovery matrix to verify basic
# recovery functionality. Intended for CI or rapid iteration.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
QUICK_LOG="${PROJECT_DIR}/phase5_quick_validation.log"

PASS=0
FAIL=0

log()   { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$QUICK_LOG"; }
pass()  { log "PASS: $1"; ((PASS++)); }
fail()  { log "FAIL: $1"; ((FAIL++)); }

cd "$PROJECT_DIR"
mkdir -p "${PROJECT_DIR}/phase5_results"

# 1. Verify build
log "Checking build..."
if [ -f "${BUILD_DIR}/mysql_recovery_pro" ]; then
    pass "Build exists"
else
    fail "Build missing — run cmake --build build"
fi

# 2. Verify test executables
log "Checking test executables..."
for test_bin in test_p0_regression test_p1_salvage_fuzz \
                test_p1_layout_fuzz test_p1_type_classifier_fuzz; do
    if [ -f "${BUILD_DIR}/tests/${test_bin}" ]; then
        pass "Test binary: ${test_bin}"
    else
        fail "Test binary missing: ${test_bin}"
    fi
done

# 3. Run P0 regression tests
log "Running P0 regression tests..."
if [ -f "${BUILD_DIR}/tests/test_p0_regression" ]; then
    if "${BUILD_DIR}/tests/test_p0_regression" --gtest_filter="*" 2>&1 | tail -5 | grep -q "PASSED"; then
        pass "P0 regression tests"
    else
        fail "P0 regression tests"
    fi
fi

# 4. Run P1 fuzz tests
log "Running P1 salvage fuzz tests..."
if [ -f "${BUILD_DIR}/tests/test_p1_salvage_fuzz" ]; then
    if timeout 60 "${BUILD_DIR}/tests/test_p1_salvage_fuzz" 2>&1 | tail -5 | grep -q "PASSED"; then
        pass "P1 salvage fuzz tests"
    else
        fail "P1 salvage fuzz tests"
    fi
fi

log "Running P1 layout fuzz tests..."
if [ -f "${BUILD_DIR}/tests/test_p1_layout_fuzz" ]; then
    if timeout 30 "${BUILD_DIR}/tests/test_p1_layout_fuzz" 2>&1 | tail -5 | grep -q "PASSED"; then
        pass "P1 layout fuzz tests"
    else
        fail "P1 layout fuzz tests"
    fi
fi

log "Running P1 type classifier fuzz tests..."
if [ -f "${BUILD_DIR}/tests/test_p1_type_classifier_fuzz" ]; then
    if timeout 30 "${BUILD_DIR}/tests/test_p1_type_classifier_fuzz" 2>&1 | tail -5 | grep -q "PASSED"; then
        pass "P1 type classifier fuzz tests"
    else
        fail "P1 type classifier fuzz tests"
    fi
fi

# Summary
echo ""
echo "========================================="
echo "  Quick Validation Summary"
echo "  PASS: ${PASS}  FAIL: ${FAIL}"
echo "========================================="

exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)