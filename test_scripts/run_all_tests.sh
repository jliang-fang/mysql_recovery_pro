#!/bin/bash
# =============================================================================
# MySQL Recovery Test Suite
# =============================================================================
# Tests extreme recovery scenarios for mysql_recovery_pro
# Target: MySQL 5.6 with ≥99.9% recovery rate
# =============================================================================

set -e

# Configuration
MYSQL_VERSION="5.6"
TEST_DATA_ROWS=1000000
RECOVERY_BINARY="/app/mysql_recovery_pro/bin/mysql_recovery_pro"
OUTPUT_DIR="/tmp/mysql_recovery_test"
RESULTS_FILE="${OUTPUT_DIR}/test_results.json"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# =============================================================================
# Helper Functions
# =============================================================================

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_dependencies() {
    log_info "Checking dependencies..."
    
    # Check Podman
    if ! command -v podman &> /dev/null; then
        log_error "Podman is not installed"
        exit 1
    fi
    log_info "Podman version: $(podman --version)"
    
    # Check mysql_recovery_pro
    if [[ ! -x "${RECOVERY_BINARY}" ]]; then
        log_error "mysql_recovery_pro binary not found or not executable"
        log_info "Building mysql_recovery_pro..."
        cd /app/mysql_recovery_pro
        cmake -B build
        cmake --build build --target mysql_recovery_pro
    fi
    log_info "mysql_recovery_pro found: ${RECOVERY_BINARY}"
    
    # Check disk space
    local disk_space=$(df -h "${OUTPUT_DIR}" | awk 'NR==2 {print $4}')
    log_info "Available disk space: ${disk_space}"
}

setup_environment() {
    log_info "Setting up test environment..."
    
    mkdir -p "${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}/mysql_data"
    mkdir -p "${OUTPUT_DIR}/recovery_output"
    mkdir -p "${OUTPUT_DIR}/disk_images"
    
    # Create init.sql for MySQL
    cat > "${OUTPUT_DIR}/init.sql" <<EOF
-- Recovery test initialization
ALTER USER 'root'@'localhost' IDENTIFIED BY 'test123';
FLUSH PRIVILEGES;
EOF
    
    log_info "Environment setup complete"
}

start_mysql() {
    local container_name="mysql_recovery_test"
    local data_dir="${OUTPUT_DIR}/mysql_data"
    
    log_info "Starting MySQL container..."
    
    # Stop existing container
    podman stop "${container_name}" 2>/dev/null || true
    podman rm -f "${container_name}" 2>/dev/null || true
    
    # Start new container
    podman run -d \
        --name "${container_name}" \
        -e MYSQL_ROOT_PASSWORD=test123 \
        -p 3306:3306 \
        -v "${data_dir}:/var/lib/mysql" \
        -v "${OUTPUT_DIR}/init.sql:/docker-entrypoint-initdb.d/init.sql:ro" \
        "mysql:${MYSQL_VERSION}" 2>/dev/null
    
    # Wait for MySQL to be ready
    log_info "Waiting for MySQL to be ready..."
    local max_wait=60
    local waited=0
    
    while ! podman exec "${container_name}" mysql -uroot -ptest123 -e "SELECT 1" &>/dev/null; do
        sleep 2
        waited=$((waited + 2))
        if [[ ${waited} -ge ${max_wait} ]]; then
            log_error "MySQL failed to start within ${max_wait} seconds"
            return 1
        fi
    done
    
    log_info "MySQL is ready"
    return 0
}

stop_mysql() {
    local container_name="mysql_recovery_test"
    
    log_info "Stopping MySQL container..."
    podman stop -t 10 "${container_name}" 2>/dev/null || true
    podman rm -f "${container_name}" 2>/dev/null || true
}

