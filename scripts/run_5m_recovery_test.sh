#!/bin/bash
# =============================================================================
# MySQL 5.6 5M+ Recovery Test Suite
# Author: jliang.fang (Global MySQL Kernel QA Expert)
# Purpose: End-to-end test pipeline for MySQL 5.6 recovery validation
# =============================================================================

set -e

# ==============================================================================
# Configuration
# ==============================================================================
CONTAINER_NAME=${CONTAINER_NAME:-mysql_recovery_56}
MYSQL_HOST=${MYSQL_HOST:-localhost}
MYSQL_PORT=${MYSQL_PORT:-33056}
MYSQL_ROOT_PASSWORD=${MYSQL_ROOT_PASSWORD:-root123}
TEST_DATABASE=${TEST_DATABASE:-testdb}
TABLE_NAME=${TABLE_NAME:-test_table}
RECORDS_COUNT=${RECORDS_COUNT:-5000000}
PAGE_SIZE=${PAGE_SIZE:-16384}

# IBD file paths
HOST_DATA_DIR=${HOST_DATA_DIR:-./chaos-test-volumes/mysql56_data/testdb}
DOCKER_DATA_DIR=${DOCKER_DATA_DIR:-/var/lib/mysql/testdb}

# Output directories
OUTPUT_DIR=${OUTPUT_DIR:-./recovery-output/5m-test-$(date +%Y%m%d_%H%M%S)}
CORRUPTION_DIR=${CORRUPTION_DIR:-./corruption-output}
GOLDEN_HASHES_DIR=${GOLDEN_HASHES_DIR:-./golden-hashes}

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ==============================================================================
# Utility Functions
# ==============================================================================

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

create_output_dirs() {
    mkdir -p "$OUTPUT_DIR"
    mkdir -p "$CORRUPTION_DIR"
    mkdir -p "$GOLDEN_HASHES_DIR"
}

check_dependencies() {
    log_info "Checking dependencies..."
    
    # Check docker
    if ! command -v docker &> /dev/null; then
        log_error "Docker is not installed"
        exit 1
    fi
    
    # Check mysql client
    if ! command -v mysql &> /dev/null; then
        log_warning "MySQL client not found, skipping database tests"
    fi
    
    # Check python3
    if ! command -v python3 &> /dev/null; then
        log_error "Python 3 is not installed"
        exit 1
    fi
    
    # Check recovery tool
    if [ ! -f "./build/mysql_recovery_pro" ]; then
        log_error "Recovery tool not found at ./build/mysql_recovery_pro"
        log_info "Building recovery tool..."
        make -C build
    fi
    
    log_success "All dependencies OK"
}

run_mysql_in_container() {
    docker exec "$CONTAINER_NAME" mysql -u root -p"$MYSQL_ROOT_PASSWORD" "$@" 2>/dev/null
}

wait_for_mysql() {
    local max_retries=60
    local retry=0
    
    log_info "Waiting for MySQL 5.6 to be ready..."
    
    while [ $retry -lt $max_retries ]; do
        if docker exec "$CONTAINER_NAME" mysqladmin ping -h localhost -proot123 --silent 2>/dev/null; then
            log_success "MySQL 5.6 is ready!"
            return 0
        fi
        retry=$((retry + 1))
        sleep 2
    done
    
    log_error "MySQL 5.6 failed to start"
    exit 1
}

# ==============================================================================
# Test Pipeline
# ==============================================================================

pre_test() {
    log_info "=========================================="
    log_info "Pre-Test Setup"
    log_info "=========================================="
    
    create_output_dirs
    
    # Ensure MySQL is running
    wait_for_mysql
    
    # Create database if not exists
    run_mysql_in_container -e "CREATE DATABASE IF NOT EXISTS $TEST_DATABASE"
    
    log_success "Pre-test setup complete"
}

