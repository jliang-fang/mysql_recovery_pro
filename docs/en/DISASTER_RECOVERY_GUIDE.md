# MySQL Disaster Recovery Practical Guide

## Table of Contents
1. [TRUNCATE TABLE Recovery](#truncate-table-recovery)
2. [DROP TABLE Recovery](#drop-table-recovery)
3. [rm -rf Recovery](#rm-rf-recovery)
4. [Schema Parameter Practice](#schema-parameter-practice)
5. [MySQL Version Differences](#mysql-version-differences)
6. [Recovery Validation Results](#recovery-validation-results)

---

## TRUNCATE TABLE Recovery

### MySQL Version Differences

TRUNCATE TABLE has different implementations across MySQL versions:

| MySQL Version | TRUNCATE Implementation | Data Recoverability | Recovery Method |
|---------------|-------------------------|---------------------|-----------------|
| **8.0+** | DROP + CREATE | **Data completely lost** | Disk carving (raw disk) |
| **5.6/5.7** | DELETE row-by-row | **Recoverable** | Undo Log + Disk carving |

### MySQL 8.0+ TRUNCATE Recovery Process

```bash
# Note: MySQL 8.0+ TRUNCATE = DROP + CREATE
# Original data pages are freed, new file is empty table

# Step 1: Immediately stop MySQL service (prevent data overwrite)
sudo systemctl stop mysqld

# Step 2: Copy current .ibd file (may be rebuilt as empty)
cp /var/lib/mysql/dbname/table.ibd /recovery/table_after_truncate.ibd

# Step 3: Need disk carving from original disk device
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --output=/recovery/output \
    --num-workers=8

# Step 4: Use Schema parameter to improve data quality
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --schema="id INT PRIMARY KEY, name VARCHAR(100), status ENUM('active','inactive')" \
    --output=/recovery/output
```

### MySQL 5.6/5.7 TRUNCATE Recovery Process

```bash
# MySQL 5.6/5.7 uses DELETE method, can recover from Undo Log

# Step 1: Stop MySQL service
sudo systemctl stop mysqld

# Step 2: Copy system tablespace (ibdata1)
cp /var/lib/mysql/ibdata1 /recovery/ibdata1_backup

# Step 3: Recover from Undo Log
./mysql_recovery_pro --truncate-recovery \
    --system-tablespace=/recovery/ibdata1_backup \
    --output=/recovery/output

# Step 4: Also try disk carving (from .ibd file)
cp /var/lib/mysql/dbname/table.ibd /recovery/table.ibd
./mysql_recovery_pro --disk-carving \
    --device-path=/recovery/table.ibd \
    --frm=/var/lib/mysql/dbname/table.frm \
    --output=/recovery/output
```

---

## DROP TABLE Recovery

### Prerequisites Check

```bash
# Check innodb_file_per_table configuration
mysql -e "SHOW VARIABLES LIKE 'innodb_file_per_table';"

# If ON: Each table has independent .ibd file, file deleted after DROP TABLE
# If OFF: All data in ibdata1, can recover from ibdata1
```

### innodb_file_per_table=ON Scenario

```bash
# Step 1: Immediately unmount filesystem
sudo umount /dev/sda1
# Or switch to read-only
sudo mount -o remount,ro /dev/sda1

# Step 2: Use testdisk/debugfs to recover deleted files
sudo testdisk /dev/sda1
# Or
sudo debugfs -R "lsdel" /dev/sda1 > deleted_files.txt
sudo debugfs -R "dump <inode> /recovery/table.ibd" /dev/sda1

# Step 3: Recover data
./mysql_recovery_pro --ibd=/recovery/table.ibd \
    --frm=/backup/table.frm \
    --output=/recovery/output

# Step 4: If no FRM backup, use Schema parameter
./mysql_recovery_pro --ibd=/recovery/table.ibd \
    --schema="id INT PRIMARY KEY, name VARCHAR(100), created_at DATETIME" \
    --output=/recovery/output
```

### innodb_file_per_table=OFF Scenario

```bash
# Data still in ibdata1, recover directly from system tablespace
./mysql_recovery_pro --disk-carving \
    --device-path=/var/lib/mysql/ibdata1 \
    --output=/recovery/output
```

---

## rm -rf Recovery

### Emergency Response Process

```bash
# ⚠️ Immediately stop all write operations

# Step 1: Unmount filesystem (most critical)
sudo umount /dev/sda1

# Step 2: If cannot unmount, switch to read-only
sudo mount -o remount,ro /dev/sda1

# Step 3: Create recovery working directory
mkdir -p /recovery/output
```

### Disk Carving Recovery

```bash
# Scan all InnoDB pages from raw disk device
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --output=/recovery/output \
    --num-workers=8 \
    --verbose

# Output results
ls /recovery/output/
# recovered_index_123.csv  - One CSV per index ID
# stats.txt                - Recovery statistics report
```

### Large Disk Optimization

```bash
# Recommended parameters for 100GB+ disks
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --output=/recovery/output \
    --num-workers=16 \
    --page-size=16384 \
    --verbose

# Scan specific range only (save time)
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --output=/recovery/output \
    --start-offset=0 \
    --end-offset=107374182400  # 100GB
```

---

## Schema Parameter Practice

### Three Schema Methods Comparison

| Method | Use Case | ENUM Support | Validation Result |
|--------|----------|--------------|-------------------|
| `--schema=<def>` | Quick schema definition | ✅ SQL format | 100% |
| `--schema-file=<path>` | Complex schema | ✅ JSON array | 100% |
| `--frm=<path>` | MySQL 5.6/5.7 | ✅ Auto-parse | 100% |

### Practical Validation: products Table

**Table Structure**:
```sql
CREATE TABLE products (
    id INT PRIMARY KEY AUTO_INCREMENT,
    product_name VARCHAR(200),
    category ENUM('electronics', 'clothing', 'food', 'toys'),
    price DECIMAL(10,2),
    stock INT,
    created_at DATETIME,
    json_attr JSON
);
```

**Test Data**: 5 records with ENUM, JSON, DECIMAL complex types

#### Method 1: SQL Format Schema String

```bash
./mysql_recovery_pro \
    --ibd=/backup/products.ibd \
    --schema="id INT PRIMARY KEY, product_name VARCHAR(200), category ENUM('electronics','clothing','food','toys'), price DECIMAL(10,2), stock INT, created_at DATETIME, json_attr JSON" \
    --output=/recovery/sql_schema
```

**Output**:
```csv
id,product_name,category,price,stock,created_at,json_attr
1,Laptop Pro 15,electronics,1299.99,50,2026-06-28 10:00:00,{"brand":"TechCorp","warranty":2}
2,Cotton T-Shirt,clothing,29.99,200,2026-06-28 11:00:00,{"brand":"ClothCo","sizes":["S","M","L","XL"]}
```

**Result**: 5/5 recovered, ENUM correctly displayed as actual values

#### Method 2: JSON Schema File

```bash
# Create JSON file
cat > products_schema.json << 'EOF'
{
  "table_name": "products",
  "columns": [
    {"name": "id", "type": "INT", "is_nullable": false, "column_key": "PRIMARY"},
    {"name": "product_name", "type": "VARCHAR", "length": 200},
    {"name": "category", "type": "ENUM", "elements": ["electronics", "clothing", "food", "toys"]},
    {"name": "price", "type": "DECIMAL", "precision": 10, "scale": 2},
    {"name": "stock", "type": "INT"},
    {"name": "created_at", "type": "DATETIME"},
    {"name": "json_attr", "type": "JSON"}
  ]
}
EOF

./mysql_recovery_pro \
    --ibd=/backup/products.ibd \
    --schema-file=products_schema.json \
    --output=/recovery/json_schema
```

**Result**: 5/5 recovered, ENUM correctly displayed

#### Method 3: FRM File (MySQL 5.6/5.7)

```bash
./mysql_recovery_pro \
    --ibd=/backup/products_56/products.ibd \
    --frm=/backup/products_56/products.frm \
    --output=/recovery/frm_recovery
```

**Output**:
```csv
id,product_name,category,price,stock,created_at,description
1,Laptop Pro 15,electronics,1299.99,50,2026-06-28 10:00:00,High performance laptop
```

**Result**: 3/3 recovered, ENUM correctly displayed

---

## MySQL Version Differences

### TRUNCATE TABLE Implementation Differences

| Version | Implementation | File Status | Data Remains |
|---------|----------------|-------------|--------------|
| 5.6 | DELETE row-by-row | File size unchanged | Recoverable |
| 5.7 | DELETE row-by-row | File size unchanged | Recoverable |
| 8.0 | DROP+CREATE | File rebuilt empty | **Data lost** |
| 8.4 | DROP+CREATE | File rebuilt empty | **Data lost** |
| 9.0 | DROP+CREATE | File rebuilt empty | **Data lost** |

### SDI/FRM Differences

| Version | Schema Storage | Disk Carving Schema Source |
|---------|----------------|---------------------------|
| 5.6 | `.frm` file | Must provide FRM or manual Schema |
| 5.7 | `.frm` file | Must provide FRM or manual Schema |
| 8.0 | SDI embedded in .ibd | Auto-extract from SDI |
| 8.4 | SDI embedded in .ibd | Auto-extract from SDI |
| 9.0 | SDI embedded in .ibd | Auto-extract from SDI |

### JSON Field Support

| Version | JSON Field Support |
|---------|-------------------|
| 5.6 | ❌ Not supported |
| 5.7 | ✅ Supported |
| 8.0+ | ✅ Supported |

---

## Recovery Validation Results

### Test Environment

| Version | Container | Test Data | Test Scenario |
|---------|-----------|-----------|---------------|
| MySQL 5.6 | mysql56_test | 5 records | FRM recovery |
| MySQL 8.0 | mysql80_test | 5 records | Schema parameter recovery |

### Schema Parameter Validation Summary

| Parameter Method | Records Recovered | ENUM Display | Data Integrity |
|------------------|-------------------|--------------|----------------|
| `--schema=<def>` (SQL format) | 5/5 | Actual value | 100% |
| `--schema-file=<path>` | 5/5 | Actual value | 100% |
| `--frm=<path>` | 3/3 | Actual value | 100% |

### TRUNCATE Recovery Validation

| Version | Expected Recovery | Actual Recovery | Notes |
|---------|-------------------|-----------------|-------|
| MySQL 8.0 | 0 records | 0 records | TRUNCATE=DROP+CREATE, data lost |
| MySQL 5.7 | Recoverable | Needs testing | Undo Log + Disk carving |

### Disk Carving Validation

| Source | Pages | Records | Schema Method |
|--------|-------|---------|---------------|
| ibdata1 | 102 | 9,051 | Auto SDI |
| products.ibd | 7 | 5 | Manual Schema |
| FRM file | - | 3 | FRM auto-parse |

---

## Best Practices Summary

### 1. Emergency Response Priorities

1. **Immediately unmount filesystem** - Prevent data overwrite
2. **Stop MySQL service** - Prevent new data writes
3. **Backup existing files** - Even if files may be corrupted
4. **Start recovery operation** - Use appropriate recovery mode

### 2. Schema Selection Recommendations

| Scenario | Recommended Method |
|----------|-------------------|
| MySQL 8.0+ with SDI | Auto-extract (no specification needed) |
| MySQL 5.6/5.7 with FRM backup | `--frm=<path>` |
| No FRM backup, simple table structure | `--schema=<def>` SQL format |
| Complex table structure (multiple ENUM/SET) | `--schema-file=<path>` |

### 3. ENUM/SET Handling Points

- **Must use SQL format or JSON file**
- Colon-separated format does not support ENUM elements
- FRM file automatically parses ENUM elements

### 4. Data Quality Verification

```bash
# Check recovered record count
wc -l /recovery/output/*.csv

# Check if ENUM values are correctly displayed
grep -E "electronics|clothing|food|toys" /recovery/output/*.csv

# Check JSON field completeness
grep "brand" /recovery/output/*.csv
```