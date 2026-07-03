#!/bin/bash
# XFS Disaster Recovery Quick Validation Script
# Tests MySQL 5.6-9.0 with 100K rows on XFS filesystem
# Validates: rm -rf, DROP TABLE, TRUNCATE TABLE recovery

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_BASE="/app/mysql_recovery_test/xfs_validation_$(date +%Y%m%d_%H%M%S)"
BLOCK_DEVICE="/dev/nvme0n2p1"
MOUNT_POINT="/mnt/xfs_test"
RECOVERY_TOOL="/app/mysql_recovery_pro/build/mysql_recovery_pro"

# Test configuration
MYSQL_VERSIONS=("5.6" "5.7" "8.0" "8.4" "9.0")
DISASTER_TYPES=("rm_rf" "drop_table" "truncate")
NUM_ROWS=100000
NUM_WORKERS=8

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[$(date +%H:%M:%S)] [INFO]${NC} $1"
}

log_pass() {
    echo -e "${GREEN}[$(date +%H:%M:%S)] [PASS]${NC} $1"
}

log_fail() {
    echo -e "${RED}[$(date +%H:%M:%S)] [FAIL]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[$(date +%H:%M:%S)] [WARN]${NC} $1"
}

cleanup() {
    log_info "Cleaning up..."
    podman ps -q | xargs -r podman rm -f 2>/dev/null || true
    if mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
        umount "$MOUNT_POINT" || true
    fi
    rm -rf "$MOUNT_POINT" 2>/dev/null || true
}

trap cleanup EXIT

setup_xfs() {
    log_info "Setting up XFS filesystem on $BLOCK_DEVICE..."

    # Unmount if mounted
    if mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
        umount "$MOUNT_POINT"
    fi

    # Create mount point
    mkdir -p "$MOUNT_POINT"

    # Format as XFS
    wipefs -a "$BLOCK_DEVICE" 2>/dev/null || true
    mkfs.xfs -f "$BLOCK_DEVICE" > /dev/null 2>&1

    # Mount XFS
    mount -t xfs "$BLOCK_DEVICE" "$MOUNT_POINT"

    log_pass "XFS filesystem ready at $MOUNT_POINT"
}

start_mysql() {
    local version=$1
    local container_name="mysql_xfs_${version//.}"
    local data_dir="$MOUNT_POINT/mysql_${version//.}"

    log_info "Starting MySQL $version container..."

    # Remove existing container
    podman rm -f "$container_name" > /dev/null 2>&1 || true

    # Create data directory
    mkdir -p "$data_dir"

    # Start MySQL container
    podman run -d \
        --name "$container_name" \
        -e MYSQL_ROOT_PASSWORD=root123 \
        -e MYSQL_DATABASE=testdb \
        -v "$data_dir:/var/lib/mysql" \
        "mysql:$version" > /dev/null

    # Wait for MySQL to be ready
    local max_wait=180
    local waited=0
    while [ $waited -lt $max_wait ]; do
        if podman exec "$container_name" mysqladmin -uroot -proot123 ping 2>/dev/null | grep -q "alive"; then
            log_pass "MySQL $version is ready"
            return 0
        fi
        sleep 1
        ((waited++))
    done

    log_fail "MySQL $version failed to start within ${max_wait}s"
    return 1
}

