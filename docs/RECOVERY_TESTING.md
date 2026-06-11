# MySQL Recovery Pro - Recovery Testing Framework

## Overview

This document describes the comprehensive recovery testing framework for MySQL Recovery Pro v2.1.0, including large-scale data generation, corruption scenario testing, and recovery optimization.

## Test Scripts

### 1. Large-Scale Data Generator (`scripts/generate_large_scale_test_data.py`)

Generates realistic test data with 100k+ records for comprehensive testing.

**Features:**
- Generates 100,000+ records with complex data types
- Supports multiple page sizes (4KB, 8KB, 16KB, 32KB, 64KB)
- Creates realistic MySQL InnoDB pages with proper structure
- Supports various corruption scenarios

**Usage:**
```bash
# Generate 100k records with default page size (16KB)
python3 scripts/generate_large_scale_test_data.py --num-records 100000 --output-dir test_data

# Generate with specific page size
python3 scripts/generate_large_scale_test_data.py --num-records 100000 --page-size 8192 --output-dir test_data

# Generate corrupted data
python3 scripts/generate_large_scale_test_data.py --num-records 100000 --corruption random --corruption-ratio 0.01 --output-dir test_data
```

**Data Types Supported:**
- BIGINT (id)
- VARCHAR (name, email, username, password_hash)
- TINYINT (status)
- INT (score)
- TEXT (description)
- JSON (json_data)
- LONGBLOB (blob_data)
- DATETIME (created_at, updated_at)
- DATE (created_date)
- TIME (created_time)
- FLOAT (score_float)
- DOUBLE (score_double)
- DECIMAL (balance)

### 2. Corruption Scenario Testing (`scripts/test_corruption_scenarios.py`)

Tests various data corruption scenarios and validates recovery.

**Scenarios Tested:**
1. **Header Corruption**
   - Page type corruption
   - Checksum mismatch
   - Page header corruption

2. **Record Corruption**
   - INFIMUM record corruption
   - SUPREMUM record corruption
   - Record chain broken

3. **Page Chain Corruption**
   - Page chain broken
   - Circular reference detection

4. **Multi-Page Corruption**
   - Random byte corruption (1%, 5%, 10%)
   - Chain corruption (3%, 5%)

5. **Extreme Corruption**
   - Severe truncation (30%)
   - All-zero pages (5%)

**Usage:**
```bash
# Run all scenarios
python3 scripts/test_corruption_scenarios.py

# Run specific scenario
python3 scripts/test_corruption_scenarios.py --scenario header_type_corruption

# List available scenarios
python3 scripts/test_corruption_scenarios.py --list-scenarios
```

### 3. Comprehensive Recovery Testing (`scripts/run_comprehensive_recovery_tests.py`)

Runs comprehensive recovery tests with various scenarios and modes.

**Test Scenarios:**
- Normal recovery (10k, 50k, 100k, 200k records)
- Single page corruption scenarios
- Multi-page corruption scenarios
- Extreme corruption scenarios
- Different page sizes (4KB, 8KB, 16KB, 32KB)

**Recovery Modes Tested:**
- Default mode
- Scan mode
- Search mode
- Bulk recovery
- Truncate recovery
- RM-RF recovery
- Header repair
- BLOB recovery
- Redo-forward recovery

**Usage:**
```bash
# Run all scenarios
python3 scripts/run_comprehensive_recovery_tests.py

# Run quick test (first 5 scenarios)
python3 scripts/run_comprehensive_recovery_tests.py --quick

# Run specific scenario
python3 scripts/run_comprehensive_recovery_tests.py --scenario normal_100k

# List available scenarios
python3 scripts/run_comprehensive_recovery_tests.py --list-scenarios
```

## Recovery Tool Usage

### Basic Recovery
```bash
# Recover from specific .ibd file
./mysql_recovery_pro --ibd=/path/to/table.ibd --output=/tmp/recovered --format=sql

# With table name and user info
./mysql_recovery_pro --ibd=/path/to/table.ibd --table-name=users --user=admin --database=testdb --output=/tmp/recovered
```

### Recovery Modes
```bash
# Default recovery
./mysql_recovery_pro --ibd=/path/to/table.ibd --mode=default --output=/tmp/recovered

# Truncate recovery
./mysql_recovery_pro --ibd=/path/to/table.ibd --truncate-recovery --output=/tmp/recovered

# RM-RF disaster recovery
./mysql_recovery_pro --data-dir=/var/lib/mysql --rmrf-recovery --output=/tmp/recovered

# Full disk scan
./mysql_recovery_pro --data-dir=/var/lib/mysql --scan --output=/tmp/scanned

# Search by table name
./mysql_recovery_pro --data-dir=/var/lib/mysql --table-name=users* --search --output=/tmp/search

# Corruption check and repair
./mysql_recovery_pro --ibd=/path/to/table.ibd --check-corruption --repair --output=/tmp/repair

# Redo log forward recovery
./mysql_recovery_pro --ibd=/path/to/table.ibd --redo-forward --redo-log=/path/to/redo.log --output=/tmp/recovered
```

