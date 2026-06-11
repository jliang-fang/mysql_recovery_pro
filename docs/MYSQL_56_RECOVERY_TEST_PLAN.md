# MySQL 5.6 海量数据灾难恢复专项测试计划书
# MySQL 5.6 Massive Data Disaster Recovery Special Test Plan

**Version:** 1.0  
**Author:** jliang.fang  
**Date:** 2026-04-06  
**Target:** MySQL 5.6 Recovery Migration (500万+ Records Scale)  
**Status:** Draft for Review  

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Core Benchmarks](#core-benchmarks)
3. [Scenario → Corruption → Parameters → Expected Results Mapping](#scenario--corruption--parameters--expected-results-mapping)
4. [Test Suite Architecture](#test-suite-architecture)
5. [Environment Setup](#environment-setup)
6. [Corruption Scenarios](#corruption-scenarios)
7. [Recovery Command Reference](#recovery-command-reference)
8. [Performance Optimization Strategy](#performance-optimization-strategy)
9. [Appendix](#appendix)

---

## Executive Summary

### Objective
- Migrate MySQL 8.0 recovery capabilities to MySQL 5.6
- Achieve **zero data loss +极速 extraction** for 500万+ (5 million+) records
- Ensure **99.9%+ recovery rate** across all corruption scenarios

### Key Achievements (MySQL 8.0 Baseline)
From `/app/mysql_recovery_pro/docs/FINAL_RECOVERY_TEST_REPORT.md`:
- ✅ **5/5 Tests Passed** (100% pass rate)
- ✅ **99.97%+ Recovery Rate** for normal files
- ✅ **Header Repair**: 102.27% (batch page repair)
- ✅ **rm-rf Recovery**: 99.99% (sequential scan)
- ✅ **BLOB Recovery**: 100.43%

### MySQL 5.6 vs MySQL 8.0 Architectural Gap

| Dimension | MySQL 5.6 | MySQL 8.0 | Migration Challenge |
|-----------|-----------|-----------|---------------------|
| **Metadata Storage** | .frm files | Data Dictionary | Heuristic scan needed |
| **Page Format** | Antelope (REDUNDANT/COMPACT) | Barracuda (COMPRESSED/DYNAMIC) | Different record layouts |
| **SDI** | Not available | Binary SDI in .ibd | No SDI parsing needed |
| **Row Format** | Compact/Redundant | Dynamic/Compressed | Record header parsing differs |
| **B+Tree** | Leaf pages store record data | Index pages store SDI pointers | Direct record extraction |

### Test Scale
- **Target**: 5,000,000+ records per test
- **Page Count**: ~30,000+ pages (16KB page size)
- **File Size**: ~500MB+ per test file
- **Memory Constraint**: ≤500MB peak usage

---

## Core Benchmarks

### Performance Metrics (500万+ Records)

| Metric | Baseline | Target | Monitoring |
|--------|----------|--------|------------|
| **Parse Throughput** | ≥500 Pages/s | ≥2000 Pages/s | `perf stat`, Valgrind |
| **Row Extraction Rate** | ≥50K Rows/s | ≥200K Rows/s | Count output lines |
| **Peak Memory Usage** | ≤2GB | ≤500MB | `ps aux`, Valgrind |
| **Fragment Recovery Rate** | ≥70% | ≥95% | CRC32 validation |
| **Page Repair Success** | ≥85% | ≥98% | Header校验 |
| **BLOB Chain Reconnection** | ≥60% | ≥90% | Content hash match |

### MySQL 5.6 InnoDB Page Parameters

| Parameter | 5.6 Default | Tuning Options |
|-----------|-------------|----------------|
| **page_size** | 16384 (16KB) | 4096, 8192, 16384, 32768, 65536 |
| **FIL_HEADER** | 38 bytes | Fixed offset for page定位 |
| **FIL_TRAILER** | 8 bytes | At `page_size - 8` |
| **INFIMUM/SUPERMAX** | Fixed records | Page boundary detection |
| **ROW_FORMAT** | REDUNDANT/COMPACT | Power recovery scan |
| **page_type** | 0x45BC (B+Tree) | heuristic page detection |

### Recovery Modes Status

| Mode | Status | 5.6 Compatibility |
|------|--------|-------------------|
| `--ibd` | ✅ Production | Full support |
| `--header-repair` | ✅ Test Passed |需调整页类型检测 |
| `--rmrf-recovery` | ✅ Test Passed | Full support |
| `--blob-recovery` | ✅ Test Passed |需处理5.6 BLOB页格式 |
| `--truncate-recovery` | ✅ Test Passed | Full support |
| `--scan-dir` | ⚠️ In Progress | Heuristic table discovery |
| `--rebuild-chain` | ⚠️ In Progress | B+tree structural recovery |
| `--search` | ⚠️ In Progress | Schema-less recovery |

---

## Scenario → Corruption → Parameters → Expected Results Mapping

### A. Data Integrity & Scale Pressure Testing

| # | Scenario | Corruption Method | Command | Expected Result | Validation |
|---|----------|-------------------|---------|-----------------|------------|
| A1 | 海量提取基准 (Baseline Extraction) | No corruption, normal operation | `mysql_recovery_pro --ibd=test.ibd -o csv` | Completeness ≥99.999% | CRC32 hash match |
| A2 | 内存泄漏检测 (Memory Leak) | N/A (stress test) | `valgrind --leak-check=full mysql_recovery_pro --ibd=test.ibd` | 0 OOM, ≤100KB leaks | Valgrind report |
| A3 | 速度优化验证 (Speed) | N/A | `mysql_recovery_pro --ibd=test.ibd --multithread=8 --page-size=16384` | Pages/s ≥2000 | perf stat |

### B. Physical Corruption Chaos

| # | Scenario | Corruption Method | Command | Expected Result | Validation |
|---|----------|-------------------|---------|-----------------|------------|
| B1 | 断电掉页 (Torn Pages) | `dd bs=1 count=40 seek=xxx of=xxx.ibd` | `mysql_recovery_pro --ibd=test.ibd --header-repair` | Page header repair ≥98% | Page header校验 |
| B2 | 文件碎片化 (rm-rf Recovery) | `dd if=/dev/urandom of=xxx.ibd bs=1M count=10` | `mysql_recovery_pro --ibd=test.ibd --rmrf-recovery` | Fragment recovery ≥95% | Offset mapping comparison |
| B3 | BLOB断链 (BLOB Chain Break) | `dd seek=mid_page bs=1 count=1024 of=xxx.ibd` | `mysql_recovery_pro --ibd=test.ibd --blob-recovery` | BLOB chain reconnection ≥90% | BLOB content hash |

### C. Metadata Loss Scenarios

| # | Scenario | Corruption Method | Command | Expected Result | Validation |
|---|----------|-------------------|---------|-----------------|------------|
| C1 | .frm消失挑战 (Missing .frm Files) | `rm -f *.frm` | `mysql_recovery_pro --scan-dir=/var/lib/mysql/testdb --search --table-name=test_table` | Schema recognition ≥80% | Schema diff comparison |
| C2 | B+树断裂 (B+Tree Chain Break) | `dd`破坏non-leaf nodes | `mysql_recovery_pro --ibd=test.ibd --rebuild-chain` | Tree rebuild success ≥95% | Page chain integrity |

### D. Extreme Scenarios

| # | Scenario | Corruption Method | Command | Expected Result | Validation |
|---|----------|-------------------|---------|-----------------|------------|
| D1 | Truncate救援 (Truncate Recovery) | `TRUNCATE`后立即备份 | `mysql_recovery_pro --ibd=test.ibd --truncate-recovery` | Zombie record extraction ≥85% | InnoDB header bit check |
| D2 | 多版本兼容 (Multi-Version) | 5.6.10/5.6.20/5.6.45 | `mysql_recovery_pro --ibd=test.ibd --mysql-version=5.6` | Backward compatibility ≥95% | Version detection logic |

---

## Test Suite Architecture

### Directory Structure

```
/app/mysql_recovery_pro/
├── build/
│   └── mysql_recovery_pro          # C++ recovery engine
├── docker/
│   ├── docker-compose.yml          # Multi-version MySQL test environments
│   └── mysql.d/5.6/                # MySQL 5.6 configuration
├── scripts/
│   ├── chaos_injection_suite.py    # Corruption injection suite
│   └── generate_5m_golden_data.py  # Test data generator
├── test_scripts/
│   ├── schema_50.sql               # MySQL 5.6 schema
│   └── chaos_injector.py           # Data injection into MySQL
├── golden-hashes/
│   └── hashes_56.json              # Golden hash file for validation
├── chaos-test-volumes/             # Docker volume mount point
│   └── testdb/
│       ├── test_table.ibd          # Target .ibd file
│       └── test_table.frm          # Schema (can be deleted for test)
└── recovery-output/                # Recovery results
```

### Test Pipeline Flow

```
┌─────────────────────────────────────────────────────────────────┐
│  1. Environment Setup (Docker)                                   │
│     - Start MySQL 5.6 container                                  │
│     - Mount /var/lib/mysql                                       │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  2. Data Injection (5M+ Records)                                │
│     - Create test schema                                         │
│     - Generate 5,000,000 records                                 │
│     - Calculate golden hashes                                    │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  3. Baseline Extraction (Clean State)                           │
│     - Extract records before corruption                          │
│     - Store as reference                                         │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  4. Corruption Injection (Chaos Monkey)                         │
│     - Torn pages, BLOB breaks, fragment damage, etc.            │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  5. Recovery Execution                                          │
│     - Run mysql_recovery_pro with appropriate parameters         │
│     - Use Valgrind for memory leak detection                     │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  6. Result Validation                                           │
│     - Compare recovered records vs golden hashes                 │
│     - Calculate recovery rate                                    │
│     - Generate detailed report                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## Environment Setup

### 5.6 Docker Startup Script
### 5.6 Docker Startup Script

```bash
#!/bin/bash
# Start MySQL 5.6 Environment
# See: /app/mysql_recovery_pro/scripts/start_mysql56_env.sh

# Usage:
#   bash scripts/start_mysql56_env.sh

# The script:
# 1. Creates necessary host directories
# 2. Sets up MySQL 5.6 configuration
# 3. Starts Docker container with proper mappings
# 4. Waits for MySQL to be ready
```

**Full script location:** `/app/mysql_recovery_pro/scripts/start_mysql56_env.sh`

---

## Corruption Scenarios

### Python-based Corruption Suite

**Script:** `/app/mysql_recovery_pro/scripts/corruption_scenarios_mysql56.py`

```bash
# Run all corruption scenarios on an IBD file
python3 scripts/corruption_scenarios_mysql56.py \
    /var/lib/mysql/testdb/test_table.ibd \
    --output-dir=./corruption_output \
    --scenario=all \
    --seed=42
```

**Available Scenarios:**
- `torn` - Torn pages (power loss simulation)
- `fragmented` - File fragmentation (rm -rf)
- `blob` - BLOB chain breakage
- `btree` - B+Tree pointer corruption
- `mismatch` - Page number mismatches
- `checksum` - Checksum corruption
- `random` - Random byte corruption
- `truncate` - File truncation
- `dd` - dd-style overwrites

### Bash/dd Corruption Script

**Script:** `/app/mysql_recovery_pro/scripts/dd_corruption_mysql56.sh`

```bash
#可用命令:
# Torn pages corruption
bash scripts/dd_corruption_mysql56.sh torn test.ibd 100

# Arbitrary dd-style corruption
bash scripts/dd_corruption_mysql56.sh dd test.ibd 5 0 40

# File truncation
bash scripts/dd_corruption_mysql56.sh truncate test.ibd 10

# B+Tree pointer corruption
bash scripts/dd_corruption_mysql56.sh btree test.ibd 50

# Full test suite
bash scripts/dd_corruption_mysql56.sh all test.ibd
```

---

## Recovery Command Reference

### Basic Recovery Commands

```bash
# Extract records to CSV (default mode)
./build/mysql_recovery_pro \
    --ibd=/var/lib/mysql/testdb/test_table.ibd \
    --page-size=16384 \
    --out-format=csv \
    -o /recovery/output.csv

# Recover with header repair
./build/mysql_recovery_pro \
    --ibd=corrupted.ibd \
    --header-repair \
    --out-format=sql \
    -o /recovery/output.sql

# Recover fragmented files
./build/mysql_recovery_pro \
    --ibd=fragmented.ibd \
    --rmrf-recovery \
    --out-format=json \
    --json-lines \
    -o /recovery/output.json
```

### Complete Recovery Pipeline

```bash
# 1. Scan page information first
./build/mysql_recovery_pro \
    --ibd=test.ibd \
    --scan

# 2. If header is corrupted, repair first
./build/mysql_recovery_pro \
    --ibd=test.ibd \
    --header-repair \
    --out-format=csv \
    -o /tmp/repair.csv

# 3. Extract final data
./build/mysql_recovery_pro \
    --ibd=/tmp/repair.csv.ibd \
    --out-format=csv \
    -o /recovery/final.csv
```

---

## Performance Optimization Strategy

### C++ Engine Optimizations

1. **Memory-Mapped I/O (mmap)**
   - Eliminates buffer copies
   - Uses `madvise()` for sequential hinting
   - Expected: 2-3x speedup

2. **Multi-threaded Page Processing**
   - Uses worker pool
   - Configurable via `--multithread=N`
   - Expected: Linear speedup up to 4 cores

3. **Batch Processing**
   - Processes records in batches of 100-1000
   - Reduces function call overhead
   - Expected: 1.5-2x speedup

4. **Streaming Output**
   - Writes directly without buffering
   - Reduces memory usage by 90%+
   - Critical for 5M+ records

### CLI-Level Optimizations

```bash
# Optimized command for 5M+ records
./build/mysql_recovery_pro \
    --ibd=test.ibd \
    --page-size=16384 \
    --multithread=4 \
    --page-batch=1000 \
    --stream-threshold=100000 \
    --out-format=csv \
    -o output.csv
```

### Performance Targets

| Metric | Baseline | Optimized |
|--------|----------|-----------|
| Pages/s | 500 | 2000+ |
| Rows/s | 50K | 200K+ |
| Memory | 2GB | 500MB |

---

## Test Results Dashboard

### Expected Recovery Rates by Scenario

| Scenario | Expected Rate | Method |
|----------|---------------|--------|
| Normal (no corruption) | 100.00% | Direct extraction |
| Torn pages | 99.90% | --header-repair |
| rm-rf fragmentation | 99.95% | --rmrf-recovery |
| BLOB chain break | 99.50% | --blob-recovery |
| Truncate recovery | 98.50% | --truncate-recovery |
| B+Tree damage | 99.00% | --rebuild-chain |
| Missing .frm | 85.00% | --scan-dir --search |

### Memory Usage Profile

| Records | Default Mode | Streaming Mode |
|---------|--------------|----------------|
| 100K | 150MB | 100MB |
| 1M | 500MB | 200MB |
| 5M | 2000MB+ | 500MB |

---

## Appendix A: File Format Reference

### MySQL 5.6 InnoDB Page Structure

```
Offset  Size  Description
------  ----  -----------
0       4     FIL_PAGE_PREV (previous page number)
4       4     FIL_PAGE_NEXT (next page number)
8       8     FIL_PAGE_LSN (log sequence number)
16      4     FIL_PAGE_SPACE_ID (tablespace ID)
16      2     FIL_PAGE_TYPE (page type - overlaps with space_id)
20      4     FIL_PAGE_OFFSET (page number)
24      8     FIL_PAGE_CHECKSUM (double-write checksum)
36      4     FIL_PAGE_ARCH_LOG_NO
38      N     Page Header (varies by page type)
56      10    BTree Header
66      N     Record Data (variable)
```

### Page Types (MySQL 5.6)

| Type (hex) | Type Name | Description |
|------------|-----------|-------------|
| 0x0000 | ALLOCATED | Unused page |
| 0x45BC | INDEX | B+Tree index page (Compact) |
| 0x45BD | INDEX | B+Tree index page (Alternate) |
| 0x26F1 | INDEX | B+Tree index page (New format) |
| 0x26F2 | UNDO_LOG | Undo log page |
| 0x26F4 | BLOB | BLOB page |

---

## Appendix B: Command-Line Options

### Core Options

| Option | Default | Description |
|--------|---------|-------------|
| `--ibd` | N/A | Input IBD file path |
| `--scan-dir` | N/A | Scan entire data directory |
| `--search` | false | Search for tables by name |
| `--page-size` | 16384 | Page size in bytes |
| `--page-batch` | 100 | Pages per batch |
| `--multithread` | 1 | Number of threads |
| `--out-format` | csv | Output format (csv/sql/json) |
| `-o` | stdout | Output file path |

### Recovery Modes

| Option | Description |
|--------|-------------|
| `--default-recovery` | Default truncate recovery |
| `--truncate-recovery` | Recover from TRUNCATE |
| `--rmrf-recovery` | Recover from rm -rf |
| `--header-repair` | Repair corrupted headers |
| `--blob-recovery` | Enable BLOB recovery |
| `--rebuild-chain` | Rebuild B+Tree chain |

### Validation Options

| Option | Description |
|--------|-------------|
| `--validate-crc32` | Validate against golden hashes |
| `--max-errors N` | Stop after N errors |
| `--no-checksum` | Skip checksum validation |

---

## Appendix C: Recovery Verification

### Verify Recovery Completeness

```bash
# Count records
wc -l recovered.csv

# Calculate checksum
md5sum recovered.csv

# Compare with original hash
diff <(md5sum original.csv) <(md5sum recovered.csv)
```

### Valgrind Memory Check

```bash
valgrind --leak-check=full \
    --show-leak-kinds=all \
    --track-origins=yes \
    ./build/mysql_recovery_pro \
    --ibd=test.ibd
```

---

## Appendix D: Troubleshooting

### Common Issues

1. **"Invalid page size"**
   ```bash
   # Try different page sizes
   --page-size=4096 --page-size=8192 --page-size=32768
   ```

2. **"No records found"**
   ```bash
   # Use rmrf-recovery mode
   --rmrf-recovery
   
   # Or disable validation
   --no-validation
   ```

3. **Out of Memory**
   ```bash
   # Use streaming mode
   --stream-threshold=10000
   
   # Use fewer threads
   --multithread=1
   ```

---

## Appendix E: Integration with MySQL

### Import Recovered Data

```bash
# Recover to SQL
./build/mysql_recovery_pro \
    --ibd=test.ibd \
    --out-format=sql \
    -o recovered.sql

# Import to MySQL
mysql -u root -p testdb < recovered.sql

# Or use CSV with LOAD DATA
./build/mysql_recovery_pro \
    --ibd=test.ibd \
    --out-format=csv \
    -o recovered.csv

mysql -u root -p -e \
    "LOAD DATA INFILE '/path/recovered.csv' INTO TABLE test_table"
```

---

## Appendix F: Performance Benchmark Commands

### Speed Test

```bash
time ./build/mysql_recovery_pro \
    --ibd=test.ibd \
    --page-size=16384 \
    --multithread=4 \
    --out-format=csv \
    -o output.csv

# Expected: ~2000 Pages/s, ~200K Rows/s
```

### Memory Test

```bash
valgrind --leak-check=full \
    ./build/mysql_recovery_pro \
    --ibd=test.ibd \
    --out-format=csv \
    -o output.csv

# Expected: No leaks, <500MB peak
```

---

**End of Test Plan Appendix**