generate_test_data() {
    local output_file="${OUTPUT_DIR}/test_data.sql"
    local row_count=${1:-${TEST_DATA_ROWS}}
    
    log_info "Generating ${row_count} rows of test data..."
    
    cat > "${output_file}" <<'SQL_HEADER'
-- Test data for MySQL Recovery Validation
-- Generated at: $(date)
-- Row count: ${row_count}

CREATE DATABASE IF NOT EXISTS test_recovery;
USE test_recovery;

CREATE TABLE IF NOT EXISTS test_users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(255),
    email VARCHAR(255),
    created_at DATETIME,
    phone VARCHAR(20),
    age INT,
    score DECIMAL(10,2),
    description TEXT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
SQL_HEADER
    
    # Insert data in batches
    local batch_size=10000
    for ((i=0; i<row_count; i++)); do
        local name_idx=$((i % 20))
        local domain_idx=$((i % 5))
        
        local names=("Zhang Wei" "Li Na" "Wang Gang" "Liu Xiu" "Chen Jie" \
                     "Yang Mi" "Huang Bo" "Zhou Jie" "Wu Meng" "Zheng Kai" \
                     "Sun Qian" "Zhu Qi" "He Liang" "Ma Chao" "Hu Yan" \
                     "Song Huan" "Lin Jun" "Luo Wei" "Guo Jing" "Tuo Yong")
        
        local domains=("test.com" "example.com" "mail.com" "company.org" "domain.net")
        
        local name="${names[$name_idx]}_${i}"
        local email="user${i}@${domains[$domain_idx]}"
        local year=$((2024 + i / 365))
        local month=$(( (i % 12) + 1 ))
        local day=$(( (i % 28) + 1 ))
        local hour=$((i % 24))
        local minute=$((i % 60))
        local second=$((i % 60))
        local created_at=$(printf "%04d-%02d-%02d %02d:%02d:%02d" $year $month $day $hour $minute $second)
        local phone="138$(printf '%04d' $((i % 10000)))$(printf '%04d' $((i % 10000)))"
        local age=$(( (i % 100) + 18 ))
        local score=$(awk "BEGIN {printf \"%.2f\", ${i} % 1000 * 1.5}")
        local description="This is record number ${i} with user data for testing recovery. Email: ${email}"
        
        if [[ $((i % batch_size)) -eq 0 ]]; then
            echo "  Inserted $i rows..."
        fi
        
        echo "INSERT INTO test_users (name, email, created_at, phone, age, score, description) VALUES ('${name}', '${email}', '${created_at}', '${phone}', ${age}, ${score}, '${description}');" >> "${output_file}"
    done
    
    log_info "Test data generated: ${output_file}"
}

import_test_data() {
    log_info "Importing test data into MySQL..."
    
    podman exec -i mysql_recovery_test mysql -uroot -ptest123 test_recovery < "${OUTPUT_DIR}/test_data.sql" 2>/dev/null || \
    podman exec -i mysql_recovery_test mysql -uroot -ptest123 -e "SOURCE ${OUTPUT_DIR}/test_data.sql" 2>/dev/null
    
    # Verify data was imported
    local count=$(podman exec mysql_recovery_test mysql -uroot -ptest123 -N -e "SELECT COUNT(*) FROM test_recovery.test_users;" 2>/dev/null || echo "0")
    log_info "Records in database: ${count}"
    
    echo "${count}"
}

calculate_baseline_hash() {
    local ibd_file="${OUTPUT_DIR}/mysql_data/test_recovery/test_users.ibd"
    
    if [[ ! -f "${ibd_file}" ]]; then
        log_error "IBD file not found: ${ibd_file}"
        return 1
    fi
    
    log_info "Calculating baseline SHA-256 hash..."
    local hash=$(sha256sum "${ibd_file}" | cut -d' ' -f1)
    log_info "Baseline hash: ${hash}"
    
    echo "${hash}"
}

