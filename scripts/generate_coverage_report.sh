#!/usr/bin/env bash
# Script to generate code coverage report
# Usage: ./generate_coverage_report.sh

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
COVERAGE_DIR="${BUILD_DIR}/coverage"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== MySQL 8.0 Recovery Engine - Code Coverage Report ===${NC}"

# Clean previous coverage data
echo -e "${YELLOW}Cleaning previous coverage data...${NC}"
rm -rf "${COVERAGE_DIR}"
mkdir -p "${COVERAGE_DIR}"

# Build with coverage flags
echo -e "${YELLOW}Building with coverage flags...${NC}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_COVERAGE=ON \
    -DCMAKE_CXX_FLAGS="--coverage -g -O0" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage"

make -j$(nproc)

# Run tests
echo -e "${YELLOW}Running tests...${NC}"
ctest --verbose

# Generate coverage info
echo -e "${YELLOW}Generating coverage info...${NC}"
lcov --capture \
    --directory "${BUILD_DIR}" \
    --output-file "${COVERAGE_DIR}/coverage.info" \
    --base-directory "${PROJECT_ROOT}"

# Remove system headers
lcov --remove "${COVERAGE_DIR}/coverage.info" \
    '/usr/*' \
    'nlohmann/*' \
    'build/*' \
    --output-file "${COVERAGE_DIR}/coverage_filtered.info"

# Generate HTML report
echo -e "${YELLOW}Generating HTML report...${NC}"
genhtml "${COVERAGE_DIR}/coverage_filtered.info" \
    --output-directory "${COVERAGE_DIR}/html" \
    --title "MySQL 8.0 Recovery Engine Coverage Report" \
    --legend \
    --show-details

# Print summary
echo -e "${GREEN}=== Coverage Summary ===${NC}"
lcov --list "${COVERAGE_DIR}/coverage_filtered.info"

# Show report location
echo -e "${GREEN}=== Coverage Report Generated ===${NC}"
echo -e "${GREEN}HTML Report: ${COVERAGE_DIR}/html/index.html${NC}"
echo -e "${GREEN}Coverage Data: ${COVERAGE_DIR}/coverage_filtered.info${NC}"

# Open report in browser (if running locally)
if [ -n "$DISPLAY" ] && command -v xdg-open &> /dev/null; then
    echo -e "${YELLOW}Opening coverage report in browser...${NC}"
    xdg-open "${COVERAGE_DIR}/html/index.html" 2>/dev/null || true
fi

echo -e "${GREEN}Done!${NC}"
