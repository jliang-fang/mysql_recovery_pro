#!/bin/bash
# build_mysql_images.sh - Build MySQL Docker Images for Recovery Testing
# Author: jliang.fang (Global MySQL Kernel QA Expert)
# Purpose: Build multi-version MySQL images with custom configurations for chaos testing

set -e

echo "============================================"
echo "MySQL Recovery Pro - Image Build Suite"
echo "============================================"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-context"

# Create build directory
mkdir -p "${BUILD_DIR}"

# Function to build MySQL image with custom config
build_mysql_image() {
    local VERSION=$1
    local TAG=$2
    local DOCKERFILE=$3
    
    echo "Building MySQL ${VERSION}..."
    
    # Create build context directory
    local BUILD_CTX="${BUILD_DIR}/mysql-${VERSION}"
    mkdir -p "${BUILD_CTX}"
    
    # Copy Dockerfile
    cp "${SCRIPT_DIR}/dockerfiles/${DOCKERFILE}" "${BUILD_CTX}/Dockerfile"
    
    # Build image
    docker build -t "mysql:${TAG}" "${BUILD_CTX}" \
        --build-arg MYSQL_VERSION="${VERSION}" \
        --build-arg BUILD_DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    
    echo "✓ MySQL ${VERSION} (tag: ${TAG}) built successfully"
    echo ""
}

# Function to build simplified MySQL image for injection
build_injector_image() {
    local VERSION=$1
    local TAG=$2
    
    echo "Building Data Injector for MySQL ${VERSION}..."
    
    local BUILD_CTX="${BUILD_DIR}/injector-${VERSION}"
    mkdir -p "${BUILD_CTX}"
    
    cat > "${BUILD_CTX}/Dockerfile" <<EOF
FROM mysql:${VERSION}

# Install Python and required tools
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    curl \
    wget \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install --no-cache-dir mysql-connector-python requests

# Copy data injector script
COPY ../scripts/data_injector.py /injector-scripts/
COPY ../golden-hashes /golden-hashes

# Set working directory
WORKDIR /injector-scripts

# Make script executable
RUN chmod +x /injector-scripts/data_injector.py

# Entry point
CMD ["python3", "/injector-scripts/data_injector.py", "--num-records", "100000"]
EOF
    
    docker build -t "mysql-injector:${TAG}" "${BUILD_CTX}"
    echo "✓ Injector for MySQL ${VERSION} built successfully"
    echo ""
}

# Function to build chaos monkey image
build_chaos_image() {
    local TAG=$1
    
    echo "Building Chaos Monkey image..."
    
    local BUILD_CTX="${BUILD_DIR}/chaos-monkey"
    mkdir -p "${BUILD_CTX}"
    
    # Copy chaos scripts
    cp -r "${SCRIPT_DIR}/../scripts/chaos_monkey.py" "${BUILD_CTX}/"
    cp -r "${SCRIPT_DIR}/../scripts/multi_badblock_generator.py" "${BUILD_CTX}/"
    
    cat > "${BUILD_CTX}/Dockerfile" <<'EOF'
FROM python:3.11-slim

RUN apt-get update && apt-get install -y \
    mysql-client \
    dd \
    coreutils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /chaos-scripts

COPY *.py /chaos-scripts/

RUN chmod +x /chaos-scripts/*.py

CMD ["python3", "/chaos-scripts/chaos_monkey.py"]
EOF
    
    docker build -t "mysql-chaos:${TAG}" "${BUILD_CTX}"
    echo "✓ Chaos Monkey image built successfully"
    echo ""
}

# Function to build recovery engine image
build_recovery_image() {
    local TAG=$1
    
    echo "Building Recovery Engine image..."
    
    local BUILD_CTX="${BUILD_DIR}/recovery-engine"
    mkdir -p "${BUILD_CTX}"
    
    # Copy recovery engine and dependencies
    cp -r "${SCRIPT_DIR}/../build/" "${BUILD_CTX}/" 2>/dev/null || true
    cp -r "${SCRIPT_DIR}/../src/" "${BUILD_CTX}/" 2>/dev/null || true
    cp -r "${SCRIPT_DIR}/../include/" "${BUILD_CTX}/" 2>/dev/null || true
    cp -r "${SCRIPT_DIR}/../CMakeLists.txt" "${BUILD_CTX}/" 2>/dev/null || true
    
    cat > "${BUILD_CTX}/Dockerfile" <<'EOF'
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    g++ \
    make \
    libmysqlclient-dev \
    zlib1g-dev \
    liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc) && \
    cp build/mysql_recovery_pro /usr/local/bin/

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libmysqlclient21 \
    zlib1g \
    liburing1 \
    gdb \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/bin/mysql_recovery_pro /usr/local/bin/

WORKDIR /recovery

CMD ["mysql_recovery_pro", "--help"]
EOF
    
    docker build -t "mysql-recovery:${TAG}" "${BUILD_CTX}"
    echo "✓ Recovery Engine image built successfully"
    echo ""
}

# Main execution
main() {
    echo "Step 1: Building MySQL Server Images..."
    echo ""
    
    # Build MySQL server images for each version
    build_mysql_image "5.0.96" "5.0" "Dockerfile.mysql-5.0"
    build_mysql_image "5.5.62" "5.5" "Dockerfile.mysql-5.5"
    build_mysql_image "5.6.51" "5.6" "Dockerfile.mysql-5.6"
    build_mysql_image "5.7.44" "5.7" "Dockerfile.mysql-5.7"
    build_mysql_image "8.0.36" "8.0" "Dockerfile.mysql-8.0"
    build_mysql_image "8.4.0" "8.4" "Dockerfile.mysql-8.4"
    build_mysql_image "9.0.0" "9.0" "Dockerfile.mysql-9.0"
    
    echo ""
    echo "Step 2: Building Data Injector Images..."
    echo ""
    
    # Build injector images for each version
    build_injector_image "5.0" "5.0"
    build_injector_image "5.5" "5.5"
    build_injector_image "5.6" "5.6"
    build_injector_image "5.7" "5.7"
    build_injector_image "8.0" "8.0"
    build_injector_image "8.4" "8.4"
    build_injector_image "9.0" "9.0"
    
    echo ""
    echo "Step 3: Building Specialized Images..."
    echo ""
    
    # Build specialized tool images
    build_chaos_image "latest"
    build_recovery_image "latest"
    
    echo "============================================"
    echo "All images built successfully!"
    echo "============================================"
    echo ""
    echo "Available images:"
    docker images | grep -E "(mysql:mysql-injector|mysql-chaos|mysql-recovery)"
}

# Check if specific version requested
if [ $# -gt 0 ]; then
    case "$1" in
        5.0|5.5|5.6|5.7|8.0|8.4|9.0)
            build_mysql_image "$1" "$1" "Dockerfile.mysql-$1"
            ;;
        injector)
            build_injector_image "$2" "$2"
            ;;
        chaos)
            build_chaos_image "latest"
            ;;
        recovery)
            build_recovery_image "latest"
            ;;
        all)
            main
            ;;
        *)
            echo "Usage: $0 [5.0|5.5|5.6|5.7|8.0|8.4|9.0|injector|chaos|recovery|all]"
            exit 1
            ;;
    esac
else
    main
fi
