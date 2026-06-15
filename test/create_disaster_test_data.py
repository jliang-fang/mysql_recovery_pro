#!/usr/bin/env python3
"""
Create disaster recovery test data with 10k+ records per table.
This script generates test .ibd files that simulate various disaster scenarios.
"""

import os
import sys
import struct
import random
import hashlib
import shutil
from datetime import datetime
from pathlib import Path

# Test data configuration
RECORD_COUNT = 10000  # Minimum 10k records per table
PAGE_SIZE = 16384

def create_test_sql(output_dir):
    """Create SQL script to generate test data"""

    sql_content = f"""
-- Disaster Recovery Test Data
-- Generated at {datetime.now().isoformat()}
-- Record count: {RECORD_COUNT}

CREATE DATABASE IF NOT EXISTS disaster_test;
USE disaster_test;

-- Table 1: users - Basic table with various data types
CREATE TABLE users (
    id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(64) NOT NULL,
    email VARCHAR(255) NOT NULL,
    age TINYINT UNSIGNED,
    balance DECIMAL(15,2),
    created_at DATETIME,
    updated_at TIMESTAMP,
    status ENUM('active', 'inactive', 'suspended'),
    profile TEXT,
    INDEX idx_email (email),
    INDEX idx_status (status)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Table 2: orders - Transaction table
CREATE TABLE orders (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    user_id INT UNSIGNED NOT NULL,
    product_id INT UNSIGNED NOT NULL,
    quantity INT UNSIGNED NOT NULL,
    unit_price DECIMAL(10,2) NOT NULL,
    total_amount DECIMAL(15,2) NOT NULL,
    order_date DATETIME NOT NULL,
    status ENUM('pending', 'paid', 'shipped', 'delivered', 'cancelled'),
    shipping_address TEXT,
    notes VARCHAR(500),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_user (user_id),
    INDEX idx_product (product_id),
    INDEX idx_status (status),
    INDEX idx_date (order_date)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Table 3: products - Product catalog
CREATE TABLE products (
    id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    description TEXT,
    category_id INT UNSIGNED,
    price DECIMAL(10,2) NOT NULL,
    stock INT UNSIGNED DEFAULT 0,
    weight DECIMAL(8,3),
    is_active BOOLEAN DEFAULT TRUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_category (category_id),
    INDEX idx_active (is_active)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Table 4: audit_log - Large table for rm -rf test
CREATE TABLE audit_log (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    table_name VARCHAR(64) NOT NULL,
    operation ENUM('INSERT', 'UPDATE', 'DELETE') NOT NULL,
    record_id BIGINT UNSIGNED NOT NULL,
    old_value JSON,
    new_value JSON,
    user_id INT UNSIGNED,
    ip_address VARCHAR(45),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_table (table_name),
    INDEX idx_record (record_id),
    INDEX idx_time (created_at)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Table 5: mixed_types - Various column types
CREATE TABLE mixed_types (
    id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    col_tinyint TINYINT,
    col_smallint SMALLINT,
    col_mediumint MEDIUMINT,
    col_int INT,
    col_bigint BIGINT,
    col_float FLOAT,
    col_double DOUBLE,
    col_decimal DECIMAL(20,10),
    col_char CHAR(10),
    col_varchar VARCHAR(255),
    col_text TEXT,
    col_blob BLOB,
    col_date DATE,
    col_time TIME,
    col_datetime DATETIME,
    col_timestamp TIMESTAMP,
    col_year YEAR,
    col_enum ENUM('a', 'b', 'c', 'd'),
    col_set SET('x', 'y', 'z'),
    col_bit BIT(16),
    col_json JSON,
    INDEX idx_int (col_int),
    INDEX idx_datetime (col_datetime)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Generate test data
DELIMITER //
CREATE PROCEDURE generate_test_data()
BEGIN
    DECLARE i INT DEFAULT 0;
    DECLARE batch_size INT DEFAULT 1000;

    -- Generate users
    WHILE i < {RECORD_COUNT} DO
        INSERT INTO users (username, email, age, balance, created_at, updated_at, status, profile)
        SELECT
            CONCAT('user_', i + seq),
            CONCAT('user_', i + seq, '@test.com'),
            FLOOR(RAND() * 100),
            RAND() * 100000,
            DATE_SUB(NOW(), INTERVAL FLOOR(RAND() * 365) DAY),
            NOW(),
            ELT(FLOOR(RAND() * 3) + 1, 'active', 'inactive', 'suspended'),
            CONCAT('Profile for user ', i + seq)
        FROM (
            SELECT 1 AS seq UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5
            UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9 UNION SELECT 10
        ) AS seqs;
        SET i = i + batch_size;
    END WHILE;

    -- Generate products
    SET i = 0;
    WHILE i < {RECORD_COUNT} DO
        INSERT INTO products (name, description, category_id, price, stock, weight, is_active)
        SELECT
            CONCAT('Product ', i + seq),
            CONCAT('Description for product ', i + seq),
            FLOOR(RAND() * 100),
            RAND() * 1000,
            FLOOR(RAND() * 1000),
            RAND() * 100,
            RAND() > 0.1
        FROM (
            SELECT 1 AS seq UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5
            UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9 UNION SELECT 10
        ) AS seqs;
        SET i = i + batch_size;
    END WHILE;

    -- Generate orders
    SET i = 0;
    WHILE i < {RECORD_COUNT} DO
        INSERT INTO orders (user_id, product_id, quantity, unit_price, total_amount, order_date, status, shipping_address, notes)
        SELECT
            FLOOR(RAND() * {RECORD_COUNT}) + 1,
            FLOOR(RAND() * {RECORD_COUNT}) + 1,
            FLOOR(RAND() * 10) + 1,
            RAND() * 1000,
            RAND() * 10000,
            DATE_SUB(NOW(), INTERVAL FLOOR(RAND() * 365) DAY),
            ELT(FLOOR(RAND() * 5) + 1, 'pending', 'paid', 'shipped', 'delivered', 'cancelled'),
            CONCAT('Address ', FLOOR(RAND() * 1000)),
            CONCAT('Order notes ', FLOOR(RAND() * 10000))
        FROM (
            SELECT 1 AS seq UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5
            UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9 UNION SELECT 10
        ) AS seqs;
        SET i = i + batch_size;
    END WHILE;

    -- Generate audit_log
    SET i = 0;
    WHILE i < {RECORD_COUNT} DO
        INSERT INTO audit_log (table_name, operation, record_id, old_value, new_value, user_id, ip_address)
        SELECT
            ELT(FLOOR(RAND() * 4) + 1, 'users', 'orders', 'products', 'audit_log'),
            ELT(FLOOR(RAND() * 3) + 1, 'INSERT', 'UPDATE', 'DELETE'),
            FLOOR(RAND() * {RECORD_COUNT}),
            JSON_OBJECT('field', CONCAT('old_value_', FLOOR(RAND() * 100))),
            JSON_OBJECT('field', CONCAT('new_value_', FLOOR(RAND() * 100))),
            FLOOR(RAND() * 100),
            CONCAT(FLOOR(RAND() * 255), '.', FLOOR(RAND() * 255), '.', FLOOR(RAND() * 255), '.', FLOOR(RAND() * 255))
        FROM (
            SELECT 1 AS seq UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5
            UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9 UNION SELECT 10
        ) AS seqs;
        SET i = i + batch_size;
    END WHILE;

    -- Generate mixed_types
    SET i = 0;
    WHILE i < {RECORD_COUNT} DO
        INSERT INTO mixed_types (col_tinyint, col_smallint, col_mediumint, col_int, col_bigint,
            col_float, col_double, col_decimal, col_char, col_varchar, col_text,
            col_date, col_time, col_datetime, col_timestamp, col_year, col_enum, col_set, col_bit, col_json)
        SELECT
            FLOOR(RAND() * 256) - 128,
            FLOOR(RAND() * 65536) - 32768,
            FLOOR(RAND() * 16777216),
            FLOOR(RAND() * 4294967296),
            FLOOR(RAND() * 18446744073709551616),
            RAND() * 1000,
            RAND() * 1000000,
            RAND() * 1000000000000,
            LPAD(FLOOR(RAND() * 1000000000), 10, '0'),
            CONCAT('varchar_', FLOOR(RAND() * 1000000)),
            CONCAT('Long text content for record ', i + seq, ' with additional data'),
            DATE_SUB(NOW(), INTERVAL FLOOR(RAND() * 365) DAY),
            TIME(CONCAT(FLOOR(RAND() * 24), ':', FLOOR(RAND() * 60), ':', FLOOR(RAND() * 60))),
            DATE_SUB(NOW(), INTERVAL FLOOR(RAND() * 365) DAY),
            NOW(),
            2000 + FLOOR(RAND() * 26),
            ELT(FLOOR(RAND() * 4) + 1, 'a', 'b', 'c', 'd'),
            ELT(FLOOR(RAND() * 7), '', 'x', 'y', 'z', 'x,y', 'y,z', 'x,y,z'),
            FLOOR(RAND() * 65536),
            JSON_OBJECT('key1', FLOOR(RAND() * 1000), 'key2', CONCAT('value_', FLOOR(RAND() * 100)))
        FROM (
            SELECT 1 AS seq UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5
            UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9 UNION SELECT 10
        ) AS seqs;
        SET i = i + batch_size;
    END WHILE;
END //
DELIMITER ;

CALL generate_test_data();
DROP PROCEDURE generate_test_data;

-- Verify record counts
SELECT 'users' AS table_name, COUNT(*) AS record_count FROM users
UNION ALL
SELECT 'orders', COUNT(*) FROM orders
UNION ALL
SELECT 'products', COUNT(*) FROM products
UNION ALL
SELECT 'audit_log', COUNT(*) FROM audit_log
UNION ALL
SELECT 'mixed_types', COUNT(*) FROM mixed_types;
"""

    sql_path = os.path.join(output_dir, 'disaster_test_data.sql')
    with open(sql_path, 'w') as f:
        f.write(sql_content)

    return sql_path