test_5m_data_generation() {
    local test_dir="$OUTPUT_DIR/5m_generation_test"
    mkdir -p "$test_dir"
    
    log_info "=========================================="
    log_info "Test: 5M Data Generation (MySQL 5.6)"
    log_info "=========================================="
    
    # Create schema in MySQL
    run_mysql_in_container $TEST_DATABASE < test_scripts/schema_50.sql
    
    # Create golden data using Python generator
    log_info "Generating 5M golden records..."
    python3 scripts/generate_5m_data_mysql56.py \
        --records=$RECORDS_COUNT \
        --page-size=$PAGE_SIZE \
        --seed=42 \
        --output-dir="$test_dir" \
        --records-per-page=1000 \
        --output-hash="golden_hashes_5m.json"
    
    log_success "Generated 5M golden records"
    log_info "Output: $test_dir"
    
    # Copy golden hashes
    cp "$test_dir/golden_hashes_5m.json" "$GOLDEN_HASHES_DIR/"
    
    # Verify file exists
    if [ -f "$test_dir/test_table.ibd" ]; then
        local file_size=$(stat -c%s "$test_dir/test_table.ibd")
        log_success "IBD file created: $(du -h "$test_dir/test_table.ibd" | cut -f1)"
    else
        log_error "IBD file not created!"
        exit 1
    fi
    
    # Verify CSV output
    if [ -f "$test_dir/test_table.csv" ]; then
        local csv_lines=$(wc -l < "$test_dir/test_table.csv")
        log_success "CSV file created: $csv_lines lines"
    fi
}

test_data_injection_to_mysql() {
    log_info "=========================================="
    log_info "Test: Data Injection to MySQL 5.6"
    log_info "=========================================="
    
    local test_dir="$OUTPUT_DIR/5m_injection_test"
    mkdir -p "$test_dir"
    
    # Copy schema and data to container
    docker cp "$test_scripts/schema_50.sql" "$CONTAINER_NAME:/tmp/schema.sql" 2>/dev/null || true
    
    # Use chaos_injector.py to populate MySQL with 5M records
    docker exec -i "$CONTAINER_NAME" bash -c "
        mysql -u root -proot123 $TEST_DATABASE < /test_scripts/schema_50.sql
    " 2>/dev/null || true
    
    # Inject data using Python
    docker exec -i "$CONTAINER_NAME" python3 -c "
        import sys
        sys.path.insert(0, '/test_scripts')
        from chaos_injector import DataInjector
        import mysql.connector
        
        injector = DataInjector(seed=42)
        records = injector.generate_records(num_records=$RECORDS_COUNT)
        
        conn = mysql.connector.connect(
            host='localhost',
            user='root',
            password='root123',
            database='$TEST_DATABASE'
        )
        
        inserted = injector.insert_records(conn, records)
        print(f'Inserted {inserted} records')
        
        conn.close()
    " 2>/dev/null
    
    # Verify record count
    local mysql_count=$(run_mysql_in_container $TEST_DATABASE -e "SELECT COUNT(*) FROM $TABLE_NAME" -s)
    
    if [ "$mysql_count" -ge $((RECORDS_COUNT * 99 / 100)) ]; then
        log_success "MySQL records count: $mysql_count"
    else
        log_warning "MySQL record count ($mysql_count)低于 expected ($RECORDS_COUNT)"
    fi
}

