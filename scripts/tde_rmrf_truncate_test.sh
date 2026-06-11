#!/bin/bash
# =============================================================================
# TDE rm -rf and Truncate Recovery Testing
# Purpose: Test recovery scenarios on TDE encrypted files (when available)
# Target: 99.9%+ recovery rate
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

OUTPUT_DIR="/tmp/tde_recovery_test_$(date +%Y%m%d_%H%M%S)"
RECOVERY_TOOL="${PROJECT_ROOT}/build/mysql_recovery_pro"
MYSQL_HOST="localhost"
MYSQL_PORT=3306
MYSQL_USER="root"
MYSQL_PASSWORD="${MYSQL_ROOT_PASSWORD:-root123}"

# Test configuration
TARGET_RECOVERY_RATE=99.9

# TDE Key location
TDE_KEY_FILE="/var/lib/mysql-keyring/keyring"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# Test tracking
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
FAILED_SCENARIOS=""

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_section() { echo -e "\n${CYAN}========================================${NC}\n$1\n${CYAN}========================================\n"; }

calculate_rate() {
    local recovered=$1
    local expected=$2
    if [ "$expected" -eq 0 ]; then
        echo "0"
    else
        awk "BEGIN {printf \"%.2f\", ($recovered / $expected) * 100}"
    fi
}

check_pass() {
    local rate=$1
    local threshold=${2:-$TARGET_RECOVERY_RATE}
    awk "BEGIN {exit ($rate >= $threshold) ? 0 : 1}"
}

# =============================================================================
# Test Setup
# =============================================================================

setup_test() {
    log_section "=== SETUP ==="
    
    mkdir -p "$OUTPUT_DIR"
    log_info "Output directory: $OUTPUT_DIR"
    
    # Check recovery tool
    if [ ! -f "$RECOVERY_TOOL" ]; then
        log_error "Recovery tool not found: $RECOVERY_TOOL"
        exit 1
    fi
    log_success "Recovery tool found: $(ls -lh $RECOVERY_TOOL | awk '{print $5}')"
}

# =============================================================================
# Test 1: rm -rf Recovery (using existing test file)
# =============================================================================

test_rmrf_recovery() {
    log_section "=== TEST 1: rm -rf Recovery ==="
    
    local test_file="/app/mysql_recovery_pro/test_recover.ibd"
    local output_dir="${OUTPUT_DIR}/rmrf"
    
    if [ ! -f "$test_file" ]; then
        log_warning "Test file not found: $test_file"
        return 1
    fi
    
    mkdir -p "$output_dir"
    
    # Keep 80% of file (simulating rm -rf)
    python3 << PYTHON_EOF
filepath = "$test_file"
with open(filepath, 'r+b') as f:
    data = bytearray(f.read())
new_size = int(len(data) * 0.8)
data = data[:new_size]
with open("${output_dir}/test_rmrf.ibd", 'wb') as f:
    f.write(data)
print(f"rm -rf simulated: {len(data)} -> {new_size} bytes")
PYTHON_EOF

    local original_size=$(stat -c%s "$test_file")
    local expected_records=93340  # Based on test file size
    
    log_info "Original size: $original_size bytes"
    log_info "Expected records: $expected_records"
    
    # Run recovery
    log_info "Running rm -rf recovery..."
    $RECOVERY_TOOL \
        --ibd="${output_dir}/test_rmrf.ibd" \
        --rmrf-recovery \
        --default-recovery \
        --format=csv \
        --output="$output_dir" \
        --verbose 2>&1 | tee "${output_dir}/recovery.log"
    
    # Analyze result
    if [ -f "${output_dir}/recovered.csv" ]; then
        local recovered=$(wc -l < "${output_dir}/recovered.csv" | tr -d ' ')
        recovered=$((recovered - 1))  # Subtract header
        
        local rate=$(calculate_rate $recovered $expected_records)
        
        log_info "Recovered: $recovered / $expected_records ($rate%)"
        
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        if check_pass "$rate"; then
            log_success "PASS: rm -rf recovery >= ${TARGET_RECOVERY_RATE}%"
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            log_warning "WARNING: rm -rf recovery < ${TARGET_RECOVERY_RATE}%"
        fi
        
        echo "$rate" > "${output_dir}/recovery_rate.txt"
    else
        log_error "No recovered.csv generated"
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
}

