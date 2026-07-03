#!/bin/bash
# XFS Schema-Aware Recovery Test
# Tests recovery with manual schema specification to improve data quality

set -e

TEST_DIR="/app/mysql_recovery_test/xfs_schema_test_$(date +%Y%m%d_%H%M%S)"
BLOCK_DEVICE="/dev/nvme0n2p1"
MOUNT_POINT="/mnt/xfs_schema_test"
CONTAINER_NAME="mysql_xfs_schema"
RECOVERY_TOOL="/app/mysql_recovery_pro/build/mysql_recovery_pro"
NUM_ROWS=5000

echo "=== XFS Schema-Aware Recovery Test ==="
echo "Date: $(date)"
echo "Rows: $NUM_ROWS"
echo "Test directory: $TEST_DIR"

mkdir -p "$TEST_DIR"

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    podman rm -f "$CONTAINER_NAME" 2>/dev/null || true
    if mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
        umount "$MOUNT_POINT" || true
    fi
    rm -rf "$MOUNT_POINT" 2>/dev/null || true
}

trap cleanup EXIT

# Setup XFS filesystem
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
echo "Waiting for MySQL..."
for i in {1..120}; do
    if podman exec "$CONTAINER_NAME" mysqladmin -uroot -proot123 ping 2>/dev/null | grep -q "alive"; then
        echo "MySQL ready!"
        break
    fi
    sleep 1
done

# Create table with known schema
echo "Creating test table..."
podman exec -i "$CONTAINER_NAME" mysql -uroot -proot123 -e "
DROP TABLE IF EXISTS testdb.test_schema_table;
CREATE TABLE testdb.test_schema_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100),
    email VARCHAR(100),
    age INT,
    salary DECIMAL(10,2),
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
" > /dev/null 2>&1

echo "Table created with schema:"
echo "  id INT PRIMARY KEY"
echo "  name VARCHAR(100)"
echo "  email VARCHAR(100)"
echo "  age INT"
echo "  salary DECIMAL(10,2)"
echo "  created_at DATETIME"

# Insert data with known patterns
echo "Inserting $NUM_ROWS rows..."
for i in {1..$NUM_ROWS}; do
    podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -e \
        "INSERT INTO testdb.test_schema_table (name, email, age, salary) VALUES ('User_$i', 'user$i@example.com', $((20 + i % 50)), $((3000 + i * 10)).00);" \
        2>/dev/null
    if [ $((i % 1000)) -eq 0 ]; then
        echo "Inserted $i rows..."
    fi
done
echo "All rows inserted"

# Flush to disk
echo "Flushing data..."
podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -e \
    "SET GLOBAL innodb_flush_log_at_trx_commit=1; SET GLOBAL innodb_max_dirty_pages_pct=0; FLUSH TABLES;" > /dev/null 2>&1
podman exec "$CONTAINER_NAME" sync > /dev/null 2>&1
sleep 5

# Get row count
actual_rows=$(podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -Nse "SELECT COUNT(*) FROM testdb.test_schema_table;" 2>/dev/null)
echo "Actual rows: $actual_rows"

# Execute DROP TABLE
echo "Executing DROP TABLE..."
podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -e "DROP TABLE testdb.test_schema_table;" > /dev/null 2>&1
podman exec "$CONTAINER_NAME" mysql -uroot -proot123 -e "FLUSH TABLES;" > /dev/null 2>&1
podman exec "$CONTAINER_NAME" sync > /dev/null 2>&1
sleep 3
echo "Table dropped"

# Stop container
podman stop "$CONTAINER_NAME" > /dev/null 2>&1
sleep 2

# Unmount filesystem
umount "$MOUNT_POINT" || true

echo ""
echo "=== Recovery Tests ==="

# Test 1: Schema-agnostic recovery (baseline)
OUTPUT1="$TEST_DIR/recovery_no_schema"
mkdir -p "$OUTPUT1"
echo ""
echo "Test 1: Schema-agnostic recovery (no schema specified)"
time "$RECOVERY_TOOL" \
    --disk-carving \
    --device-path "$BLOCK_DEVICE" \
    --filesystem xfs \
    --output "$OUTPUT1" \
    --num-workers 4 \
    --verbose 2>&1 | tee "$OUTPUT1/recovery.log" | tail -20

