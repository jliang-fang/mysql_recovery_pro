-- MySQL Recovery Pro Test Schema - MySQL 5.7/8.0/8.4/9.0
-- Author: jliang.fang

CREATE DATABASE IF NOT EXISTS testdb CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE testdb;

-- Test table with various data types (MySQL 5.7+ format)
CREATE TABLE test_table (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(255),
    email VARCHAR(255),
    username VARCHAR(100),
    password_hash VARCHAR(64),
    status TINYINT,
    score INT,
    description TEXT,
    json_data JSON,
    blob_data LONGBLOB,
    created_at DATETIME(6),
    updated_at DATETIME(6),
    created_date DATE,
    created_time TIME(6),
    score_float FLOAT,
    score_double DOUBLE,
    balance DECIMAL(20,10),
    vector_data VECTOR(128),
    INDEX idx_name (name),
    INDEX idx_email (email),
    INDEX idx_created_at (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Additional tables for comprehensive testing
CREATE TABLE test_varchar (
    id BIGINT PRIMARY KEY,
    short_varchar VARCHAR(50),
    medium_varchar VARCHAR(255),
    long_varchar VARCHAR(1000),
    fulltext_index FULLTEXT KEY idx_fulltext (medium_varchar)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE test_text (
    id BIGINT PRIMARY KEY,
    tiny_text TINYTEXT,
    normal_text TEXT,
    medium_text MEDIUMTEXT,
    long_text LONGTEXT,
    FULLTEXT KEY idx_fulltext (tiny_text, normal_text)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE test_datetime (
    id BIGINT PRIMARY KEY,
    dt DATETIME(6),
    ts TIMESTAMP(6) DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    tm TIME(6),
    yd YEAR
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE test_numeric (
    id BIGINT PRIMARY KEY,
    bi BIGINT,
    si SMALLINT,
    ti TINYINT,
    mi MEDIUMINT,
    ii INT,
    flt FLOAT,
    dbl DOUBLE,
    dec_val DECIMAL(30,15),
    bit_val BIT(64),
    alloc_date DATE,
    alloc_time TIME(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE test_blob (
    id BIGINT PRIMARY KEY,
    tiny_blob TINYBLOB,
    normal_blob BLOB,
    medium_blob MEDIUMBLOB,
    long_blob LONGBLOB
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE test_enum (
    id BIGINT PRIMARY KEY,
    status ENUM('active', 'inactive', 'pending', 'suspended'),
    category ENUM('A', 'B', 'C', 'D', 'E')
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE test_set (
    id BIGINT PRIMARY KEY,
    permissions SET('read', 'write', 'delete', 'admin')
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE test_geometry (
    id BIGINT PRIMARY KEY,
    pt POINT SRID 4326,
    poly POLYGON SRID 4326,
    SPATIAL KEY idx_spatial (pt)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Partitioned table for advanced testing
CREATE TABLE test_partitioned (
    id BIGINT PRIMARY KEY,
    created_at DATETIME(6),
    data TEXT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
PARTITION BY RANGE (YEAR(created_at)) (
    PARTITION p2020 VALUES LESS THAN (2021),
    PARTITION p2021 VALUES LESS THAN (2022),
    PARTITION p2022 VALUES LESS THAN (2023),
    PARTITION p2023 VALUES LESS THAN (2024),
    PARTITION pmax VALUES LESS THAN MAXVALUE
);

-- Spatial reference system for MySQL 8.0+
INSERT INTO spatial_ref_sys (srs_id, srs_name, organization, organization_coordsys_id, definition, description)
VALUES (4326, 'WGS 84', 'EPSG', 4326, 'GEOGCRS["WGS 84",_DATUM["World Geodetic System 1984",ELLIPSOID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]]', 'World Geodetic System 1984')
ON DUPLICATE KEY UPDATE srs_name = VALUES(srs_name);

-- Create vector index for MySQL 8.0+ (if supported)
-- CREATE VECTOR INDEX idx_vector ON test_table (vector_data);
