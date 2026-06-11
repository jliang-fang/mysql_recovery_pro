#!/bin/bash
# =============================================================================
# MySQL 5.6 Podman Setup with 5M Records
# Purpose: Set up MySQL 5.6 container with 5+ million records for validation
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Configuration
CONTAINER_NAME="mysql_recovery_56"
MYSQL_PORT=33056
MYSQL_ROOT_PWD="root123"
DB_NAME="recovery_test"
TABLE_NAME="test_users_5m"

# Paths
IBD_SOURCE="${PROJECT_ROOT}/tools/test_mysql56_5m.ibd"
OUTPUT_DIR="/tmp/mysql56_setup_$(date +%Y%m%d_%H%M%S)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[PASS]${NC} $1"; }
log_error() { echo -red "[ERROR]${NC} $1"; }
log_section() { echo -e "\n${CYAN}========================================${NC}\n$1\n${CYAN}========================================\n"; }

# =============================================================================
# Step 1: Start MySQL 5.6 Container
# =============================================================================

start_mysql_56() {
    log_section "Step 1: Start MySQL 5.6 Container"
    
    # Check if container already exists
    if podman ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
        if podman ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            log_info "Container already running: ${CONTAINER_NAME}"
            return 0
        else
            log_info "Container exists but stopped, starting..."
            podman start "${CONTAINER_NAME}"
            sleep 3
            return 0
        fi
    fi
    
    log_info "Starting MySQL 5.6 container..."
    
    # Pull image if not present
    if ! podman images | grep -q "mysql:5.6"; then
        log_info "Pulling mysql:5.6 image..."
        podman pull mysql:5.6
    fi
    
    # Create container with exposed port and volume
    podman run -d \
        --name "${CONTAINER_NAME}" \
        -e MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PWD}" \
        -e MYSQL_DATABASE="${DB_NAME}" \
        -p "${MYSQL_PORT}:3306" \
        -v "${PROJECT_ROOT}/test_data:/docker-entrypoint-initdb.d" \
        --shm-size=1g \
        mysql:5.6 \
        --max_allowed_packet=1G \
        --innodb_buffer_pool_size=2G \
        --innodb_log_file_size=256M \
        --innodb_flush_log_at_trx_commit=2
    
    log_success "MySQL 5.6 container started: ${CONTAINER_NAME}"
    
    # Wait for MySQL to be ready
    log_info "Waiting for MySQL to be ready..."
    local max_attempts=30
    local attempt=0
    while [ $attempt -lt $max_attempts ]; do
        if podman exec "${CONTAINER_NAME}" mysqladmin -uroot -p"${MYSQL_ROOT_PWD}" ping &>/dev/null; then
            log_success "MySQL is ready!"
            return 0
        fi
        attempt=$((attempt + 1))
        log_info "Attempt ${attempt}/${max_attempts} - waiting..."
        sleep 2
    done
    
    log_error "MySQL failed to start within timeout"
    return 1
}

# =============================================================================
# Step 2: Create Test Table
# =============================================================================

create_test_table() {
    log_section "Step 2: Create Test Table"
    
    log_info "Creating database and table..."
    
    podman exec "${CONTAINER_NAME}" mysql -uroot -p"${MYSQL_ROOT_PWD}" -e "
        CREATE DATABASE IF NOT EXISTS ${DB_NAME};
        USE ${DB_NAME};
        
        DROP TABLE IF EXISTS ${TABLE_NAME};
        
        CREATE TABLE ${TABLE_NAME} (
            id BIGINT PRIMARY KEY AUTO_INCREMENT,
            user_id VARCHAR(50),
            email VARCHAR(255),
            name VARCHAR(100),
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            INDEX idx_email (email),
            INDEX idx_user_id (user_id)
        ) ENGINE=InnoDB ROW_FORMAT=COMPACT;
    "
    
    log_success "Table ${TABLE_NAME} created in ${DB_NAME}"
}

# =============================================================================
# Step 3: Bulk Insert 5M Records
# =============================================================================

