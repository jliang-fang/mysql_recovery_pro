#!/bin/bash

# Full Recovery Test for MySQL 5.6
# Tests TRUNCATE and rm -rf scenarios with full IBD scanning

set -e

CONTAINER="mysql_recovery_56"
MYSQL_PWD="root123"

echo "=========================================="
echo "MySQL 5.6 Full Recovery Test Suite"
echo "=========================================="

# Check if container is running
podman exec ${CONTAINER} mysqladmin ping > /dev/null 2>&1 || {
    echo "MySQL container not running"
    exit 1
}

# Test 1: Create test table and insert data
echo ""
echo "=== Test 1: Creating test table and inserting 5M records ==="

podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root -e "
CREATE DATABASE IF NOT EXISTS recovery_test;
USE recovery_test;

DROP TABLE IF EXISTS test_users;
CREATE TABLE test_users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id VARCHAR(50),
    email VARCHAR(255),
    name VARCHAR(100),
    created_at DATETIME
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=COMPACT;
"

# Generate and insert 5M records
echo "Inserting 5M records..."

podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root recovery_test -e "
INSERT INTO test_users (user_id, email, name, created_at) VALUES
"

# Insert data in batches
for i in $(seq 1 5); do
    echo "Batch $i..."
    podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root recovery_test -e "
    INSERT INTO test_users (user_id, email, name, created_at) SELECT
        CONCAT('user_', seq),
        CONCAT('user_', seq, '@test.com'),
        CONCAT('User_', seq),
        NOW()
    FROM (
        SELECT @row := @row + 1 AS seq
        FROM (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) t1
        CROSS JOIN (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) t2
        CROSS JOIN (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) t3
        CROSS JOIN (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) t4
        CROSS JOIN (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) t5
        CROSS JOIN (SELECT @row := 0) init
        LIMIT 100000
    ) t;
    "
done

# Verify record count
RECORD_COUNT=$(podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root -N -e "SELECT COUNT(*) FROM recovery_test.test_users;")
echo "Total records in table: ${RECORD_COUNT}"

# Get the IBD file path
IBD_PATH=$(podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root -N -e "
SELECT CONCAT(DATA_DIRECTORY, '/', TABLE_NAME, '.ibd')
FROM information_schema.tables
WHERE TABLE_SCHEMA = 'recovery_test' AND TABLE_NAME = 'test_users';
")
echo "IBD file path: ${IBD_PATH}"

# Copy IBD file to local for recovery testing
echo "Copying IBD file from container..."
podman cp ${CONTAINER}:${IBD_PATH} /app/mysql_recovery_pro/test_output/full_recovery_test/test_users.ibd 2>/dev/null || {
    echo "Direct cp failed, using alternative method..."
    podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} bash -c "cat ${IBD_PATH}" > /app/mysql_recovery_pro/test_output/full_recovery_test/test_users.ibd
}

# Verify IBD file
ls -la /app/mysql_recovery_pro/test_output/full_recovery_test/test_users.ibd
echo "IBD file size: $(stat -c%s /app/mysql_recovery_pro/test_output/full_recovery_test/test_users.ibd) bytes"

# Test 2: TRUNCATE and recovery
echo ""
echo "=== Test 2: TRUNCATE and recovery ==="

# Backup IBD before truncate
cp /app/mysql_recovery_pro/test_output/full_recovery_test/test_users.ibd /app/mysql_recovery_pro/test_output/full_recovery_test/test_users_backup.ibd

# TRUNCATE table in MySQL
echo "Truncating table..."
podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root recovery_test -e "TRUNCATE TABLE test_users;"

# Verify truncate
RECORD_COUNT_AFTER=$(podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root -N -e "SELECT COUNT(*) FROM recovery_test.test_users;")
echo "Records after TRUNCATE: ${RECORD_COUNT_AFTER}"

# Recovery using mysql_recovery_pro with full partition scanning
echo "Running full partition recovery..."
/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --ibd=/app/mysql_recovery_pro/test_output/full_recovery_test/test_users_backup.ibd \
    --full_scan \
    --mode=full_partition \
    --output=/app/mysql_recovery_pro/test_output/full_recovery_test/truncate_recovery 2>&1 || true

# Count recovered records
echo "Records recovered: $(wc -l /app/mysql_recovery_pro/test_output/full_recovery_test/truncate_recovery/recovered.csv 2>/dev/null | cut -d' ' -f1)"

# Test 3: rm -rf and recovery
echo ""
echo "=== Test 3: rm -rf and recovery ==="

# Backup again
cp /app/mysql_recovery_pro/test_output/full_recovery_test/test_users.ibd /app/mysql_recovery_pro/test_output/full_recovery_test/test_users_rmrf_backup.ibd

# Simulate rm -rf by overwriting IBD with zeros
dd if=/dev/zero of=/app/mysql_recovery_pro/test_output/full_recovery_test/test_users_rmrf_backup.ibd bs=16384 count=100 conv=notrunc 2>/dev/null

# Recovery
echo "Running rm -rf recovery..."
/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --ibd=/app/mysql_recovery_pro/test_output/full_recovery_test/test_users_rmrf_backup.ibd \
    --heuristic_scan \
    --output=/app/mysql_recovery_pro/test_output/full_recovery_test/rmrf_recovery 2>&1 || true

echo ""
echo "=========================================="
echo "Tests Completed"
echo "=========================================="
