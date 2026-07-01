#!/bin/bash
# XFS rm -rf Recovery Test
# Tests XFS disk carving after rm -rf /var/lib/mysql disaster

set -e

echo "=== XFS rm -rf Recovery Test ==="
echo ""

# Configuration
TEST_DIR="/app/mysql_recovery_test/xfs_rmrf_test"
DISK_IMG="$TEST_DIR/xfs_rmrf.img"
MOUNT_POINT="/mnt/xfs_rmrf_test"
OUTPUT_DIR="/tmp/xfs_rmrf_recovery"
MYSQL_CONTAINER="mysql_xfs_rmrf_test"
DB_NAME="testdb"
TABLE_NAME="products"
ROW_COUNT=10

# Cleanup previous test
echo "[1] Cleaning up previous test..."
docker rm -f $MYSQL_CONTAINER 2>/dev/null || true
umount $MOUNT_POINT 2>/dev/null || true
rm -rf $TEST_DIR $OUTPUT_DIR
mkdir -p $TEST_DIR

# Create XFS disk image (500MB)
echo "[2] Creating XFS disk image (500MB)..."
dd if=/dev/zero of=$DISK_IMG bs=1M count=500 2>&1 | tail -1
mkfs.xfs -f $DISK_IMG 2>&1 | grep -E "(meta-data|data|log)" | head -3

# Mount XFS
echo "[3] Mounting XFS filesystem..."
mkdir -p $MOUNT_POINT
mount -o loop $DISK_IMG $MOUNT_POINT
df -h $MOUNT_POINT

# Start MySQL container
echo "[4] Starting MySQL 8.0 container..."
docker run -d --name $MYSQL_CONTAINER \
    -v $MOUNT_POINT:/var/lib/mysql \
    -e MYSQL_ROOT_PASSWORD=testpass \
    -e MYSQL_DATABASE=$DB_NAME \
    mysql:8.0 2>&1

# Wait for MySQL to initialize
echo "[5] Waiting for MySQL to initialize..."
sleep 45
docker logs $MYSQL_CONTAINER 2>&1 | tail -3

# Create test table with more rows
echo "[6] Creating test table '$TABLE_NAME' with $ROW_COUNT rows..."
docker exec $MYSQL_CONTAINER mysql -uroot -ptestpass -e "
CREATE TABLE $DB_NAME.$TABLE_NAME (
    id INT PRIMARY KEY AUTO_INCREMENT,
    product_name VARCHAR(200),
    price DECIMAL(10,2),
    quantity INT,
    category VARCHAR(50),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    metadata JSON
);

INSERT INTO $DB_NAME.$TABLE_NAME (product_name, price, quantity, category, metadata) VALUES
('Laptop Pro', 1299.99, 50, 'Electronics', '{\"brand\": \"TechCorp\", \"warranty\": \"2 years\"}'),
('Wireless Mouse', 29.99, 200, 'Electronics', '{\"brand\": \"InputTech\", \"wireless\": true}'),
('USB-C Hub', 49.99, 150, 'Electronics', '{\"ports\": 7, \"color\": \"silver\"}'),
('Desk Chair', 299.00, 30, 'Furniture', '{\"material\": \"mesh\", \"adjustable\": true}'),
('Standing Desk', 599.00, 20, 'Furniture', '{\"height_range\": [28, 48], \"width\": 60}'),
('Monitor 27\"', 399.99, 75, 'Electronics', '{\"resolution\": \"4K\", \"refresh_rate\": 144}'),
('Mechanical Keyboard', 149.99, 100, 'Electronics', '{\"switches\": \"Cherry MX\", \"rgb\": true}'),
('Webcam HD', 79.99, 120, 'Electronics', '{\"resolution\": \"1080p\", \"mic\": true}'),
('Desk Lamp', 39.99, 80, 'Office', '{\"color_temp\": [2700, 6500], \"dimmer\": true}'),
('Cable Organizer', 14.99, 300, 'Office', '{\"material\": \"plastic\", \"color\": \"black\"}');

SELECT COUNT(*) as row_count FROM $DB_NAME.$TABLE_NAME;
SELECT id, product_name, price, category FROM $DB_NAME.$TABLE_NAME;
" 2>&1

# Stop MySQL cleanly first
echo ""
echo "[7] Stopping MySQL..."
docker stop $MYSQL_CONTAINER 2>&1
docker rm $MYSQL_CONTAINER 2>&1

# Simulate disaster: rm -rf
echo "[8] Simulating disaster: rm -rf /var/lib/mysql/*"
rm -rf $MOUNT_POINT/*
ls -la $MOUNT_POINT

# Unmount XFS
umount $MOUNT_POINT

# Run XFS disk carving recovery
echo ""
echo "[9] Running XFS disk carving recovery..."
mkdir -p $OUTPUT_DIR
/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --disk-carving \
    --filesystem=xfs \
    --device-path=$DISK_IMG \
    --output=$OUTPUT_DIR \
    --verbose 2>&1 | tee $TEST_DIR/recovery.log | grep -E "(INFO|ERROR|Pages|records|index_id)" | head -30

# Check recovery results
echo ""
echo "[10] Recovery results:"
echo "Output files:"
ls -la $OUTPUT_DIR/*.csv 2>/dev/null | wc -l
echo ""
echo "Total rows recovered:"
cat $OUTPUT_DIR/*.csv 2>/dev/null | wc -l

# Look for product data
echo ""
echo "Looking for product-related data..."
for f in $OUTPUT_DIR/*.csv; do
    if grep -qi "laptop\|mouse\|keyboard\|monitor\|desk" "$f" 2>/dev/null; then
        echo "=== $(basename $f) ==="
        head -20 "$f"
        echo ""
        break
    fi
done

echo "=== XFS rm -rf Recovery Test Completed ==="
