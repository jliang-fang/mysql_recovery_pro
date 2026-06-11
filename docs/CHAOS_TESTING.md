# MySQL Recovery Pro - Chaos Testing Pipeline Documentation

## Overview

This document describes the complete chaos testing pipeline for MySQL Recovery Pro v2.1.0. The pipeline implements a "break-validate-self-heal" cycle that drives the recovery engine towards 100% recovery accuracy through continuous refinement.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Chaos Testing Pipeline                            │
├─────────────────────────────────────────────────────────────────────┤
│ 1. Multi-Version MySQL Setup (5.0 - 9.0)                           │
│    - Docker Compose orchestration                                   │
│    - Version-specific configurations                                │
├─────────────────────────────────────────────────────────────────────┤
│ 2. Data Injection Engine                                             │
│    - 1M+ records with edge cases                                    │
│    - Version-specific data types                                    │
│    - Concurrent injection                                           │
├─────────────────────────────────────────────────────────────────────┤
│ 3. Golden Hash Base Generation                                     │
│    - SHA-256 hash baseline                                          │
│    - Per-record verification                                        │
├─────────────────────────────────────────────────────────────────────┤
│ 4. Chaos Simulation                                                  │
│    - 12 corruption scenarios                                        │
│    - Physical file modification                                     │
├─────────────────────────────────────────────────────────────────────┤
│ 5. Recovery Execution with ASAN/Valgrind                           │
│    - Memory error detection                                         │
│    - AddressSanitizer instrumentation                               │
├─────────────────────────────────────────────────────────────────────┤
│ 6. Self-Healing Loop                                                 │
│    - GDB core dump analysis                                         │
│    - AI patch generation                                            │
│    - Automatic recompilation                                        │
├─────────────────────────────────────────────────────────────────────┤
│ 7. Reporting & Metrics                                               │
│    - Recovery scoring                                               │
│    - Performance benchmarks                                         │
│    - Code evolution tracking                                        │
└─────────────────────────────────────────────────────────────────────┘
```

## Component Breakdown

### 1. Docker Multi-Version Environment

**File:** `docker/docker-compose.multiversions.yml`

Supports testing against:
- MySQL 5.0.96 - Far Ancient (Antelope format)
- MySQL 5.5.62 - Legacy (Barracuda transition)
- MySQL 5.6.51 - Barracuda default
- MySQL 5.7.44 - JSON support
- MySQL 8.0.36 - LTS
- MySQL 8.4.0 - LTS改进版
- MySQL 9.0.0 - Vector support preview

```bash
# Start all versions
docker-compose -f docker/docker-compose.multiversions.yml up -d

# Check status
docker-compose -f docker/docker-compose.multiversions.yml ps
```

### 2. Ultra Data Injector

**File:** `scripts/ultra_data_injector.py`

Features:
- 1M+ records with concurrent batch injection
- Version-specific schema generation
- Extreme edge cases (max-length fields, NULL overlap, geometric data)
- Real-time progress reporting with ETA

```bash
# Inject 1M records for MySQL 8.0
python3 scripts/ultra_data_injector.py \
    --host localhost --port 3306 \
    --num-records 1000000 \
    --version 8.0 \
    --vector --gis
```

### 3. Golden Hash Base

**File:** `scripts/golden_hash_base.py`

Generates SHA-256 hashes for all records to create immutable baseline:
- Per-record hash computation
- Summarized file storage
- Version-agnostic verification format

```bash
# Generate hash baseline
python3 scripts/golden_hash_base.py \
    --output-dir ./golden-hashes \
    --version 8.0
```

### 4. Advanced Chaos Monkey

**File:** `scripts/advanced_chaos_monkey.py`

Corruption scenarios:
1. **torn_page** - Simulate write-thy断电, last 50% of page corrupted
2. **metadata_annihilation** - Erase .frm (5.x) or SDI (8.x)
3. **purge_race** - DROP table + immediate kill -9
4. **swiss_cheese** - 20% random page corruption
5. **grid_corruption** - Every Nth page corrupted
6. **crc_sabotage** - Invalid checksums
7. **type_confusion** - Wrong page types
8. **lsn_corruption** - Invalid log sequence numbers
9. **undo_destroy** - Corrupted rollback segments
10. **blob_attack** - Broken BLOB chains
11. **page_truncation** - File shortened
12. **all_scenarios** - Combined extreme

```bash
# Apply corruption
python3 scripts/advanced_chaos_monkey.py \
    --input test_clean.ibd \
    --output test_corrupted.ibd \
    --scenario swiss_cheese \
    --corruption-ratio 0.20
