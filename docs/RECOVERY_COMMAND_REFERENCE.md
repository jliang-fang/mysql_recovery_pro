# MySQL 5.6 Recovery Command Reference

**Author:** jliang.fang  
**Version:** 1.0  
**Date:** 2026-04-06

---

## Quick Start

### Basic Recovery

```bash
# Extract records from IBD file to CSV
./build/mysql_recovery_pro --ibd=/var/lib/mysql/testdb/test_table.ibd -o csv

# To SQL format
./build/mysql_recovery_pro --ibd=/var/lib/mysql/testdb/test_table.ibd -o sql

# To JSON format
./build/mysql_recovery_pro --ibd=/var/lib/mysql/testdb/test_table.ibd -o json
```

### Full Recovery Pipeline

```bash
# Step 1: Detect page size and file format
./build/mysql_recovery_pro --ibd=test.ibd --scan

# Step 2: Extract to CSV (default mode)
./build/mysql_recovery_pro --ibd=test.ibd -o csv

# Step 3: Verify recovery rate
wc -l test.csv
```

---

## Recovery Modes

### 1. Default Recovery (Truncate Recovery)

```bash
./build/mysql_recovery_pro --ibd=test.ibd --default-recovery
```

**Use Case:** Table was truncated or records were deleted  
**What it does:** Scans for zombie records in freed pages

### 2. RM-RF Recovery

```bash
./build/mysql_recovery_pro --ibd=test.ibd --rmrf-recovery
```

**Use Case:** File was partially deleted (rm -rf scenario)  
**What it does:** Sequential scan for record patterns

### 3. Header Repair

```bash
./build/mysql_recovery_pro --ibd=test.ibd --header-repair
```

**Use Case:** Page headers corrupted (power loss, incomplete writes)  
**What it does:** Repairs page type, checksums, and chain links

### 4. BLOB Recovery

```bash
./build/mysql_recovery_pro --ibd=test.ibd --blob-recovery
```

**Use Case:** BLOB data was partially corrupted  
**What it does:** Reconstructs BLOB chains from fragmented pages

### 5. B+Tree Rebuild

```bash
./build/mysql_recovery_pro --ibd=test.ibd --rebuild-chain
```

**Use Case:** B+Tree index structure is damaged  
**What it does:** Rebuilds page chain from leaf to root

### 6. Truncate Recovery

```bash
./build/mysql_recovery_pro --ibd=test.ibd --truncate-recovery
```

**Use Case:** Table was truncated using TRUNCATE command  
**What it does:** Finds records marked as deleted in header bits

---

## Scan & Discovery Options

### Scan Page Information

```bash
# Scan and display page information
./build/mysql_recovery_pro --ibd=test.ibd --scan

# Scan with verbose output
./build/mysql_recovery_pro --ibd=test.ibd --scan --verbose
```

### Scan Directory for Tables

```bash
# Scan entire data directory
./build/mysql_recovery_pro --scan-dir=/var/lib/mysql/testdb

# Scan with table name filter
./build/mysql_recovery_pro --scan-dir=/var/lib/mysql --table-name=test_table
```

### Search for Tables by Name

```bash
# Search for a specific table
./build/mysql_recovery_pro --search --table-name=test_table

# Search with path filter
./build/mysql_recovery_pro --search --table-name=test_table --path=/var/lib/mysql
```

### Extract SDI Metadata

```bash
# Extract SDI from MySQL 8.0+ files
./build/mysql_recovery_pro --ibd=test.ibd --extract-sdi

# Output SDI to JSON file
./build/mysql_recovery_pro --ibd=test.ibd --extract-sdi -o sdi.json
```

---

## Output Format Options

### CSV Output

```bash
# Basic CSV
./build/mysql_recovery_pro --ibd=test.ibd --out-format=csv -o output.csv

# CSV with headers
./build/mysql_recovery_pro --ibd=test.ibd --out-format=csv --csv-header -o output.csv

# CSV with custom delimiter
./build/mysql_recovery_pro --ibd=test.ibd --out-format=csv --csv-delimiter=';' -o output.csv
```

### SQL Output

```bash
# SQL INSERT statements
./build/mysql_recovery_pro --ibd=test.ibd --out-format=sql -o output.sql

# SQL with REPLACE INTO
./build/mysql_recovery_pro --ibd=test.ibd --out-format=sql --sql-replace -o output.sql

# SQL batch size (records per INSERT)
./build/mysql_recovery_pro --ibd=test.ibd --out-format=sql --sql-batch=1000 -o output.sql
```

### JSON Output

```bash
# JSON array
./build/mysql_recovery_pro --ibd=test.ibd --out-format=json -o output.json

# JSON lines (one JSON per line)
./build/mysql_recovery_pro --ibd=test.ibd --out-format=json --json-lines -o output.json

# JSON with pretty print
./build/mysql_recovery_pro --ibd=test.ibd --out-format=json --pretty -o output.json
```