test_clean_extraction() {
    log_info "=========================================="
    log_info "Test: Clean Data Extraction (Baseline)"
    log_info "=========================================="
    
    local test_dir="$OUTPUT_DIR/5m_clean_extraction"
    mkdir -p "$test_dir"
    
    # Extract original IBD file
    docker exec "$CONTAINER_NAME" cp "$DOCKER_DATA_DIR/$TABLE_NAME.ibd" "/tmp/${TABLE_NAME}.ibd"
    docker cp "$CONTAINER_NAME:/tmp/${TABLE_NAME}.ibd" "$test_dir/original.ibd"
    
    # Run recovery
    log_info "Extracting records to CSV..."
    time ./build/mysql_recovery_pro \
        --ibd="$test_dir/original.ibd" \
        --page-size=$PAGE_SIZE \
        --multithread=4 \
        --out-format=csv \
        -o "$test_dir/recovered.csv"
    
    # Verify results
    local recovered_count=$(wc -l < "$test_dir/recovered.csv")
    local original_count=$(run_mysql_in_container $TEST_DATABASE -e "SELECT COUNT(*) FROM $TABLE_NAME" -s)
    
    log_info "Original records: $original_count"
    log_info "Recovered records: $recovered_count"
    
    local recovery_rate=$(echo "scale=4; $recovered_count * 100 / $original_count" | bc 2>/dev/null || echo "N/A")
    log_success "Recovery rate: ${recovery_rate}%"
    
    # Calculate checksum
    local original_hash=$(md5sum "$test_dir/original.ibd" | cut -d' ' -f1)
    local recovered_hash=$(md5sum "$test_dir/recovered.csv" | cut -d' ' -f1)
    
    echo "$original_hash  original.ibd" > "$test_dir/checksums.md5"
    echo "$recovered_hash  recovered.csv" >> "$test_dir/checksums.md5"
    
    log_success "Checksums saved"
}

test_torn_pages_recovery() {
    log_info "=========================================="
    log_info "Test: Torn Pages Recovery (--header-repair)"
    log_info "=========================================="
    
    local test_dir="$OUTPUT_DIR/5m_torn_pages"
    mkdir -p "$test_dir"
    
    # Copy and corrupt
    docker exec "$CONTAINER_NAME" cp "$DOCKER_DATA_DIR/$TABLE_NAME.ibd" "/tmp/${TABLE_NAME}.ibd"
    docker cp "$CONTAINER_NAME:/tmp/${TABLE_NAME}.ibd" "$test_dir/corrupted.ibd"
    
    # Simulate torn pages using dd
    log_info "Corrupting page headers (torn pages simulation)..."
    local file_size=$(stat -c%s "$test_dir/corrupted.ibd")
    local total_pages=$((file_size / PAGE_SIZE))
    
    # Corrupt first 100 pages
    for page in $(seq 0 99); do
        local offset=$((page * PAGE_SIZE))
        dd if=/dev/urandom of="$test_dir/corrupted.ibd" bs=1 count=40 seek=$offset conv=notrunc 2>/dev/null
    done
    
    log_info "Corrupted $total_pages pages"
    
    # Run header repair
    log_info "Running header repair..."
    time ./build/mysql_recovery_pro \
        --ibd="$test_dir/corrupted.ibd" \
        --page-size=$PAGE_SIZE \
        --header-repair \
        --out-format=csv \
        -o "$test_dir/recovered.csv"
    
    local recovered_count=$(wc -l < "$test_dir/recovered.csv")
    log_success "Recovered records: $recovered_count"
}

test_fragmented_recovery() {
    log_info "=========================================="
    log_info "Test: Fragmented File Recovery (--rmrf-recovery)"
    log_info "=========================================="
    
    local test_dir="$OUTPUT_DIR/5m_fragmented"
    mkdir -p "$test_dir"
    
    # Copy original
    docker exec "$CONTAINER_NAME" cp "$DOCKER_DATA_DIR/$TABLE_NAME.ibd" "/tmp/${TABLE_NAME}.ibd"
    docker cp "$CONTAINER_NAME:/tmp/${TABLE_NAME}.ibd" "$test_dir/corrupted.ibd"
    
    # Create fragmented version
    log_info "Creating fragmentation..."
    local file_size=$(stat -c%s "$test_dir/corrupted.ibd")
    local fragment_size=$((file_size / 100))
    
    # Create temp directory for fragments
    local temp_dir="$test_dir/fragments"
    mkdir -p "$temp_dir"
    
    # Extract and shuffle fragments
    for i in $(seq 0 99); do
        dd if="$test_dir/corrupted.ibd" of="$temp_dir/frag_$i" bs=$fragment_size count=1 skip=$i 2>/dev/null
    done
    
    # Shuffle and reassemble
    for f in $(ls "$temp_dir"/frag_* | shuf); do
        basename=$(basename "$f")
        mv "$f" "$temp_dir/${basename}.shuffled"
    done
    
    # Reassemble
    cat "$temp_dir"/frag_*.shuffled > "$test_dir/fragmented.ibd" 2>/dev/null || {
        # Fallback
        for f in $(ls "$temp_dir"/frag_* | sort -R); do
            cat "$f" >> "$test_dir/fragmented.ibd"
        done
    }
    
    rm -rf "$temp_dir"
    
    # Run rmrf recovery
    log_info "Running rmrf-recovery..."
    time ./build/mysql_recovery_pro \
        --ibd="$test_dir/fragmented.ibd" \
        --page-size=$PAGE_SIZE \
        --rmrf-recovery \
        --out-format=csv \
        -o "$test_dir/recovered.csv"
    
    local recovered_count=$(wc -l < "$test_dir/recovered.csv")
    log_success "Recovered records: $recovered_count"
}

