#!/bin/bash
#
# Docker Test Environment Setup for MySQL Recovery Pro
# Deploys MySQL 5.6, 5.7, 8.0, 8.4(LTS), 9.0 containers for testing
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TEST_DATA_DIR="$PROJECT_DIR/test_data"
RESULTS_DIR="$PROJECT_DIR/test_results"

# MySQL versions to test
MYSQL_VERSIONS=("5.6" "5.7" "8.0" "8.4" "9.0")

# Container names
CONTAINER_PREFIX="mysql_recovery_test"

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

# Check if Docker is available
check_docker() {
    if ! command -v docker &> /dev/null; then
        log_error "Docker is not installed or not in PATH"
        exit 1
    fi
    
    if ! docker info &> /dev/null; then
        log_error "Docker daemon is not running"
        exit 1
    fi
    
    log_info "Docker is available"
}

# Create Docker network for testing
create_network() {
    log_info "Creating Docker network for testing..."
    docker network create mysql_recovery_net 2>/dev/null || true
}

# Start MySQL container for specific version
start_mysql_container() {
    local version=$1
    local container_name="${CONTAINER_PREFIX}_${version//./_}"
    
    log_info "Starting MySQL $version container..."
    
    # Remove existing container if exists
    docker rm -f "$container_name" 2>/dev/null || true
    
    # Start container
    case $version in
        "5.6")
            docker run -d \
                --name "$container_name" \
                --network mysql_recovery_net \
                -e MYSQL_ROOT_PASSWORD=test123 \
                -e MYSQL_DATABASE=testdb \
                -v "$TEST_DATA_DIR:/docker-entrypoint-initdb.d" \
                -p 330${version//./}_$version:3306 \
                mysql:5.6 \
                --character-set-server=utf8mb4 \
                --collation-server=utf8mb4_unicode_ci
            ;;
        "5.7")
            docker run -d \
                --name "$container_name" \
                --network mysql_recovery_net \
                -e MYSQL_ROOT_PASSWORD=test123 \
                -e MYSQL_DATABASE=testdb \
                -v "$TEST_DATA_DIR:/docker-entrypoint-initdb.d" \
                -p 330${version//./}_$version:3306 \
                mysql:5.7 \
                --character-set-server=utf8mb4 \
                --collation-server=utf8mb4_unicode_ci
            ;;
        "8.0")
            docker run -d \
                --name "$container_name" \
                --network mysql_recovery_net \
                -e MYSQL_ROOT_PASSWORD=test123 \
                -e MYSQL_DATABASE=testdb \
                -v "$TEST_DATA_DIR:/docker-entrypoint-initdb.d" \
                -p 330${version//./}_$version:3306 \
                mysql:8.0 \
                --character-set-server=utf8mb4 \
                --collation-server=utf8mb4_unicode_ci
            ;;
        "8.4")
            docker run -d \
                --name "$container_name" \
                --network mysql_recovery_net \
                -e MYSQL_ROOT_PASSWORD=test123 \
                -e MYSQL_DATABASE=testdb \
                -v "$TEST_DATA_DIR:/docker-entrypoint-initdb.d" \
                -p 330${version//./}_$version:3306 \
                mysql:8.4-lts \
                --character-set-server=utf8mb4 \
                --collation-server=utf8mb4_unicode_ci
            ;;
        "9.0")
            docker run -d \
                --name "$container_name" \
                --network mysql_recovery_net \
                -e MYSQL_ROOT_PASSWORD=test123 \
                -e MYSQL_DATABASE=testdb \
                -v "$TEST_DATA_DIR:/docker-entrypoint-initdb.d" \
                -p 330${version//./}_$version:3306 \
                mysql:9.0 \
                --character-set-server=utf8mb4 \
                --collation-server=utf8mb4_unicode_ci
            ;;
    esac
    
    # Wait for MySQL to be ready
    log_info "Waiting for MySQL $version to be ready..."
    sleep 10
    
    # Verify MySQL is ready
    local max_attempts=30
    local attempt=0
    while [ $attempt -lt $max_attempts ]; do
        if docker exec "$container_name" mysqladmin -uroot -ptest123 ping &> /dev/null; then
            log_info "MySQL $version is ready"
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 2
    done
    
    log_error "MySQL $version failed to start"
    return 1
}

# Stop MySQL container
stop_mysql_container() {
    local version=$1
    local container_name="${CONTAINER_PREFIX}_${version//./_}"
    
    log_info "Stopping MySQL $version container..."
    docker stop "$container_name" 2>/dev/null || true
    docker rm -f "$container_name" 2>/dev/null || true
}