---

## Page Size Configuration

### Auto-detect Page Size

```bash
# Automatic page size detection
./build/mysql_recovery_pro --ibd=test.ibd --page-size=auto
```

### Manual Page Size

```bash
# 4KB page size (XFS with 4K sector)
./build/mysql_recovery_pro --ibd=test.ibd --page-size=4096

# 8KB page size
./build/mysql_recovery_pro --ibd=test.ibd --page-size=8192

# 16KB page size (default)
./build/mysql_recovery_pro --ibd=test.ibd --page-size=16384

# 32KB page size
./build/mysql_recovery_pro --ibd=test.ibd --page-size=32768

# 64KB page size
./build/mysql_recovery_pro --ibd=test.ibd --page-size=65536
```

---

## Parallel Processing

### Multi-threaded Recovery

```bash
# Use 4 threads
./build/mysql_recovery_pro --ibd=test.ibd --multithread=4

# Use maximum threads (CPU cores - 1)
./build/mysql_recovery_pro --ibd=test.ibd --multithread=max

# Disable multi-threading (single-threaded)
./build/mysql_recovery_pro --ibd=test.ibd --multithread=1
```

### Batch Processing

```bash
# Process 1000 pages at a time
./build/mysql_recovery_pro --ibd=test.ibd --page-batch=1000

# Process 10000 records at a time
./build/mysql_recovery_pro --ibd=test.ibd --record-batch=10000
```

---

## Validation & Integrity

### CRC32 Validation

```bash
# Validate recovered records against golden hashes
./build/mysql_recovery_pro --ibd=test.ibd --validate-crc32 --golden-hashes=golden.json
```

### Error Tolerance

```bash
# Allow 100 errors before stopping
./build/mysql_recovery_pro --ibd=test.ibd --max-errors=100

# Continue through all errors (dangerous!)
./build/mysql_recovery_pro --ibd=test.ibd --max-errors=-1
```

### Progress Monitoring

```bash
# Show progress bar
./build/mysql_recovery_pro --ibd=test.ibd --progress

# Output performance stats
./build/mysql_recovery_pro --ibd=test.ibd --stats
```

---

## Recovery Command Examples by Scenario

### Scenario A: Torn Pages (Power Loss)

```bash
# Repair page headers and extract
./build/mysql_recovery_pro --ibd=test_table.ibd \
    --header-repair \
    --out-format=csv \
    -o recovery_result.csv
```

### Scenario B: rm -rf Fragmentation

```bash
# Sequential scan for fragments
./build/mysql_recovery_pro --ibd=test_table.ibd \
    --rmrf-recovery \
    --out-format=sql \
    -o recovery_result.sql
```

### Scenario C: BLOB Chain Break

```bash
# Reconstruct BLOB chains
./build/mysql_recovery_pro --ibd=test_table.ibd \
    --blob-recovery \
    --out-format=json \
    --json-lines \
    -o recovery_result.json
```

### Scenario D: Missing .frm Files

```bash
# Scan directory and auto-detect schema
./build/mysql_recovery_pro --scan-dir=/var/lib/mysql/testdb \
    --search \
    --out-format=csv \
    -o recovered_data.csv
```

### Scenario E: TRUNCATE Recovery

```bash
# Extract zombie records after TRUNCATE
./build/mysql_recovery_pro --ibd=test_table.ibd \
    --truncate-recovery \
    --out-format=sql \
    -o recovered_data.sql
```

### Scenario F: B+Tree Structure Damage

```bash
# Rebuild page chain and extract
./build/mysql_recovery_pro --ibd=test_table.ibd \
    --rebuild-chain \
    --header-repair \
    --out-format=csv \
    -o recovery_result.csv
```

---

## Advanced Usage

### Custom Page Offset Recovery

```bash
# Start reading from a specific page offset
./build/mysql_recovery_pro --ibd=test.ibd --start-page=100

# Stop at specific page
./build/mysql_recovery_pro --ibd=test.ibd --end-page=500

# Range recovery (pages 100-500)
./build/mysql_recovery_pro --ibd=test.ibd --start-page=100 --end-page=500
```

### Extract Specific Columns

```bash
# Extract only id and name columns
./build/mysql_recovery_pro --ibd=test.ibd --columns=id,name

# Extract to CSV with specific columns
./build/mysql_recovery_pro --ibd=test.ibd --columns=id,name,email -o output.csv
```

### Filter Records

```bash
# Filter by status value
./build/mysql_recovery_pro --ibd=test.ibd --filter="status=1"

# Filter by id range
./build/mysql_recovery_pro --ibd=test.ibd --filter="id>10000 and id<20000"

# Filter by name pattern (regex)
./build/mysql_recovery_pro --ibd=test.ibd --filter="name~'^张'"

# Multiple conditions
./build/mysql_recovery_pro --ibd=test.ibd --filter="status=1 and score>500"
```

