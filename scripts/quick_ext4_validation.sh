#!/bin/bash
# Quick ext4 recovery validation - verify schemaless output produces decoded records
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(dirname "$SCRIPT_DIR")"
RECOVERY_BIN="${PROJ_DIR}/build/mysql_recovery_pro"

IMG="${SCRIPT_DIR}/ext4_valid_test.img"
MNT="${SCRIPT_DIR}/ext4_valid_mnt"
DATA_DIR="${MNT}/data"
SOCKET="${MNT}/mysql.sock"
PID_FILE="${MNT}/mysql.pid"
OUTPUT_DIR="/tmp/ext4_valid_output"

echo "=== Step 1: Create 256MB ext4 image ==="
rm -f "$IMG"
truncate -s 256M "$IMG"
mkfs.ext4 -F "$IMG" 2>&1 | tail -3
mkdir -p "$MNT"
mount -o loop "$IMG" "$MNT"
mkdir -p "$DATA_DIR"
chown -R mysql:mysql "$MNT" 2>/dev/null || true

echo "=== Step 2: Initialize MySQL ==="
mysqld --initialize-insecure --user=root --datadir="$DATA_DIR" 2>&1 | tail -3

echo "=== Step 3: Start MySQL ==="
mysqld --user=root --datadir="$DATA_DIR" --socket="$SOCKET" \
    --pid-file="$PID_FILE" --port=33307 --skip-networking=0 \
    --port=33307 --bind-address=127.0.0.1 &
MYSQLD_PID=$!
sleep 3

# Wait for MySQL to be ready
for i in $(seq 1 30); do
    if mysql -u root --socket="$SOCKET" -e "SELECT 1" &>/dev/null; then
        echo "MySQL ready after ${i}s"
        break
    fi
    sleep 1
done

echo "=== Step 4: Create test database and table ==="
mysql -u root --socket="$SOCKET" <<'EOSQL'
CREATE DATABASE test_valid;
USE test_valid;
CREATE TABLE t1 (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100),
    age INT,
    salary DECIMAL(10,2),
    created_at DATETIME
) ENGINE=InnoDB;

INSERT INTO t1 (name, age, salary, created_at) VALUES
('Alice Johnson', 30, 75000.50, '2024-01-15 10:30:00'),
('Bob Smith', 45, 92000.00, '2024-02-20 14:00:00'),
('Charlie Brown', 28, 65000.75, '2024-03-10 09:15:00'),
('Diana Prince', 35, 110000.00, '2024-04-05 16:45:00'),
('Eve Davis', 52, 88000.25, '2024-05-12 11:30:00');

SELECT * FROM t1;
EOSQL

echo "=== Step 5: Find the .ibd file and record golden data ==="
IBD_FILE=$(find "$DATA_DIR/test_valid" -name "t1.ibd" 2>/dev/null | head -1)
if [ -z "$IBD_FILE" ]; then
    echo "ERROR: Could not find t1.ibd"
    mysqladmin -u root --socket="$SOCKET" shutdown 2>/dev/null || true
    exit 1
fi
echo "IBD file: $IBD_FILE"
ls -la "$IBD_FILE"

# Get golden checksum
md5sum "$IBD_FILE" > /tmp/ext4_golden_md5.txt

echo "=== Step 6: Stop MySQL and rm the .ibd file ==="
mysqladmin -u root --socket="$SOCKET" shutdown 2>/dev/null || true
sleep 2
kill "$MYSQLD_PID" 2>/dev/null || true
sleep 1

rm -f "$IBD_FILE"
echo "Removed: $IBD_FILE"
sync

echo "=== Step 7: Unmount ==="
umount "$MNT" 2>/dev/null || true

echo "=== Step 8: Run ext4 recovery ==="
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

"$RECOVERY_BIN" \
    --disk-carving \
    --device-path="$IMG" \
    --filesystem=ext4 \
    --output="$OUTPUT_DIR" \
    --page-size=16384 \
    --num-workers=4 \
    --verbose 2>&1 | tail -30

echo "=== Step 9: Check recovery output ==="
echo "Output files:"
find "$OUTPUT_DIR" -type f -ls 2>/dev/null | head -20

echo ""
echo "CSV files found:"
find "$OUTPUT_DIR" -name "*.csv" -type f 2>/dev/null | while read csv; do
    lines=$(wc -l < "$csv")
    echo "  $csv: $lines lines"
    echo "  First 3 lines:"
    head -3 "$csv"
    echo "  ---"
done

echo "=== Step 10: Cleanup ==="
rm -f "$IMG"
rm -rf "$MNT"

echo "=== Done ==="