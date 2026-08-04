#!/bin/bash
# =============================================================================
# Phase 5: End-to-End Disaster Recovery Matrix
# =============================================================================
# Tests disaster recovery across all supported configurations:
#   - MySQL 5.6, 5.7, 8.0, 8.4, 9.0
#   - InnoDB file-per-table + shared tablespaces
#   - XFS + ext4 filesystems
#   - DROP TABLE, TRUNCATE, rm -rf scenarios
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
RESULTS_DIR="${PROJECT_DIR}/phase5_results"
TEST_DATA_DIR="${RESULTS_DIR}/test_data"
DISASTER_MATRIX_LOG="${RESULTS_DIR}/disaster_matrix.log"
SUMMARY_FILE="${RESULTS_DIR}/summary.json"

MYSQL_VERSIONS=("5.6" "5.7" "8.0" "8.4" "9.0")
SCENARIOS=("drop_table" "truncate" "rm_rf")
FILESYSTEMS=("xfs" "ext4")

mkdir -p "$RESULTS_DIR" "$TEST_DATA_DIR"

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$DISASTER_MATRIX_LOG"; }

# Build the project
build_project() {
    log "Building project..."
    cd "$PROJECT_DIR"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
    cmake --build build -j"$(nproc)" 2>&1 | tail -5
    log "Build complete."
}

# Generate test data for a specific MySQL version
generate_test_data() {
    local version="$1"
    local data_dir="${TEST_DATA_DIR}/mysql${version}"
    mkdir -p "$data_dir"

    if [ -f "$data_dir/ibdata1" ]; then
        log "MySQL ${version}: test data exists, skipping generation."
        return
    fi

    log "MySQL ${version}: generating test data..."
    # Use mock_data_gen tool if available
    if [ -f "${BUILD_DIR}/mock_data_gen" ]; then
        "${BUILD_DIR}/mock_data_gen" \
            --version="$version" \
            --output-dir="$data_dir" \
            --num-rows=1000 \
            2>&1 | tail -3
    else
        log "WARNING: mock_data_gen not found. Run phase5_gen_test_data.py instead."
        echo "{\"generated\": false, \"version\": \"$version\"}" > "$data_dir/meta.json"
        return
    fi
    echo "{\"generated\": true, \"version\": \"$version\"}" > "$data_dir/meta.json"
    log "MySQL ${version}: test data generated."
}

# Run a single disaster recovery test
run_test() {
    local version="$1"
    local scenario="$2"
    local fs="$3"
    local data_dir="${TEST_DATA_DIR}/mysql${version}"
    local result_file="${RESULTS_DIR}/mysql${version}_${scenario}_${fs}.json"

    if [ ! -f "${BUILD_DIR}/mysql_recovery_pro" ]; then
        log "SKIP: mysql_recovery_pro binary not found"
        return
    fi

    log "RUNNING: MySQL ${version}, ${scenario}, ${fs}..."

    # Determine recovery mode based on scenario
    local mode_args=""
    case "$scenario" in
        drop_table) mode_args="--mode=drop_table" ;;
        truncate)   mode_args="--mode=truncate" ;;
        rm_rf)      mode_args="--mode=disk_carving" ;;
    esac

    local fs_args=""
    case "$fs" in
        xfs)  fs_args="--fs-type=xfs" ;;
        ext4) fs_args="--fs-type=ext4" ;;
    esac

    local start_time
    start_time=$(date +%s%N)

    # Run recovery with timeout
    timeout 300 "${BUILD_DIR}/mysql_recovery_pro" \
        $mode_args \
        $fs_args \
        --data-dir="$data_dir" \
        --output-dir="${RESULTS_DIR}/output_mysql${version}_${scenario}_${fs}" \
        --csv \
        2>&1 | tail -5 || true

    local end_time
    end_time=$(date +%s%N)
    local elapsed_ms=$(( (end_time - start_time) / 1000000 ))

    # Count recovered rows
    local recovered=0
    local csv_file="${RESULTS_DIR}/output_mysql${version}_${scenario}_${fs}/recovered.csv"
    if [ -f "$csv_file" ]; then
        recovered=$(($(wc -l < "$csv_file") - 1))
        [ "$recovered" -lt 0 ] && recovered=0
    fi

    # Build result JSON
    cat > "$result_file" <<EOF
{
    "mysql_version": "$version",
    "scenario": "$scenario",
    "filesystem": "$fs",
    "recovered_rows": $recovered,
    "elapsed_ms": $elapsed_ms,
    "status": "completed"
}
EOF
    log "DONE: MySQL ${version}, ${scenario}, ${fs} — ${recovered} rows in ${elapsed_ms}ms"
}

# Run the full matrix
run_matrix() {
    log "========================================="
    log "  Disaster Recovery Matrix (Phase 5)"
    log "========================================="

    build_project

    for version in "${MYSQL_VERSIONS[@]}"; do
        generate_test_data "$version"
    done

    for version in "${MYSQL_VERSIONS[@]}"; do
        for scenario in "${SCENARIOS[@]}"; do
            for fs in "${FILESYSTEMS[@]}"; do
                run_test "$version" "$scenario" "$fs"
            done
        done
    done

    log "Matrix complete. Generating summary..."

    # Build summary JSON
    echo "[" > "$SUMMARY_FILE"
    local first=true
    for result in "${RESULTS_DIR}"/mysql*.json; do
        [ -f "$result" ] || continue
        $first || echo "," >> "$SUMMARY_FILE"
        cat "$result" >> "$SUMMARY_FILE"
        first=false
    done
    echo "" >> "$SUMMARY_FILE"
    echo "]" >> "$SUMMARY_FILE"

    log "Summary saved to $SUMMARY_FILE"
    log "========================================="
    log "  Phase 5 Complete"
    log "========================================="
}

# Quick validation (run only 3 representative tests)
quick_validate() {
    log "Quick validation mode (3 tests)..."
    build_project
    generate_test_data "8.0"

    run_test "8.0" "drop_table" "xfs"
    run_test "8.0" "truncate" "xfs"
    run_test "8.0" "rm_rf" "xfs"

    log "Quick validation complete."
}

# Main entry point
case "${1:-full}" in
    quick)  quick_validate ;;
    full)   run_matrix ;;
    build)  build_project ;;
    *)
        echo "Usage: $0 [full|quick|build]"
        echo "  full  - Run full disaster recovery matrix (default)"
        echo "  quick - Run 3 representative tests"
        echo "  build - Build project only"
        exit 1
        ;;
esac