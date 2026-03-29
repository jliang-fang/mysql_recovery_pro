# MySQL 8.x Data Recovery Engine - User Guide (English)

**Version**: v1.0.0  
**Author**: jliang.fang  
**Date**: 2026-03-29

---

## 1. Overview

MySQL 8.x Data Recovery Engine is a high-performance offline MySQL data recovery tool supporting deep data recovery for MySQL 5.6/5.7/8.0/8.1/8.2/8.3/8.4/9.0.

### 1.1 Core Features

- **InnoDB Page Parsing**: Full support for COMPACT/REDUNDANT/DYNAMIC/COMPRESSED row formats
- **B+Tree Traversal**: Recursive index traversal with page chain validation
- **BLOB Recovery**: Multi-page BLOB reconstruction with checksum validation
- **Redo Log Replay**: Redo log parsing and replay for all InnoDB row formats
- **Undo Log Parsing**: Transaction rollback support
- **SDI Metadata Parsing**: Space Descriptor Index parsing and table structure reconstruction
- **Cross-platform**: Linux/Windows unified API
- **CLI/GUI**: Command-line tool and graphical interface

### 1.2 Supported Disaster Scenarios

| Scenario | Recovery Capability | Notes |
|----------|---------------------|-------|
| Truncate | ✅ Full Recovery | Based on page chain and Redo Log |
| rm -rf | ✅ Partial Recovery | Requires non-overwritten data pages |
| File Header Corruption | ✅ Repair | Rebuild file header |
| Tablespace Corruption | ✅ Partial Recovery | Based on available pages |
| Redo Log Application | ✅ Full Recovery | Forward recovery |
| BLOB Corruption | ✅ Partial Recovery | Based on page chain |

---

## 2. Quick Start

### 2.1 Build

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

### 2.2 Command Line Tool

```bash
# Recover table data
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered

# Specify table ID
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --table-id=12345

# Truncate recovery
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --truncate-recovery

# rm -rf recovery
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --rmrf-recovery

# Show help
./mysql8_recover --help
```

### 2.3 GUI Tool

```bash
# Launch GUI
cd build/gui
./mysql8_recovery_gui
```

---

## 3. Command Line Arguments

### 3.1 Required Arguments

| Argument | Description | Example |
|----------|-------------|---------|
| `--ibd=<path>` | Path to .ibd file | `--ibd=/var/lib/mysql/test/table.ibd` |

### 3.2 Optional Arguments

| Argument | Description | Default | Example |
|----------|-------------|---------|---------|
| `--output=<dir>` | Output directory | `/tmp/recovered` | `--output=/data/recovered` |
| `--table-id=<id>` | Table ID for SDI lookup | 0 | `--table-id=12345` |
| `--page-size=<size>` | Page size: 4096/8192/16384/32768/65536 | 16384 | `--page-size=8192` |
| `--check-headers` | Check page headers only | false | `--check-headers` |
| `--truncate-recovery` | Truncate recovery mode | false | `--truncate-recovery` |
| `--rmrf-recovery` | rm -rf recovery mode | false | `--rmrf-recovery` |
| `--blob-recovery` | Enable BLOB recovery | false | `--blob-recovery` |
| `--format=<csv\|sql\|ibd>` | Output format | csv | `--format=sql` |

### 3.3 Examples

```bash
# Basic recovery
./mysql8_recover --ibd=/var/lib/mysql/test/users.ibd --output=/tmp/users_recovered

# Truncate recovery
./mysql8_recover --ibd=/var/lib/mysql/test/orders.ibd --output=/tmp/orders_recovered --truncate-recovery --format=sql

# rm -rf recovery
./mysql8_recover --ibd=/var/lib/mysql/test/products.ibd --output=/tmp/products_recovered --rmrf-recovery --format=csv

# Specify page size (for non-standard page sizes)
./mysql8_recover --ibd=/var/lib/mysql/test/large.ibd --output=/tmp/large_recovered --page-size=32768

# Check headers only
./mysql8_recover --ibd=/var/lib/mysql/test/check.ibd --check-headers
```

---

## 4. Graphical Interface

### 4.1 Interface Layout

```
┌─────────────────────────────────────────────────────────────┐
│  MySQL 8.x Recovery Engine v1.0.0                          │
├─────────────────────────────────────────────────────────────┤
│  [File] [Recovery] [Export] [View] [Help]                  │
├─────────────────────────────────────────────────────────────┤
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Recovery Options                                     │  │
│  │  ┌─────────────────────────────────────────────────┐  │  │
│  │  │ Row Format: [Auto Detect ▼]                    │  │  │
│  │  │ Page Size: [16384]                              │  │  │
│  │  │ [✓] Validate Checksums                          │  │  │
│  │  │ [✓] Enable BLOB Recovery                        │  │  │
│  │  │ Output Path: [/tmp/recovered] [Browse]         │  │  │
│  │  └─────────────────────────────────────────────────┘  │  │
│  │  [Start Recovery]                                     │  │
│  └───────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Pages | Records | Log                                │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 Features

#### 4.2.1 Recovery Tab

- **Row Format**: Select row format (Auto Detect auto-detects)
- **Page Size**: Set page size (default 16384)
- **Validate Checksums**: Enable checksum validation
- **Enable BLOB Recovery**: Enable BLOB recovery
- **Output Path**: Set output directory

#### 4.2.2 Pages Tab

- Display list of all pages
- Show page type, record count, status
- Support page-level preview

#### 4.2.3 Records Tab

- Display parsed records
- Support record-level preview
- Support export

#### 4.2.4 Log Tab

- Display operation logs
- Support log search
- Support log export

---

## 5. Data Export

### 5.1 CSV Export

```bash
# Command line
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --format=csv

