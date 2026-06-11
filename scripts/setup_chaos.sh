#!/bin/bash
# =============================================================================
# MySQL Recovery Pro - Initial Chaos Setup
# Author: jliang.fang (Global MySQL Kernel QA & Chaos Engineering)
# Purpose: Quickstart script for chaos testing infrastructure
# =============================================================================

set -e

echo "========================================="
echo "  MySQL Recovery Pro - Chaos Setup"
echo "========================================="

cd "$(dirname "$0")"

# ==================== Check Prerequisites ====================

echo "[1/6] Checking prerequisites..."

command -v docker > /dev/null 2>&1 || { echo "ERROR: docker required"; exit 1; }
command -v python3 > /dev/null 2>&1 || { echo "ERROR: python3 required"; exit 1; }
command -v cmake > /dev/null 2>&1 || { echo "ERROR: cmake required"; exit 1; }

echo "✓ Docker: $(docker --version)"
echo "✓ Python3: $(python3 --version)"
echo "✓ CMake: $(cmake --version | head -1)"

# ==================== Build Engine ====================

echo ""
echo "[2/6] Building mysql_recovery_pro engine..."

if [ ! -d "build" ]; then
    mkdir -p build
fi

if [ ! -f "build/mysql_recovery_pro" ]; then
    cmake -B build -S .
    make -C build -j$(nproc)
    echo "✓ Engine built successfully"
else
    echo "✓ Engine already exists"
fi

# ==================== Generate Test Data ====================

echo ""
echo "[3/6] Generating test data..."

mkdir -p test_data

if [ -z "$(ls test_data/*.ibd 2>/dev/null)" ]; then
    python3 scripts/generate_test_data/generate_test_data.py \
        --output test_data/test.ibd \
        --type "all" \
        --page-size 16384
    echo "✓ Test data generated"
else
    echo "✓ Test data already exists"
fi

# ==================== Setup Chaos Volumes ====================

echo ""
echo "[4/6] Setting up chaos test volumes..."

mkdir -p chaos-test-volumes
mkdir -p chaos-test-results
mkdir -p auto_heal_patches

echo "✓ Chaos volumes initialized"

# ==================== Check Docker Compose ====================

echo ""
echo "[5/6] Checking Docker Compose infrastructure..."

if [ -f "docker/docker-compose.chaos.yml" ]; then
    echo "✓ Docker Compose file exists"
    echo ""
    echo "To start all MySQL containers run:"
    echo "  docker-compose -f docker/docker-compose.chaos.yml up -d"
else
    echo "⚠ Docker Compose file not found, skipping"
fi

# ==================== Run Quick Test ====================

echo ""
echo "[6/6] Running quick chaos injection test..."

# Create a simple test IBD file
python3 -c "
import struct
import os

# Create minimal IBD file (16KB page, 5 pages)
page_size = 16384
ibd_data = bytearray(page_size * 5)

# Page 0: FSP Header
struct.pack_into('<H', ibd_data, 12, 15)  # FSP_HDR page type

# Page 1: Index Page
struct.pack_into('<H', ibd_data, page_size + 12, 8)  # INDEX page type

# Write file
with open('test_data/quick_test.ibd', 'wb') as f:
    f.write(ibd_data)

print('✓ Quick test IBD created')
"

# Test chaos injection
python3 scripts/chaos_injection/chaos_injector.py \
    --ibd test_data/quick_test.ibd \
    --page-size 16384 \
    --chaos tear-page \
    --page-num 0

if [ $? -eq 0 ]; then
    echo "✓ Chaos injection test passed"
else
    echo "⚠ Chaos injection test failed (expected if binary not built)"
fi

# ==================== Summary ====================

echo ""
echo "========================================="
echo "  Setup Complete!"
echo "========================================="
echo ""
echo "Next steps:"
echo ""
echo "1. Run full chaos test suite:"
echo "   python3 scripts/chaos_orchestrator.py \\"
echo "       --engine build/mysql_recovery_pro \\"
echo "       --test-data test_data \\"
echo "       --results chaos_test_results \\"
echo "       --asan"
echo ""
echo "2. Start Docker containers:"
echo "   docker-compose -f docker/docker-compose.chaos.yml up -d"
echo ""
echo "3. Run chaos injection:"
echo "   python3 scripts/chaos_injection/chaos_injector.py \\"
echo "       --ibd test_data/test.ibd \\"
echo "       --chaos all"
echo ""
echo "4. Analyze core dumps (if any):"
echo "   python3 scripts/self_healing/bug_to_patch.py \\"
echo "       --gdb chaos_test_results/core_dumps/*.dump \\"
echo "       --output patches/fix.diff"
echo ""
echo "Docs: CHAOS_TESTING_ANGEL_GUIDE.md"
echo ""