bulk_insert_records() {
    log_section "Step 3: Bulk Insert 5M Records"
    
    local num_records=${1:-5500000}
    local batch_size=10000
    
    log_info "Inserting ${num_records} records in batches of ${batch_size}..."
    
    local start_time=$(date +%s)
    local records_inserted=0
    
    # Use MySQL's LOAD DATA INFILE approach - generate SQL file and load
    local sql_file="/tmp/insert_${TABLE_NAME}.sql"
    
    log_info "Generating SQL insert file..."
    
    echo "USE ${DB_NAME};" > "${sql_file}"
    echo "SET autocommit=0;" >> "${sql_file}"
    
    local batch_count=0
    for ((i = 1; i <= num_records; i++)); do
        if [ $((i % batch_size)) -eq 1 ]; then
            echo "INSERT INTO ${TABLE_NAME} (user_id, email, name, created_at) VALUES" >> "${sql_file}"
            batch_count=$((batch_count + 1))
        fi
        
        user_id="user_$(printf '%08d' $i)"
        email="user${i}@test.com"
        name="User_$i"
        timestamp="2024-01-$(printf '%02d' $(( (i % 28) + 1 ))) 10:00:00"
        
        echo "  ('${user_id}', '${email}', '${name}', '${timestamp}')${i%batch_size}" >> "${sql_file}"
        
        if [ $((i % batch_size)) -eq 0 ]; then
            echo ";" >> "${sql_file}"
        elif [ $i -eq $num_records ]; then
            echo ";" >> "${sql_file}"
        else
            echo "," >> "${sql_file}"
        fi
        
        # Progress indicator
        if [ $((i % 1000000)) -eq 0 ]; then
            log_info "Inserted $i records..."
        fi
    done
    
    echo "COMMIT;" >> "${sql_file}"
    echo "SET autocommit=1;" >> "${sql_file}"
    
    local end_time=$(date +%s)
    log_success "SQL file generated in $((end_time - start_time))s"
    
    # Load data into MySQL
    log_info "Loading data into MySQL..."
    
    local load_start=$(date +%s)
    
    # Import the SQL file
    cat "${sql_file}" | podman exec -i "${CONTAINER_NAME}" mysql -uroot -p"${MYSQL_ROOT_PWD}"
    
    local load_end=$(date +%s)
    log_success "Data loaded in $((load_end - load_start))s"
    
    # Verify count
    local actual_count=$(podman exec "${CONTAINER_NAME}" mysql -uroot -p"${MYSQL_ROOT_PWD}" -e "
        SELECT COUNT(*) FROM ${DB_NAME}.${TABLE_NAME};
    " | tail -1)
    
    log_success "Inserted ${actual_count} records"
    
    # Show table info
    log_info "Table info:"
    podman exec "${CONTAINER_NAME}" mysql -uroot -p"${MYSQL_ROOT_PWD}" -e "
        SELECT 
            TABLE_NAME, 
            ENGINE, 
            ROW_FORMAT, 
            TABLE_ROWS, 
            ROUND((DATA_LENGTH + INDEX_LENGTH) / 1024 / 1024, 2) AS 'Size (MB)'
        FROM information_schema.TABLES 
        WHERE TABLE_SCHEMA = '${DB_NAME}' AND TABLE_NAME = '${TABLE_NAME}';
    "
    
    return 0
}

# =============================================================================
# Step 4: Export Golden Data (for hash verification)
# =============================================================================

export_golden_data() {
    log_section "Step 4: Export Golden Data"
    
    local output_csv="${OUTPUT_DIR}/golden_data.csv"
    
    log_info "Exporting golden data to CSV..."
    
    # Export to CSV
    podman exec "${CONTAINER_NAME}" mysql -uroot -p"${MYSQL_ROOT_PWD}" -N -e "
        SELECT id, user_id, email, name, created_at 
        FROM ${DB_NAME}.${TABLE_NAME} 
        ORDER BY id
    " > "${output_csv}"
    
    local record_count=$(wc -l < "${output_csv}")
    log_success "Exported ${record_count} records to ${output_csv}"
    
    # Calculate hash
    local csv_hash=$(sha256sum "${output_csv}" | cut -d' ' -f1)
    local file_size=$(stat -c%s "${output_csv}")
    
    log_info "Golden data hash: ${csv_hash}"
    log_info "File size: ${file_size} bytes (~$((file_size/1024/1024)) MB)"
    
    return 0
}

# =============================================================================
# Step 5: Verify Data Integrity
# =============================================================================

verify_data_integrity() {
    log_section "Step 5: Verify Data Integrity"
    
    log_info "Checking data statistics..."
    
    podman exec "${CONTAINER_NAME}" mysql -uroot -p"${MYSQL_ROOT_PWD}" -e "
        USE ${DB_NAME};
        
        SELECT 
            'Total Records' AS Metric, 
            COUNT(*) AS Value 
        FROM ${TABLE_NAME}
        
        UNION ALL
        
        SELECT 
            'Min ID', 
            MIN(id) 
        FROM ${TABLE_NAME}
        
        UNION ALL
        
        SELECT 
            'Max ID', 
            MAX(id) 
        FROM ${TABLE_NAME}
        
        UNION ALL
        
        SELECT 
            'Sum of IDs', 
            SUM(id) 
        FROM ${TABLE_NAME};
        
        SELECT 
            'Unique user_ids', 
            COUNT(DISTINCT user_id) 
        FROM ${TABLE_NAME};
        
        SELECT 
            'Unique emails', 
            COUNT(DISTINCT email) 
        FROM ${TABLE_NAME};
    "
    
    return 0
}

# =============================================================================
# Main Execution
# =============================================================================

main() {
    echo ""
    echo -e "${GREEN}==========================================${NC}"
    echo -e "${GREEN}MySQL 5.6 Setup with 5M Records${NC}"
    echo -e "${GREEN}Validation Pipeline${NC}"
    echo -e "${GREEN}==========================================${NC}"
    echo ""
    
    # Create output directory
    mkdir -p "${OUTPUT_DIR}"
    
    # Run steps
    start_mysql_56 || exit 1
    create_test_table || exit 1
    bulk_insert_records 5500000 || exit 1
    export_golden_data || exit 1
    verify_data_integrity || exit 1
    
    echo ""
    echo -e "${GREEN}==========================================${NC}"
    echo -e "${GREEN}SETUP COMPLETE${NC}"
    echo -e "${GREEN}==========================================${NC}"
    echo ""
    echo "  Container: ${CONTAINER_NAME}"
    echo "  Database: ${DB_NAME}"
    echo "  Table: ${TABLE_NAME}"
    echo "  Port: ${MYSQL_PORT}"
    echo "  Golden Data: ${OUTPUT_DIR}/golden_data.csv"
    echo ""
    echo "Next steps:"
    echo "  1. Run recovery tests: bash scripts/test_truncate_rmrf_recovery.sh"
    echo "  2. Generate hashes: python3 tools/generate_5m_golden_data.py --gen-hashes <csv>"
    echo "  3. Validate recovery: Compare recovered data against golden hash"
    echo ""
}

main "$@"