create_test_table() {
    local version=$1
    local container_name="mysql_xfs_${version//.}"

    log_info "Creating test table with all field types..."

    # Create table SQL based on version
    local create_sql="DROP TABLE IF EXISTS testdb.full_types;
CREATE TABLE testdb.full_types (
    id INT PRIMARY KEY AUTO_INCREMENT,
    col_tinyint TINYINT,
    col_smallint SMALLINT,
    col_mediumint MEDIUMINT,
    col_int INT,
    col_bigint BIGINT,
    col_float FLOAT,
    col_double DOUBLE,
    col_decimal DECIMAL(15,4),
    col_char CHAR(50),
    col_varchar VARCHAR(255),
    col_text TEXT,
    col_blob BLOB,
    col_date DATE,
    col_time TIME,
    col_datetime DATETIME,
    col_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    col_year YEAR,
    col_binary BINARY(16),
    col_bit BIT(8),
    col_bool BOOLEAN"

    # Add JSON for MySQL 5.7+
    if [[ "$version" != "5.6" ]]; then
        create_sql+=",
    col_json JSON"
    fi

    # Add ENUM/SET for MySQL 5.6/5.7
    if [[ "$version" == "5.6" || "$version" == "5.7" ]]; then
        create_sql+=",
    col_enum ENUM('active','inactive','pending'),
    col_set SET('read','write','execute')"
    fi

    create_sql+=") ENGINE=InnoDB ROW_FORMAT=DYNAMIC;"

    podman exec -i "$container_name" mysql -uroot -proot123 -e "$create_sql" > /dev/null 2>&1

    log_pass "Test table created"
}

insert_data() {
    local version=$1
    local container_name="mysql_xfs_${version//.}"

    log_info "Inserting $NUM_ROWS rows..."

    local batch_size=1000
    local total_batches=$(( (NUM_ROWS + batch_size - 1) / batch_size ))

    for ((batch=0; batch<total_batches; batch++)); do
        local start_id=$((batch * batch_size + 1))
        local end_id=$(( (batch + 1) * batch_size ))
        [ $end_id -gt $NUM_ROWS ] && end_id=$NUM_ROWS

        # Generate values
        local values=""
        for ((i=start_id; i<=end_id; i++)); do
            if [[ "$version" == "5.6" ]]; then
                values+="($i, $((i%128)), $((i%32768)), $((i%8388607)), $i, $((i*1000)), $(awk "BEGIN {printf \"%.1f\", $i*1.5}"), $(awk "BEGIN {printf \"%.1f\", $i*2.5}"), $i.1234, 'User_$i', 'Description_$i', 'Text_$i', X'ABCD', '2026-07-03', '12:30:45', '2026-07-03 12:30:45', '2026', X'00000000000000000000000000000000', b'$((i%256))', $((i%2)), 'active', 'read'),"
            elif [[ "$version" == "5.7" ]]; then
                values+="($i, $((i%128)), $((i%32768)), $((i%8388607)), $i, $((i*1000)), $(awk "BEGIN {printf \"%.1f\", $i*1.5}"), $(awk "BEGIN {printf \"%.1f\", $i*2.5}"), $i.1234, 'User_$i', 'Description_$i', 'Text_$i', X'ABCD', '2026-07-03', '12:30:45', '2026-07-03 12:30:45', '2026', X'00000000000000000000000000000000', b'$((i%256))', $((i%2)), '{\"id\":$i}', 'active', 'read'),"
            else
                values+="($i, $((i%128)), $((i%32768)), $((i%8388607)), $i, $((i*1000)), $(awk "BEGIN {printf \"%.1f\", $i*1.5}"), $(awk "BEGIN {printf \"%.1f\", $i*2.5}"), $i.1234, 'User_$i', 'Description_$i', 'Text_$i', X'ABCD', '2026-07-03', '12:30:45', '2026-07-03 12:30:45', '2026', X'00000000000000000000000000000000', b'$((i%256))', $((i%2)), '{\"id\":$i}'),"
            fi
        done

        # Remove trailing comma
        values="${values%,}"

        local insert_sql="INSERT INTO testdb.full_types (
            col_tinyint, col_smallint, col_mediumint, col_int, col_bigint,
            col_float, col_double, col_decimal,
            col_char, col_varchar, col_text, col_blob,
            col_date, col_time, col_datetime, col_year, col_binary, col_bit, col_bool"

        if [[ "$version" != "5.6" ]]; then
            insert_sql+=", col_json"
        fi

        if [[ "$version" == "5.6" || "$version" == "5.7" ]]; then
            insert_sql+=", col_enum, col_set"
        fi

        insert_sql+=") VALUES $values;"

        podman exec -i "$container_name" mysql -uroot -proot123 testdb <<< "$insert_sql" > /dev/null 2>&1

        if [ $(( (batch + 1) % 10 )) -eq 0 ]; then
            log_info "Inserted $(( (batch + 1) * batch_size )) rows..."
        fi
    done

    log_pass "All $NUM_ROWS rows inserted"
}

flush_to_disk() {
    local container_name=$1

    log_info "Flushing data to disk..."

    podman exec "$container_name" mysql -uroot -proot123 -e "SET GLOBAL innodb_flush_log_at_trx_commit=1;" > /dev/null 2>&1
    podman exec "$container_name" mysql -uroot -proot123 -e "SET GLOBAL innodb_max_dirty_pages_pct=0;" > /dev/null 2>&1
    podman exec "$container_name" mysql -uroot -proot123 -e "FLUSH NO_WRITE_TO_BINLOG ENGINE INNODB;" > /dev/null 2>&1
    podman exec "$container_name" mysql -uroot -proot123 -e "FLUSH NO_WRITE_TO_BINLOG TABLES;" > /dev/null 2>&1
    podman exec "$container_name" sync > /dev/null 2>&1

    sleep 10

    log_pass "Data flushed to disk"
}

execute_disaster() {
    local version=$1
    local disaster=$2
    local container_name="mysql_xfs_${version//.}"
    local data_dir="$MOUNT_POINT/mysql_${version//.}"

    log_info "Executing disaster: $disaster"

    case "$disaster" in
        "rm_rf")
            podman stop "$container_name" > /dev/null 2>&1 || true
            rm -rf "$data_dir"
            log_pass "Data directory deleted (rm -rf)"
            ;;
        "drop_table")
            podman exec "$container_name" mysql -uroot -proot123 -e "DROP TABLE testdb.full_types;" > /dev/null 2>&1
            flush_to_disk "$container_name"
            log_pass "Table dropped"
            ;;
        "truncate")
            podman exec "$container_name" mysql -uroot -proot123 -e "TRUNCATE TABLE testdb.full_types;" > /dev/null 2>&1
            flush_to_disk "$container_name"
            log_pass "Table truncated"
            ;;
    esac
}

