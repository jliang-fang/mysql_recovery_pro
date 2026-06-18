#!/bin/bash
# =============================================================================
# Ultimate Testing Framework Quick Start Script
# Author: jliang.fang (Database Kernel Forensics Expert)
# Purpose: 快速启动终极测试框架
# =============================================================================

set -e

PROJECT_ROOT="/app/mysql_recovery_pro"
SCRIPTS_DIR="${PROJECT_ROOT}/scripts"
BUILD_DIR="${PROJECT_ROOT}/build"

echo "=============================================================="
echo "MySQL Recovery Pro - Ultimate Testing Framework v2.0"
echo "=============================================================="
echo ""

# 1. 检查恢复工具是否编译
echo "[Step 1] 检查恢复工具..."
if [ ! -f "${BUILD_DIR}/mysql_recovery_pro" ]; then
    echo "  恢复工具未编译，开始编译..."
    cd "${BUILD_DIR}"
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    echo "  ✓ 编译完成"
else
    echo "  ✓ 恢复工具已存在: ${BUILD_DIR}/mysql_recovery_pro"
fi

# 2. 检查MySQL容器状态
echo ""
echo "[Step 2] 检查MySQL容器..."
CONTAINERS_RUNNING=0

for VER in "5.6" "5.7" "8.0" "8.4" "9.0"; do
    CONTAINER_NAME="mysql${VER}_test"
    if podman ps --filter "name=${CONTAINER_NAME}" --format "{{.Status}}" 2>/dev/null | grep -q "Up"; then
        echo "  ✓ MySQL ${VER}: 运行中"
        CONTAINERS_RUNNING=$((CONTAINERS_RUNNING + 1))
    elif docker ps --filter "name=${CONTAINER_NAME}" --format "{{.Status}}" 2>/dev/null | grep -q "Up"; then
        echo "  ✓ MySQL ${VER}: 运行中"
        CONTAINERS_RUNNING=$((CONTAINERS_RUNNING + 1))
    else
        echo "  ✗ MySQL ${VER}: 未运行"
    fi
done

if [ $CONTAINERS_RUNNING -eq 0 ]; then
    echo ""
    echo "  警告: 没有MySQL容器运行，测试将受限"
    echo "  请启动容器后再运行测试"
fi

# 3. 运行快速测试
echo ""
echo "[Step 3] 运行快速测试..."
echo "  测试配置: MySQL 8.0, 10000行数据"
echo "  破坏类型: rm -rf, header_erasure, random_page"
echo ""

cd "${PROJECT_ROOT}"

python3 "${SCRIPTS_DIR}/ultimate_test_framework.py" \
    --project-root "${PROJECT_ROOT}" \
    --versions 8.0 \
    --row-counts 10000 \
    --corruptions rm_rf_single header_erasure random_page

echo ""
echo "=============================================================="
echo "测试完成"
echo "=============================================================="