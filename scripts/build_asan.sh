#!/bin/bash
set -euo pipefail

# Build script for P0 regression tests with ASan and UBSan enabled
BUILD_DIR="/app/mysql_recovery_pro/build_asan"

echo "=== Building MySQL Recovery Pro with ASan+UBSan ==="
echo "Build directory: ${BUILD_DIR}"

# Clean build directory
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with sanitizers enabled
cmake .. \
    -DENABLE_ASAN=ON \
    -DENABLE_UBSAN=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_STANDARD=20

echo "=== Building ==="
cmake --build . -j$(nproc) 2>&1

echo "=== Build complete ==="
echo "Build directory: ${BUILD_DIR}"