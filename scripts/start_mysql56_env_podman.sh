#!/bin/bash
# =============================================================================
# MySQL 5.6 Recovery Test Environment Setup (Podman)
# Author: jliang.fang (Global MySQL Kernel QA Expert)
# Purpose: Start MySQL 5.6 Podman environment for 5M record testing
# =============================================================================

set -e

# Configuration
CONTAINER_NAME="mysql_recovery_56"
MYSQL_PORT="33056"
MYSQL_ROOT_PASSWORD="root123"
MYSQL_DATABASE="testdb"
DATA_VOLUME="/var/lib/mysql"
LOG_VOLUME="/var/log/mysql"
CONF_VOLUME="/etc/mysql/conf.d"
CHAOS_TEST_VOLUME="/chaos-test"
GOLDEN_HASHES_VOLUME="/golden-hashes"
TEST_SCRIPTS_VOLUME="/test_scripts"

# Paths (host)
HOST_DATA="./chaos-test-volumes/mysql56_data"
HOST_LOGS="./chaos-test-volumes/mysql56_logs"
HOST_GOLDEN="./golden-hashes"
HOST_SCRIPTS="./scripts"
HOST_CONF="./docker/mysql.d/5.6"

# Create host directories
mkdir -p "$HOST_DATA"
mkdir -p "$HOST_LOGS"
mkdir -p "$HOST_GOLDEN"
mkdir -p "$HOST_SCRIPTS"

echo "=================================================="
echo "MySQL 5.6 Recovery Test Environment Setup (Podman)"
echo "=================================================="
echo ""
echo "Container: $CONTAINER_NAME"
echo "Host Port: $MYSQL_PORT"
echo "Data Path: $HOST_DATA"
echo "Log Path: $HOST_LOGS"
echo ""
echo "Starting MySQL 5.6 container with Podman..."
echo ""

# Stop and remove existing container if exists
if podman ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "Stopping existing container..."
    podman stop "$CONTAINER_NAME" 2>/dev/null || true
    podman rm -f "$CONTAINER_NAME" 2>/dev/null || true
fi

# Create MySQL 5.6 specific configuration
mkdir -p "$HOST_CONF"
cat > "${HOST_CONF}/56-custom.cnf" << 'EOF'
[mysqld]
# Basic settings
user                    = mysql
pid-file                = /var/run/mysqld/mysqld.pid
socket                  = /var/run/mysqld/mysqld.sock
port                    = 3306
basedir                 = /usr
datadir                 = /var/lib/mysql
tmpdir                  = /tmp
lc-messages-dir         = /usr/share/mysql

# Character set
character-set-server    = utf8mb4
collation-server        = utf8mb4_unicode_ci

# InnoDB settings (MySQL 5.6 defaults)
innodb_file_per_table   = 1
innodb_file_format      = Barracuda
innodb_large_prefix     = ON
innodb_buffer_pool_size = 1G
innodb_log_file_size    = 512M
innodb_log_buffer_size  = 64M
innodb_flush_log_at_trx_commit = 0
sync_binlog            = 0
innodb_flush_method    = O_DIRECT

# Query cache
query_cache_type        = 1
query_cache_size        = 64M
query_cache_limit       = 2M

# Temporary tables
tmp_table_size          = 512M
max_heap_table_size     = 512M

# Connections
max_connections         = 500
max_allowed_packet      = 512M

# Logging
slow_query_log          = 1
slow_query_log_file     = /var/log/mysql/slow.log
long_query_time         = 1
log_error               = /var/log/mysql/error.log

# Skip external locking
skip-external-locking

# Performance settings
bulk_insert_buffer_size = 64M
sort_buffer_size        = 4M
read_buffer_size        = 2M
read_rnd_buffer_size    = 4M

# SQL Mode (relaxed for recovery testing)
sql_mode                = ""
EOF

# Run MySQL 5.6 container with Podman
podman run -d \
    --name "$CONTAINER_NAME" \
    -e MYSQL_ROOT_PASSWORD="$MYSQL_ROOT_PASSWORD" \
    -e MYSQL_DATABASE="$MYSQL_DATABASE" \
    -e MYSQL_CHARACTER_SET_SERVER=utf8mb4 \
    -e MYSQL_COLLATION_SERVER=utf8mb4_unicode_ci \
    -p "$MYSQL_PORT:3306" \
    -v "$HOST_DATA:$DATA_VOLUME:Z" \
    -v "$HOST_LOGS:$LOG_VOLUME:Z" \
    -v "$HOST_CONF:$CONF_VOLUME:ro,Z" \
    -v "$HOST_GOLDEN:$GOLDEN_HASHES_VOLUME:Z" \
    -v "$HOST_SCRIPTS:$TEST_SCRIPTS_VOLUME:ro,Z" \
    -v "$HOST_DATA:$CHAOS_TEST_VOLUME:Z" \
    --restart unless-stopped \
    mysql:5.6 \
    --innodb_file_per_table=1 \
    --innodb_file_format=Barracuda \
    --innodb_large_prefix=ON \
    --sql-mode="" \
    --character-set-server=utf8mb4 \
    --collation-server=utf8mb4_unicode_ci \
    --max_allowed_packet=512M \
    --innodb_buffer_pool_size=1G \
    --innodb_log_file_size=512M \
    --innodb_log_buffer_size=64M \
    --sync_binlog=0 \
    --innodb_flush_log_at_trx_commit=0 \
    --tmp_table_size=512M \
    --max_heap_table_size=512M

echo ""
echo "Waiting for MySQL 5.6 to become healthy..."
sleep 30

# Verify MySQL is running
MAX_RETRIES=30
RETRY_COUNT=0
while [ $RETRY_COUNT -lt $MAX_RETRIES ]; do
    if podman exec "$CONTAINER_NAME" mysqladmin ping -h localhost -proot123 --silent 2>/dev/null; then
        echo "✅ MySQL 5.6 is ready!"
        break
    fi
    RETRY_COUNT=$((RETRY_COUNT + 1))
    echo "Waiting... ($RETRY_COUNT/$MAX_RETRIES)"
    sleep 5
done

if [ $RETRY_COUNT -eq $MAX_RETRIES ]; then
    echo "❌ MySQL 5.6 failed to start within timeout"
    echo "Checking container logs..."
    podman logs "$CONTAINER_NAME" 2>&1 | tail -50
    exit 1
fi

echo ""
echo "=================================================="
echo "Environment Setup Complete!"
echo "=================================================="
echo ""
echo "MySQL 5.6 is accessible at: localhost:$MYSQL_PORT"
echo "Username: root"
echo "Password: $MYSQL_ROOT_PASSWORD"
echo "Database: $MYSQL_DATABASE"
echo ""
echo "Next steps:"
echo "1. Connect to MySQL: podman exec -it $CONTAINER_NAME mysql -uroot -proot123 testdb"
echo "2. Run data injection: bash scripts/chaos_injector.sh --records 5000000"
echo "3. Use recovery tool: ./build/mysql_recovery_pro --ibd=/var/lib/mysql/testdb/test_table.ibd"
echo ""
echo "To stop the container: podman stop $CONTAINER_NAME"
echo "To remove the container: podman rm -f $CONTAINER_NAME"
echo ""