# Analyze result 1
csv1_count=$(find "$OUTPUT1" -name "*.csv" | wc -l)
rows1_total=0
for csv in "$OUTPUT1"/*.csv; do
    [ -f "$csv" ] && rows1_total=$((rows1_total + $(wc -l < "$csv") - 1))
done
echo "  CSV files: $csv1_count"
echo "  Total rows: $rows1_total"
echo "  Format: Schema-agnostic (hex fields)"

# Test 2: Schema-aware recovery with manual schema
OUTPUT2="$TEST_DIR/recovery_manual_schema"
mkdir -p "$OUTPUT2"
echo ""
echo "Test 2: Schema-aware recovery with manual schema"
SCHEMA="id INT PRIMARY KEY, name VARCHAR(100), email VARCHAR(100), age INT, salary DECIMAL(10,2), created_at DATETIME"

time "$RECOVERY_TOOL" \
    --disk-carving \
    --device-path "$BLOCK_DEVICE" \
    --filesystem xfs \
    --schema="$SCHEMA" \
    --output "$OUTPUT2" \
    --num-workers 4 \
    --verbose 2>&1 | tee "$OUTPUT2/recovery.log" | tail -20

# Analyze result 2
csv2_count=$(find "$OUTPUT2" -name "*.csv" | wc -l)
rows2_total=0
user_rows2=0
for csv in "$OUTPUT2"/*.csv; do
    if [ -f "$csv" ]; then
        rows_in_csv=$(wc -l < "$csv")
        rows2_total=$((rows2_total + rows_in_csv - 1))
        # Check if data contains User_ pattern
        user_count=$(grep -c "User_" "$csv" 2>/dev/null || echo 0)
        user_rows2=$((user_rows2 + user_count))
    fi
done
echo "  CSV files: $csv2_count"
echo "  Total rows: $rows2_total"
echo "  User pattern rows: $user_rows2"

# Check if schema fields appear in output
if [ -f "$OUTPUT2"/*.csv ]; then
    sample_csv=$(ls "$OUTPUT2"/*.csv | head -1)
    echo "  Sample header:"
    head -1 "$sample_csv"
    echo "  Sample data:"
    head -3 "$sample_csv" | tail -2
fi

# Test 3: Schema-aware recovery with JSON schema file
OUTPUT3="$TEST_DIR/recovery_json_schema"
mkdir -p "$OUTPUT3"
SCHEMA_FILE="$TEST_DIR/schema.json"

echo ""
echo "Creating JSON schema file..."
cat > "$SCHEMA_FILE" << 'EOF'
{
  "table_name": "test_schema_table",
  "columns": [
    {
      "name": "id",
      "type": "INT",
      "is_nullable": false,
      "column_key": "PRIMARY",
      "ordinal_position": 1
    },
    {
      "name": "name",
      "type": "VARCHAR",
      "length": 100,
      "is_nullable": true,
      "ordinal_position": 2
    },
    {
      "name": "email",
      "type": "VARCHAR",
      "length": 100,
      "is_nullable": true,
      "ordinal_position": 3
    },
    {
      "name": "age",
      "type": "INT",
      "is_nullable": true,
      "ordinal_position": 4
    },
    {
      "name": "salary",
      "type": "DECIMAL",
      "precision": 10,
      "scale": 2,
      "is_nullable": true,
      "ordinal_position": 5
    },
    {
      "name": "created_at",
      "type": "DATETIME",
      "is_nullable": true,
      "ordinal_position": 6
    }
  ]
}
EOF

echo "Test 3: Schema-aware recovery with JSON schema file"
time "$RECOVERY_TOOL" \
    --disk-carving \
    --device-path "$BLOCK_DEVICE" \
    --filesystem xfs \
    --schema-file="$SCHEMA_FILE" \
    --output "$OUTPUT3" \
    --num-workers 4 \
    --verbose 2>&1 | tee "$OUTPUT3/recovery.log" | tail -20

# Analyze result 3
csv3_count=$(find "$OUTPUT3" -name "*.csv" | wc -l)
rows3_total=0
user_rows3=0
for csv in "$OUTPUT3"/*.csv; do
    if [ -f "$csv" ]; then
        rows_in_csv=$(wc -l < "$csv")
        rows3_total=$((rows3_total + rows_in_csv - 1))
        user_count=$(grep -c "User_" "$csv" 2>/dev/null || echo 0)
        user_rows3=$((user_rows3 + user_count))
    fi
done
echo "  CSV files: $csv3_count"
echo "  Total rows: $rows3_total"
echo "  User pattern rows: $user_rows3"

# Check if schema fields appear
if [ -f "$OUTPUT3"/*.csv ]; then
    sample_csv=$(ls "$OUTPUT3"/*.csv | head -1)
    echo "  Sample header:"
    head -1 "$sample_csv"
    echo "  Sample data:"
    head -3 "$sample_csv" | tail -2
fi

# Generate comparison report
echo ""
echo "=== Comparison Report ==="
echo "Method               | CSV Files | Total Rows | User Data | Quality Score"
echo "---------------------|-----------|------------|-----------|--------------"
echo "No Schema            | $csv1_count         | $rows1_total      | N/A       | Schema-agnostic"
echo "Manual Schema        | $csv2_count         | $rows2_total      | $user_rows2     | $([ $user_rows2 -gt 0 ] && echo 'Schema-aware' || echo 'Unknown')"
echo "JSON Schema File     | $csv3_count         | $rows3_total      | $user_rows3     | $([ $user_rows3 -gt 0 ] && echo 'Schema-aware' || echo 'Unknown')"

# Recovery rate calculation
if [ $actual_rows -gt 0 ]; then
    rate1=$((rows1_total * 100 / actual_rows))
    rate2=$((rows2_total * 100 / actual_rows))
    rate3=$((rows3_total * 100 / actual_rows))
    echo ""
    echo "Recovery Rates (relative to actual rows):"
    echo "  No Schema: ${rate1}%"
    echo "  Manual Schema: ${rate2}%"
    echo "  JSON Schema: ${rate3}%"
fi

echo ""
echo "=== Test Complete ==="
echo "Test directory: $TEST_DIR"
echo "Results saved to respective subdirectories"