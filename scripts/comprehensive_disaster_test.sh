#!/bin/bash
# Comprehensive Disaster Recovery Test for ext4 and XFS
# Uses real MySQL instances to generate properly checksummed InnoDB data,
# then tests DROP TABLE, TRUNCATE, and rm -rf recovery scenarios.
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="${SCRIPT_DIR}/../build/mysql_recovery_pro"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
TEST_DIR="/tmp/disaster_test_${TIMESTAMP}"
RESULTS_DIR="${TEST_DIR}/results"
DISK_SIZE_MB=500
TEST_ROWS=2000
MYSQL_USER="root"
TES_DB="disaster_test"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $(date '+%H:%M:%S') $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $(date '+%H:%M:%S') $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $(date '+%H:%M:%S') $1"; }
log_step()  { echo -e "${BLUE}[STEP]${NC} $(date '+%H:%M:%S') $1"; }

# ===============================================================
setup() {
    log_info "Setting up test environment at ${TEST_DIR}"
    mkdir -p "${TEST_DIR}"/{xfs_mount,ext4_mount}
    mkdir -p "${RESULTS_DIR}"

    # Create disk images
    log_info "Creating disk images (${DISK_SIZE_MB}MB each)"
    dd if=/dev/zero of="${TEST_DIR}/xfs_disk.img" bs=1M count=${DISK_SIZE_MB} status=none
    dd if=/dev/zero of="${TEST_DIR}/ext4_disk.img" bs=1M count=${DISK_SIZE_MB} status=none

    # Setup loop devices
    XFS_LOOP=$(losetup -f --show "${TEST_DIR}/xfs_disk.img")
    EXT4_LOOP=$(losetup -f --show "${TEST_DIR}/ext4_disk.img")
    log_info "Loop devices: xfs=${XFS_LOOP}, ext4=${EXT4_LOOP}"
    echo "${XFS_LOOP} ${EXT4_LOOP}" > "${TEST_DIR}/loops.txt"

    # Format filesystems
    log_info "Formatting XFS on ${XFS_LOOP}"
    mkfs.xfs -f -q "${XFS_LOOP}" 2>&1
    log_info "Formatting ext4 on ${EXT4_LOOP}"
    mkfs.ext4 -F -q "${EXT4_LOOP}" 2>&1

    # Mount
    log_info "Mounting filesystems"
    mount "${XFS_LOOP}" "${TEST_DIR}/xfs_mount"
    mount "${EXT4_LOOP}" "${TEST_DIR}/ext4_mount"

    # Create MySQL data dirs
    for fs in xfs ext4; do
        mkdir -p "${TEST_DIR}/${fs}_mount/mysql"
        mkdir -p "${TEST_DIR}/${fs}_mount/logs"
    done

    log_info "Setup complete"
}

