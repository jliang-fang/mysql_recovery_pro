#!/bin/bash
# =============================================================================
# Generate 5M+ Test Data for MySQL Recovery Testing (Optimized)
# Uses LOAD DATA with secure_file_priv directory
# =============================================================================

DB="recovery_test"
TABLE="test_users"
NUM_RECORDS=5500000

echo "Generating ${NUM_RECORDS} test records..."

# Create temp directory in MySQL's secure_file_priv
TEMP_DIR="/var/lib/mysql-files/recovery_test_$$"
mkdir -p "$TEMP_DIR"
echo "Using temp directory: ${TEMP_DIR}"

# Generate CSV file
echo "Generating CSV data..."
start_time=$(date +%s)

# Generate data in chunks to avoid memory issues
for chunk in {1..110}; do
    chunk_start=$(( (chunk - 1) * 50000 + 1 ))
    chunk_end=$(( chunk * 50000 ))
    
    for i in $(seq $chunk_start $chunk_end); do
        printf "user_%08d,user_%08d@gmail.com,User_%08d\n" $i $i $i
    done
done > "$TEMP_DIR/users.csv"

echo "CSV generated: $(wc -l < "$TEMP_DIR/users.csv") lines"
echo "CSV size: $(du -h "$TEMP_DIR/users.csv" | cut -f1)"

elapsed=$(($(date +%s) - start_time))
echo "Data generation elapsed: ${elapsed} seconds"

# Create table in MySQL
echo ""
echo "Creating table in MySQL..."
mysql -e "
CREATE DATABASE IF NOT EXISTS ${DB};
USE ${DB};
DROP TABLE IF EXISTS ${TABLE};
CREATE TABLE ${TABLE} (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL,
    email VARCHAR(100) NOT NULL,
    full_name VARCHAR(100),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_username (username),
    INDEX idx_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
" 2>&1

# Load data using LOAD DATA INFILE
echo ""
echo "Loading data with LOAD DATA INFILE..."
mysql -e "
LOAD DATA INFILE '${TEMP_DIR}/users.csv'
INTO TABLE ${DB}.${TABLE}
FIELDS TERMINATED BY ','
LINES TERMINATED BY '\n'
(username, email, full_name);
" 2>&1

load_result=$?

if [ $load_result -eq 0 ]; then
    echo "LOAD DATA completed successfully"
else
    echo "LOAD DATA failed, exit code: $load_result"
fi

# Cleanup
rm -rf "$TEMP_DIR"

# Verify count
final_count=$(mysql -N -e "SELECT COUNT(*) FROM ${DB}.${TABLE}" 2>/dev/null)
echo ""
echo "============================================"
echo "Generated ${final_count} records"
echo ""

# Table info
echo "Table information:"
mysql -e "SHOW TABLE STATUS FROM ${DB} LIKE '${TABLE}'" 2>/dev/null
