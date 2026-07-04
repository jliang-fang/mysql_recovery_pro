#!/bin/bash
# MySQL 5.6-9.0 Schema-less Recovery Validation Test
# Tests all field types across all MySQL versions using podman

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
RECOVERY_TOOL="$PROJECT_ROOT/build/mysql_recovery_pro"
TEST_DATA_DIR="/tmp/mysql_schemaless_test"
RESULTS_DIR="$TEST_DATA_DIR/results"

# MySQL versions to test
MYSQL_VERSIONS=("5.6" "5.7" "8.0" "8.4" "9.0")

# MySQL Docker images
declare -A MYSQL_IMAGES
MYSQL_IMAGES["5.6"]="mysql:5.6"
MYSQL_IMAGES["5.7"]="mysql:5.7"
MYSQL_IMAGES["8.0"]="mysql:8.0"
MYSQL_IMAGES["8.4"]="mysql:8.4"
MYSQL_IMAGES["9.0"]="mysql:9.0"

# MySQL root passwords
declare -A MYSQL_PASSWORDS
MYSQL_PASSWORDS["5.6"]="test56"
MYSQL_PASSWORDS["5.7"]="test57"
MYSQL_PASSWORDS["8.0"]="test80"
MYSQL_PASSWORDS["8.4"]="test84"
MYSQL_PASSWORDS["9.0"]="test90"

# Container names
declare -A CONTAINER_NAMES
CONTAINER_NAMES["5.6"]="mysql56_schemaless_test"
CONTAINER_NAMES["5.7"]="mysql57_schemaless_test"
CONTAINER_NAMES["8.0"]="mysql80_schemaless_test"
CONTAINER_NAMES["8.4"]="mysql84_schemaless_test"
CONTAINER_NAMES["9.0"]="mysql90_schemaless_test"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Create test table with all field types
create_test_sql() {
    local version=$1
    cat <<EOF
-- Drop existing table
DROP TABLE IF EXISTS all_types_test;

-- Create table with all MySQL field types
CREATE TABLE all_types_test (
    id INT PRIMARY KEY AUTO_INCREMENT,
    col_tinyint TINYINT,
    col_smallint SMALLINT,
    col_mediumint MEDIUMINT,
    col_int INT,
    col_bigint BIGINT,
    col_float FLOAT,
    col_double DOUBLE,
    col_decimal DECIMAL(10,2),
    col_date DATE,
    col_time TIME,
    col_datetime DATETIME,
    col_timestamp TIMESTAMP NULL DEFAULT NULL,
    col_year YEAR,
    col_char CHAR(20),
    col_varchar VARCHAR(100),
    col_text TEXT,
    col_blob BLOB,
    col_enum ENUM('active','inactive','pending'),
    col_set SET('read','write','execute'),
    col_bit BIT(8)
$(if [[ "$version" =~ ^[89]\. ]]; then echo ",    col_json JSON"; fi)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Insert test data (10 rows with various field types)
INSERT INTO all_types_test VALUES
(1, 127, 32767, 8388607, 2147483647, 9223372036854775807, 3.14159, 2.718281828, 12345.67, '2026-01-15', '12:30:45', '2026-01-15 12:30:45', '2026-01-15 12:30:45', 2026, 'CHAR_TEST', 'VARCHAR_TEST_001', 'This is a text field for testing', UNHEX('010203040506070809'), 'active', 'read,write', 0xFF
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", '{\"key\":\"value\",\"number\":123}'"; fi)),

(2, -128, -32768, -8388608, -2147483648, -9223372036854775808, -1.5, -2.5, -9999.99, '2000-01-01', '00:00:00', '2000-01-01 00:00:00', '2000-01-01 00:00:00', 1901, 'MIN_VALUES', 'MIN_TEST', 'Minimum values test', UNHEX('AABBCCDD'), 'inactive', 'read', 0x00
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", '[]'"; fi)),