### Exclude Columns

```bash
# Exclude sensitive columns
./build/mysql_recovery_pro --ibd=test.ibd --exclude-columns=password_hash,blob_data
```

---

## Performance Tuning Examples

### Fast Recovery (Low Memory)

```bash
./build/mysql_recovery_pro --ibd=test.ibd \
    --page-size=16384 \
    --multithread=4 \
    --page-batch=1000 \
    --stream-threshold=100000 \
    --out-format=csv \
    -o output.csv
```

### Maximum Accuracy (Slower)

```bash
./build/mysql_recovery_pro --ibd=test.ibd \
    --page-size=16384 \
    --multithread=1 \
    --validate-crc32 \
    --max-errors=0 \
    --out-format=json \
    --pretty \
    -o output.json
```

### Emergency Recovery (Speed priority)

```bash
./build/mysql_recovery_pro --ibd=test.ibd \
    --page-size=16384 \
    --multithread=max \
    --page-batch=5000 \
    --record-batch=50000 \
    --out-format=csv \
    --no-validation \
    -o output.csv
```

---

## Monitoring Recovery Progress

### Real-time Statistics

```bash
# Enable stats output
./build/mysql_recovery_pro --ibd=test.ibd --stats

# Output format:
# [INFO] Pages scanned: 15234
# [INFO] Records found: 4,823,567
# [INFO] Velocity: 2,847 pages/s
# [INFO] Memory: 412 MB / 512 MB
# [INFO] Estimated time remaining: 5s
```

### Output to File

```bash
# Log to file
./build/mysql_recovery_pro --ibd=test.ibd --stats --log=recovery.log
```

---

## Troubleshooting

### Common Issues

#### Issue 1: "Invalid page size detected"

```bash
# Try different page sizes
./build/mysql_recovery_pro --ibd=test.ibd --page-size=4096
./build/mysql_recovery_pro --ibd=test.ibd --page-size=8192
./build/mysql_recovery_pro --ibd=test.ibd --page-size=32768
```

#### Issue 2: "No records found"

```bash
# Try rmrf-recovery mode (most aggressive)
./build/mysql_recovery_pro --ibd=test.ibd --rmrf-recovery

# Try with validation disabled
./build/mysql_recovery_pro --ibd=test.ibd --no-validation
```

#### Issue 3: Out of Memory

```bash
# Use streaming mode
./build/mysql_recovery_pro --ibd=test.ibd --stream-threshold=10000

# Use fewer threads
./build/mysql_recovery_pro --ibd=test.ibd --multithread=1

# Process in chunks
./build/mysql_recovery_pro --ibd=test.ibd --start-page=0 --end-page=10000
```

#### Issue 4: Checksum Errors

```bash
# Ignore checksum errors
./build/mysql_recovery_pro --ibd=test.ibd --no-checksum

# Repair headers first
./build/mysql_recovery_pro --ibd=test.ibd --header-repair --no-checksum
```

---

## Integration with MySQL

### Import to MySQL after Recovery

```bash
# Recover to SQL file
./build/mysql_recovery_pro --ibd=test.ibd --out-format=sql -o recovered.sql

# Import to MySQL
mysql -u root -p testdb < recovered.sql

# Alternative: Import to CSV then use LOAD DATA
./build/mysql_recovery_pro --ibd=test.ibd --out-format=csv -o recovered.csv
mysql -u root -p -e "LOAD DATA INFILE '/path/recovered.csv' INTO TABLE test_table"
```

### Verify Data Integrity

```bash
# Get record counts
MYSQL_COUNT=$(mysql -u root -p -e "SELECT COUNT(*) FROM test_table" -s)
RECOVERED_COUNT=$(wc -l < recovered.csv)

echo "MySQL: $MYSQL_COUNT, Recovered: $RECOVERED_COUNT"

# Compare checksums
./build/mysql_recovery_pro --ibd=test.ibd --validate-crc32 --golden-hashes=golden.json
```

---

## Environment Variables

### Configure Recovery Tool

```bash
# Set output directory
export RECOVERY_OUTPUT_DIR=/var/recovery

# Set page size
export PAGE_SIZE=16384

# Set thread count
export RECOVERY_THREADS=4

# Run with environment
./build/mysql_recovery_pro --ibd=test.ibd
```

---

## Help Commands

```bash
# Show help
./build/mysql_recovery_pro --help

# Show help for specific mode
./build/mysql_recovery_pro --help --header-repair
./build/mysql_recovery_pro --help --out-format

# Show version
./build/mysql_recovery_pro --version
```

---

**End of Recovery Command Reference**