run_truncate_recovery() {
    local baseline_hash=$1
    log_info "Testing TRUNCATE recovery..."
    
    # Get baseline data before TRUNCATE
    local ibd_file="${OUTPUT_DIR}/mysql_data/test_recovery/test_users.ibd"
    local pre_truncate_hash
    pre_truncate_hash=$(calculate_baseline_hash)
    
    # Backup IBD file
    local backup_file="${OUTPUT_DIR}/disk_images/pre_truncate.ibd"
    cp "${ibd_file}" "${backup_file}"
    
    # Get record count
    local pre_count=$(podman exec mysql_recovery_test mysql -uroot -ptest123 -N -e "SELECT COUNT(*) FROM test_recovery.test_users;" 2>/dev/null)
    
    # TRUNCATE table
    log_info "Executing TRUNCATE TABLE..."
    podman exec mysql_recovery_test mysql -uroot -ptest123 -e "TRUNCATE TABLE test_recovery.test_users;" 2>/dev/null
    
    # Verify table is empty
    local post_count=$(podman exec mysql_recovery_test mysql -uroot -ptest123 -N -e "SELECT COUNT(*) FROM test_recovery.test_users;" 2>/dev/null)
    log_info "Records after TRUNCATE: ${post_count}"
    
    # Run recovery
    local output_dir="${OUTPUT_DIR}/recovery_output/truncate"
    mkdir -p "${output_dir}"
    
    log_info "Running mysql_recovery_pro for TRUNCATE recovery..."
    
    # Create schema template file
    cat > "${output_dir}/schema.json" <<EOF
{
    "table_name": "test_users",
    "database_name": "test_recovery",
    "columns": [
        {"name": "id", "type": "BIGINT", "is_nullable": false},
        {"name": "name", "type": "VARCHAR", "length": 255, "is_nullable": false},
        {"name": "email", "type": "VARCHAR", "length": 255, "is_nullable": false},
        {"name": "created_at", "type": "DATETIME", "is_nullable": true},
        {"name": "phone", "type": "VARCHAR", "length": 20, "is_nullable": true},
        {"name": "age", "type": "INT", "is_nullable": true},
        {"name": "score", "type": "DECIMAL", "precision": 10, "scale": 2, "is_nullable": true},
        {"name": "description", "type": "TEXT", "is_nullable": true}
    ]
}
EOF
    
    local start_time=$(date +%s)
    
    "${RECOVERY_BINARY}" \
        --recovery-mode truncate \
        --ibd-file "${backup_file}" \
        --output-dir "${output_dir}" \
        --export-format sql \
        2>&1 | tee "${output_dir}/recovery.log"
    
    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    
    log_info "Recovery completed in ${elapsed} seconds"
    
    # Count recovered records
    local recovered_count=0
    if [[ -f "${output_dir}/recovered.sql" ]]; then
        recovered_count=$(grep -c "INSERT INTO" "${output_dir}/recovered.sql" 2>/dev/null || echo "0")
    fi
    
    # Calculate recovery rate
    local recovery_rate=0
    if [[ ${pre_count} -gt 0 ]]; then
        recovery_rate=$(awk "BEGIN {printf \"%.4f\", ${recovered_count} / ${pre_count} * 100}")
    fi
    
    log_info "Records recovered: ${recovered_count} / ${pre_count}"
    log_info "Recovery rate: ${recovery_rate}%"
    
    # Verify hash match
    local recovered_hash=""
    if [[ -f "${output_dir}/recovered_data.sql" ]]; then
        recovered_hash=$(sha256sum "${output_dir}/recovered_data.sql" | cut -d' ' -f1)
        log_info "Recovered hash: ${recovered_hash}"
    fi
    
    # Return results
    echo "{
        \"test\": \"truncate_recovery\",
        \"pre_truncate_count\": ${pre_count},
        \"recovered_count\": ${recovered_count},
        \"recovery_rate\": ${recovery_rate},
        \"elapsed_seconds\": ${elapsed},
        \"status\": \"$([ $(echo "${recovery_rate} >= 99.9" | bc -l) -eq 1 ] && echo 'PASS' || echo 'FAIL')\"
    }"
}