# GUI
1. Open GUI
2. Select Recovery tab
3. Click "Start Recovery"
4. View export results in Log tab
```

### 5.2 SQL Export

```bash
# Command line
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --format=sql

# GUI
1. Open GUI
2. Select Recovery tab
3. Click "Start Recovery"
4. View export results in Log tab
```

### 5.3 IBD Export

```bash
# Command line
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/repaired --format=ibd

# GUI
1. Open GUI
2. Select Recovery tab
3. Click "Start Recovery"
4. View export results in Log tab
```

---

## 6. Disaster Recovery Scenarios

### 6.1 Truncate Recovery

#### 6.1.1 Scenario Description

When `TRUNCATE TABLE` command is executed, data pages are marked as unallocated, but actual data remains on disk.

#### 6.1.2 Recovery Steps

1. **Stop database writes immediately**
2. **Backup current disk state**
3. **Use recovery tool to scan .ibd file**
4. **Parse page chain and extract records**
5. **Export as CSV/SQL/IBD**

#### 6.1.3 Command Line Example

```bash
./mysql8_recover --ibd=/var/lib/mysql/test/users.ibd \
    --output=/tmp/users_recovered \
    --truncate-recovery \
    --format=sql
```

### 6.2 rm -rf Recovery

#### 6.2.1 Scenario Description

When `rm -rf *.ibd` is executed, file system entries are deleted, but data may still exist on disk.

#### 6.2.2 Recovery Steps

1. **Stop disk writes immediately**
2. **Use disk scanning tool to recover files**
3. **Or use recovery tool to scan disk directly**
4. **Identify InnoDB page structure**
5. **Extract table structure and data**

#### 6.2.3 Command Line Example

```bash
./mysql8_recover --ibd=/var/lib/mysql/test/users.ibd \
    --output=/tmp/users_recovered \
    --rmrf-recovery \
    --format=csv
```

### 6.3 File Header Corruption Recovery

#### 6.3.1 Scenario Description

File header corruption may be caused by disk bad sectors, file system errors, forced power-off, etc.

#### 6.3.2 Recovery Steps

1. **Read corrupted .ibd file**
2. **Identify corrupted page headers**
3. **Repair page type, checksum, etc.**
4. **Rebuild file header**
5. **Export repaired file**

#### 6.3.3 Command Line Example

```bash
./mysql8_recover --ibd=/var/lib/mysql/test/users.ibd \
    --output=/tmp/users_repaired \
    --format=ibd
```

---

## 7. Advanced Features

### 7.1 SDI Metadata Parsing

SDI (Space Descriptor Index) stores table metadata including:

- Table name
- Column definitions
- Index definitions
- Partition information

### 7.2 BLOB Recovery

BLOB data is stored in independent page chains. The recovery tool supports:

- Multi-page BLOB reconstruction
- Page chain integrity validation
- Circular reference detection

### 7.3 Redo Log Replay

Redo Log records all data modification operations. The recovery tool supports:

- Redo Log parsing
- Forward recovery
- Transaction rollback

---

## 8. Performance Optimization

### 8.1 Memory Mapping

Use mmap for file mapping to avoid additional memory copies.

### 8.2 CRC32C Lookup Table

Use compile-time generated CRC32C table for zero runtime overhead.

### 8.3 Zero-Copy View

Use `std::span` to provide zero-copy page data view.

---

## 9. Troubleshooting

### 9.1 Common Issues

#### 9.1.1 Cannot Open File

**Error**: `Failed to open file: /path/to/file.ibd`

**Solution**:
1. Check file path is correct
2. Check file permissions
3. Check file is not locked by another process

#### 9.1.2 Parse Failure

**Error**: `Failed to parse page 0`

**Solution**:
1. Check page size is correct
2. Check row format is correct
3. Check file is not corrupted

#### 9.1.3 Low Recovery Rate

**Issue**: Recovered record count is much lower than expected

**Solution**:
1. Check if data has been overwritten
2. Check if page chain is complete
3. Try different row formats

### 9.2 Log Analysis

Log files are located at `recovery.log` in the output directory, containing:

- Operation steps
- Error messages
- Performance statistics

---

## 10. Best Practices

### 10.1 Disaster Recovery

1. **Stop writes immediately**: Stop database writes immediately after discovering accidental operation
2. **Backup current state**: Backup current disk state first
3. **Use professional tools**: Use verified recovery tools
4. **Test recovery**: Verify recovery results in test environment
5. **Validate data**: Verify data integrity after recovery

### 10.2 Performance Optimization

1. **Use SSD**: SSD provides faster I/O performance
2. **Increase memory**: More memory reduces disk I/O
3. **Parallel processing**: Use multi-threading to accelerate recovery

---

## 11. Technical Support

### 11.1 Documentation

- [Architecture Design](./architecture.md)
- [Principle Documents](./principle_*.md)
- [Test Strategy](./test_strategy.md)

### 11.2 Community

- GitHub Issues: https://github.com/your-org/mysql8_recovery/issues
- Email: support@mysql8-recovery.com

---

## 12. License

MIT License

---

## 13. Author

**jliang.fang** - Chief Database Kernel Architect

---

<div align="center">
  <p>Building the world's leading MySQL data recovery tool</p>
</div>