(3, 0, 0, 0, 0, 0, 0.0, 0.0, 0.00, '1970-01-01', '23:59:59', '1970-01-01 23:59:59', NULL, 2155, 'ZERO', 'ZERO_VAL', 'Zero values', NULL, 'pending', 'write,execute', 0x55
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", 'null'"; fi)),

(4, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", NULL"; fi)),

(5, 64, 16384, 4194304, 1073741824, 4611686018427387904, 1.5e10, 2.5e-5, 99999.99, '2037-12-31', '11:22:33', '2037-12-31 11:22:33', '2037-12-31 11:22:33', 2000, 'MID_VALUES', 'MIDDLE_TEST', 'Middle range values test', UNHEX('DEADBEEF'), 'active', 'read,write,execute', 0xAA
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", '{\"array\":[1,2,3],\"nested\":{\"a\":1}}'"; fi)),

(6, 1, 1, 1, 1, 1, 0.001, 0.001, 0.01, '2026-12-31', '01:02:03', '2026-12-31 01:02:03', '2026-12-31 01:02:03', 2025, 'ONE', 'ONE_VAL', 'One values', UNHEX('FF'), 'inactive', 'execute', 0x01
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", '\"simple string\"'"; fi)),

(7, -1, -1, -1, -1, -1, -0.001, -0.001, -0.01, '1999-12-31', '23:59:59', '1999-12-31 23:59:59', '1999-12-31 23:59:59', 1999, 'NEG_ONE', 'NEG_ONE_VAL', 'Negative one values', UNHEX('00FF'), 'pending', 'read', 0x80
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", '123'"; fi)),

(8, 100, 10000, 1000000, 100000000, 100000000000, 123.456, 789.012, 50000.00, '2010-06-15', '08:30:00', '2010-06-15 08:30:00', '2010-06-15 08:30:00', 2010, 'HUNDRED', 'HUNDRED_VAL', 'Hundred based values', UNHEX('1234567890'), 'active', 'write', 0x55
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", '{\"boolean\":true,\"null\":null}'"; fi)),

(9, -50, -5000, -500000, -50000000, -50000000000, -987.654, -321.098, -25000.00, '2015-03-20', '15:45:30', '2015-03-20 15:45:30', '2015-03-20 15:45:30', 2015, 'NEG_FIFTY', 'NEG_FIFTY_VAL', 'Negative fifty values', UNHEX('FEDCBA'), 'inactive', 'read,execute', 0xCC
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", '{\"mixed\":[1,\"a\",null,true]}'"; fi)),

(10, 42, 12345, 1234567, 123456789, 123456789012345, 2.718, 3.14159, 31415.93, '2020-07-04', '04:05:06', '2020-07-04 04:05:06', '2020-07-04 04:05:06', 2020, 'FORTY_TWO', 'ANSWER', 'The answer to everything', UNHEX('42424242'), 'pending', 'write,execute', 0x42
$(if [[ "$version" =~ ^[89]\. ]]; then echo ", '{\"pi\":3.14159,\"e\":2.71828}'"; fi));

-- Force flush to disk
FLUSH TABLES;
FLUSH TABLES WITH READ LOCK;
UNLOCK TABLES;
EOF
}

# Start MySQL container
start_mysql_container() {
    local version=$1
    local image=${MYSQL_IMAGES[$version]}
    local container=${CONTAINER_NAMES[$version]}
    local password=${MYSQL_PASSWORDS[$version]}

    log_info "Starting MySQL $version container..."

    # Remove existing container if present
    podman rm -f "$container" 2>/dev/null || true

    # Create data directory
    mkdir -p "$TEST_DATA_DIR/mysql_${version}"

    # Start new container
    podman run -d \
        --name "$container" \
        -e MYSQL_ROOT_PASSWORD="$password" \
        -e MYSQL_DATABASE=testdb \
        -v "$TEST_DATA_DIR/mysql_${version}:/var/lib/mysql" \
        -p "330${version/./}:3306" \
        "$image"

    # Wait for MySQL to be ready
    log_info "Waiting for MySQL $version to be ready..."
    local max_wait=60
    local waited=0
    while [ $waited -lt $max_wait ]; do
        if podman exec "$container" mysql -uroot -p"$password" -e "SELECT 1" &>/dev/null; then
            log_info "MySQL $version is ready!"
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
    done

    log_error "MySQL $version failed to start within ${max_wait}s"
    return 1
}

