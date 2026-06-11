#!/bin/bash
# Test MySQL 5.6 recovery with proper InnoDB format using Podman

set -e

CONTAINER="mysql_recovery_pro_56"
DB_NAME="recovery_test"
TABLE_NAME="test_users"

echo "=== MySQL 5.6 Proper Recovery Test ==="
echo "Container: $CONTAINER"
echo "Database: $DB_NAME"
echo "Table: $TABLE_NAME"

# Start MySQL 5.6 container if not running
echo "Checking MySQL container..."
if ! podman ps | grep -q "$CONTAINER"; then
    echo "Starting MySQL 5.6 container..."
    podman run -d \
        --name "$CONTAINER" \
        -e MYSQL_ROOT_PASSWORD=rootpass \
        -e MYSQL_DATABASE="$DB_NAME" \
        -p 33056:3306 \
        -v /app/mysql_recovery_pro/test_data:/docker-entrypoint-initdb.d \
        docker.m.daocloud.io/mysql:5.6 \
        --character-set-server=utf8mb4 \
        --collation-server=utf8mb4_unicode_ci
    sleep 10
fi

# Create test table
echo "Creating test table..."
podman exec "$CONTAINER" mysql -uroot -prootpass -e "
CREATE DATABASE IF NOT EXISTS $DB_NAME;
USE $DB_NAME;
DROP TABLE IF EXISTS $TABLE_NAME;
CREATE TABLE $TABLE_NAME (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL,
    email VARCHAR(100) NOT NULL,
    full_name VARCHAR(100) DEFAULT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=COMPACT;
"

# Insert 5.5M records
echo "Inserting 5,500,000 records..."
podman exec "$CONTAINER" mysql -uroot -prootpass -e "
USE $DB_NAME;
SET SQL_LOG_BIN = 0;
SET autocommit = 0;
INSERT INTO $TABLE_NAME (username, email, full_name) VALUES
" > /dev/null

# Generate INSERT statements for 5.5M records
echo "Generating test data..."
(
echo "USE $DB_NAME;"
echo "SET autocommit = 0;"

for i in $(seq 1 5500000); do
    username="user_$(printf '%08d' $i)"
    email="${username}@gmail.com"
    full_name="User_$(printf '%08d' $i)"
    
    if [ $((i % 1000)) -eq 1 ]; then
        echo "INSERT INTO $TABLE_NAME (username, email, full_name) VALUES ('$username', '$email', '$full_name'),"
    elif [ $((i % 1000)) -eq 0 ]; then
        echo "('$username', '$email', '$full_name');"
    else
        echo "('$username', '$email', '$full_name'),"
    fi
    
    if [ $((i % 100000)) -eq 0 ]; then
        echo "COMMIT; START TRANSACTION; SET autocommit = 0;" | podman exec -i "$CONTAINER" mysql -uroot -prootpass
        echo "Inserted $i records..."
    fi
done

echo "COMMIT;"
) > /tmp/insert_5m.sql

echo "Executing INSERT statements..."
podman exec -i "$CONTAINER" mysql -uroot -prootpass "$DB_NAME" < /tmp/insert_5m.sql

# Verify data
echo "Verifying inserted data..."
podman exec "$CONTAINER" mysql -uroot -prootpass -e "
USE $DB_NAME;
SELECT COUNT(*) as count FROM $TABLE_NAME;
SELECT MIN(id), MAX(id) FROM $TABLE_NAME;
"

# Get IBD file path
echo "Getting IBD file location..."
IBD_PATH=$(podman exec "$CONTAINER" mysql -uroot -prootpass -e "
USE $DB_NAME;
SELECT FILE_NAME FROM information_schema.files 
WHERE TABLESPACE_NAME = 'innodb_temporary' OR FILE_NAME LIKE '%$TABLE_NAME%.ibd' 
LIMIT 1;
" | grep -v FILE_NAME)

echo "IBD file path: $IBD_PATH"

# Copy IBD file from container
echo "Copying IBD file from container..."
podman cp "$CONTAINER:$IBD_PATH" /app/mysql_recovery_pro/test_data/test_mysql56_proper.ibd

# Test recovery
echo "=== Running Recovery Test ==="
cd /app/mysql_recovery_pro
./build/mysql_recovery_pro \
    --ibd=/app/mysql_recovery_pro/test_data/test_mysql56_proper.ibd \
    --output=/app/mysql_recovery_pro/test_output/mysql56_proper \
    --format=csv \
    --verbose

# Verify recovered records
echo "=== Verification ==="
RECOVERED_COUNT=$(wc -l < /app/mysql_recovery_pro/test_output/mysql56_proper/$TABLE_NAME.csv)
EXPECTED_COUNT=5500000
echo "Expected records: $EXPECTED_COUNT"
echo "Recovered records: $RECOVERED_COUNT"

if [ "$RECOVERED_COUNT" -ge 5499000 ]; then
    echo "SUCCESS: Recovery rate >= 99.9%"
else
    echo "WARNING: Recovery rate < 99.9%"
fi

# Cleanup
echo "=== Cleanup ==="
podman stop "$CONTAINER"
podman rm "$CONTAINER"
