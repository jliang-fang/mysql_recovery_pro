#!/bin/bash
# Quick XFS Recovery Test - MySQL 8.0 with 10K rows
# Test DROP TABLE recovery

set -e

TEST_DIR="/app/mysql_recovery_test/xfs_quick_test_$(date +%Y%m%d_%H%M%S)"
BLOCK_DEVICE="/dev/nvme0n2p1"
MOUNT_POINT="/mnt/xfs_quick_test"
CONTAINER_NAME="mysql_xfs_quick"
RECOVERY_TOOL="/app/mysql_recovery_pro/build/mysql_recovery_pro"
NUM_ROWS=10000

echo "=== XFS Quick Recovery Test ==="
echo "Date: $(date)"
echo "Rows: $NUM_ROWS"
echo "Block device: $BLOCK_DEVICE"
echo "Test directory: $TEST_DIR"

mkdir -p "$TEST_DIR"

# Cleanup
cleanup() {
    echo "Cleaning up..."
    podman rm -f "$CONTAINER_NAME" 2>/dev/null || true
    if mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
        umount "$MOUNT_POINT" || true
    fi
    rm -rf "$MOUNT_POINT" 2>/dev/null || true
}

trap cleanup EXIT

# Setup XFS
echo "Setting up XFS filesystem..."
if mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
    umount "$MOUNT_POINT"
fi
mkdir -p "$MOUNT_POINT"
wipefs -a "$BLOCK_DEVICE" 2>/dev/null || true
mkfs.xfs -f "$BLOCK_DEVICE" > /dev/null 2>&1
mount -t xfs "$BLOCK_DEVICE" "$MOUNT_POINT"
echo "XFS filesystem ready"

# Start MySQL 8.0
echo "Starting MySQL 8.0..."
podman rm -f "$CONTAINER_NAME" > /dev/null 2>&1 || true
mkdir -p "$MOUNT_POINT/mysql_data"
podman run -d \
    --name "$CONTAINER_NAME" \
    -e MYSQL_ROOT_PASSWORD=root123 \
    -e MYSQL_DATABASE=testdb \
    -v "$MOUNT_POINT/mysql_data:/var/lib/mysql" \
    mysql:8.0 > /dev/null

# Wait for MySQL
echo "Waiting for MySQL to be ready..."
for i in {1..180}; do
    if podman exec "$CONTAINER_NAME" mysqladmin -uroot -proot123 ping 2>/dev/null | grep -q "alive"; then
        echo "MySQL is ready!"
        break
    fi
    sleep 1
done

# Create table
echo "Creating test table..."
podman exec -i "$CONTAINER_NAME" mysql -uroot -proot123 -e "
DROP TABLE IF EXISTS testdb.test_table;
CREATE TABLE testdb.test_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100),
    email VARCHAR(100),
    age INT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
" > /dev/null 2>&1

# Insert data
echo "Inserting $NUM_ROWS rows..."
for ((i=1; i<=NUM_ROWS; i++)); do
    podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -e \
        "INSERT INTO testdb.test_table (name, email, age) VALUES ('User_$i', 'user$i@test.com', $((i % 100)));"
    if [ $((i % 1000)) -eq 0 ]; then
        echo "Inserted $i rows..."
    fi
done
echo "All rows inserted"

# Flush to disk
echo "Flushing to disk..."
podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -e \
    "SET GLOBAL innodb_flush_log_at_trx_commit=1; SET GLOBAL innodb_max_dirty_pages_pct=0; FLUSH TABLES;" > /dev/null 2>&1
podman exec "$CONTAINER_NAME" sync > /dev/null 2>&1
sleep 5

# Get row count
actual_rows=$(podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -Nse "SELECT COUNT(*) FROM testdb.test_table;" 2>/dev/null)
echo "Actual rows in table: $actual_rows"

# Execute disaster: DROP TABLE
echo "Executing DROP TABLE..."
podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -e "DROP TABLE testdb.test_table;" > /dev/null 2>&1
podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -e "FLUSH TABLES;" > /dev/null 2>&1
podman exec "$CONTAINER_NAME" sync > /dev/null 2>&1
sleep 3
echo "Table dropped"

# Stop container to ensure all writes are flushed
podman stop "$CONTAINER_NAME" > /dev/null 2>&1
sleep 2

# Run recovery
OUTPUT_DIR="$TEST_DIR/recovery_output"
mkdir -p "$OUTPUT_DIR"

echo "Running XFS disk carving recovery..."
echo "Command: $RECOVERY_TOOL --disk-carving --device-path $BLOCK_DEVICE --filesystem xfs --output $OUTPUT_DIR --verbose"

time "$RECOVERY_TOOL" \
    --disk-carving \
    --device-path "$BLOCK_DEVICE" \
    --filesystem xfs \
    --output "$OUTPUT_DIR" \
    --num-workers 4 \
    --verbose 2>&1 | tee "$OUTPUT_DIR/recovery.log"

# Analyze results
echo ""
echo "=== Recovery Results ==="
csv_files=$(find "$OUTPUT_DIR" -name "*.csv" 2>/dev/null | wc -l)
echo "CSV files found: $csv_files"

if [ $csv_files -gt 0 ]; then
    for csv in "$OUTPUT_DIR"/*.csv; do
        [ -f "$csv" ] && rows=$(wc -l < "$csv" | awk '{print $1}')
        echo "  $csv: $((rows - 1)) rows (excluding header)"
    done

    total_recovered=0
    for csv in "$OUTPUT_DIR"/*.csv; do
        [ -f "$csv" ] && total_recovered=$((total_recovered + $(wc -l < "$csv") - 1))
    done

    recovery_rate=$(awk "BEGIN {printf \"%.2f\", ($total_recovered / $NUM_ROWS) * 100}")
    echo ""
    echo "Total rows recovered: $total_recovered"
    echo "Recovery rate: $recovery_rate%"

    if [ $total_recovered -ge $((NUM_ROWS * 90 / 100)) ]; then
        echo "✅ Recovery EXCELLENT (>= 90%)"
        exit 0
    elif [ $total_recovered -ge $((NUM_ROWS * 70 / 100)) ]; then
        echo "✅ Recovery GOOD (>= 70%)"
        exit 0
    else
        echo "❌ Recovery POOR (< 70%)"
        exit 1
    fi
else
    echo "❌ No CSV files recovered"
    exit 1
fi