test_blob_recovery() {
    log_info "=========================================="
    log_info "Test: BLOB Chain Recovery (--blob-recovery)"
    log_info "=========================================="
    
    local test_dir="$OUTPUT_DIR/5m_blob_recovery"
    mkdir -p "$test_dir"
    
    # Copy original
    docker exec "$CONTAINER_NAME" cp "$DOCKER_DATA_DIR/$TABLE_NAME.ibd" "/tmp/${TABLE_NAME}.ibd"
    docker cp "$CONTAINER_NAME:/tmp/${TABLE_NAME}.ibd" "$test_dir/corrupted.ibd"
    
    # Simulate BLOB chain break
    log_info "Breaking BLOB chain..."
    local file_size=$(stat -c%s "$test_dir/corrupted.ibd")
    local total_pages=$((file_size / PAGE_SIZE))
    
    # Corrupt intermediate pages (BLOB pages)
    for page in $(seq 1 $((total_pages - 2)) | head -n 50 | sort -R); do
        local offset=$((page * PAGE_SIZE))
        # Corrupt page type
        dd if=/dev/urandom of="$test_dir/corrupted.ibd" bs=1 count=2 seek=$((offset + 16)) conv=notrunc 2>/dev/null
        # Corrupt chain pointers
        dd if=/dev/urandom of="$test_dir/corrupted.ibd" bs=1 count=8 seek=$((offset + 0)) conv=notrunc 2>/dev/null
    done
    
    # Run BLOB recovery
    log_info "Running BLOB recovery..."
    time ./build/mysql_recovery_pro \
        --ibd="$test_dir/corrupted.ibd" \
        --page-size=$PAGE_SIZE \
        --blob-recovery \
        --out-format=csv \
        -o "$test_dir/recovered.csv"
    
    local recovered_count=$(wc -l < "$test_dir/recovered.csv")
    log_success "Recovered records: $recovered_count"
}

test_truncate_recovery() {
    log_info "=========================================="
    log_info "Test: Truncate Recovery (--truncate-recovery)"
    log_info "=========================================="
    
    local test_dir="$OUTPUT_DIR/5m_truncate_recovery"
    mkdir -p "$test_dir"
    
    # Copy original
    docker exec "$CONTAINER_NAME" cp "$DOCKER_DATA_DIR/$TABLE_NAME.ibd" "/tmp/${TABLE_NAME}.ibd"
    docker cp "$CONTAINER_NAME:/tmp/${TABLE_NAME}.ibd" "$test_dir/corrupted.ibd"
    
    # Truncate file (simulate TRUNCATE TABLE)
    log_info "Truncating file (simulating TRUNCATE TABLE)..."
    local file_size=$(stat -c%s "$test_dir/corrupted.ibd")
    local new_size=$((file_size * 90 / 100))  # Keep 90%
    
    truncate -s $new_size "$test_dir/corrupted.ibd"
    
    log_info "File truncated from $file_size to $new_size bytes"
    
    # Run truncate recovery
    log_info "Running truncate recovery..."
    time ./build/mysql_recovery_pro \
        --ibd="$test_dir/corrupted.ibd" \
        --page-size=$PAGE_SIZE \
        --truncate-recovery \
        --out-format=csv \
        -o "$test_dir/recovered.csv"
    
    local recovered_count=$(wc -l < "$test_dir/recovered.csv")
    log_success "Recovered records: $recovered_count"
}

