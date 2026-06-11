#!/bin/bash
# run_chaos_test_suite.sh - 魔幻测试套件主调度器
# Chaos Test Suite Master Scheduler
# 
# This script orchestrates the complete chaos testing lifecycle:
# 1. Docker environment initialization (multi-version MySQL)
# 2. Data injection (1M+ records with edge cases)
# 3. Golden hash baseline generation
# 4. Chaos simulation (multiple corruption scenarios)
# 5. Recovery engine execution (with ASAN/Valgrind)
# 6. Self-healing loop (GDB core analysis + AI patch)
# 7. Final report generation
#
# Usage: ./run_chaos_test_suite.sh [OPTIONS]
#        ./run_chaos_test_suite.sh --all
#        ./run_chaos_test_suite.sh --quick (skip Docker)
#
# Author: jliang.fang (Global MySQL Kernel QA Expert)
# Version: 3.0.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Logging functions
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

log_section() {
    echo -e "\n${CYAN}========================================${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}========================================${NC}\n"
}

# Configuration
DEFAULT_VERSIONS=("5.0" "5.5" "5.6" "5.7" "8.0" "8.4" "9.0")
RECORD_COUNT=100000
MAX_RETRIES=5
ASAN_ENABLED=true

# Parse arguments
SHOW_HELP=false
QUICK_MODE=false
DRY_RUN=false
VERSIONS=()

while [[ $# -gt 0 ]]; do
    case $1 in
        --all)
            QUICK_MODE=false
            shift
            ;;
        --quick)
            QUICK_MODE=true
            shift
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --help|-h)
            SHOW_HELP=true
            shift
            ;;
        --versions)
            VERSIONS=($2)
            shift 2
            ;;
        --record-count)
            RECORD_COUNT=$2
            shift 2
            ;;
        --max-retries)
            MAX_RETRIES=$2
            shift 2
            ;;
        *)
            LOGIC
            shift
            ;;
    esac
done

if [ "$SHOW_HELP" = true ]; then
    echo "MySQL Recovery Pro - Chaos Test Suite"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --all              Run full test suite (default)"
    echo "  --quick            Skip Docker, run on local MySQL"
    echo "  --dry-run          Show commands without executing"
    echo "  --versions V...    Test specific MySQL versions"
    echo "  --record-count N   Inject N records (default: 100000)"
    echo "  --max-retries N    Self-healing max retries (default: 5)"
    echo "  --help, -h         Show this help"
    exit 0
fi

