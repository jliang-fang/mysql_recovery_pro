-- MySQL Recovery Pro Test Schema - MySQL 5.0/5.5/5.6
--Author: jliang.fang

CREATE DATABASE IF NOT EXISTS testdb CHARACTER SET utf8mb4;
USE testdb;

-- Test table with various data types
CREATE TABLE test_table (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(255),
    email VARCHAR(255),
    username VARCHAR(100),
    password_hash VARCHAR(64),
    status TINYINT,
    score INT,
    description TEXT,
    json_data TEXT,
    blob_data LONGBLOB,
    created_at DATETIME,
    updated_at DATETIME,
    created_date DATE,
    created_time TIME,
    score_float FLOAT,
    score_double DOUBLE,
    balance DECIMAL(10,2),
    INDEX idx_name (name),
    INDEX idx_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;

-- Additional tables for comprehensive testing
CREATE TABLE test_varchar (
    id BIGINT PRIMARY KEY,
    short_varchar VARCHAR(50),
    medium_varchar VARCHAR(255),
    long_varchar VARCHAR(1000)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE test_text (
    id BIGINT PRIMARY KEY,
    tiny_text TINYTEXT,
    normal_text TEXT,
    medium_text MEDIUMTEXT,
    long_text LONGTEXT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE test_datetime (
    id BIGINT PRIMARY KEY,
    dt DATETIME,
    ts TIMESTAMP,
    tm TIME,
    yd YEAR
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE test_numeric (
    id BIGINT PRIMARY KEY,
    bi BIGINT,
    si SMALLINT,
    ti TINYINT,
    mi MEDIUMINT,
    ii INT,
    flt FLOAT,
    dbl DOUBLE,
    dec_val DECIMAL(20,10),
    bit_val BIT(64)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE test_blob (
    id BIGINT PRIMARY KEY,
    tiny_blob TINYBLOB,
    normal_blob BLOB,
    medium_blob MEDIUMBLOB,
    long_blob LONGBLOB
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE test_enum (
    id BIGINT PRIMARY KEY,
    status ENUM('active', 'inactive', 'pending', 'suspended'),
    category ENUM('A', 'B', 'C', 'D', 'E')
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE test_set (
    id BIGINT PRIMARY KEY,
    permissions SET('read', 'write', 'delete', 'admin')
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE test_geometry (
    id BIGINT PRIMARY KEY,
    pt POINT,
    poly POLYGON
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Partitioned table for advanced testing
CREATE TABLE test_partitioned (
    id BIGINT PRIMARY KEY,
    created_at DATETIME,
    data TEXT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (YEAR(created_at)) (
    PARTITION p2020 VALUES LESS THAN (2021),
    PARTITION p2021 VALUES LESS THAN (2022),
    PARTITION p2022 VALUES LESS THAN (2023),
    PARTITION p2023 VALUES LESS THAN (2024),
    PARTITION pmax VALUES LESS THAN MAXVALUE
);