# ===============================================================
create_and_populate() {
    local fs_type=$1
    local data_dir="${TEST_DIR}/${fs_type}_mount/mysql"
    local socket="${TEST_DIR}/${fs_type}_mount/mysql.sock"
    local pid_file="${TEST_DIR}/${fs_type}_mount/mysql.pid"
    local log_file="${TEST_DIR}/${fs_type}_mount/logs/mysql.log"

    log_step "[${fs_type^^}] Creating MySQL instance..."

    # Initialize MySQL data directory if needed
    if [ ! -f "${data_dir}/ibdata1" ]; then
        log_info "[${fs_type^^}] Initializing MySQL data directory..."
        mysqld --initialize-insecure --datadir="${data_dir}" --user=root 2>&1 | tail -3
    fi

    # Start MySQL with minimal config
    log_info "[${fs_type^^}] Starting MySQL..."
    mysqld --datadir="${data_dir}" \
           --user=root \
           --socket="${socket}" \
           --port=0 \
           --skip-networking \
           --innodb-file-per-table=ON \
           --innodb-flush-method=fsync \
           --innodb-flush-log-at-trx-commit=1 \
           --sync-binlog=1 \
           --innodb-buffer-pool-size=32M \
           --innodb-log-file-size=16M \
           --innodb-log-buffer-size=8M \
           --early-plugin-load=keyring_file.so \
           --keyring_file_data="${TEST_DIR}/${fs_type}_mount/keyring" \
           --log-error="${log_file}" \
           --pid-file="${pid_file}" &

    local mysql_pid=$!
    echo $mysql_pid > "${pid_file}"

    # Wait for MySQL to be ready
    local retries=60
    while [ $retries -gt 0 ]; do
        if mysql -u root -S "${socket}" -e "SELECT 1" &>/dev/null; then
            break
        fi
        sleep 1
        retries=$((retries - 1))
        if [ $((retries % 10)) -eq 0 ]; then
            log_info "[${fs_type^^}] Waiting for MySQL... (${retries}s left)"
        fi
    done

    if [ $retries -eq 0 ]; then
        log_error "[${fs_type^^}] MySQL failed to start. Check log: ${log_file}"
        return 1
    fi

    log_info "[${fs_type^^}] MySQL started (PID: ${mysql_pid})"

    # Create database
    mysql -u root -S "${socket}" -e "CREATE DATABASE IF NOT EXISTS ${TES_DB};"

    # Create test tables
    log_info "[${fs_type^^}] Creating test tables..."

    mysql -u root -S "${socket}" -D "${TES_DB}" <<'EOSQL'
-- Table 1: All field types (25+ types)
DROP TABLE IF EXISTS all_types;
CREATE TABLE all_types (
    id INT PRIMARY KEY AUTO_INCREMENT,
    tinyint_col TINYINT,
    smallint_col SMALLINT,
    mediumint_col MEDIUMINT,
    int_col INT,
    bigint_col BIGINT,
    float_col FLOAT,
    double_col DOUBLE,
    decimal_col DECIMAL(15,2),
    char_col CHAR(50),
    varchar_col VARCHAR(255),
    text_col TEXT,
    mediumtext_col MEDIUMTEXT,
    blob_col BLOB,
    mediumblob_col MEDIUMBLOB,
    date_col DATE,
    time_col TIME,
    datetime_col DATETIME,
    timestamp_col TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    year_col YEAR,
    enum_col ENUM('active','inactive','pending'),
    set_col SET('read','write','execute'),
    json_col JSON,
    binary_col BINARY(16),
    varbinary_col VARBINARY(255),
    bit_col BIT(8),
    bool_col BOOLEAN
) ENGINE=InnoDB;

-- Table 2: Large BLOBs (multi-page chains)
DROP TABLE IF EXISTS large_blobs;
CREATE TABLE large_blobs (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100),
    data LONGBLOB,
    description TEXT
) ENGINE=InnoDB;

-- Table 3: Many small rows
DROP TABLE IF EXISTS many_rows;
CREATE TABLE many_rows (
    id INT PRIMARY KEY AUTO_INCREMENT,
    value INT,
    data VARCHAR(100)
) ENGINE=InnoDB;

-- Table 4: TDE encrypted (if keyring plugin available)
DROP TABLE IF EXISTS tde_table;
CREATE TABLE tde_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    secret_data VARCHAR(500),
    notes TEXT
) ENGINE=InnoDB ENCRYPTION='Y';

-- Table 5: Compressed
DROP TABLE IF EXISTS comp_table;
CREATE TABLE comp_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    chunk INT,
    payload VARCHAR(500)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;

-- Table 6: Instant DDL history
DROP TABLE IF EXISTS instant_table;
CREATE TABLE instant_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    col_a VARCHAR(100),
    col_b INT DEFAULT 42,
    col_c VARCHAR(50) DEFAULT 'default_val'
) ENGINE=InnoDB;

-- Table 7: TDE + compressed combined
DROP TABLE IF EXISTS tde_comp_table;
CREATE TABLE tde_comp_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    secret_chunk INT,
    secret_payload VARCHAR(500)
) ENGINE=InnoDB ENCRYPTION='Y' ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;
EOSQL

    log_info "[${fs_type^^}] Inserting test data (${TEST_ROWS} rows)..."

    # Generate and insert data for all_types using Python
    python3 - "${TEST_ROWS}" > "${TEST_DIR}/insert_data_${fs_type}.sql" <<'EOPYTHON'
import sys, hashlib, json, random

n = int(sys.argv[1])