run_rmrf_recovery() {
    local baseline_hash=$1
    log_info "Testing rm -rf recovery..."
    
    # Get baseline data
    local ibd_file="${OUTPUT_DIR}/mysql_data/test_recovery/test_users.ibd"
    local pre_count=$(podman exec mysql_recovery_test mysql -uroot -ptest123 -N -e "SELECT COUNT(*) FROM test_recovery.test_users;" 2>/dev/null)
    
    # Backup IBD file
    local backup_file="${OUTPUT_DIR}/disk_images/before_rm.ibd"
    cp "${ibd_file}" "${backup_file}"
    
    log_info "Simulating rm -rf..."
    
    # Remove table files
    podman exec mysql_recovery_test rm -f "${ibd_file}" 2>/dev/null || true
    podman exec mysql_recovery_test rm -f "${OUTPUT_DIR}/mysql_data/test_recovery/test_users.frm" 2>/dev/null || true
    podman exec mysql_recovery_test mysql -uroot -ptest123 -e "DROP TABLE IF EXISTS test_recovery.test_users;" 2>/dev/null || true
    
    log_info "Files removed. Verifying..."
    
    # Verify table is gone
    local after_count=$(podman exec mysql_recovery_test mysql -uroot -ptest123 -N -e "SELECT COUNT(*) FROM test_recovery.test_users;" 2>/dev/null || echo "table_gone")
    log_info "Table status: ${after_count}"
    
    # Run recovery
    local output_dir="${OUTPUT_DIR}/recovery_output/rmrf"
    mkdir -p "${output_dir}"
    
    log_info "Running mysql_recovery_pro for rm -rf recovery (PhotoRec mode)..."
    
    local start_time=$(date +%s)
    
    "${RECOVERY_BINARY}" \
        --recovery-mode photorec \
        --disk-image "${backup_file}" \
        --output-dir "${output_dir}" \
        --page-size 16384 \
        --min-pages 2 \
        --export-format sql \
        2>&1 | tee "${output_dir}/recovery.log"
    
    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    
    log_info "Recovery completed in ${elapsed} seconds"
    
    # Count recovered records
    local recovered_count=0
    if [[ -f "${output_dir}/recovered.sql" ]]; then
        recovered_count=$(grep -c "INSERT INTO" "${output_dir}/recovered.sql" 2>/dev/null || echo "0")
    fi
    
    # Calculate recovery rate
    local recovery_rate=0
    if [[ ${pre_count} -gt 0 ]]; then
        recovery_rate=$(awk "BEGIN {printf \"%.4f\", ${recovered_count} / ${pre_count} * 100}")
    fi
    
    log_info "Records recovered: ${recovered_count} / ${pre_count}"
    log_info "Recovery rate: ${recovery_rate}%"
    
    # Verify hash match
    local recovered_hash=""
    if [[ -f "${output_dir}/recovered_data.sql" ]]; then
        recovered_hash=$(sha256sum "${output_dir}/recovered_data.sql" | cut -d' ' -f1)
        log_info "Recovered hash: ${recovered_hash}"
    fi
    
    # Return results
    echo "{
        \"test\": \"rmrf_recovery\",
        \"pre_removal_count\": ${pre_count},
        \"recovered_count\": ${recovered_count},
        \"recovery_rate\": ${recovery_rate},
        \"elapsed_seconds\": ${elapsed},
        \"status\": \"$([ $(echo "${recovery_rate} >= 99.9" | bc -l) -eq 1 ] && echo 'PASS' || echo 'FAIL')\"
    }"
}

run_corruption_tests() {
    log_info "Testing corruption resistance..."
    
    local ibd_file="${OUTPUT_DIR}/mysql_data/test_recovery/test_users.ibd"
    local backup_file="${OUTPUT_DIR}/disk_images/corruption_test.ibd"
    
    cp "${ibd_file}" "${backup_file}"
    
    # Test scenarios
    local tests=(
        "Page 3 corruption (SDI page)"
        "Random page corruption"
        "Page 0 corruption (FSP header)"
    )
    
    for test in "${tests[@]}"; do
        log_info "Testing: ${test}"
        
        # Create copy for corruption
        local test_file="${backup_file}.test"
        cp "${backup_file}" "${test_file}"
        
        # Apply corruption
        case "${test}" in
            *SDI*)
                dd if=/dev/zero of="${test_file}" bs=16384 seek=3 count=1 conv=notrunc 2>/dev/null
                ;;
            *Random*)
                local page=$((RANDOM % 100 + 1))
                dd if=/dev/urandom of="${test_file}" bs=16384 seek=${page} count=1 conv=notrunc 2>/dev/null
                ;;
            *Page\ 0*)
                dd if=/dev/zero of="${test_file}" bs=16384 seek=0 count=1 conv=notrunc 2>/dev/null
                ;;
        esac
        
        log_info "  Corruption applied"
        
        # Run recovery
        local output_dir="${OUTPUT_DIR}/recovery_output/corruption"
        mkdir -p "${output_dir}"
        
        "${RECOVERY_BINARY}" \
            --recovery-mode standard \
            --ibd-file "${test_file}" \
            --output-dir "${output_dir}/corruption_test" \
            --export-format sql \
            2>&1 | grep -E "(recordsRecovered|recovered)" | head -5
    done
    
    log_info "Corruption tests completed"
}