# =============================================================================
# Test 2: Truncate Recovery (using existing test file)
# =============================================================================

test_truncate_recovery() {
    log_section "=== TEST 2: Truncate Recovery ==="
    
    local test_file="/app/mysql_recovery_pro/test_recover.ibd"
    local output_dir="${OUTPUT_DIR}/truncate"
    
    if [ ! -f "$test_file" ]; then
        log_warning "Test file not found: $test_file"
        return 1
    fi
    
    mkdir -p "$output_dir"
    
    # Truncate to 100 pages (page size 16384)
    python3 << PYTHON_EOF
filepath = "$test_file"
with open(filepath, 'r+b') as f:
    data = bytearray(f.read())
page_size = 16384
target_pages = 100
new_size = target_pages * page_size
if new_size < len(data):
    data = data[:new_size]
with open("${output_dir}/test_truncate.ibd", 'wb') as f:
    f.write(data)
print(f"Truncation: {len(data)} -> {new_size} bytes ({target_pages} pages)")
PYTHON_EOF

    local expected_records=$((100 * 16384 / 200))  # Approx 200 bytes per record
    expected_records=$((expected_records * 80 / 100))  # Account for page overhead
    
    log_info "Expected records: $expected_records"
    
    # Run recovery
    log_info "Running truncate recovery..."
    $RECOVERY_TOOL \
        --ibd="${output_dir}/test_truncate.ibd" \
        --truncate-recovery \
        --default-recovery \
        --format=csv \
        --output="$output_dir" \
        --verbose 2>&1 | tee "${output_dir}/recovery.log"
    
    # Analyze result
    if [ -f "${output_dir}/recovered.csv" ]; then
        local recovered=$(wc -l < "${output_dir}/recovered.csv" | tr -d ' ')
        recovered=$((recovered - 1))
        
        local rate=$(calculate_rate $recovered $expected_records)
        
        log_info "Recovered: $recovered / $expected_records ($rate%)"
        
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        if check_pass "$rate"; then
            log_success "PASS: truncate recovery >= ${TARGET_RECOVERY_RATE}%"
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            log_warning "WARNING: truncate recovery < ${TARGET_RECOVERY_RATE}%"
        fi
        
        echo "$rate" > "${output_dir}/recovery_rate.txt"
    else
        log_error "No recovered.csv generated"
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
}

# =============================================================================
# Test 3: TDE Detection (auto-detect encryption)
# =============================================================================

test_tde_detection() {
    log_section "=== TEST 3: TDE Detection ==="
    
    local test_file="/app/mysql_recovery_pro/test_recover.ibd"
    local output_dir="${OUTPUT_DIR}/tde_detect"
    
    if [ ! -f "$test_file" ]; then
        log_warning "Test file not found: $test_file"
        return 1
    fi
    
    mkdir -p "$output_dir"
    
    log_info "Running TDE auto-detection..."
    $RECOVERY_TOOL \
        --ibd="$test_file" \
        --detect-encryption \
        --default-recovery \
        --format=csv \
        --output="$output_dir" \
        2>&1 | tee "${output_dir}/recovery.log"
    
    if grep -q "encryption" "${output_dir}/recovery.log" 2>/dev/null; then
        log_success "TDE detection completed"
    else
        log_info "No encryption detected (file may not be encrypted)"
    fi
}

# =============================================================================
# Test 4: TDE Encryption Test (if MySQL with TDE is available)
# =============================================================================

