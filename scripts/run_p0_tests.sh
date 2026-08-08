#!/bin/bash
set -euo pipefail

echo "=== P0 Regression Tests with ASan/UBSan ==="
echo ""

BUILD_DIR="${BUILD_DIR:-/app/mysql_recovery_test/build/asan}"
REPORT_DIR="/app/mysql_recovery_test/reports"
mkdir -p "${REPORT_DIR}"

# Check if build directory exists
if [ ! -d "${BUILD_DIR}" ]; then
    echo "ERROR: Build directory not found at ${BUILD_DIR}"
    echo "Please run build_asan.sh first"
    exit 1
fi

echo "Running P0 regression tests..."
echo "==========================================="

# Run the P0 regression binary directly. The gtest_discover_tests() macro
# registers tests under their gtest names (FILHeaderEndiannessTest.*,
# RollPtrDecoderTest.*, etc.), so `ctest -R test_p0_regression` matches
# nothing. Running the binary directly is the reliable path.
"${BUILD_DIR}/tests/test_p0_regression" 2>&1 | tee "${REPORT_DIR}/p0_regression_results.log"

EXIT_CODE=${PIPESTATUS[0]}

echo ""
echo "==========================================="
echo "Exit code: ${EXIT_CODE}"

if [ ${EXIT_CODE} -eq 0 ]; then
    echo "=== All P0 regression tests PASSED ==="
else
    echo "=== Some P0 regression tests FAILED ==="
fi

exit ${EXIT_CODE}