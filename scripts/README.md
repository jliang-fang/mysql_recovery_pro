# MySQL Recovery Pro - Chaos Testing Pipeline
# 
# Overview
# --------
# This directory contains the complete chaos testing infrastructure for MySQL Recovery Pro.
# The pipeline implements a "break-validate-self-heal" cycle to drive towards 100% recovery accuracy.

# File Structure
# --------------

## docker/
# - docker-compose.multiversions.yml    # Multi-version MySQL orchestration (5.0-9.0)
# - build_mysql_images.sh               # Script to build MySQL Docker images
# - dockerfiles/                        # MySQL version-specific Dockerfiles
#   - Dockerfile.mysql-5.0
#   - Dockerfile.mysql-5.5
#   - Dockerfile.mysql-5.6
#   - Dockerfile.mysql-5.7
#   - Dockerfile.mysql-8.0
#   - Dockerfile.mysql-8.4
#   - Dockerfile.mysql-9.0

## scripts/
# - ultra_data_injector.py             # 1M+ concurrent data injection with version-specific types
# - golden_hash_base.py                # SHA-256 hash baseline generation
# - advanced_chaos_monkey.py           # 12 corruption scenarios simulation
# - self_healing_loop.py              # GDB core analysis + AI patch generation
# - generate_final_report.py          # Comprehensive metrics and reporting
# - run_chaos_test_suite.sh           # Master scheduler script
# - run_pipeline.py                   # Python pipeline runner
# - verify_setup.sh                   # Setup verification and quick test
# - chaos_scripts/                    # Additional chaos testing utilities

## docs/
# - CHAOS_TESTING.md                  # Complete documentation
# - report_template.json              # Report JSON template

## Directories (created at runtime)
# - golden-hashes/                    # SHA-256 hash baselines
# - chaos-test-volumes/               # Corrupted .ibd test files
# - recovery-output/                  # Recovery results per version
# - diagnosis/                        # Self-healing diagnosis reports

## Configuration
# The pipeline uses global configuration in the master scheduler:
# - DEFAULT_VERSIONS: MySQL 5.0, 5.5, 5.6, 5.7, 8.0, 8.4, 9.0
# - RECORD_COUNT: 100000 records per version (configurable)
# - MAX_RETRIES: 5 (self-healing attempts)

# QuickStart
# ----------

# 1. Verify setup
cd scripts
./verify_setup.sh

# 2. Run full pipeline (quick mode - uses local MySQL)
cd scripts
python3 scripts/run_pipeline.py --quick

# Or use the bash scheduler:
cd scripts
./run_chaos_test_suite.sh --quick

# 3. Run with Docker (production)
cd scripts
./run_chaos_test_suite.sh --all

# Usage Examples
# --------------

# Data injection for single version
python3 scripts/ultra_data_injector.py \
    --host localhost --port 3306 \
    --num-records 100000 \
    --version 8.0 \
    --vector --gis --hash

# Apply specific corruption
python3 scripts/advanced_chaos_monkey.py \
    --input test.ibd \
    --output test_corrupted.ibd \
    --scenario torn_page \
    --corruption-percent 80

# Self-healing loop
python3 scripts/self_healing_loop.py \
    --input test_corrupted.ibd \
    --max-retries 5 \
    --report healing_report.json

# Generate report
python3 scripts/generate_final_report.py \
    --output-dir ./recovery-output \
    --versions 5.0 5.5 5.6 5.7 8.0 8.4 9.0

# Development Pipeline
# --------------------

# For iterative development, run stages separately:
cd scripts

# 1. Inject test data
python3 ultra_data_injector.py --num-records 10000 --version 8.0

# 2. Build engine (with ASAN)
cd ../build && cmake .. -DENABLE_ASAN=ON && make -j4

# 3. Apply corruption
python3 ../scripts/advanced_chaos_monkey.py \
    --input ../test.ibd \
    --output ../corrupted.ibd \
    --scenario crc_sabotage

# 4. Recover and debug
python3 ../scripts/self_healing_loop.py \
    --input ../corrupted.ibd \
    --max-retries 3

# 5. View diagnostic report
cat ../diagnosis/diagnosis.json | jq

# Corruption Scenarios
# --------------------

Scenario               | Description                              | Damage Pattern
----------------------|------------------------------------------|------------------
torn_page             | Write-thy断电, last 50% corrupted         | Single page, partial
metadata_annihilation | Erase .frm (5.x) or SDI (8.x)            | Catalog pages
purge_race            | DROP + kill -9, purge incomplete         | Record headers
swiss_cheese          | 20% random pages corrupted               | Scattered, 0/1/random
grid_corruption       | Every Nth page (default 10)              | Regular pattern
crc_sabotage          | Invalid CRC32 checksums                  | Header corruption
type_confusion        | Wrong page types                           | Misleading info
lsn_corruption        | Invalid log sequence numbers             | Transaction log
undo_destroy          | Corrupt undo segment headers             | Rollback segment
blob_attack           | Break BLOB chain pointers                | Large object chains
page_truncation       | Remove pages from file end               | Shortened file
all_scenarios         | Combined extreme corruption              | Everything mixed

# Recovery Scoring
# ----------------

Score Range | Level                  | Description
-----------|------------------------|----------------------------------------
100        | 100% EXACT RECOVERY    | All SHA-256 hashes match baseline
90-99.9    | 90% HEURISTIC          | >90% records recovered with slight reconstruction
50-89.9    | 50% PARTIAL RECOVERY   | 50-89% records recovered
0-49.9     | CRITICAL FAILURE       | <50% recovered, crash during recovery
0          | TOTAL FAILURE          | 0% recovered, engine crashed

# Metrics
# -------

Recovery Engine (per version):
- Parse rate (MB/s)
- Peak memory (RAM)
- Records per second
- Average recovery time

Corruption Success Rate:
- Total scenarios run
- Passed scenarios
- Failed scenarios
- Average success rate

# Author
# ------
jliang.fang (Global MySQL Kernel QA Expert)  
Version: 2.1.0

# License
# -------
Proprietary - MySQL Recovery Pro Chaos Testing Infrastructure