# Build INSERT for all_types
rows = []
for i in range(1, n + 1):
    row = (
        f"({i},"
        f"{(i-128)%256-128},"                         # tinyint
        f"{(i*7)%65536-32768},"                         # smallint
        f"{(i*13)%16777216-8388608},"                    # mediumint
        f"{(i*31)%4294967296-2147483648},"               # int
        f"{i*999999999},"                                 # bigint
        f"{i*1.5},"                                      # float
        f"{i*2.718281828},"                              # double
        f"{i*123.45},"                                   # decimal
        f"'char_{i:05d}',"                              # char
        f"'varchar_row_{i}_padding_abcdefghijklmnop',"  # varchar
        f"'Text content for row {i}. ',"                 # text
        f"CONCAT('Medium text row {i} ', REPEAT('AB', {20+(i%30)})),"  # mediumtext
        f"REPEAT(X'DEADBEEFCAFE', {5+(i%10)}),"         # blob
        f"REPEAT(X'FACEB00CDEAD', {10+(i%15)}),"         # mediumblob
        f"DATE_ADD('2020-01-01', INTERVAL {i} DAY),"     # date
        f"SEC_TO_TIME({(i*137)%86400}),"                # time
        f"DATE_ADD('2020-01-01 00:00:00', INTERVAL {i*137} SECOND),"  # datetime
        f"NULL,"                                          # timestamp (default)
        f"{2000+(i%26)},"                                # year
        f"{i%3+1},"                                      # enum
        f"{i%7+1},"                                      # set
        f"JSON_OBJECT('key', 'value_{i}', 'num', {i}, 'arr', JSON_ARRAY(1,2,3)),"  # json
        f"UNHEX('{hashlib.md5(str(i).encode()).hexdigest()}'),"  # binary(16)
        f"UNHEX('{hashlib.sha256(str(i).encode()).hexdigest()[:64]}'),"  # varbinary(255)
        f"{i%256},"                                      # bit(8)
        f"{i%2}"                                          # bool
        f")"
    )
    rows.append(row)

print(f"INSERT INTO all_types VALUES {','.join(rows)};")
EOPYTHON

    mysql -u root -S "${socket}" -D "${TES_DB}" < "${TEST_DIR}/insert_data_${fs_type}.sql" 2>&1 | head -1

    # Insert large BLOBs
    log_info "[${fs_type^^}] Inserting large BLOBs..."
    local blob_count=30
    for i in $(seq 1 $blob_count); do
        mysql -u root -S "${socket}" -D "${TES_DB}" -e \
            "INSERT INTO large_blobs VALUES ($i, 'blob_$i', REPEAT(X'AABBCCDEEFF00112233445566778899AABBCCDEEFF', 200), REPEAT('Large text data for BLOB row $i. ', 100))"
    done

    # Insert many_rows
    log_info "[${fs_type^^}] Inserting many rows..."
    python3 - "${TEST_ROWS}" > "${TEST_DIR}/insert_many_${fs_type}.sql" <<'EOPYTHON'
import sys
n = int(sys.argv[1])
values = [f"({i}, {i*7}, 'row_{i}_data_abcdefghij')" for i in range(1, n+1)]
print(f"INSERT INTO many_rows VALUES {','.join(values)};")
EOPYTHON
    mysql -u root -S "${socket}" -D "${TES_DB}" < "${TEST_DIR}/insert_many_${fs_type}.sql" 2>&1 | head -1

    # Insert TDE table data
    log_info "[${fs_type^^}] Inserting TDE table data..."
    python3 - "${TEST_ROWS}" > "${TEST_DIR}/insert_tde_${fs_type}.sql" <<'EOPYTHON'
import sys
n = int(sys.argv[1])
values = [f"({i}, CONCAT('secret_data_row_{i}_', REPEAT('X', 100)), 'notes for row {i}')" for i in range(1, n+1)]
print(f"INSERT INTO tde_table VALUES {','.join(values)};")
EOPYTHON
    mysql -u root -S "${socket}" -D "${TES_DB}" < "${TEST_DIR}/insert_tde_${fs_type}.sql" 2>&1 | head -1

    # Insert compressed table data
    log_info "[${fs_type^^}] Inserting compressed table data..."
    python3 - "${TEST_ROWS}" > "${TEST_DIR}/insert_comp_${fs_type}.sql" <<'EOPYTHON'