run_recovery() {
    local version=$1
    local disaster=$2
    local output_dir="$TEST_BASE/recovery_${version}_${disaster}"

    log_info "Running XFS disk carving recovery..."

    mkdir -p "$output_dir"

    local start_time=$(date +%s)

    "$RECOVERY_TOOL" \
        --disk-carving \
        --device-path "$BLOCK_DEVICE" \
        --filesystem xfs \
        --output "$output_dir" \
        --num-workers $NUM_WORKERS \
        --verbose > "$output_dir/recovery.log" 2>&1

    local exit_code=$?
    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))

    if [ $exit_code -eq 0 ]; then
        log_pass "Recovery completed in ${elapsed}s"

        # Analyze results
        local csv_count=$(find "$output_dir" -name "*.csv" | wc -l)
        local total_rows=0
        for csv in "$output_dir"/*.csv; do
            [ -f "$csv" ] && total_rows=$((total_rows + $(wc -l < "$csv" | awk '{print $1}') - 1))
        done

        log_info "Recovery results:"
        log_info "  CSV files: $csv_count"
        log_info "  Total rows: $total_rows"
        log_info "  Recovery rate: $(awk "BEGIN {printf \"%.1f\", ($total_rows / $NUM_ROWS) * 100}")%"

        # Check data quality
        if [ $total_rows -ge $((NUM_ROWS * 90 / 100)) ]; then
            log_pass "Recovery rate >= 90%: EXCELLENT"
            return 0
        elif [ $total_rows -ge $((NUM_ROWS * 70 / 100)) ]; then
            log_warn "Recovery rate >= 70%: GOOD"
            return 0
        else
            log_fail "Recovery rate < 70%: POOR"
            return 1
        fi
    else
        log_fail "Recovery failed (exit code: $exit_code)"
        return 1
    fi
}

run_single_test() {
    local version=$1
    local disaster=$2

    log_info "========================================="
    log_info "Testing MySQL $version - $disaster"
    log_info "========================================="

    local container_name="mysql_xfs_${version//.}"

    # Setup
    setup_xfs

    # Start MySQL
    start_mysql "$version" || return 1

    # Create table
    create_test_table "$version" || return 1

    # Insert data
    insert_data "$version" || return 1

    # Flush to disk
    flush_to_disk "$container_name" || return 1

    # Get actual row count
    local actual_rows=$(podman exec "$container_name" mysql -uroot -proot123 -Nse "SELECT COUNT(*) FROM testdb.full_types;" 2>/dev/null)
    log_info "Actual rows in table: $actual_rows"

    # Execute disaster
    execute_disaster "$version" "$disaster" || return 1

    # Run recovery
    run_recovery "$version" "$disaster" || return 1

    return 0
}

# Main
main() {
    log_info "XFS Disaster Recovery Validation Test"
    log_info "Test directory: $TEST_BASE"
    log_info "Block device: $BLOCK_DEVICE"
    log_info "Rows per test: $NUM_ROWS"
    log_info "Workers: $NUM_WORKERS"

    mkdir -p "$TEST_BASE"

    # Initialize summary
    echo "XFS Disaster Recovery Test Results" > "$TEST_BASE/summary.txt"
    echo "Date: $(date)" >> "$TEST_BASE/summary.txt"
    echo "Rows: $NUM_ROWS" >> "$TEST_BASE/summary.txt"
    echo "================================" >> "$TEST_BASE/summary.txt"

    local total_tests=0
    local passed_tests=0

    for version in "${MYSQL_VERSIONS[@]}"; do
        for disaster in "${DISASTER_TYPES[@]}"; do
            ((total_tests++))

            if run_single_test "$version" "$disaster"; then
                ((passed_tests++))
                echo "MySQL $version - $disaster: PASS" >> "$TEST_BASE/summary.txt"
            else
                echo "MySQL $version - $disaster: FAIL" >> "$TEST_BASE/summary.txt"
            fi
        done
    done

    log_info "========================================="
    log_info "TEST SUMMARY"
    log_info "========================================="
    log_info "Total tests: $total_tests"
    log_info "Passed: $passed_tests"
    log_info "Failed: $((total_tests - passed_tests))"
    log_info "Success rate: $(awk "BEGIN {printf \"%.1f\", ($passed_tests / $total_tests) * 100}")%"

    cat "$TEST_BASE/summary.txt"

    if [ $passed_tests -eq $total_tests ]; then
        log_pass "All tests passed!"
        exit 0
    else
        log_fail "Some tests failed"
        exit 1
    fi
}

main "$@"
