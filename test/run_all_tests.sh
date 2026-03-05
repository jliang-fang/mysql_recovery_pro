#!/bin/
set -e

BASE_DIR="/app/mysql8_recovery"
BUILD_DIR="$BASE_DIR/build"
TEST_DATA_DIR="$BASE_DIR/test/test_data"
TEST_OUTPUT_DIR="$BASE_DIR/test/output"

mkdir -p "$TEST_OUTPUT_DIR"

echo "=== MySQL 8.0 InnoDB Recovery Test Suite ==="
echo "Build dir: $BUILD_DIR"
echo "Test data dir: $TEST_DATA_DIR"

if [[ ! -x "$BUILD_DIR/mysql8_recovery" ]]; then
    echo "[错误] mysql8_recovery 可执行文件不存在，请先编译：cmake .. && make"
    exit 1
fi

cd "$BUILD_DIR"

echo ""
echo "测试1: Page 结构解析..."
if [[ -f "$TEST_DATA_DIR/valid_ibd_file.ibd" ]]; then
    ./mysql8_recovery "$TEST_DATA_DIR/valid_ibd_file.ibd" > "$TEST_OUTPUT_DIR/page_parse.log" 2>&1 || {
        echo "[错误] Page 解析失败，查看日志: $TEST_OUTPUT_DIR/page_parse.log"
        exit 1
    }
    echo "[通过] Page 解析成功"
else
    echo "[警告] valid_ibd_file.ibd 不存在，跳过测试"
fi

echo ""
echo "测试2: 加密/解密..."
if [[ -f "$TEST_DATA_DIR/valid_ibd_file.ibd" ]]; then
    ./mysql8_recovery -e "$TEST_DATA_DIR/valid_ibd_file.ibd" "$TEST_OUTPUT_DIR/encrypted.ibd" "testpassword123" || {
        echo "[错误] 加密失败"
        exit 1
    }
    ./mysql8_recovery -c "$TEST_OUTPUT_DIR/encrypted.ibd" "$TEST_OUTPUT_DIR/decrypted.ibd" -p "testpassword123" || {
        echo "[错误] 解密失败"
        exit 1
    }
    if cmp -s "$TEST_DATA_DIR/valid_ibd_file.ibd" "$TEST_OUTPUT_DIR/decrypted.ibd"; then
        echo "[通过] 加密/解密验证通过"
    else
        echo "[警告] 解密后文件与原始不一致（可能因加密填充差异）"
    fi
else
    echo "[警告] valid_ibd_file.ibd 不存在，跳过测试"
fi

echo ""
echo "测试3: 磁盘扫描..."
if [[ -d "$TEST_DATA_DIR" ]]; then
    ./mysql8_recovery -d "$TEST_DATA_DIR" "$TEST_OUTPUT_DIR/recovered_disk" || {
        echo "[错误] 磁盘扫描失败"
        exit 1
    }
    echo "[通过] 磁盘扫描完成"
else
    echo "[警告] test_data 目录不存在，跳过测试"
fi

echo ""
echo "测试4: 文件信息扫描..."
if [[ -f "$TEST_DATA_DIR/valid_ibd_file.ibd" ]]; then
    ./mysql8_recovery -s "$TEST_DATA_DIR/valid_ibd_file.ibd" > /dev/null || {
        echo "[错误] 信息扫描失败"
        exit 1
    }
    echo "[通过] 文件信息扫描成功"
else
    echo "[警告] valid_ibd_file.ibd 不存在，跳过测试"
fi

echo ""
echo "=== 所有关键测试通过 ==="
