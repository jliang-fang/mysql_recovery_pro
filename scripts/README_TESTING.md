# MySQL Recovery Pro - Automated Test Pipeline

## Overview

This document describes the complete automated validation pipeline for MySQL Recovery Pro.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Test Pipeline Orchestrator                    │
│  (test_scheduler.py / complete_pipeline.py)                     │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│  Docker Env   │    │  Data Injector│    │ Chaos Monkey  │
│  Setup/Teardown│   │  (100k recs) │   │  (Corruption) │
└───────────────┘    └───────────────┘    └───────────────┘
        │                     │                     │
        │                     ▼                     ▼
        │            ┌───────────────┐    ┌───────────────┐
        │            │  MySQL 5.6-9.x│    │  Corrupted    │
        │            │  Containers   │    │   .ibd Files  │
        │            └───────────────┘    └───────────────┘
        │                     │                     │
        │                     │                     ▼
        │                     │            ┌───────────────┐
        │                     │            │ Recovery Tool │
        │                     │            │  (C++)        │
        │                     │            └───────────────┘
        │                     │                     │
        │                     │                     ▼
        │                     │            ┌───────────────┐
        │                     │            │ Validation    │
        │                     │            │ (Hash Compare)│
        │                     │            └───────────────┘
        │                     │                     │
        └─────────────────────┼─────────────────────┘
                              │
                              ▼
                    ┌───────────────┐
                    │  Report Gen   │
                    │  (JSON/MD)    │
                    └───────────────┘
```

## Components

### 1. Docker Test Environment (`docker_test_env.sh`)

Deploys MySQL containers for versions 5.6, 5.7, 8.0, 8.4, 9.0:

```bash
# Setup environment
./scripts/docker_test_env.sh setup

# Inject test data
./scripts/docker_test_env.sh inject 8.0 100000

# Teardown
./scripts/docker_test_env.sh teardown
```

### 2. Data Injector (`data_injector.py`)

Generates 100,000 records with complex data types:

```bash
# Generate test data
python3 scripts/data_injector.py --num-records 100000 --vector

# Generate ground truth hashes
python3 scripts/data_injector.py --hash --output ground_truth.json
```

### 3. Chaos Monkey (`chaos_monkey.py`)

Simulates database disasters:

```bash
# Random page corruption
python3 scripts/chaos_monkey.py \
  --input test_table.ibd \
  --output corrupted.ibd \
  --scenario random_5_pages

# Metadata loss
python3 scripts/chaos_monkey.py \
  --input test_table.ibd \
  --output corrupted.ibd \
  --scenario metadata_lost

# File fragments
python3 scripts/chaos_monkey.py \
  --input test_table.ibd \
  --output corrupted.ibd \
  --scenario file_fragments
```

**Scenarios:**
- `random_3_pages` - Random 3 pages corrupted
- `random_5_pages` - Random 5 pages corrupted
- `random_10_pages` - Random 10 pages corrupted
- `all_headers` - All page headers corrupted
- `btree_chain_broken` - B+Tree page chain broken
- `metadata_lost` - SDI and file header corrupted
- `file_fragments` - File fragments created
- `undo_log_corrupt` - Undo log pages corrupted
- `record_chain_broken` - Record chains broken
- `sdn_only` - SDI pages only
- `header_only` - Page headers only

### 4. Validation Pipeline (`validation_pipeline.py`)

Compares recovered data against ground truth:

```bash
# Run validation
python3 scripts/validation_pipeline.py --scenario all

# Validate specific scenario
python3 scripts/validation_pipeline.py \
  --scenario corruption \
  --ground-truth ground_truth.json
```

### 5. Auto-Fix System (`auto_fix_system.py`)

Analyzes failures and suggests fixes:

```bash
# Analyze failure log
python3 scripts/auto_fix_system.py \
  --log-file recovery.log \
  --apply-fix

# Generate specific fix
python3 scripts/auto_fix_system.py \
  --fix-type header_validation

# Run regression tests
python3 scripts/auto_fix_system.py \
  --regression
```

### 6. Test Scheduler (`test_scheduler.py`)

Orchestrates the complete pipeline:

```bash
# Run full validation
python3 scripts/test_scheduler.py --scenario full

# Version compatibility test
python3 scripts/test_scheduler.py \
  --scenario version \
  --version 8.0

# Corruption scenario test
python3 scripts/test_scheduler.py \
  --scenario corruption \
  --corruption random_5_pages
```

### 7. Complete Pipeline (`complete_pipeline.py`)

End-to-end validation:

```bash
# Run complete pipeline
python3 scripts/complete_pipeline.py --scenario full

# Build only
python3 scripts/complete_pipeline.py --build-only

# Version test
python3 scripts/complete_pipeline.py \
  --scenario version \
  --version 8.0
```

## Usage

### Quick Start

```bash
# 1. Build the project
cd /app/mysql_recovery_pro
bash scripts/build_linux.sh