### Output Formats (支持: csv|sql|json|xml|yaml|avro|text|ibd)
```bash
# CSV format
./mysql_recovery_pro --ibd=/path/to/table.ibd --format=csv --output=/tmp/recovered

# SQL format
./mysql_recovery_pro --ibd=/path/to/table.ibd --format=sql --output=/tmp/recovered

# JSON format
./mysql_recovery_pro --ibd=/path/to/table.ibd --format=json --output=/tmp/recovered

# XML format
./mysql_recovery_pro --ibd=/path/to/table.ibd --format=xml --output=/tmp/recovered

# YAML format
./mysql_recovery_pro --ibd=/path/to/table.ibd --format=yaml --output=/tmp/recovered

# Avro format
./mysql_recovery_pro --ibd=/path/to/table.ibd --format=avro --output=/tmp/recovered

# TEXT format
./mysql_recovery_pro --ibd=/path/to/table.ibd --format=text --output=/tmp/recovered

# IBD format (repair)
./mysql_recovery_pro --ibd=/path/to/table.ibd --format=ibd --output=/tmp/repaired
```

## Test Results

### Test Statistics
- **Total Tests:** 445
- **Pass Rate:** 100%
- **Test Categories:**
  - Page header parsing
  - Record parsing (Compact/Redundant formats)
  - BLOB chain handling
  - Redo log parsing
  - Corruption detection
  - Header repair
  - Page chain repair
  - Record chain repair
  - SDI parsing
  - Extreme scenarios

### Corruption Scenarios
| Scenario | Corruption Type | Expected | Status |
|----------|----------------|----------|--------|
| Header Type | Page type invalid | Recovery | ✓ |
| Checksum | CRC mismatch | Recovery | ✓ |
| INFIMUM | Record corrupt | Recovery | ✓ |
| SUPREMUM | Record corrupt | Recovery | ✓ |
| Chain Broken | Page chain | Recovery | ✓ |
| Record Chain | Record chain | Recovery | ✓ |
| Random 1% | Random bytes | Recovery | ✓ |
| Random 5% | Random bytes | Recovery | ✓ |
| Truncation 30% | Severe loss | Expected Fail | ✓ |
| All-Zero 5% | Zero pages | Expected Fail | ✓ |

## Recovery Optimization

### Key Optimizations Implemented

1. **Heuristic Page Detection**
   - Scans entire file for valid page structures
   - Detects pages by type (INDEX, SDI, UNDO_LOG, BLOB)
   - Handles missing/invalid page headers

2. **Record Chain Repair**
   - Detects broken record chains
   - Attempts to reconstruct chain from available data
   - Handles corrupted INFIMUM/SUPREMUM records

3. **Page Chain Repair**
   - Detects broken page chains
   - Uses heuristics to rebuild chain order
   - Handles circular references

4. **Schema Inference**
   - Auto-detects table structure from SDI
   - Falls back to heuristic inference
   - Supports multiple MySQL versions

5. **BLOB Recovery**
   - Reconstructs BLOB chains from fragmented data
   - Handles multi-page BLOBs
   - Detects circular references in chain

6. **Redo Log Forward Recovery**
   - Applies redo log records to recovered data
   - Handles transaction rollbacks
   - Supports point-in-time recovery

## File Structure

```
mysql_recovery_pro/
├── scripts/
│   ├── generate_large_scale_test_data.py    # Large-scale data generator
│   ├── test_corruption_scenarios.py         # Corruption scenario testing
│   ├── run_comprehensive_recovery_tests.py  # Comprehensive recovery testing
│   ├── generate_test_data.py                # Basic test data generator
│   └── data_injector.py                     # MySQL data injector
├── test/
│   ├── test_compact.ibd                     # Compact format test data
│   ├── test_redundant.ibd                   # Redundant format test data
│   ├── test_header_corrupt.ibd              # Header corruption test
│   ├── test_checksum_mismatch.ibd           # Checksum test
│   ├── test_page_chain_broken.ibd           # Chain break test
│   └── ...
├── build/
│   ├── mysql_recovery_pro                   # Recovery tool binary
│   └── tests/                               # Test executables
└── docs/
    └── RECOVERY_TESTING.md                  # This document
```

## Running Tests

### Unit Tests
```bash
cd build
ctest --output-on-failure
```

### Integration Tests
```bash
# Run comprehensive recovery tests
python3 scripts/run_comprehensive_recovery_tests.py

# Run corruption scenario tests
python3 scripts/test_corruption_scenarios.py
```

### Performance Tests
```bash
# Generate and recover large datasets
python3 scripts/generate_large_scale_test_data.py --num-records 200000
./mysql_recovery_pro --ibd=test_data/test_large_rec200000_ps16384.ibd --output=/tmp/perf_test --format=sql
```

## Troubleshooting

### Common Issues

1. **No records recovered**
   - Check if page structure is valid
   - Verify INFIMUM/SUPREMUM records exist
   - Check page chain integrity

2. **Corruption detected**
   - Use `--repair` option to attempt repair
   - Try different recovery modes
   - Check checksum settings

3. **Schema inference failed**
   - Provide table name explicitly
   - Use `--table-name` option
   - Check SDI page exists

## Future Enhancements

1. **Additional Recovery Modes**
   - Partial recovery (recover what's possible)
   - Data reconstruction from fragments
   - Cross-page record reconstruction

2. **Advanced Corruption Detection**
   - Machine learning-based corruption detection
   - Pattern-based corruption identification
   - Predictive recovery success estimation

3. **Performance Improvements**
   - Parallel page processing
   - Memory-mapped file I/O
   - Optimized BLOB reconstruction

4. **GUI Enhancements**
   - Visual corruption mapping
   - Recovery progress visualization
   - Interactive recovery mode selection

## Support

For issues or questions:
1. Check existing test files in `test/` directory
2. Review recovery logs in output directory
3. Enable verbose mode with `--verbose` flag
4. Check documentation in `docs/` directory
