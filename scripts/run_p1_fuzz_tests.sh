#!/bin/bash
set -euo pipefail

echo "=== P1 Format Correctness Fuzz Tests ==="
echo ""

BUILD_DIR="/app/mysql_recovery_pro/build"
REPORT_DIR="/app/mysql_recovery_test/reports"

mkdir -p "$REPORT_DIR"

cd "$BUILD_DIR"

echo "Building P1 fuzz test targets..."
cmake --build . --target test_p1_salvage_fuzz -j$(nproc) 2>&1 | tail -5
cmake --build . --target test_p1_layout_fuzz -j$(nproc) 2>&1 | tail -5
cmake --build . --target test_p1_type_classifier_fuzz -j$(nproc) 2>&1 | tail -5

echo ""
echo "Running P1 fuzz tests via ctest..."
ctest --test-dir . -R p1_fuzz -V 2>&1 | tee "$REPORT_DIR/p1_fuzz_results.log"

EXIT_CODE=$?
echo ""
echo "Exit code: $EXIT_CODE"
exit $EXIT_CODE