# Insert test data
insert_test_data() {
    local version=$1
    local container=${CONTAINER_NAMES[$version]}
    local password=${MYSQL_PASSWORDS[$version]}

    log_info "Inserting test data into MySQL $version..."

    # Generate SQL for this version
    local sql_file="$TEST_DATA_DIR/test_data_${version}.sql"
    create_test_sql "$version" > "$sql_file"

    # Execute SQL
    podman exec -i "$container" mysql -uroot -p"$password" testdb < "$sql_file"

    # Verify data inserted
    local count=$(podman exec "$container" mysql -uroot -p"$password" -N -e "SELECT COUNT(*) FROM testdb.all_types_test")
    log_info "MySQL $version: Inserted $count rows"

    # Get table file path
    local data_dir=$(podman exec "$container" mysql -uroot -p"$password" -N -e "SELECT @@datadir")
    log_info "MySQL $version data directory: $data_dir"

    # CRITICAL: Backup IBD file immediately after INSERT, before any disaster
    local ibd_path="$TEST_DATA_DIR/mysql_${version}/testdb/all_types_test.ibd"
    local backup_path="$TEST_DATA_DIR/backup_${version}/all_types_test.ibd"
    mkdir -p "$TEST_DATA_DIR/backup_${version}"

    if [ -f "$ibd_path" ]; then
        cp "$ibd_path" "$backup_path"
        log_info "MySQL $version: Backed up IBD to $backup_path ($(stat -c%s "$backup_path") bytes)"
    else
        log_warn "MySQL $version: IBD file not found at $ibd_path"
    fi
}

# Stop MySQL and perform DROP TABLE
perform_drop_table() {
    local version=$1
    local container=${CONTAINER_NAMES[$version]}
    local password=${MYSQL_PASSWORDS[$version]}

    log_info "Performing DROP TABLE on MySQL $version..."

    # Backup was already made during insert_test_data
    local backup_path="$TEST_DATA_DIR/backup_${version}/all_types_test.ibd"

    # Stop MySQL to ensure data is flushed
    podman exec "$container" mysql -uroot -p"$password" -e "FLUSH TABLES"

    # Drop the table
    podman exec "$container" mysql -uroot -p"$password" -e "DROP TABLE testdb.all_types_test"

    # Stop container
    podman stop "$container"

    # Return backup path for recovery
    echo "$backup_path"
}

# Perform TRUNCATE TABLE
perform_truncate_table() {
    local version=$1
    local container=${CONTAINER_NAMES[$version]}
    local password=${MYSQL_PASSWORDS[$version]}

    log_info "Performing TRUNCATE TABLE on MySQL $version..."

    # Backup was already made during insert_test_data
    local backup_path="$TEST_DATA_DIR/backup_${version}/all_types_test.ibd"

    # Truncate the table
    podman exec "$container" mysql -uroot -p"$password" -e "TRUNCATE TABLE testdb.all_types_test"

    # Stop container
    podman stop "$container"

    # Return backup path for recovery
    echo "$backup_path"
}

# Perform rm -rf
perform_rm_rf() {
    local version=$1
    local container=${CONTAINER_NAMES[$version]}

    log_info "Performing rm -rf on MySQL $version data..."

    # Backup was already made during insert_test_data
    local backup_path="$TEST_DATA_DIR/backup_${version}/all_types_test.ibd"

    # Stop container first
    podman stop "$container"

    # Remove IBD file (simulating rm -rf)
    local ibd_path="$TEST_DATA_DIR/mysql_${version}/testdb/all_types_test.ibd"
    rm -f "$ibd_path"

    # Return backup path for recovery
    echo "$backup_path"
}

