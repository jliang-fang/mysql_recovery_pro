#!/bin/bash
# Collect integrity-verified InnoDB .ibd fixtures for schema-free inference tests.
#
# Usage:
#   tests/collect_inference_fixtures.sh
#   tests/collect_inference_fixtures.sh --replace

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
WORK_DIR="${TMPDIR:-/tmp}/inference_fixture_collect"
REPLACE="${1:-}"

MYSQL_VERSIONS=("5.6" "5.7" "8.0" "8.4" "9.0")

declare -A MYSQL_IMAGES=(
    ["5.6"]="mysql:5.6" ["5.7"]="mysql:5.7" ["8.0"]="mysql:8.0"
    ["8.4"]="mysql:8.4" ["9.0"]="mysql:9.0"
)
declare -A MYSQL_PASSWORDS=(
    ["5.6"]="test56" ["5.7"]="test57" ["8.0"]="test80"
    ["8.4"]="test84" ["9.0"]="test90"
)
declare -A CONTAINER_NAMES=(
    ["5.6"]="inference_fixture_56" ["5.7"]="inference_fixture_57"
    ["8.0"]="inference_fixture_80" ["8.4"]="inference_fixture_84"
    ["9.0"]="inference_fixture_90"
)

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_info() { printf '%b\n' "${GREEN}[INFO]${NC} $1"; }
log_warn() { printf '%b\n' "${YELLOW}[WARN]${NC} $1"; }
log_error() { printf '%b\n' "${RED}[ERROR]${NC} $1"; }

CURRENT_CONTAINER=""
LOCK_LOG=""
LOCK_PID=""
TEMP_IBD=""
TEMP_JSON=""

cleanup_export_lock() {
    if [[ -n "$LOCK_PID" ]] && kill -0 "$LOCK_PID" 2>/dev/null; then
        printf 'UNLOCK TABLES;\nexit\n' >&"${EXPORT_LOCK[1]}" 2>/dev/null || true
        eval "exec ${EXPORT_LOCK[1]}>&-" 2>/dev/null || true
        kill "$LOCK_PID" 2>/dev/null || true
        wait "$LOCK_PID" 2>/dev/null || true
    fi
    [[ -n "$LOCK_LOG" ]] && rm -f "$LOCK_LOG"
    LOCK_PID=""
    LOCK_LOG=""
}