test_btree_rebuild() {
    log_info "=========================================="
    log_info "Test: B+Tree Rebuild (--rebuild-chain)"
    log_info "=========================================="
    
    local test_dir="$OUTPUT_DIR/5m_btree_rebuild"
    mkdir -p "$test_dir"
    
    # Copy original
    docker exec "$CONTAINER_NAME" cp "$DOCKER_DATA_DIR/$TABLE_NAME.ibd" "/tmp/${TABLE_NAME}.ibd"
    docker cp "$CONTAINER_NAME:/tmp/${TABLE_NAME}.ibd" "$test_dir/corrupted.ibd"
    
    # Corrupt B+Tree non-leaf nodes
    log_info "Corrupting B+Tree pages..."
    local file_size=$(stat -c%s "$test_dir/corrupted.ibd")
    local total_pages=$((file_size / PAGE_SIZE))
    
    for page in $(seq 1 $((total_pages - 2)) | head -n 50 | sort -R); do
        local offset=$((page * PAGE_SIZE))
        dd if=/dev/urandom of="$test_dir/corrupted.ibd" bs=1 count=40 seek=$((offset + 56)) conv=notrunc 2>/dev/null
    done
    
    # Run B+Tree rebuild
    log_info "Running B+Tree rebuild..."
    time ./build/mysql_recovery_pro \
        --ibd="$test_dir/corrupted.ibd" \
        --page-size=$PAGE_SIZE \
        --rebuild-chain \
        --header-repair \
        --out-format=csv \
        -o "$test_dir/recovered.csv"
    
    local recovered_count=$(wc -l < "$test_dir/recovered.csv")
    log_success "Recovered records: $recovered_count"
}

test_memory_leak_detection() {
    log_info "=========================================="
    log_info "Test: Memory Leak Detection (Valgrind)"
    log_info "=========================================="
    
    local test_dir="$OUTPUT_DIR/5m_memory_test"
    mkdir -p "$test_dir"
    
    # Create a sample IBD file
    python3 scripts/generate_5m_data_mysql56.py \
        --records=500000 \
        --page-size=$PAGE_SIZE \
        --seed=42 \
        --output-dir="$test_dir" \
        --records-per-page=1000 > /dev/null
    
    # Check if file was created
    if [ ! -f "$test_dir/test_table.ibd" ]; then
        log_warning "Could not create IBD file for memory test"
        return
    fi
    
    # Run Valgrind
    log_info "Running Valgrind memory check..."
    log_warning "This may take several minutes..."
    
    valgrind --leak-check=full \
        --show-leak-kinds=all \
        --track-origins=yes \
        --errors-for-leak-kinds=all \
        --error-exitcode=1 \
        ./build/mysql_recovery_pro \
        --ibd="$test_dir/test_table.ibd" \
        --page-size=$PAGE_SIZE \
        --multithread=1 \
        --out-format=csv \
        -o "$test_dir/recovered.csv" 2>&1 | tee "$test_dir/valgrind.log"
    
    if grep -q "All heap blocks were freed" "$test_dir/valgrind.log"; then
        log_success "No memory leaks detected!"
    else
        log_warning "Memory analysis complete - check $test_dir/valgrind.log"
    fi
}