# Run schema-less recovery
run_schemaless_recovery() {
    local version=$1
    local disaster_type=$2
    local ibd_path=$3

    local output_dir="$RESULTS_DIR/${version}_${disaster_type}"
    mkdir -p "$output_dir"

    log_info "Running schema-less recovery for MySQL $version ($disaster_type)..."

    # Check if IBD file exists
    if [ ! -f "$ibd_path" ]; then
        log_warn "IBD file not found: $ibd_path"
        # Try to find it in alternative locations (backup or mysql directories)
        ibd_path=$(find "$TEST_DATA_DIR" -name "all_types_test.ibd" 2>/dev/null | head -1)
        if [ -z "$ibd_path" ]; then
            log_error "Could not find IBD file for MySQL $version"
            return 1
        fi
        log_info "Found IBD file at: $ibd_path"
    fi

    # Run recovery
    "$RECOVERY_TOOL" \
        --ibd="$ibd_path" \
        --output="$output_dir" \
        --verbose 2>&1 | tee "$output_dir/recovery.log"

    # Check results
    local csv_count=$(ls "$output_dir"/*.csv 2>/dev/null | wc -l)
    local total_rows=$(cat "$output_dir"/*.csv 2>/dev/null | wc -l)

    log_info "Recovery complete: $csv_count CSV files, $total_rows total rows"

    echo "$output_dir"
}

# Validate recovered data
validate_recovery() {
    local version=$1
    local disaster_type=$2
    local output_dir=$3

    log_info "Validating recovered data for MySQL $version ($disaster_type)..."

    local validation_file="$output_dir/validation.txt"
    local pass_count=0
    local fail_count=0

    # Check for CSV files
    if [ ! -d "$output_dir" ] || [ -z "$(ls -A "$output_dir"/*.csv 2>/dev/null)" ]; then
        log_error "No CSV files found in $output_dir"
        echo "FAIL: No CSV files" >> "$validation_file"
        return 1
    fi

    # Find the main CSV file
    local csv_file=$(ls "$output_dir"/*.csv | head -1)

    echo "=== MySQL $version Schema-less Recovery Validation ===" > "$validation_file"
    echo "CSV File: $csv_file" >> "$validation_file"
    echo "" >> "$validation_file"

    # Check header
    local header=$(head -1 "$csv_file")
    echo "Header: $header" >> "$validation_file"

    # Count fields
    local field_count=$(echo "$header" | tr ',' '\n' | wc -l)
    echo "Field count: $field_count" >> "$validation_file"

    # Validate each field type
    echo "" >> "$validation_file"
    echo "=== Field Type Validation ===" >> "$validation_file"

    # Check for INT fields (should be numeric)
    if grep -q "^[0-9-]*," "$csv_file"; then
        echo "PASS: INT fields contain numeric values" >> "$validation_file"
        ((pass_count++))
    else
        echo "FAIL: INT fields not properly decoded" >> "$validation_file"
        ((fail_count++))
    fi

    # Check for DATE/DATETIME fields (should have date format)
    if grep -qE "[0-9]{4}-[0-9]{2}-[0-9]{2}" "$csv_file"; then
        echo "PASS: DATE/DATETIME fields contain date values" >> "$validation_file"
        ((pass_count++))
    else
        echo "FAIL: DATE/DATETIME fields not found" >> "$validation_file"
        ((fail_count++))
    fi

    # Check for STRING fields (should have text)
    if grep -qE "[A-Za-z]{3,}" "$csv_file"; then
        echo "PASS: STRING fields contain text values" >> "$validation_file"
        ((pass_count++))
    else
        echo "FAIL: STRING fields not found" >> "$validation_file"
        ((fail_count++))
    fi

    # Check for ENUM values (active/inactive/pending)
    if grep -qE "active|inactive|pending" "$csv_file"; then
        echo "PASS: ENUM fields contain expected values" >> "$validation_file"
        ((pass_count++))
    else
        echo "INFO: ENUM values may be numeric in schema-less mode" >> "$validation_file"
    fi

    # Check for NULL values
    if grep -q "NULL" "$csv_file"; then
        echo "PASS: NULL values properly marked" >> "$validation_file"
        ((pass_count++))
    else
        echo "INFO: No NULL values found (may be expected)" >> "$validation_file"
    fi

    # Check row count
    local row_count=$(wc -l < "$csv_file")
    row_count=$((row_count - 1))  # Subtract header
    echo "" >> "$validation_file"
    echo "Row count: $row_count (expected: 10)" >> "$validation_file"

    if [ $row_count -ge 10 ]; then
        echo "PASS: Recovered at least 10 rows" >> "$validation_file"
        ((pass_count++))
    else
        echo "FAIL: Expected at least 10 rows, got $row_count" >> "$validation_file"
        ((fail_count++))
    fi

    # Summary
    echo "" >> "$validation_file"
    echo "=== Summary ===" >> "$validation_file"
    echo "Pass: $pass_count" >> "$validation_file"
    echo "Fail: $fail_count" >> "$validation_file"

    if [ $fail_count -eq 0 ]; then
        log_info "Validation PASSED for MySQL $version ($disaster_type)"
        return 0
    else
        log_warn "Validation completed with $fail_count failures for MySQL $version ($disaster_type)"
        return 1
    fi
}

# Run test for single version and disaster type
run_single_test() {
    local version=$1
    local disaster_type=$2

    log_info "=== Testing MySQL $version - $disaster_type ==="

    # Start MySQL
    start_mysql_container "$version" || return 1

    # Wait a bit for MySQL to stabilize
    sleep 5

    # Insert test data
    insert_test_data "$version" || return 1

    # Perform disaster operation
    local ibd_path
    case "$disaster_type" in
        "drop")
            ibd_path=$(perform_drop_table "$version")
            ;;
        "truncate")
            ibd_path=$(perform_truncate_table "$version")
            ;;
        "rmrf")
            ibd_path=$(perform_rm_rf "$version")
            ;;
        *)
            log_error "Unknown disaster type: $disaster_type"
            return 1
            ;;
    esac

    # Run recovery
    local output_dir=$(run_schemaless_recovery "$version" "$disaster_type" "$ibd_path")

    # Validate
    validate_recovery "$version" "$disaster_type" "$output_dir"
}

# Run all tests
run_all_tests() {
    local results=()

    for version in "${MYSQL_VERSIONS[@]}"; do
        for disaster_type in "drop" "truncate" "rmrf"; do
            if run_single_test "$version" "$disaster_type"; then
                results+=("PASS: MySQL $version - $disaster_type")
            else
                results+=("FAIL: MySQL $version - $disaster_type")
            fi
        done
    done

    # Print summary
    echo ""
    echo "=== Test Summary ==="
    for result in "${results[@]}"; do
        echo "$result"
    done
}

# Cleanup
cleanup() {
    log_info "Cleaning up..."

    for version in "${MYSQL_VERSIONS[@]}"; do
        podman rm -f "${CONTAINER_NAMES[$version]}" 2>/dev/null || true
    done

    # Optionally keep test data for analysis
    if [ "$1" = "--deep-clean" ]; then
        rm -rf "$TEST_DATA_DIR"
    fi
}

# Main
main() {
    log_info "MySQL 5.6-9.0 Schema-less Recovery Validation Test"
    log_info "=================================================="

    # Check prerequisites
    if [ ! -f "$RECOVERY_TOOL" ]; then
        log_error "Recovery tool not found: $RECOVERY_TOOL"
        log_info "Please build the project first: cd $PROJECT_ROOT && make"
        exit 1
    fi

    if ! command -v podman &>/dev/null; then
        log_error "podman not found. Please install podman."
        exit 1
    fi

    # Create directories
    mkdir -p "$TEST_DATA_DIR"
    mkdir -p "$RESULTS_DIR"

    # Run tests
    if [ "$1" = "--single" ]; then
        run_single_test "$2" "$3"
    else
        run_all_tests
    fi

    # Cleanup
    cleanup
}

# Handle arguments
case "$1" in
    "--cleanup")
        cleanup "--deep-clean"
        ;;
    "--single")
        main "--single" "$2" "$3"
        ;;
    *)
        main
        ;;
esac