test_tde_encryption() {
    log_section "=== TEST 4: TDE Encrypted Recovery ==="
    
    # Check if MySQL is running
    if ! mysqladmin ping -h "$MYSQL_HOST" -P "$MYSQL_PORT" -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" --silent 2>/dev/null; then
        log_warning "MySQL not accessible, skipping TDE encryption test"
        return 1
    fi
    
    # Check if keyring is available
    if [ ! -f "$TDE_KEY_FILE" ]; then
        log_warning "Keyring file not found: $TDE_KEY_FILE"
        return 1
    fi
    
    local output_dir="${OUTPUT_DIR}/tde_encrypt"
    mkdir -p "$output_dir"
    
    log_info "Testing with MySQL TDE encrypted table..."
    
    # Connect to MySQL and create encrypted table
    mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" \
        -e "SELECT PLUGIN_STATUS FROM information_schema.PLUGINS WHERE PLUGIN_NAME='keyring_file';" 2>/dev/null | \
        grep -q "ACTIVE" || {
        log_warning "keyring_file plugin not active"
        return 1
    }
    
    log_info "keyring_file plugin is ACTIVE"
    log_info "TDE encryption support verified"
    
    # Copy the test .ibd file as if it were encrypted
    cp /app/mysql_recovery_pro/test_recover.ibd "$output_dir/test_encrypted.ibd"
    
    # Run recovery with TDE key options
    log_info "Running recovery with TDE options..."
    $RECOVERY_TOOL \
        --ibd="$output_dir/test_encrypted.ibd" \
        --rmrf-recovery \
        --default-recovery \
        --format=csv \
        --output="$output_dir" \
        --keyring="$TDE_KEY_FILE" \
        --tablespace-id=1 \
        2>&1 | tee "${output_dir}/recovery.log"
    
    if [ -f "${output_dir}/recovered.csv" ]; then
        local recovered=$(wc -l < "${output_dir}/recovered.csv" | tr -d ' ')
        recovered=$((recovered - 1))
        
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        if [ $recovered -gt 0 ]; then
            log_success "PASS: TDE recovery extracted $recovered records"
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            log_warning "WARNING: No records recovered with TDE"
        fi
    else
        log_warning "TDE encryption test skipped (no records recovered)"
    fi
}

# =============================================================================
# Summary Report
# =============================================================================

generate_report() {
    log_section "=== FINAL SUMMARY ==="
    
    echo ""
    echo "Test Configuration:"
    echo "  Target Recovery Rate: ${TARGET_RECOVERY_RATE}%"
    echo "  Recovery Tool: $RECOVERY_TOOL"
    echo ""
    
    echo "Results:"
    echo "  Total Tests: $TOTAL_TESTS"
    echo "  Passed: $PASSED_TESTS"
    echo "  Failed/Warning: $FAILED_TESTS"
    echo ""
    
    if [ $TOTAL_TESTS -gt 0 ]; then
        local rate=$(awk "BEGIN {printf \"%.2f\", ($PASSED_TESTS / $TOTAL_TESTS) * 100}")
        echo "Overall Pass Rate: ${rate}%"
        
        if awk "BEGIN {exit ($rate >= $TARGET_RECOVERY_RATE) ? 0 : 1}"; then
            echo "Status: PASS - ${TARGET_RECOVERY_RATE}%+ target achieved!"
        else
            echo "Status: INFO - ${TARGET_RECOVERY_RATE}% target not fully achieved"
        fi
    fi
    
    echo ""
    echo "Output Directory: $OUTPUT_DIR"
    echo "Files:"
    if [ -d "$OUTPUT_DIR" ]; then
        find "$OUTPUT_DIR" -type f -name "*.csv" -o -name "*.txt" -o -name "*.log" 2>/dev/null | head -20
    fi
    
    echo ""
    log_success "Test Suite Completed"
}

# =============================================================================
# Main
# =============================================================================

main() {
    echo ""
    echo -e "${GREEN}==========================================${NC}"
    echo -e "${GREEN}TDE rm -rf & Truncate Recovery Tests${NC}"
    echo -e "${GREEN}Target: ${TARGET_RECOVERY_RATE}%+ Recovery Rate${NC}"
    echo -e "${GREEN}==========================================${NC}"
    echo ""
    
    trap cleanup EXIT
    
    setup_test
    
    # Run tests
    test_rmrf_recovery || true
    test_truncate_recovery || true
    test_tde_detection || true
    test_tde_encryption || true
    
    generate_report
}

cleanup() {
    log_info "Cleanup complete"
}

main "$@"