```

### 5. Self-Healing Loop

**File:** `scripts/self_healing_loop.py`

The self-healing cycle:
1. Run recovery engine under ASAN
2. Capture crash (SIGSEGV, SIGABRT)
3. Generate core dump with GDB
4. Analyze stack trace
5. Extract source location
6. Generate Unified Diff patch
7. Apply patch to source
8. Rebuild with `cmake --build`
9. Re-run test
10. Iterate until pass or max_retries reached

```bash
# Run with auto-healing
python3 scripts/self_healing_loop.py \
    --input test_corrupted.ibd \
    --max-retries 5 \
    --report diagnosis_report.json
```

### 6. Final Report Generator

**File:** `scripts/generate_final_report.py`

Output includes:
- Recovery confidence scores by version
- Corruption scenario success rates
- Performance benchmarks (MB/s, RAM)
- Code evolution tracking
- Recommendations

## Running the Pipeline

### Option 1: Run Full Suite (Docker)

```bash
cd scripts
./run_chaos_test_suite.sh --all
```

This orchestrates:
1. Docker environment setup (all versions)
2. Data injection (1M records each version)
3. Golden hash generation
4. Chaos simulation (all scenarios)
5. Recovery with ASAN
6. Self-healing loops
7. Report generation

### Option 2: Quick Test (Local MySQL)

```bash
./run_chaos_test_suite.sh --quick
```

Uses local MySQL installation, skips Docker orchestration.

### Option 3: Manual Stages

```bash
# Stage 1: Start Docker
docker-compose -f docker/docker-compose.multiversions.yml up -d

# Stage 2: Inject data
python3 scripts/ultra_data_injector.py --host localhost --port 3306 --version 8.0 --num-records 100000

# Stage 3: Generate golden hash
python3 scripts/golden_hash_base.py --host localhost --port 3306

# Stage 4: Apply corruption
python3 scripts/advanced_chaos_monkey.py \
    --input golden_test.ibd \
    --output corrupted_test.ibd \
    --scenario torn_page

# Stage 5: Recover & debug
python3 scripts/self_healing_loop.py \
    --input corrupted_test.ibd \
    --max-retries 5
```

## Recovery Scoring

### Score Levels

| Level | Score | Description |
|-------|-------|-------------|
| 100% EXACT | 100 | All records SHA-256 match baseline |
| 90% HEURISTIC | 90-99 | Most records match, some field reconstruction |
| 50% PARTIAL | 50-89 | Significant data loss |
| CRITICAL | 0-49 | Engine crashed during recovery |
| TOTAL FAILURE | 0 | No records recovered |

### Scoring Formula

```
Recovery Score = (Record Recovery Rate × 0.5) + (Hash Match Rate × 0.5) × 100
```

## Output Files

```
recovery-output/
├── mysql-5.0/
│   ├── results.json
│   └── recovery_*/
│       └── *.sql
├── mysql-8.0/
│   └── ...
└── final_chaos_report.json

golden-hashes/
├── hashes_test_users.json
├── hashes_test_users_edge.json
└── summary.json

chaos-test-volumes/
├── clean_test.ibd
└── *corrupted.ibd

diagnosis/
└── healing_*.json
```

## Troubleshooting

### Docker containers won't start
```bash
# Check if port is in use
docker ps
docker stop <container_name>

# Rebuild images
cd docker
./build_mysql_images.sh all
```

### ASAN crashes in recovery engine
```bash
# Enable verbose ASAN output
export ASAN_OPTIONS=verbose_stack_trace=1:symbolize=1
```

### Hash mismatches
```bash
# Regenerate golden hashes
python3 scripts/golden_hash_base.py --verify
```

## Performance Benchmarks

Expected recovery rates by version:

| Version | Expected Rate | Notes |
|---------|--------------|-------|
| 5.0 | 80-95% | Simple .frm recovery |
| 5.5 | 85-95% | Barracuda support |
| 5.6 | 90-98% | DYNAMIC format |
| 5.7 | 90-99% | JSON support |
| 8.0 | 85-98% | SDI recovery |
| 8.4 | 85-98% | LSR improvements |
| 9.0 | 80-95% | Vector recovery |

## Future Enhancements

1. **Machine Learning-based Repair**: Train models on corruption patterns
2. **Distributed Testing**: Scale across multiple containers
3. **Fuzz Integration**: AFL-based input fuzzing
4. **Property-based Testing**: QuickCheck-style invariants
5. **Continuous Benchmarking**: Track performance regressions

## Support

For issues or questions:
- Check `docs/` directory
- Review `recovery-output/final_chaos_report.json`
- Enable `--verbose` for detailed logging

---

**Author:** jliang.fang (Global MySQL Kernel QA Expert)  
**Version:** 2.1.0  
**Last Updated:** 2026-04-04