# 2. Run complete pipeline
python3 scripts/complete_pipeline.py --scenario full

# 3. View results
cat test_results/validation_report.md
```

### Docker-Based Testing

```bash
# Setup Docker environment
bash scripts/docker_test_env.sh setup

# Inject test data into MySQL 8.0
python3 scripts/data_injector.py \
  --host localhost \
  --port 33060 \
  --num-records 100000

# Generate ground truth
python3 scripts/data_injector.py --hash

# Run corruption simulation
python3 scripts/chaos_monkey.py \
  --input /var/lib/mysql/testdb/test_users.ibd \
  --output /tmp/corrupted.ibd \
  --scenario random_5_pages

# Run recovery
./build/mysql_recovery_pro \
  --ibd=/tmp/corrupted.ibd \
  --output=/tmp/recovered \
  --format=csv

# Validate
python3 scripts/validation_pipeline.py \
  --scenario corruption \
  --ground-truth ground_truth.json
```

### CI/CD Integration

```yaml
# .github/workflows/automated-tests.yml
name: Automated Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Build project
        run: bash scripts/build_linux.sh
      
      - name: Run complete pipeline
        run: python3 scripts/complete_pipeline.py --scenario full
      
      - name: Upload results
        uses: actions/upload-artifact@v3
        with:
          name: test-results
          path: test_results/
```

## Configuration

Edit `scripts/test_config.json`:

```json
{
  "mysql_versions": ["5.6", "5.7", "8.0", "8.4", "9.0"],
  "corruption_scenarios": ["random_3_pages", "all_headers", ...],
  "record_counts": [1000, 10000, 100000],
  "docker_enabled": true,
  "parallel_tests": 4,
  "timeout_seconds": 600
}
```

## Output Reports

### JSON Report (`test_results/validation_report.json`)

```json
{
  "timestamp": "2024-01-01T00:00:00",
  "duration_seconds": 3600,
  "summary": {
    "total_tests": 50,
    "passed": 48,
    "failed": 2,
    "pass_rate": 96.0
  },
  "results": [...]
}
```

### Markdown Report (`test_results/validation_report.md`)

```markdown
# MySQL Recovery Pro - Validation Report

## Summary

| Metric | Value |
|--------|-------|
| Total Tests | 50 |
| Passed | 48 |
| Failed | 2 |
| Pass Rate | 96.0% |

## Test Results

### Test 1: random_3_pages (MySQL 8.0)
- **Status:** ✅ PASS
- **Records:** 100000
```

## Failure Analysis

The auto-fix system analyzes failures and suggests fixes:

```
Failure Pattern: page_header_corrupt
Location: src/innodb/page_parser.cpp
Fix Type: header_validation
Suggestion: Relax page header validation or implement header recovery

Generated Fix:
// Fix: Relax page header validation
bool PageParser::validate_page_header(const PageHeader& header) {
    // Original strict validation
    // if (header.page_type != FIL_PAGE_INDEX) return false;
    
    // New relaxed validation
    if (header.page_type != FIL_PAGE_INDEX && 
        header.page_type != FIL_PAGE_SDI) {
        std::cerr << "[WARN] Unknown page type" << std::endl;
    }
    return true;
}
```

## Regression Testing

```bash
# Run regression tests
python3 scripts/auto_fix_system.py --regression

# Results saved to: build/regression_results.json
```

## Troubleshooting

### Docker Setup Issues

```bash
# Check Docker status
docker info

# Verify containers
docker ps -a | grep mysql_recovery

# Check logs
docker logs mysql_recovery_test_8_0
```

### Build Issues

```bash
# Clean rebuild
rm -rf build
bash scripts/build_linux.sh

# Check dependencies
ldd build/mysql_recovery_pro
```

### Data Generation Issues

```bash
# Verify MySQL connection
mysql -h localhost -P 33060 -uroot -ptest123

# Check data
mysql -e "SELECT COUNT(*) FROM testdb.test_users"
```

## Performance Targets

| Metric | Target |
|--------|--------|
| Recovery time (100k records) | < 60 seconds |
| Record recovery rate | > 95% |
| Hash match rate | > 99% |
| Test execution time | < 4 hours |

## Extending the Pipeline

### Add New Corruption Scenario

1. Add scenario to `chaos_monkey.py`:
```python
elif scenario == "new_scenario":
    data, result["corrupted_pages"] = corrupt_new(data, page_size)
    result["corruption_types"] = ["new"] * len(result["corrupted_pages"])
```

2. Add to config:
```json
"corruption_scenarios": [..., "new_scenario"]
```

### Add New MySQL Version

1. Update `docker_test_env.sh`:
```bash
"9.1")
    docker run -d --name "$container_name" mysql:9.1 ...
    ;;
```

2. Update config:
```json
"mysql_versions": [..., "9.1"]
```

## License

MIT License - See LICENSE file for details.
