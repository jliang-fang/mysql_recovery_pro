# MySQL 8.0 Recovery Engine - User Guide

## 1. Quick Start

### 1.1 Build

```bash
# Clone project
cd /app/mysql8_recovery

# Create build directory
mkdir -p build && cd build

# Configure CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
make -j$(nproc)

# Run tests
ctest --verbose
```

### 1.2 CLI Tool

```bash
# Basic usage
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered

# Specify table ID
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --table-id=12345

# Show help
./mysql8_recover --help
```

### 1.3 GUI Tool

```bash
# Launch GUI
cd build/gui
./mysql8_recovery_gui
```

## 2. Features

### 2.1 Page Health Check

```bash
# Check page checksums
./mysql8_recover --ibd=/path/to/table.ibd --check-headers
```

### 2.2 Truncate Recovery

```bash
# Recover from truncated table
./mysql8_recover --ibd=/path/to/truncated.ibd --output=/tmp/recovered --truncate-recovery
```

### 2.3 BLOB Recovery

```bash
# Recover table with BLOB fields
./mysql8_recover --ibd=/path/to/table_with_blob.ibd --output=/tmp/recovered --blob-recovery
```

### 2.4 Redo Log Replay

```bash
# Apply Redo Log for forward recovery
./mysql8_recover --ibd=/path/to/table.ibd --redo-log=/path/to/ib_logfile0 --output=/tmp/recovered
```

### 2.5 Export to Multiple Formats

```bash
# Export as CSV
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --format=csv

# Export as SQL
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --format=sql

# Export as ibd (repaired tablespace)
./mysql8_recover --ibd=/path/to/corrupted.ibd --output=/tmp/repaired.ibd --format=ibd
```

## 3. Supported MySQL Versions

| Version | Row Formats | Checksums | SDI | Status |
|---------|-------------|-----------|-----|--------|
| 5.6     | REDUNDANT, COMPACT | INNODB, OLD | None | ✅ Basic Support |
| 5.7     | REDUNDANT, COMPACT | INNODB, CRC32C | None | ✅ Basic Support |
| 8.0     | REDUNDANT, COMPACT, DYNAMIC, COMPRESSED | CRC32C | Supported | ✅ Full Support |
| 8.1+    | All 8.0 formats | CRC32C | Supported | ✅ Full Support |
| 9.0+    | All 8.0 formats | CRC32C | Supported | 🔄 Future Support |

## 4. Supported Disaster Scenarios

| Scenario | Recovery Capability | Notes |
|----------|---------------------|-------|
| Truncate | ✅ Full Recovery | Based on page chain and Redo Log |
| rm -rf | ✅ Partial Recovery | Requires uncovered data pages |
| File header corruption | ✅ Repair | Rebuild file header |
| Tablespace corruption | ✅ Partial Recovery | Based on available pages |
| Redo Log application | ✅ Full Recovery | Forward recovery |
| BLOB corruption | ✅ Partial Recovery | Based on page chain |

## 5. Output Formats

### 5.1 CSV Format

```csv
id,name,email,created_at
1,John Doe,john@example.com,2024-01-01 10:00:00
2,Jane Smith,jane@example.com,2024-01-02 11:30:00
```

### 5.2 SQL Format

```sql
-- Recovered from table: users
-- Engine: InnoDB
-- Row Format: DYNAMIC

CREATE TABLE `users` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL,
  `email` varchar(255) DEFAULT NULL,
  `created_at` datetime DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

INSERT INTO `users` (`id`, `name`, `email`, `created_at`) VALUES
(1, 'John Doe', 'john@example.com', '2024-01-01 10:00:00'),
(2, 'Jane Smith', 'jane@example.com', '2024-01-02 11:30:00');
```

### 5.3 IBD Format

Repaired tablespace file, can be used directly for MySQL recovery.

## 6. Common Issues

### 6.1 Cannot Read File

**Problem**: `[ERROR] Cannot open file: /path/to/file.ibd`

**Solution**:
1. Check file permissions: `ls -l /path/to/file.ibd`
2. Confirm file exists: `file /path/to/file.ibd`
3. Check if file is being used by another process

### 6.2 Checksum Validation Failed

**Problem**: `[WARN] Page checksum validation failed`

**Solution**:
1. Use `--no-checksum` option to skip checksum validation
2. Check if file is corrupted
3. Try recovering from backup

### 6.3 SDI Parsing Failed

**Problem**: `[WARN] Failed to parse SDI JSON`

**Solution**:
1. Check if file is a valid InnoDB tablespace
2. Try extracting SDI from ibdata file
3. Use `--force` option for forced recovery

### 6.4 BLOB Recovery Failed

**Problem**: `[WARN] Failed to reconstruct BLOB data`

**Solution**:
1. Check if BLOB page chain is complete
2. Confirm all BLOB pages are accessible
3. Try using `--partial-blob` option for partial recovery

## 7. Advanced Options

### 7.1 Debug Mode

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --debug
```

### 7.2 Verbose Logging

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --verbose
```

### 7.3 Custom Page Size

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --page-size=8192
```

Supported page sizes: 4096, 8192, 16384, 32768, 65536

### 7.4 Specify Row Format

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --row-format=compact
```

Supported row formats: compact, dynamic, redundant, compressed

## 8. Performance Optimization

### 8.1 Multi-threading

```bash
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --threads=4
```

### 8.2 Memory Mapping

Memory mapping is enabled by default for better performance.

### 8.3 Cache Optimization

Page caching is used to reduce disk I/O.

## 9. Contribution Guidelines

### 9.1 Code Style

- Follow Google C++ Style Guide
- Use clang-format for formatting
- Add unit tests

### 9.2 Commit Convention

```
type(scope): description

body

footer
```

Types: feat, fix, docs, style, refactor, test, chore

### 9.3 Testing

```bash
# Run all tests
ctest --verbose

# Run specific test
./build/tests/test_page_header

# Generate test data
python3 tools/ibd_generator.py test_table
```

## 10. License

MIT License