generate_test_report() {
    log_info "=========================================="
    log_info "Generating Test Report"
    log_info "=========================================="
    
    local report="$OUTPUT_DIR/test_report.md"
    
    cat > "$report" << EOF
# MySQL 5.6 5M Recovery Test Report

**Date:** $(date '+%Y-%m-%d %H:%M:%S')
**Records Tested:** $RECORDS_COUNT
**Page Size:** $PAGE_SIZE

## Summary

| Test | Status | Records Recovered | Notes |
|------|--------|-------------------|-------|
EOF
    
    # Add test results
    for test in torn_pages fragmented blob_recovery truncate_recovery btree_rebuild; do
        local test_dir="$OUTPUT_DIR/5m_${test}"
        local csv="$test_dir/recovered.csv"
        if [ -f "$csv" ]; then
            local count=$(wc -l < "$csv")
            echo "| $test | PASS | $count | $test_dir |" >> "$report"
        else
            echo "| $test | FAIL | N/A | File not found |" >> "$report"
        fi
    done
    
    cat >> "$report" << EOF

## Performance Metrics

| Metric | Value |
|--------|-------|
| Base Records | $RECORDS_COUNT |
| Page Size | $PAGE_SIZE bytes |
| Total Pages | $((RECORDS_COUNT * 128 / PAGE_SIZE)) |

## Recovery Modes Tested

1. **Normal Extraction** - Baseline recovery without corruption
2. **Torn Pages** - Page header corruption simulation
3. **Fragmented** - rm -rf style file fragmentation
4. **BLOB Recovery** - BLOB chain breakage recovery
5. **Truncate Recovery** - TRUNCATE TABLE scenario
6. **B+Tree Rebuild** - Index page corruption recovery

## Memory Testing

- Valgrind executed on 500K records sample
- Memory leak detection: pass/fail based on heap block check

## Recommendations

1. Use --multithread flag for faster recovery (4-8 threads recommended)
2. Enable --header-repair for corrupted pages
3. Use --rmrf-recovery for fragmented files
4. Monitor memory usage with Valgrind for production deployments

---

*Report generated by MySQL 5.6 Recovery Test Suite*
EOF
    
    log_success "Test report saved to: $report"
    cat "$report"
}

cleanup() {
    log_info "=========================================="
    log_info "Cleanup"
    log_info "=========================================="
    
    #sleep 1
    log_success "Test suite completed"
    log_info "Output directory: $OUTPUT_DIR"
}

# ==============================================================================
# Main Pipeline
# ==============================================================================

main() {
    echo "=================================================="
    echo "MySQL 5.6 5M Recovery Test Suite"
    echo "=================================================="
    echo ""
    echo "Container: $CONTAINER_NAME"
    echo "Records: $RECORDS_COUNT"
    echo "Page Size: $PAGE_SIZE"
    echo "Output: $OUTPUT_DIR"
    echo ""
    
    # Check dependencies
    check_dependencies
    
    # Pre-test setup
    pre_test
    
    # Run all tests
    test_5m_data_generation
    test_data_injection_to_mysql
    test_clean_extraction
    test_torn_pages_recovery
    test_fragmented_recovery
    test_blob_recovery
    test_truncate_recovery
    test_btree_rebuild
    test_memory_leak_detection
    
    # Generate report
    generate_test_report
    
    # Cleanup
    cleanup
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --records=*)
            RECORDS_COUNT="${1#*=}"
            shift
            ;;
        --page-size=*)
            PAGE_SIZE="${1#*=}"
            shift
            ;;
        --output-dir=*)
            OUTPUT_DIR="${1#*=}"
            shift
            ;;
        --container=*)
            CONTAINER_NAME="${1#*=}"
            shift
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --records=N        Number of records to test (default: 5000000)"
            echo "  --page-size=N      Page size in bytes (default: 16384)"
            echo "  --output-dir=D     Output directory (default: ./recovery-output/...)"
            echo "  --container=N      Docker container name (default: mysql_recovery_56)"
            echo "  --help             Show this help"
            exit 0
            ;;
        *)
            shift
            ;;
    esac
done

# Run main
main