# Inject test data into MySQL container
inject_test_data() {
    local version=$1
    local container_name="${CONTAINER_PREFIX}_${version//./_}"
    local num_records=${2:-100000}
    
    log_info "Injecting $num_records records into MySQL $version..."
    
    # Create test data SQL
    local temp_sql="/tmp/test_data_$version.sql"
    
    cat > "$temp_sql" << EOF
USE testdb;

-- Create test table with various data types
CREATE TABLE IF NOT EXISTS test_users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(255),
    email VARCHAR(255),
    created_at DATETIME,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    status TINYINT DEFAULT 1,
    score INT,
    description TEXT,
    json_data JSON,
    blob_data BLOB,
    created_date DATE,
    created_time TIME
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Insert test data
INSERT INTO test_users (name, email, created_at, status, score, description, json_data, blob_data, created_date, created_time) VALUES
EOF
    
    # Generate INSERT statements
    for i in $(seq 1 $num_records); do
        local name="user_$i"
        local email="user$i@example.com"
        local created_at="2024-01-01 00:00:00"
        local status=$((i % 3 + 1))
        local score=$((RANDOM % 1000))
        local description="Test user $i description with some random text to make it longer."
        local json_data="{\"user_id\": $i, \"type\": \"test\", \"version\": $version}"
        local blob_data="BLOB_DATA_$i"
        local created_date="2024-01-01"
        local created_time="00:00:00"
        
        if [ $i -lt $num_records ]; then
            echo "('$name', '$email', '$created_at', $status, $score, '$description', '$json_data', '$blob_data', '$created_date', '$created_time')," >> "$temp_sql"
        else
            echo "('$name', '$email', '$created_at', $status, $score, '$description', '$json_data', '$blob_data', '$created_date', '$created_time');" >> "$temp_sql"
        fi
        
        # Progress indicator
        if [ $((i % 10000)) -eq 0 ]; then
            echo "  Inserted $i records..."
        fi
    done
    
    # Import data into MySQL
    docker cp "$temp_sql" "$container_name:/tmp/test_data.sql"
    docker exec "$container_name" mysql -uroot -ptest123 testdb < /tmp/test_data.sql
    
    # Verify data
    local count=$(docker exec "$container_name" mysql -uroot -ptest123 -N -e "SELECT COUNT(*) FROM testdb.test_users")
    log_info "Inserted $count records into MySQL $version"
    
    # Clean up
    rm -f "$temp_sql"
}

# Generate a non-authoritative client-text observation
record_client_text_observation() {
    local version=$1
    local container_name="${CONTAINER_PREFIX}_${version//./_}"
    local observation_path="$RESULTS_DIR/client_text_observation_$version.txt"

    log_warn "Generating non-authoritative client-text observation for MySQL $version"
    mkdir -p "$RESULTS_DIR"
    docker exec "$container_name" mysql -uroot -ptest123 testdb \
        -e "SELECT * FROM test_users ORDER BY id" \
        > "$observation_path"

    sha256sum "$observation_path"
    log_warn "This text hash is not typed ground truth and cannot establish recovery accuracy"
}

# Execute SQL in MySQL container
execute_sql() {
    local version=$1
    local container_name="${CONTAINER_PREFIX}_${version//./_}"
    local sql=$2
    
    docker exec "$container_name" mysql -uroot -ptest123 testdb -e "$sql"
}

# Get MySQL container IP
get_container_ip() {
    local version=$1
    local container_name="${CONTAINER_PREFIX}_${version//./_}"
    
    docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$container_name"
}

# Cleanup all containers
cleanup() {
    log_info "Cleaning up all MySQL containers..."
    
    for version in "${MYSQL_VERSIONS[@]}"; do
        stop_mysql_container "$version"
    done
    
    log_info "Cleanup completed"
}

# Main function
main() {
    local action=${1:-"setup"}
    
    case $action in
        "setup")
            log_info "Setting up Docker test environment..."
            check_docker
            create_network
            
            for version in "${MYSQL_VERSIONS[@]}"; do
                start_mysql_container "$version"
            done
            
            log_info "All MySQL containers are running"
            ;;
        "teardown")
            cleanup
            ;;
        "inject")
            local version=${2:-"8.0"}
            local num_records=${3:-100000}
            
            local container_name="${CONTAINER_PREFIX}_${version//./_}"
            inject_test_data "$version" "$num_records"
            ;;
        "hash")
            local version=${2:-"8.0"}
            record_client_text_observation "$version"
            ;;
        "status")
            log_info "MySQL container status:"
            for version in "${MYSQL_VERSIONS[@]}"; do
                local container_name="${CONTAINER_PREFIX}_${version//./_}"
                if docker ps -a --format '{{.Names}}' | grep -q "^${container_name}$"; then
                    local status=$(docker inspect -f '{{.State.Status}}' "$container_name")
                    echo "  MySQL $version: $status"
                else
                    echo "  MySQL $version: Not found"
                fi
            done
            ;;
        *)
            echo "Usage: $0 {setup|teardown|inject|hash|status} [version] [num_records]"
            exit 1
            ;;
    esac
}

# Run main function
main "$@"