cleanup() {
    cleanup_export_lock
    [[ -n "$TEMP_IBD" ]] && rm -f "$TEMP_IBD"
    [[ -n "$TEMP_JSON" ]] && rm -f "$TEMP_JSON"
    if [[ -n "$CURRENT_CONTAINER" ]]; then
        podman rm -f "$CURRENT_CONTAINER" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

if [[ "$REPLACE" != "" && "$REPLACE" != "--replace" ]]; then
    log_error "Unknown argument: $REPLACE"
    exit 2
fi

for dependency in podman python3 sha256sum; do
    if ! command -v "$dependency" >/dev/null 2>&1; then
        log_error "$dependency is required to collect fixtures"
        exit 1
    fi
done

mkdir -p "$DATA_DIR" "$WORK_DIR"

create_test_sql() {
    local version=$1
    cat <<EOF
DROP TABLE IF EXISTS all_types_test;
CREATE TABLE all_types_test (
    id INT PRIMARY KEY AUTO_INCREMENT,
    col_tinyint TINYINT, col_smallint SMALLINT, col_mediumint MEDIUMINT,
    col_int INT, col_bigint BIGINT, col_float FLOAT, col_double DOUBLE,
    col_decimal DECIMAL(10,2), col_date DATE, col_time TIME,
    col_datetime DATETIME, col_timestamp TIMESTAMP NULL DEFAULT NULL,
    col_year YEAR, col_char CHAR(20), col_varchar VARCHAR(100),
    col_text TEXT, col_blob BLOB, col_enum ENUM('active','inactive','pending'),
    col_set SET('read','write','execute'), col_bit BIT(8)
$(if [[ "$version" =~ ^[89]\. ]]; then printf ',    col_json JSON'; fi)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
INSERT INTO all_types_test VALUES
(1,127,32767,8388607,2147483647,9223372036854775807,3.14159,2.718281828,12345.67,'2026-01-15','12:30:45','2026-01-15 12:30:45','2026-01-15 12:30:45',2026,'CHAR_TEST','VARCHAR_TEST_001','This is a text field for testing',UNHEX('010203040506070809'),'active','read,write',0xFF$(if [[ "$version" =~ ^[89]\. ]]; then printf ',\x27{"key":"value","number":123}\x27'; fi)),
(2,-128,-32768,-8388608,-2147483648,-9223372036854775808,-1.5,-2.5,-9999.99,'2000-01-01','00:00:00','2000-01-01 00:00:00','2000-01-01 00:00:00',1901,'MIN_VALUES','MIN_TEST','Minimum values test',UNHEX('AABBCCDD'),'inactive','read',0x00$(if [[ "$version" =~ ^[89]\. ]]; then printf ',\x27[]\x27'; fi)),
(3,0,0,0,0,0,0.0,0.0,0.00,'1970-01-01','23:59:59','1970-01-01 23:59:59',NULL,2155,'ZERO','ZERO_VAL','Zero values',NULL,'pending','write,execute',0x55$(if [[ "$version" =~ ^[89]\. ]]; then printf ',\x27null\x27'; fi)),
(4,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL$(if [[ "$version" =~ ^[89]\. ]]; then printf ',NULL'; fi));
EOF
}

validate_pair() {
    local ibd=$1 json=$2
    python3 - "$ibd" "$json" <<'PY'
import hashlib
import json
import os
import sys

ibd_path, json_path = sys.argv[1:]
required = ("version", "server_version", "table", "database", "row_count",
            "row_format", "page_size", "ddl", "byte_size", "page_count", "sha256")
if not os.path.isfile(ibd_path) or os.path.getsize(ibd_path) == 0:
    raise SystemExit("missing or empty .ibd")
with open(json_path, encoding="utf-8") as source:
    metadata = json.load(source)
for name in required:
    if name not in metadata:
        raise SystemExit(f"metadata missing {name}")
size = os.path.getsize(ibd_path)
page_size = metadata["page_size"]
if not isinstance(page_size, int) or page_size <= 0:
    raise SystemExit("invalid page_size")
if metadata["byte_size"] != size or size % page_size != 0:
    raise SystemExit("file size metadata mismatch")
if metadata["page_count"] != size // page_size:
    raise SystemExit("page count metadata mismatch")
with open(ibd_path, "rb") as source:
    digest = hashlib.sha256(source.read()).hexdigest()
if metadata["sha256"] != digest:
    raise SystemExit("SHA-256 metadata mismatch")
PY
}

start_mysql_container() {
    local version=$1 image=${MYSQL_IMAGES[$version]} password=${MYSQL_PASSWORDS[$version]}
    local datadir="$WORK_DIR/mysql_${version}"
    CURRENT_CONTAINER=${CONTAINER_NAMES[$version]}

    podman rm -f "$CURRENT_CONTAINER" >/dev/null 2>&1 || true
    rm -rf "$datadir"
    mkdir -p "$datadir"
    podman run -d --name "$CURRENT_CONTAINER" \
        -e MYSQL_ROOT_PASSWORD="$password" -e MYSQL_DATABASE=testdb \
        -v "$datadir:/var/lib/mysql" "$image" >/dev/null

    local waited=0
    while (( waited < 90 )); do
        if podman exec "$CURRENT_CONTAINER" mysql -uroot -p"$password" -e 'SELECT 1' >/dev/null 2>&1; then
            return 0
        fi
        sleep 2
        ((waited += 2))
    done
    log_error "MySQL $version did not become ready within 90 seconds"
    return 1
}

start_export_lock() {
    local password=$1
    LOCK_LOG=$(mktemp "$WORK_DIR/export_lock.XXXXXX.log")
    coproc EXPORT_LOCK {
        podman exec -i "$CURRENT_CONTAINER" \
            mysql -uroot -p"$password" -N -B -n testdb 2>"$LOCK_LOG"
    }
    LOCK_PID=$EXPORT_LOCK_PID
    printf "FLUSH TABLES testdb.all_types_test FOR EXPORT; SELECT 'EXPORT_LOCK_ACQUIRED';\n" >&"${EXPORT_LOCK[1]}"

    local waited=0 line
    while (( waited < 30 )); do
        if IFS= read -r -t 1 line <&"${EXPORT_LOCK[0]}"; then
            if [[ "$line" == "EXPORT_LOCK_ACQUIRED" ]]; then
                return 0
            fi
        fi
        if ! kill -0 "$LOCK_PID" 2>/dev/null; then
            break
        fi
        ((waited += 1))
    done
    log_error "FLUSH TABLES FOR EXPORT did not acquire its persistent lock: $(<"$LOCK_LOG")"
    return 1
}

release_export_lock() {
    printf 'UNLOCK TABLES;\nexit;\n' >&"${EXPORT_LOCK[1]}"
    eval "exec ${EXPORT_LOCK[1]}>&-" 2>/dev/null || true
    wait "$LOCK_PID" 2>/dev/null || true
    LOCK_PID=""
    rm -f "$LOCK_LOG"
    LOCK_LOG=""
}

write_metadata() {
    local destination=$1 version=$2 image=$3 server_version=$4 row_count=$5 row_format=$6
    local page_size=$7 ddl=$8 ibd=$9
    local byte_size page_count sha256
    byte_size=$(stat -c%s "$ibd")
    page_count=$((byte_size / page_size))
    sha256=$(sha256sum "$ibd" | cut -d' ' -f1)

    FIXTURE_VERSION="$version" FIXTURE_IMAGE="$image" FIXTURE_SERVER_VERSION="$server_version" \
    FIXTURE_ROW_COUNT="$row_count" FIXTURE_ROW_FORMAT="$row_format" FIXTURE_PAGE_SIZE="$page_size" \
    FIXTURE_DDL="$ddl" FIXTURE_BYTE_SIZE="$byte_size" FIXTURE_PAGE_COUNT="$page_count" \
    FIXTURE_SHA256="$sha256" python3 - > "$destination" <<'PY'
import json
import os

print(json.dumps({
    "version": os.environ["FIXTURE_VERSION"],
    "image": os.environ["FIXTURE_IMAGE"],
    "server_version": os.environ["FIXTURE_SERVER_VERSION"],
    "table": "all_types_test",
    "database": "testdb",
    "row_count": int(os.environ["FIXTURE_ROW_COUNT"]),
    "row_format": os.environ["FIXTURE_ROW_FORMAT"],
    "page_size": int(os.environ["FIXTURE_PAGE_SIZE"]),
    "ddl": os.environ["FIXTURE_DDL"],
    "byte_size": int(os.environ["FIXTURE_BYTE_SIZE"]),
    "page_count": int(os.environ["FIXTURE_PAGE_COUNT"]),
    "sha256": os.environ["FIXTURE_SHA256"],
}, indent=2, sort_keys=True))
PY
}

collect_version() {
    local version=$1 password=${MYSQL_PASSWORDS[$version]} image=${MYSQL_IMAGES[$version]}
    local output_ibd="$DATA_DIR/mysql${version}.ibd"
    local output_json="$DATA_DIR/mysql${version}.json"

    if [[ "$REPLACE" != "--replace" && ( -e "$output_ibd" || -e "$output_json" ) ]]; then
        if validate_pair "$output_ibd" "$output_json"; then
            log_info "mysql${version} fixture pair is already integrity-valid"
            return 0
        fi
        log_error "mysql${version} has an incomplete or invalid fixture pair; use --replace to regenerate"
        return 1
    fi

    log_info "Collecting fixture for MySQL $version"
    if ! start_mysql_container "$version"; then
        return 1
    fi

    local sql_file="$WORK_DIR/data_${version}.sql"
    create_test_sql "$version" > "$sql_file"
    if ! podman exec -i "$CURRENT_CONTAINER" mysql -uroot -p"$password" testdb < "$sql_file"; then
        log_error "Failed to create test data for MySQL $version"
        return 1
    fi

    local row_count row_format page_size server_version ddl datadir table_ibd
    row_count=$(podman exec "$CURRENT_CONTAINER" mysql -uroot -p"$password" -N -B \
        -e 'SELECT COUNT(*) FROM testdb.all_types_test' | tr -d '\r')
    row_format=$(podman exec "$CURRENT_CONTAINER" mysql -uroot -p"$password" -N -B \
        -e "SELECT ROW_FORMAT FROM information_schema.TABLES WHERE TABLE_SCHEMA='testdb' AND TABLE_NAME='all_types_test'" | tr -d '\r')
    page_size=$(podman exec "$CURRENT_CONTAINER" mysql -uroot -p"$password" -N -B \
        -e 'SELECT @@innodb_page_size' | tr -d '\r')
    server_version=$(podman exec "$CURRENT_CONTAINER" mysql -uroot -p"$password" -N -B \
        -e 'SELECT @@version' | tr -d '\r')
    ddl=$(podman exec "$CURRENT_CONTAINER" mysql -uroot -p"$password" -N -B \
        -e 'SHOW CREATE TABLE testdb.all_types_test' | cut -f2- | tr -d '\r')
    datadir=$(podman exec "$CURRENT_CONTAINER" mysql -uroot -p"$password" -N -B \
        -e 'SELECT @@datadir' | tr -d '\r')
    table_ibd="$datadir/testdb/all_types_test.ibd"

    [[ "$row_count" =~ ^[0-9]+$ ]] || { log_error "Invalid row count for MySQL $version"; return 1; }
    [[ "$page_size" =~ ^[0-9]+$ ]] || { log_error "Invalid page size for MySQL $version"; return 1; }
    [[ -n "$ddl" ]] || { log_error "Missing DDL for MySQL $version"; return 1; }

    if ! start_export_lock "$password"; then
        return 1
    fi
    TEMP_IBD=$(mktemp -u "$DATA_DIR/.mysql${version}.ibd.XXXXXX")
    TEMP_JSON=$(mktemp -u "$DATA_DIR/.mysql${version}.json.XXXXXX")
    if ! podman cp "$CURRENT_CONTAINER:$table_ibd" "$TEMP_IBD"; then
        log_error "Failed to copy .ibd for MySQL $version"
        return 1
    fi
    release_export_lock

    [[ -s "$TEMP_IBD" ]] || { log_error "Copied .ibd is empty for MySQL $version"; return 1; }
    if (( $(stat -c%s "$TEMP_IBD") % page_size != 0 )); then
        log_error "Copied .ibd is not aligned to its declared page size for MySQL $version"
        return 1
    fi

    write_metadata "$TEMP_JSON" "$version" "$image" "$server_version" "$row_count" \
        "$row_format" "$page_size" "$ddl" "$TEMP_IBD"
    validate_pair "$TEMP_IBD" "$TEMP_JSON"

    mv -f "$TEMP_IBD" "$output_ibd"
    TEMP_IBD=""
    mv -f "$TEMP_JSON" "$output_json"
    TEMP_JSON=""
    log_info "MySQL $version: promoted verified fixture pair ($(stat -c%s "$output_ibd") bytes)"

    podman rm -f "$CURRENT_CONTAINER" >/dev/null 2>&1 || true
    CURRENT_CONTAINER=""
}

overall=0
for version in "${MYSQL_VERSIONS[@]}"; do
    if ! collect_version "$version"; then
        log_warn "Fixture collection failed for MySQL $version"
        overall=1
    fi
    cleanup_export_lock
    if [[ -n "$CURRENT_CONTAINER" ]]; then
        podman rm -f "$CURRENT_CONTAINER" >/dev/null 2>&1 || true
        CURRENT_CONTAINER=""
    fi
done

log_info "Fixture pairs in $DATA_DIR:"
for version in "${MYSQL_VERSIONS[@]}"; do
    [[ -f "$DATA_DIR/mysql${version}.ibd" ]] && printf '  mysql%s\n' "$version"
done

exit "$overall"
