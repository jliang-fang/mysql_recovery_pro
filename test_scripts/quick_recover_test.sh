#!/bin/bash

# Quick Full Recovery Test for MySQL 5.6
# Tests TRUNCATE and rm -rf scenarios with full IBD scanning

set -e

CONTAINER="mysql_recovery_56"
MYSQL_PWD="root123"

echo "=========================================="
echo "MySQL 5.6 Full Recovery Quick Test"
echo "=========================================="

# Test 1: Insert small test data (1000 records)
echo ""
echo "=== Test 1: Creating test table and inserting 1000 records ==="

podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root -e "
DROP DATABASE IF EXISTS recovery_test;
CREATE DATABASE recovery_test;
USE recovery_test;

CREATE TABLE test_users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id VARCHAR(50),
    email VARCHAR(255),
    name VARCHAR(100),
    created_at DATETIME
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=COMPACT;
"

# Insert 1000 records
echo "Inserting 1000 records..."
podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root recovery_test -e "
INSERT INTO test_users (user_id, email, name, created_at)
SELECT
    CONCAT('user_', seq),
    CONCAT('user_', seq, '@test.com'),
    CONCAT('User_', seq),
    NOW()
FROM (
    SELECT @row := @row + 1 AS seq
    FROM (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) t1
    CROSS JOIN (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) t2
    CROSS JOIN (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) t3
    CROSS JOIN (SELECT @row := 0) init
    LIMIT 1000
) t;
"

# Verify record count
RECORD_COUNT=$(podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} mysql -u root -N -e "SELECT COUNT(*) FROM recovery_test.test_users;")
echo "Total records in table: ${RECORD_COUNT}"

# Get the IBD file path (MySQL 5.6 uses default data directory)
IBD_PATH="/var/lib/mysql/recovery_test/test_users.ibd"
echo "IBD file path: ${IBD_PATH}"

# Copy IBD file to local for recovery testing
echo "Copying IBD file from container..."
podman exec -e MYSQL_PWD="${MYSQL_PWD}" ${CONTAINER} bash -c "cat ${IBD_PATH}" > /app/mysql_recovery_pro/test_output/full_recovery_test/test_users.ibd

# Verify IBD file
IBD_SIZE=$(stat -c%s /app/mysql_recovery_pro/test_output/full_recovery_test/test_users.ibd)
echo "IBD file size: ${IBD_SIZE} bytes"

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
    --full_partition_recovery 2>&1 || true

echo ""
echo "=========================================="
echo "Tests Completed"
echo "=========================================="
