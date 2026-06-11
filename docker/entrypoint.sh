#!/bin/bash
# =============================================================================
# MySQL Recovery Pro - Docker Entrypoint for Chaos Testing
# Architect: jliang.fang (Global MySQL Kernel QA & Chaos Engineering)
# Purpose: Initialize environment and start chaos test cycle
# =============================================================================

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  MySQL Recovery Pro - Chaos Engine     ${NC}"
echo -e "${BLUE} ========================================${NC}"
echo -e "${YELLOW}  Initialization Script                 ${NC}"
echo -e "${BLUE}========================================${NC}"

# ==================== Environment Setup ====================

export CHAOS_ROOT="/app/mysql_recovery_pro"
export TEST_DATA_DIR="${CHAOS_ROOT}/test_data"
export RESULTS_DIR="${CHAOS_ROOT}/chaos_test_results"
export CORE_DUMP_DIR="${RESULTS_DIR}/core_dumps"
export PATCHES_DIR="${CHAOS_ROOT}/auto_heal_patches"

# Create directories
mkdir -p "${TEST_DATA_DIR}"
mkdir -p "${RESULTS_DIR}"
mkdir -p "${CORE_DUMP_DIR}"
mkdir -p "${PATCHES_DIR}"

# Set ASAN options if enabled
if [ "${USE_ASAN}" = "true" ]; then
    export ASAN_OPTIONS="halt_on_error=1,abort_on_error=1,detect_stack_use_after_return=1,strict_init_order=1"
    export LSAN_OPTIONS="verbosity=1:log_threads=1"
    echo -e "${GREEN}[INFO] AddressSanitizer enabled${NC}"
fi

# Set logging level
export LOG_LEVEL="${LOG_LEVEL:-INFO}"

# ==================== Cleanup Function ====================

cleanup() {
    echo -e "${YELLOW}[CLEANUP] Removing temporary files...${NC}"
    rm -rf /tmp/chaos_* 2>/dev/null || true
    echo -e "${GREEN}[CLEANUP] Done${NC}"
}

trap cleanup EXIT

# ==================== Helper Functions ====================

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_success() {
    echo -e "${BLUE}[SUCCESS]${NC} $1"
}

# ==================== Version Detection ====================

detect_mysql_version() {
    local version_file="/var/lib/mysql/ibdata1"
    if [ -f "$version_file" ]; then
        # Extract version from system header
        local magic=$(xxd -l 4 -s 0 "$version_file" | cut -d' ' -f2)
        if [ "$magic" = "42894073" ]; then
            log_info "MySQL InnoDB file detected"
        fi
    fi
}

# ==================== Data Preparation ====================

prepare_test_data() {
    log_info "Preparing test data..."
    
    # Check if test data exists
    if [ -z "$(ls -A ${TEST_DATA_DIR}/*.ibd 2>/dev/null)" ]; then
        log_info "No test data found, generating..."
        
        cd "${CHAOS_ROOT}"
        python3 scripts/generate_test_data/generate_test_data.py \
            --output "${TEST_DATA_DIR}/test.ibd" \
            --type "all" \
            --page-size 16384
        
        log_success "Test data generated"
    else
        log_info "Existing test data found"
    fi
}

# ==================== Chaos Test Execution ====================

run_chaos_cycle() {
    log_info "Starting chaos test cycle..."
    
    local engine_path="${CHAOS_ROOT}/build/mysql_recovery_pro"
    
    # Check if binary exists
    if [ ! -f "$engine_path" ]; then
        log_warn "Engine binary not found, attempting build..."
        cd "${CHAOS_ROOT}"
        
        if [ -d "build" ]; then
            make -C build clean 2>/dev/null || true
        fi
        
        mkdir -p build
        cmake -B build -S .
        make -C build -j$(nproc)
        
        if [ ! -f "$engine_path" ]; then
            log_error "Failed to build engine"
            return 1
        fi
        
        log_success "Engine built successfully"
    fi
    
    # Run orchestrator
    cd "${CHAOS_ROOT}"
    
    python3 scripts/chaos_orchestrator.py \
        --engine "$engine_path" \
        --test-data "$TEST_DATA_DIR" \
        --results "$RESULTS_DIR" \
        --asan "${USE_ASAN:-false}" \
        --debug "${DEBUG:-false}"
    
    local result=$?
    
    if [ $result -eq 0 ]; then
        log_success "Chaos cycle completed"
        
        # Display results
        if [ -f "${RESULTS_DIR}/chaos_test_report.json" ]; then
            echo -e "\n${BLUE}=== Test Results ===${NC}"
            python3 -c "
import json
with open('${RESULTS_DIR}/chaos_test_report.json') as f:
    report = json.load(f)
    print(f\"Total Tests: {report['summary']['total_tests']}\")
    print(f\"Passed: {report['summary']['passed']}\")
    print(f\"Failed: {report['summary']['failed']}\")
    print(f\"Pass Rate: {report['summary']['pass_rate']*100:.1f}%\")
    print(f\"Avg Recovery Rate: {report['summary']['average_recovery_rate']*100:.1f}%\")
"
        fi
        
        # Check for self-healing patches
        local patches_count=$(find "${PATCHES_DIR}" -name "*.diff" 2>/dev/null | wc -l)
        if [ "$patches_count" -gt 0 ]; then
            log_success "${patches_count} auto-healing patches generated"
        fi
        
    else
        log_error "Chaos cycle failed with exit code $result"
    fi
    
    return $result
}

# ==================== Infinite Loop Driver ====================

run_infinite_cycle() {
    local attempt=1
    
    log_info "Starting infinite chaos driver loop..."
    
    while true; do
        echo -e "\n${BLUE}========================================${NC}"
        echo -e "${BLUE}  Chaos Cycle Attempt #${attempt}      ${NC}"
        echo -e "${BLUE}========================================${NC}"
        
        run_chaos_cycle
        
        if [ $? -eq 0 ]; then
            # Success - continue to next cycle
            log_success "Cycle ${attempt} completed successfully"
        else
            # Failure - attempt auto-rebuild
            log_warn "Cycle ${attempt} failed, attempting rebuild..."
            
            # Try to rebuild with patches
            cd "${CHAOS_ROOT}"
            if [ -f "${RESULTS_DIR}/chaos_test_report.json" ]; then
                # Check for generated patches and apply them
                for patch in "${PATCHES_DIR}"/*.diff; do
                    if [ -f "$patch" ]; then
                        log_info "Applying patch: $patch"
                        patch -p0 < "$patch" 2>/dev/null || log_warn "Patch apply failed"
                    fi
                done
            fi
            
            # Rebuild
            cmake -B build -S .
            make -C build -j$(nproc)
        fi
        
        attempt=$((attempt + 1))
        
        # Run next cycle
        sleep "${CYCLE_INTERVAL:-60}"
    done
}

# ==================== Main ====================

main() {
    log_info "MySQL Recovery Pro Chaos Engine v1.0"
    log_info "Starting initialization..."
    
    # Detect MySQL version
    detect_mysql_version
    
    # Prepare test data
    prepare_test_data
    
    # Run test cycles
    if [ "${INFINITE_CYCLE}" = "true" ]; then
        run_infinite_cycle
    else
        run_chaos_cycle
    fi
}

# Run main
main "$@"
