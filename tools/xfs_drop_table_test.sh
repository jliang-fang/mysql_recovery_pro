#!/bin/bash
# XFS DROP TABLE Recovery Test
# Tests XFS disk carving after DROP TABLE disaster

set -e

echo "=== XFS DROP TABLE Recovery Test ==="
echo ""

# Configuration
TEST_DIR="/app/mysql_recovery_test/xfs_drop_test"
DISK_IMG="$TEST_DIR/xfs_drop.img"
MOUNT_POINT="/mnt/xfs_drop_test"
OUTPUT_DIR="/tmp/xfs_drop_recovery"
MYSQL_CONTAINER="mysql_xfs_drop_test"
DB_NAME="testdb"
TABLE_NAME="users"
ROW_COUNT=5

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
docker logs $MYSQL_CONTAINER 2>&1 | tail -5

# Create test table and insert data
echo "[6] Creating test table '$TABLE_NAME' with $ROW_COUNT rows..."
docker exec $MYSQL_CONTAINER mysql -uroot -ptestpass -e "
CREATE TABLE $DB_NAME.$TABLE_NAME (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100),
    email VARCHAR(100),
    age INT,
    salary DECIMAL(10,2),
    created_at DATETIME,
    status ENUM('active','inactive','pending'),
    data JSON
);

INSERT INTO $DB_NAME.$TABLE_NAME VALUES
(1, 'Alice', 'alice@test.com', 30, 50000.00, NOW(), 'active', '{\"role\": \"admin\"}'),
(2, 'Bob', 'bob@test.com', 25, 45000.00, NOW(), 'inactive', '{\"role\": \"user\"}'),
(3, 'Charlie', 'charlie@test.com', 35, 60000.00, NOW(), 'pending', '{\"role\": \"manager\"}'),
(4, 'Diana', 'diana@test.com', 28, 52000.00, NOW(), 'active', '{\"role\": \"user\"}'),
(5, 'Eve', 'eve@test.com', 32, 55000.00, NOW(), 'active', '{\"role\": \"admin\"}');

SELECT COUNT(*) as row_count FROM $DB_NAME.$TABLE_NAME;
SELECT * FROM $DB_NAME.$TABLE_NAME;
" 2>&1

# Simulate disaster: DROP TABLE
echo ""
echo "[7] Simulating disaster: DROP TABLE $TABLE_NAME..."
docker exec $MYSQL_CONTAINER mysql -uroot -ptestpass -e "
SHOW TABLES FROM $DB_NAME;
DROP TABLE $DB_NAME.$TABLE_NAME;
SHOW TABLES FROM $DB_NAME;
" 2>&1

# Stop container and unmount
echo ""
echo "[8] Stopping MySQL and unmounting XFS..."
docker stop $MYSQL_CONTAINER 2>&1
docker rm $MYSQL_CONTAINER 2>&1
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

# Find largest file (likely user table data)
LARGEST_FILE=$(ls -S $OUTPUT_DIR/*.csv 2>/dev/null | head -1)
if [ -n "$LARGEST_FILE" ]; then
    echo ""
    echo "Largest recovered file: $(basename $LARGEST_FILE)"
    echo "Size: $(ls -lh $LARGEST_FILE | awk '{print $5}')"
    echo "Preview (first 10 lines):"
    head -10 "$LARGEST_FILE"
fi

echo ""
echo "=== XFS DROP TABLE Recovery Test Completed ==="