import sys
n = int(sys.argv[1])
values = [f"({i}, {i%100}, REPEAT('compressed_payload_row_{i}_', 10))" for i in range(1, n+1)]
print(f"INSERT INTO comp_table VALUES {','.join(values)};")
EOPYTHON
    mysql -u root -S "${socket}" -D "${TES_DB}" < "${TEST_DIR}/insert_comp_${fs_type}.sql" 2>&1 | head -1

    # Insert instant DDL table data + perform INSTANT ADD/DROP COLUM
    log_info "[${fs_type^^}] Inserting instant DDL table data..."
    python3 - "${TEST_ROWS}" > "${TEST_DIR}/insert_instant_${fs_type}.sql" <<'EOPYTHON'
import sys
n = int(sys.argv[1])
values = [f"({i}, 'data_row_{i}', {i*11}, 'val_{i}')" for i in range(1, n+1)]
print(f"INSERT INTO instant_table VALUES {','.join(values)};")
EOPYTHON
    mysql -u root -S "${socket}" -D "${TES_DB}" < "${TEST_DIR}/insert_instant_${fs_type}.sql" 2>&1 | head -1

    # Perform INSTANT DDL operations
    log_info "[${fs_type^^}] Performing INSTANT DDL operations..."
    mysql -u root -S "${socket}" -D "${TES_DB}" -e "
        ALTER TABLE instant_table ADD COLUMN col_d VARCHAR(100) DEFAULT 'instant_added', ALGORITHM=INSTANT;
        ALTER TABLE instant_table ADD COLUMN col_e INT DEFAULT 99, ALGORITHM=INSTANT;
        INSERT INTO instant_table (col_a, col_b, col_c, col_d, col_e) VALUES ('after_ddl_1', 100, 'new_c', 'new_d', 200);
        INSERT INTO instant_table (col_a, col_b, col_c, col_d, col_e) VALUES ('after_ddl_2', 101, 'new_c2', 'new_d2', 201);
    " 2>&1

    # Insert TDE+compressed table
    log_info "[${fs_type^^}] Inserting TDE+compressed table data..."
    python3 - "${TEST_ROWS}" > "${TEST_DIR}/insert_tde_comp_${fs_type}.sql" <<'EOPYTHON'
import sys
n = int(sys.argv[1])
values = [f"({i}, {i%50}, REPEAT('tde_comp_data_{i}_', 10))" for i in range(1, n+1)]
print(f"INSERT INTO tde_comp_table VALUES {','.join(values)};")
EOPYTHON
    mysql -u root -S "${socket}" -D "${TES_DB}" < "${TEST_DIR}/insert_tde_comp_${fs_type}.sql" 2>&1 | head -1

    # Force flush everything to disk
    log_info "[${fs_type^^}] Flushing tables to disk..."
    mysql -u root -S "${socket}" -D "${TES_DB}" -e "
        FLUSH TABLES;
        SET GLOBAL innodb_fast_shutdown=0;
    "

    # Create golden reference data (CSV exports)
    log_info "[${fs_type^^}] Creating golden reference data..."
    for tbl in all_types many_rows instant_table; do
        mysql -u root -S "${socket}" -D "${TES_DB}" -e \
            "SELECT * FROM ${tbl}" --batch --raw > "${RESULTS_DIR}/${fs_type}_golen_${tbl}.csv"
    done
    mysql -u root -S "${socket}" -D "${TES_DB}" --skip-column-names -e \
        "SELECT COUNT(*) FROM all_types" > "${RESULTS_DIR}/${fs_type}_count_all_types.txt"
    mysql -u root -S "${socket}" -D "${TES_DB}" --skip-column-names -e \
        "SELECT COUNT(*) FROM many_rows" > "${RESULTS_DIR}/${fs_type}_count_many_rows.txt"
    mysql -u root -S "${socket}" -D "${TES_DB}" --skip-column-names -e \
        "SELECT COUNT(*) FROM instant_table" > "${RESULTS_DIR}/${fs_type}_count_instant_table.txt"
    mysql -u root -S "${socket}" -D "${TES_DB}" --skip-column-names -e \
        "SELECT COUNT(*) FROM tde_table" > "${RESULTS_DIR}/${fs_type}_count_tde_table.txt"
    mysql -u root -S "${socket}" -D "${TES_DB}" --skip-column-names -e \
        "SELECT COUNT(*) FROM comp_table" > "${RESULTS_DIR}/${fs_type}_count_comp_table.txt"

    local golden_count=$(wc -l < "${RESULTS_DIR}/${fs_type}_golen_all_types.csv")
    log_info "[${fs_type^^}] Golden data: ${golden_count} rows (all_types)"

    # Shutdown MySQL cleanly
    log_info "[${fs_type^^}] Shutting down MySQL..."
    mysqladmin -u root -S "${socket}" shutdown 2>/dev/null || kill $mysql_pid 2>/dev/null || true
    sleep 3
    wait $mysql_pid 2>/dev/null || true

    # Make sure everything is on disk
    sync
    echo "${TEST_ROWS}" > "${TEST_DIR}/${fs_type}_rows.txt"
    log_info "[${fs_type^^}] Data creation complete, MySQL stopped"
}