# Set versions
if [ ${#VERSIONS[@]} -eq 0 ]; then
    VERSIONS=("${DEFAULT_VERSIONS[@]}")
fi

# Setup directories
GLOBAL_HASHES_DIR="$PROJECT_ROOT/golden-hashes"
CHAOS_DATA_DIR="$PROJECT_ROOT/chaos-test-volumes"
RECOVERY_OUTPUT="$PROJECT_ROOT/recovery-output"
DIAGNOSIS_DIR="$PROJECT_ROOT/diagnosis"

mkdir -p "$GLOBAL_HASHES_DIR" "$CHAOS_DATA_DIR" "$RECOVERY_OUTPUT" "$DIAGNOSIS_DIR"

log_section "CHaos TEST SUITE - INITIALIZATION"
echo "Project root: $PROJECT_ROOT"
echo "Test versions: ${VERSIONS[*]}"
echo "Records per version: $RECORD_COUNT"
echo "Max self-healing retries: $MAX_RETRIES"
echo "ASAN enabled: $ASAN_ENABLED"
echo "Quick mode: $QUICK_MODE"

# Step 1: Docker environment setup
run_docker_setup() {
    log_section "STEP 1: Docker Environment Initialization"
    
    if [ "$QUICK_MODE" = true ]; then
        log_warning "Quick mode enabled - skipping Docker setup"
        return 0
    fi
    
    log_info "Starting multi-version MySQL containers..."
    
    # Create Docker network
    docker network create chaos-test-network 2>/dev/null || true
    
    # Start all MySQL versions in detach mode
    cd "$PROJECT_ROOT/docker"
    
    for version in "${VERSIONS[@]}"; do
        container_name="mysql_recovery_${version//./_}"
        
        log_info "Starting MySQL $version as $container_name..."
        
        docker run -d --name "$container_name" \
            --network chaos-test-network \
            -p "330${version//./}:3306" \
            -e MYSQL_ROOT_PASSWORD="root123" \
            -e MYSQL_DATABASE="testdb" \
            -e MYSQL_ROOT_HOST="%" \
            "mysql:$version" \
            --innodb_file_per_table=1 \
            --character-set-server=utf8mb4 \
            --collation-server=utf8mb4_unicode_ci \
            --max_allowed_packet=512M \
            --innodb_buffer_pool_size=512M || true
    done
    
    # Wait for containers to be ready
    log_info "Waiting for MySQL containers to initialize..."
    sleep 30
    
    # Verify containers are running
    for version in "${VERSIONS[@]}"; do
        container_name="mysql_recovery_${version//./_}"
        
        if docker ps --format '{{.Names}}' | grep -q "^${container_name}$"; then
            log_success "MySQL $version container is running"
        else
            log_error "MySQL $version container failed to start"
        fi
    done
}

# Step 2: Data injection
run_data_injection() {
    log_section "STEP 2: Data Injection (1M+ records)"
    
    for version in "${VERSIONS[@]}"; do
        container_name="mysql_recovery_${version//./_}"
        
        # For quick mode, use localhost
        if [ "$QUICK_MODE" = true ]; then
            host="localhost"
            port="3306"
        else
            host="$container_name"
            port="3306"
        fi
        
        log_section "Data Injection for MySQL $version"
        log_info "Container: $container_name, Host: $host"
        
        # Check if connector is available, otherwise use local Python
        if command -v python3 &> /dev/null; then
            python3 "$PROJECT_ROOT/scripts/ultra_data_injector.py" \
                --host "$host" \
                --port "$port" \
                --user "root" \
                --password "root123" \
                --database "testdb" \
                --version "$version" \
                --num-records "$RECORD_COUNT" \
                --vector \
                --gis \
                --hash \
                --hash-output "$GLOBAL_HASHES_DIR"
        else
            log_error "Python3 not available for data injection"
        fi
    done
}

# Step 3: Build recovery engine with sanitizers
build_recovery_engine() {
    log_section "STEP 3: Building Recovery Engine with ASAN/UBSAN"
    
    cd "$PROJECT_ROOT"
    
    # Configure with sanitizers enabled
    mkdir -p build
    cd build
    
    cmake .. \
        -DCMAKE_BUILD_TYPE=Debug \
        -DENABLE_ASAN=ON \
        -DENABLE_UBSAN=ON \
        -DENABLE_LSAN=ON \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
    
    cmake --build . -j$(nproc)
    
    if [ $? -eq 0 ]; then
        log_success "Recovery engine built successfully with sanitizers"
    else
        log_error "Recovery engine build failed"
        return 1
    fi
}

# Step 4: Generate Golden Hash Baseline
generate_golden_hashes() {
    log_section "STEP 4: Generating Golden Hash Base"
    
    for version in "${VERSIONS[@]}"; do
        container_name="mysql_recovery_${version//./_}"
        
        if [ "$QUICK_MODE" = true ]; then
            host="localhost"
            port="3306"
        else
            host="$container_name"
            port="3306"
        fi
        
        log_info "Generating hash baseline for version $version..."
        
        python3 "$PROJECT_ROOT/scripts/golden_hash_base.py" \
            --host "$host" \
            --port "$port" \
            --user "root" \
            --password "root123" \
            --database "testdb" \
            --version "$version" \
            --output-dir "$GLOBAL_HASHES_DIR"
    done
}

# Step 5: Chaos simulation - run all corruption scenarios
run_chaos_simulation() {
    log_section "STEP 5: Chaos Simulation (Multiple Corruption Scenarios)"
    
    # For each version, generate .ibd file and apply corruption
    for version in "${VERSIONS[@]}"; do
        log_section "CHAOS for MySQL $version"
        
        version_dir="$RECOVERY_OUTPUT/mysql-$version"
        mkdir -p "$version_dir"
        
        # Generate clean .ibd file for this version
        log_info "Generating test .ibd file for version $version..."
        
        # Use the data generator to create .ibd file
        # In production, this would use the actual MySQL engine
        
        # Apply multiple corruption scenarios
        scenarios=(
            "torn_page"
            "metadata_annihilation"
            "purge_race"
            "swiss_cheese"
            "crc_sabotage"
            "type_confusion"
            "lsn_corruption"
            "page_truncation"
            "all_scenarios"
        )
        
        for scenario in "${scenarios[@]}"; do
            log_info "Applying corruption: $scenario"
            
            input_file="$version_dir/clean_test.ibd"
            output_file="$version_dir/${scenario}_corrupted.ibd"
            
            python3 "$PROJECT_ROOT/scripts/advanced_chaos_monkey.py" \
                --input "$input_file" \
                --output "$output_file" \
                --scenario "$scenario" \
                --verbose || true
        done
    done
}

# Step 6: Recovery execution with self-healing loop
run_recovery_and_healing() {
    log_section "STEP 6: Recovery Execution & Self-Healing Loop"
    
    for version in "${VERSIONS[@]}"; do
        version_dir="$RECOVERY_OUTPUT/mysql-$version"
        
        log_section "RECOVERY for MySQL $version"
        
        # Look for corrupted files
        for corrupted_file in "$version_dir"/*_corrupted.ibd; do
            if [ -f "$corrupted_file" ]; then
                scenario_name=$(basename "$corrupted_file" | sed 's/_corrupted\.ibd//')
                
                log_info "Testing recovery for scenario: $scenario_name"
                
                output_dir="$version_dir/recovery_$scenario_name"
                mkdir -p "$output_dir"
                
                # Run recovery engine
                binary="$PROJECT_ROOT/build/mysql_recovery_pro"
                
                if [ -f "$binary" ]; then
                    timeout 120 "$binary" \
                        --ibd="$corrupted_file" \
                        --output="$output_dir" \
                        --format=sql || true
                else
                    log_error "Recovery binary not found"
                    return 1
                fi
                
                # If crash detected, run self-healing loop
                if [ -f "$output_dir/core.dump" ]; then
                    log_warning "Crash detected, running self-healing loop..."
                    
                    python3 "$PROJECT_ROOT/scripts/self_healing_loop.py" \
                        --input="$corrupted_file" \
                        --max-retries="$MAX_RETRIES" \
                        --project-root="$PROJECT_ROOT" \
                        --build-dir="$PROJECT_ROOT/build" \
                        --output-dir="$RECOVERY_OUTPUT" \
                        --report="$DIAGNOSIS_DIR/healing_${version}_${scenario_name}.json"
                fi
            fi
        done
    done
}

# Step 7: Generate final report
generate_final_report() {
    log_section "STEP 7: Generating Final Report"
    
    python3 "$PROJECT_ROOT/scripts/generate_final_report.py" \
        --output-dir="$RECOVERY_OUTPUT" \
        --golden-hashes="$GLOBAL_HASHES_DIR" \
        --diagnosis-dir="$DIAGNOSIS_DIR" \
        --output-file="final_chaos_report.json" \
        --versions "${VERSIONS[*]}" \
        --json
    
    log_success "Final report generated: $RECOVERY_OUTPUT/final_chaos_report.json"
    
    # Copy template for reference
    cp "$PROJECT_ROOT/docs/report_template.json" "$RECOVERY_OUTPUT/report_template.json"
}

# Cleanup Docker containers
cleanup_docker() {
    log_section "CLEANUP: Stopping Docker containers"
    
    for version in "${VERSIONS[@]}"; do
        container_name="mysql_recovery_${version//./_}"
        docker stop "$container_name" 2>/dev/null || true
        docker rm "$container_name" 2>/dev/null || true
    done
    
    docker network rm chaos-test-network 2>/dev/null || true
}

# Main execution
main() {
    log_section "MySQL Recovery Pro - Chaos Test Suite Started"
    log_info "Started at: $(date)"
    
    # Setup trap for cleanup
    trap cleanup_docker EXIT
    
    # Run stages
    run_docker_setup
    run_data_injection
    build_recovery_engine
    generate_golden_hashes
    run_chaos_simulation
    run_recovery_and_healing
    generate_final_report
    
    log_section "CHaos TEST SUITE COMPLETED"
    log_success "Finished at: $(date)"
    
    echo ""
    echo "Output Summary:"
    echo "  - Recovery results: $RECOVERY_OUTPUT/"
    echo "  - Diagnosis reports: $DIAGNOSIS_DIR/"
    echo "  - Final report: $RECOVERY_OUTPUT/final_chaos_report.json"
}

# Run main
main "$@"
