#!/bin/bash
# MySQL 5.6 startup script with correct datadir configuration

# Initialize data directory if empty
if [ ! -f /var/lib/mysql/ibdata1 ]; then
    mysql_install_db --user=mysql --datadir=/var/lib/mysql
fi

# Start MySQL with correct configuration
exec mysqld --user=mysql \
    --datadir=/var/lib/mysql \
    --max_allowed_packet=512M \
    --innodb_buffer_pool_size=1G \
    --innodb_log_file_size=512M \
    --innodb_log_buffer_size=64M \
    --innodb_flush_log_at_trx_commit=0 \
    --sync_binlog=0 \
    --innodb_file_per_table=1 \
    --innodb_file_format=Barracuda \
    --innodb_large_prefix=ON \
    --sql-mode="" \
    --character-set-server=utf8mb4 \
    --collation-server=utf8mb4_unicode_ci \
    "$@"
