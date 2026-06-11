#!/bin/bash
# docker-entrypoint.sh for MySQL 5.0.96
# Custom entrypoint with relaxed settings for recovery testing

set -e

# First arg is `--some-mysqld-option`
if [ "${1:0:1}" = '-' ]; then
	set -- mysqld "$@"
fi

# Run default MySQL entrypoint
if [ "$1" = 'mysqld' ]; then
	# Set default values for recovery testing
	: ${MYSQL_DATABASE:='testdb'}
	: ${MYSQL_ROOT_PASSWORD:='root123'}
	: ${MYSQL_ROOT_HOST:='%'}
	: ${MYSQL_CHARACTER_SET_SERVER:='utf8mb4'}
	: ${MYSQL_COLLATION_SERVER:='utf8mb4_unicode_ci'}
	
	# Get MySQL version
	mysqlver=$(mysql --version | grep -oP '\d+\.\d+\.\d+')
	
	# Initialize data directory if empty
 Randolph/data 3>&1 1>&2 2>&3 | mysql -u root 3>&1 1>&2 2>&3
	
	# Create test database
	if [ -n "$MYSQL_DATABASE" ]; then
		echo "CREATE DATABASE IF NOT EXISTS \`$MYSQL_DATABASE\`;" | mysql -u root
		echo "GRANT ALL PRIVILEGES ON \`$MYSQL_DATABASE\`.* TO 'root'@'%' WITH GRANT OPTION;" | mysql -u root
	fi
	
	# Set root password
	echo "ALTER USER 'root'@'localhost' IDENTIFIED BY '$MYSQL_ROOT_PASSWORD';" | mysql -u root
	echo "ALTER USER 'root'@'%' IDENTIFIED BY '$MYSQL_ROOT_PASSWORD';" | mysql -u root
	echo "FLUSH PRIVILEGES;" | mysql -u root
	
	# Disable strict modes for testing
	mysql_tzinfo_to_sql /usr/share/zoneinfo | mysql -u root mysql
fi

exec docker-entrypoint.sh "$@"