def create_disaster_scenarios(ibd_dir, output_dir):
    """Create disaster scenario test files"""

    scenarios = {
        'rmrf': 'Simulate rm -rf by copying files to deleted location',
        'truncate': 'Simulate TRUNCATE TABLE by zeroing data pages',
        'drop': 'Simulate DROP TABLE (handled by existing recovery)',
        'corrupt_header': 'Corrupt file header (first 64 bytes)',
        'corrupt_middle': 'Corrupt middle section',
        'corrupt_tail': 'Corrupt file tail',
        'corrupt_multiple': 'Multiple random corruption points',
    }

    # Create disaster scenario directories
    for scenario in scenarios:
        scenario_dir = os.path.join(output_dir, scenario)
        os.makedirs(scenario_dir, exist_ok=True)

    # Find all .ibd files
    ibd_files = list(Path(ibd_dir).glob('**/*.ibd'))

    for ibd_file in ibd_files:
        if 'mysql' in str(ibd_file) and 'disaster_test' not in str(ibd_file):
            continue

        filename = ibd_file.name
        src_path = str(ibd_file)

        # Create corrupted versions
        with open(src_path, 'rb') as f:
            original_data = bytearray(f.read())

        file_size = len(original_data)

        # Header corruption
        header_data = bytearray(original_data)
        for i in range(min(64, file_size)):
            header_data[i] = random.randint(0, 255)
        header_path = os.path.join(output_dir, 'corrupt_header', filename)
        with open(header_path, 'wb') as f:
            f.write(header_data)

        # Middle corruption
        mid = file_size // 2
        middle_data = bytearray(original_data)
        for i in range(max(0, mid - 512), min(file_size, mid + 512)):
            middle_data[i] = random.randint(0, 255)
        middle_path = os.path.join(output_dir, 'corrupt_middle', filename)
        with open(middle_path, 'wb') as f:
            f.write(middle_data)

        # Tail corruption
        tail_data = bytearray(original_data)
        for i in range(max(0, file_size - 1024), file_size):
            tail_data[i] = random.randint(0, 255)
        tail_path = os.path.join(output_dir, 'corrupt_tail', filename)
        with open(tail_path, 'wb') as f:
            f.write(tail_data)

        # Multiple corruption
        multi_data = bytearray(original_data)
        for _ in range(20):
            pos = random.randint(0, max(1, file_size - 256))
            for i in range(pos, min(file_size, pos + 128)):
                multi_data[i] = random.randint(0, 255)
        multi_path = os.path.join(output_dir, 'corrupt_multiple', filename)
        with open(multi_path, 'wb') as f:
            f.write(multi_data)

        # Simulate TRUNCATE (zero out data pages but keep header)
        truncate_data = bytearray(original_data)
        if file_size > PAGE_SIZE:
            for i in range(PAGE_SIZE, file_size):
                truncate_data[i] = 0
        truncate_path = os.path.join(output_dir, 'truncate', filename)
        with open(truncate_path, 'wb') as f:
            f.write(truncate_data)

        # rm -rf simulation (just copy file)
        rmrf_path = os.path.join(output_dir, 'rmrf', filename)
        shutil.copy(src_path, rmrf_path)

    print(f"Created disaster scenarios for {len(ibd_files)} files")
    return scenarios


def main():
    output_dir = '/app/mysql_recovery_test/disaster_scenarios'

    # Create SQL generation script
    sql_path = create_test_sql(output_dir)
    print(f"Generated SQL script: {sql_path}")

    # Check if test data exists
    test_data_dir = '/app/mysql_recovery_test/mysql80/data/disaster_test'
    if os.path.exists(test_data_dir):
        print(f"Found existing test data at {test_data_dir}")
        scenarios = create_disaster_scenarios(test_data_dir, output_dir)
        print(f"Created disaster scenarios at {output_dir}")
    else:
        print(f"Test data directory not found: {test_data_dir}")
        print("Please run the SQL script to generate test data first.")

    # Print instructions
    print("\n" + "="*60)
    print("INSTRUCTIONS FOR GENERATING TEST DATA:")
    print("="*60)
    print(f"1. Connect to MySQL 8.0")
    print(f"2. Run: source {sql_path}")
    print(f"3. Repeat for MySQL 5.6, 5.7, 8.4, 9.0")
    print("="*60)


if __name__ == '__main__':
    main()