generate_report() {
    log_info "Generating test report..."
    
    local report_file="${OUTPUT_DIR}/test_report.md"
    
    cat > "${report_file}" <<'REPORT_HEADER'
# MySQL Recovery Test Report
Generated at: TIMESTAMP

## Test Configuration

- MySQL Version: 5.6
- Test Data Size: 1M rows
- Page Size: 16KB
- Recovery Tool: mysql_recovery_pro

## Test Results

| Test | Records | Recovered | Rate | Status |
|------|---------|-----------|------|--------|
REPORT_HEADER
    
    # Add results
    if [[ -f "${OUTPUT_DIR}/results.json" ]]; then
        jq -r '.scenarios[] | "| \(.test) | \(.pre_truncate_count // .pre_removal_count) | \(.recovered_count) | \(.recovery_rate)% | \(.status) |"' "${OUTPUT_DIR}/results.json" >> "${report_file}"
    fi
    
    # Add summary
    local overall_rate=$(jq -r '[.scenarios[].recovery_rate] | add / length' "${OUTPUT_DIR}/results.json" 2>/dev/null || echo "N/A")
    local pass_count=$(jq '[.scenarios[] | select(.status == "PASS")] | length' "${OUTPUT_DIR}/results.json" 2>/dev/null || echo "0")
    local total_count=$(jq '.scenarios | length' "${OUTPUT_DIR}/results.json" 2>/dev/null || echo "0")
    
    cat >> "${report_file}" <<REPORT_FOOTER

## Summary

- Overall Recovery Rate: ${overall_rate}%
- Tests Passed: ${pass_count} / ${total_count}
- Eligibility for ≥99.9% Recovery: $([[ $(echo "${overall_rate} >= 99.9" | bc -l 2>/dev/null || echo 0) -eq 1 ]] && echo "ELIGIBLE" || echo "NOT ELIGIBLE")

## Conclusions

Generated manually at: $(date)

REPORT_FOOTER
    
    log_info "Report generated: ${report_file}"
    cat "${report_file}"
}

cleanup() {
    log_info "Cleaning up..."
    
    stop_mysql
    rm -rf "${OUTPUT_DIR}" 2>/dev/null || true
    
    log_info "Cleanup complete"
}

# =============================================================================
# Main
# =============================================================================

main() {
    local action="${1:-all}"
    
    echo "========================================"
    echo "MySQL Recovery Test Suite"
    echo "========================================"
    echo ""
    
    case "${action}" in
        setup)
            check_dependencies
            setup_environment
            ;;
        all)
            check_dependencies
            setup_environment
            
            echo ""
            echo "Starting full test suite..."
            echo ""
            
            start_mysql
            
            if [[ $? -eq 0 ]]; then
                # Generate and import test data
                generate_test_data
                import_test_data
                
                # Run tests
                local results=()
                
                results+=("$(run_truncate_recovery)")
                results+=("$(run_rmrf_recovery)")
                run_corruption_tests
                
                # Generate report
                generate_report
                
                stop_mysql
            fi
            
            echo ""
            echo "========================================"
            echo "Test Suite Complete"
            echo "========================================"
            ;;
        truncate)
            check_dependencies
            setup_environment
            start_mysql
            
            generate_test_data 100000
            import_test_data 100000
            
            run_truncate_recovery
            
            stop_mysql
            ;;
        rmrf)
            check_dependencies
            setup_environment
            start_mysql
            
            generate_test_data 100000
            import_test_data 100000
            
            run_rmrf_recovery
            
            stop_mysql
            ;;
        clean)
            cleanup
            ;;
        help|--help|-h)
            echo "MySQL Recovery Test Suite"
            echo ""
            echo "Usage: $0 [command]"
            echo ""
            echo "Commands:"
            echo "  setup      - Setup test environment"
            echo "  all        - Run all tests"
            echo "  truncate   - Run TRUNCATE recovery test"
            echo "  rmrf       - Run rm -rf recovery test"
            echo "  clean      - Cleanup test environment"
            echo "  help       - Show this help"
            ;;
        *)
            log_error "Unknown command: ${action}"
            echo "Usage: $0 [setup|all|truncate|rmrf|clean|help]"
            exit 1
            ;;
    esac
}

main "$@"