# ===============================================================
run_disaster_scenario() {
    local fs_type=$1
    local scenario=$2
    local data_dir="${TEST_DIR}/${fs_type}_mount/mysql/${TES_DB}"
    local socket="${TEST_DIR}/${fs_type}_mount/mysql.sock"
    local log_file="${TEST_DIR}/${fs_type}_mount/logs/mysql.log"
    local pid_file="${TEST_DIR}/${fs_type}_mount/mysql.pid"
    local device

    # Get the correct loop device
    if [ "$fs_type" = "xfs" ]; then
        device=$(awk '{print $1}' "${TEST_DIR}/loops.txt")
    else
        device=$(awk '{print $2}' "${TEST_DIR}/loops.txt")
    fi

    local output_dir="${RESULTS_DIR}/${fs_type}_${scenario}"
    mkdir -p "${output_dir}"

    log_step "[${fs_type^^}] Running scenario: ${scenario}"

    case $scenario in
        drop_table)
            # Start MySQL, drop tables, shutdown
            mysqld --datadir="${TEST_DIR}/${fs_type}_mount/mysql" \
                   --user=root \
                   --socket="${socket}" \
                   --port=0 \
                   --skip-networking \
                   --innodb-file-per-table=ON \
                   --innodb-flush-method=fsync \
                   --early-plugin-load=keyring_file.so \
                   --keyring_file_data="${TEST_DIR}/${fs_type}_mount/keyring" \
                   --log-error="${log_file}" \
                   --pid-file="${pid_file}" &
            local mysql_pid=$!
            sleep 3

            # Wait for ready
            for i in $(seq 1 30); do
                mysql -u root -S "${socket}" -e "SELECT 1" &>/dev/null && break
                sleep 1
            done

            log_info "[${fs_type^^}] Dropping tables..."
            mysql -u root -S "${socket}" --batch -e "
                DROP TABLE ${TES_DB}.all_types;
                DROP TABLE ${TES_DB}.large_blobs;
                DROP TABLE ${TES_DB}.many_rows;
                DROP TABLE ${TES_DB}.tde_table;
                DROP TABLE ${TES_DB}.comp_table;
                DROP TABLE ${TES_DB}.instant_table;
                DROP TABLE ${TES_DB}.tde_comp_table;
            "
            sleep 2
            mysqladmin -u root -S "${socket}" shutdown 2>/dev/null || kill $mysql_pid 2>/dev/null || true
            sleep 2
            wait $mysql_pid 2>/dev/null || true

            # Unmount to prevent overwriting freed blocks
            umount "${TEST_DIR}/${fs_type}_mount" 2>/dev/null || true
            ;;

        truncate)
            # Start MySQL, TRUNCATE tables, shutdown
            mysqld --datadir="${TEST_DIR}/${fs_type}_mount/mysql" \
                   --user=root \
                   --socket="${socket}" \
                   --port=0 \
                   --skip-networking \
                   --innodb-file-per-table=ON \
                   --innodb-flush-method=fsync \
                   --early-plugin-load=keyring_file.so \
                   --keyring_file_data="${TEST_DIR}/${fs_type}_mount/keyring" \
                   --log-error="${log_file}" \
                   --pid-file="${pid_file}" &
            local mysql_pid=$!
            sleep 3
            for i in $(seq 1 30); do
                mysql -u root -S "${socket}" -e "SELECT 1" &>/dev/null && break
                sleep 1
            done

            log_info "[${fs_type^^}] Truncating tables..."
            mysql -u root -S "${socket}" --batch -e "
                TRUNCATE TABLE ${TES_DB}.all_types;
                TRUNCATE TABLE ${TES_DB}.large_blobs;
                TRUNCATE TABLE ${TES_DB}.many_rows;
                TRUNCATE TABLE ${TES_DB}.tde_table;
                TRUNCATE TABLE ${TES_DB}.comp_table;
                TRUNCATE TABLE ${TES_DB}.instant_table;
                TRUNCATE TABLE ${TES_DB}.tde_comp_table;
            "
            sleep 2
            mysqladmin -u root -S "${socket}" shutdown 2>/dev/null || kill $mysql_pid 2>/dev/null || true
            sleep 2
            wait $mysql_pid 2>/dev/null || true

            umount "${TEST_DIR}/${fs_type}_mount" 2>/dev/null || true
            ;;

        rm_rf)
            # Remove IBD files directly
            log_info "[${fs_type^^}] Removing .ibd files..."
            rm -f "${data_dir}"/*.ibd
            umount "${TEST_DIR}/${fs_type}_mount" 2>/dev/null || true
            ;;
    esac

    sync
    sleep 2

    # Run recovery on the block device
    local recovery_cmd="${BINARY}"
    local start_time=$(date +%s)

    log_info "[${fs_type^^}] Running disk carving recovery on ${device}..."

    if [ -f "${BINARY}" ] && [ -b "${device}" ]; then
        ${recovery_cmd} --disk-carving \
            --device-path="${device}" \
            --filesystem="${fs_type}" \
            --output="${output_dir}" \
            --page-size=16384 \
            --format=csv \
            --num-workers=4 \
            --verbose 2>&1 | tee "${output_dir}/recovery.log" || log_warn "[${fs_type^^}] Recovery exited with code $?"
    else
        log_error "[${fs_type^^}] Binary not found or device not accessible"
        if [ ! -f "${BINARY}" ]; then
            log_error "  Binary missing: ${BINARY}"
        fi
        if [ ! -b "${device}" ]; then
            log_error "  Device not a block device: ${device} (checking if file...)"
            if [ -f "${device}" ]; then
                log_info "  Using file device path instead"
                ${recovery_cmd} --disk-carving \
                    --device-path="${device}" \
                    --filesystem="${fs_type}" \
                    --output="${output_dir}" \
                    --page-size=16384 \
                    --format=csv \
                    --num-workers=4 \
                    --verbose 2>&1 | tee "${output_dir}/recovery.log" || log_warn "[${fs_type^^}] Recovery exited with code $?"
            fi
        fi
    fi

    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))

    # Analyze results — count all records across ALL CSV files
    # Schema-aware recovery produces named CSV files (all_types.csv, etc.)
    # Schema-less recovery produces index_N.csv files with raw parsed data
    local recovered_count=0
    local csv_file="${output_dir}/all_types.csv"
    if [ -f "$csv_file" ]; then
        recovered_count=$(wc -l < "$csv_file" 2>/dev/null || echo 0)
        recovered_count=$((recovered_count - 1 > 0 ? recovered_count - 1 : 0))
    else
        # Schema-less mode: count records across all CSV files
        recovered_count=0
        for f in "${output_dir}"/*.csv; do
            if [ -f "$f" ]; then
                local csv_lines
                csv_lines=$(wc -l < "$f" 2>/dev/null || echo 0)
                recovered_count=$((recovered_count + csv_lines - 1))  # Subtract header per file
            fi
        done
        recovered_count=$((recovered_count > 0 ? recovered_count : 0))
    fi

    local golden_count=$(cat "${RESULTS_DIR}/${fs_type}_count_all_types.txt" 2>/dev/null || echo 0)

    local recovery_rate=0
    if [ "$golden_count" -gt 0 ]; then
        recovery_rate=$(python3 -c "print(f'{(${recovered_count}/${golden_count})*100:.1f}')")
    fi

    # Count other tables recovered
    local many_rows_recovered=0
    if [ -f "${output_dir}/many_rows.csv" ]; then
        many_rows_recovered=$(($(wc -l < "${output_dir}/many_rows.csv" 2>/dev/null || echo 0) - 1))
    fi
    local many_rows_golden=$(cat "${RESULTS_DIR}/${fs_type}_count_many_rows.txt" 2>/dev/null || echo 0)

    local instant_recovered=0
    if [ -f "${output_dir}/instant_table.csv" ]; then
        instant_recovered=$(($(wc -l < "${output_dir}/instant_table.csv" 2>/dev/null || echo 0) - 1))
    fi
    local instant_golden=$(cat "${RESULTS_DIR}/${fs_type}_count_instant_table.txt" 2>/dev/null || echo 0)

    # Save summary JSON
    cat > "${output_dir}/summary.json" <<EOF
{
    "filesystem": "${fs_type}",
    "scenario": "${scenario}",
    "all_types": {"golden": ${golden_count}, "recovered": ${recovered_count}, "rate": "${recovery_rate}%"},
    "many_rows": {"golden": ${many_rows_golden}, "recovered": ${many_rows_recovered}},
    "instant_table": {"golden": ${instant_golden}, "recovered": ${instant_recovered}},
    "recovery_time_seconds": ${elapsed},
    "timestamp": "$(date -Iseconds)"
}
EOF

    log_info "[${fs_type^^}] Scenario ${scenario}: all_types=${recovered_count}/${golden_count} (${recovery_rate}%), many_rows=${many_rows_recovered}/${many_rows_golden}, time=${elapsed}s"

    # Remount for next scenario
    if [ "$scenario" != "rm_rf" ]; then
        mount "${device}" "${TEST_DIR}/${fs_type}_mount" 2>/dev/null || true
    fi
}

# ============================================================
cleanup() {
    log_info "Cleaning up..."
    for fs in xfs ext4; do
        umount "${TEST_DIR}/${fs}_mount" 2>/dev/null || true
    done
    if [ -f "${TEST_DIR}/loops.txt" ]; then
        for loop_dev in $(cat "${TEST_DIR}/loops.txt"); do
            losetup -d "$loop_dev" 2>/dev/null || true
        done
    fi
    rm -f "${TEST_DIR}/xfs_disk.img" "${TEST_DIR}/ext4_disk.img"
    log_info "Results saved in ${RESULTS_DIR}"
}

# =============================================================== Main
trap cleanup EXIT

echo "================================================"
echo "  MySQL Recovery Pro - Disaster Recovery Test"
echo "  Date: $(date)"
echo "  Binary: ${BINARY}"
echo "  Test Rows: ${TEST_ROWS}"
echo "================================================"

setup

SCENARIOS=("rm_rf" "truncate" "drop_table")

for fs in ext4 xfs; do
    echo ""
    echo "================================================"
    echo "  ${fs^^} Fileystem Testing"
    echo "================================================"

    for scenario in "${SCENARIOS[@]}"; do
        echo ""
        log_step "=== ${fs^^} / ${scenario} ==="

        # Create fresh data for each scenario
        if [ "$scenario" = "rm_rf" ]; then
            # For first scenario, data is already created
            :
        else
            # Remount and recreate data
            if [ "$fs" = "xfs" ]; then
                remount_dev=$(awk '{print $1}' "${TEST_DIR}/loops.txt")
            else
                remount_dev=$(awk '{print $2}' "${TEST_DIR}/loops.txt")
            fi
            mount "${remount_dev}" "${TEST_DIR}/${fs}_mount" 2>/dev/null || true
        fi

        create_and_populate "$fs"
        run_disaster_scenario "$fs" "$scenario"
    done
done

echo ""
echo "================================================"
echo "  Test Complete! Results: ${RESULTS_DIR}"
echo "================================================"

# Print summary
for fs in ext4 xfs; do
    echo ""
    echo "--- ${fs^^} Summary ---"
    for scenario in "${SCENARIOS[@]}"; do
        json="${RESULTS_DIR}/${fs}_${scenario}/summary.json"
        if [ -f "$json" ]; then
            python3 -c "
import json
with open('$json') as f:
    d = json.load(f)
    at = d.get('all_types', {})
    mr = d.get('many_rows', {})
    it = d.get('instant_table', {})
    print(f\"  {d['scenario']:12s}: all_types={at.get('recovered',0):5d}/{at.get('golden',0):5d} ({at.get('rate','N/A'):>6s}), many_rows={mr.get('recovered',0):5d}/{mr.get('golden',0):5d}, instant={it.get('recovered',0):5d}/{it.get('golden',0):5d}, time={d['recovery_time_seconds']}s\")
" 2>/dev/null || echo "  ${scenario}: no results"
        else
            echo "  ${scenario}: no results"
        fi
    done
done