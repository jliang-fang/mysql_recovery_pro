#!/usr/bin/env python3
"""
MySQL 5.6 Container Setup Script

Sets up a MySQL 5.6 container using Podman for recovery testing.
"""

import argparse
import subprocess
import sys
import time
import json
from pathlib import Path
from typing import Optional


class MySQL56Setup:
    """MySQL 5.6 container setup"""

    def __init__(self, container_name: str = "mysql56_test",
                 mysql_dir: str = "/tmp/mysql56_test_data",
                 root_password: str = "test_password"):
        self.container_name = container_name
        self.mysql_dir = Path(mysql_dir)
        self.root_password = root_password
        self.container_id: Optional[str] = None

    def setup(self) -> bool:
        """Setup MySQL 5.6 container"""
        print(f"Setting up MySQL 5.6 container: {self.container_name}")

        # Create MySQL data directory
        self.mysql_dir.mkdir(parents=True, exist_ok=True)

        # Check if Podman is available
        result = subprocess.run(["podman", "--version"], capture_output=True, text=True)
        if result.returncode != 0:
            print("ERROR: Podman is not installed")
            return False

        print(f"Podman version: {result.stdout.strip()}")

        # Pull MySQL 5.6 image
        print("Pulling MySQL 5.6 image...")
        result = subprocess.run(
            ["podman", "pull", "mysql:5.6"],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            print(f"Failed to pull MySQL image: {result.stderr}")
            return False

        # Check if container already exists
        result = subprocess.run(
            ["podman", "ps", "-a", "--filter", f"name={self.container_name}",
             "--format", "{{.ID}}"],
            capture_output=True, text=True
        )
        if result.stdout.strip():
            print(f"Container {self.container_name} already exists, removing...")
            subprocess.run(
                ["podman", "rm", "-f", self.container_name],
                capture_output=True
            )

        # Start container
        print("Starting MySQL 5.6 container...")
        result = subprocess.run(
            [
                "podman", "run", "-d",
                "--name", self.container_name,
                "-e", f"MYSQL_ROOT_PASSWORD={self.root_password}",
                "-v", f"{self.mysql_dir}:/var/lib/mysql",
                "-p", "3306:3306",
                "mysql:5.6"
            ],
            capture_output=True, text=True
        )

        if result.returncode != 0:
            print(f"Failed to start container: {result.stderr}")
            return False

        self.container_id = result.stdout.strip()
        print(f"Container ID: {self.container_id}")

        # Wait for MySQL to be ready
        print("Waiting for MySQL to be ready...")
        for i in range(60):
            result = subprocess.run(
                [
                    "podman", "exec", self.container_name,
                    "mysql", "-uroot", f"-p{self.root_password}",
                    "-e", "SELECT 1"
                ],
                capture_output=True, text=True
            )
            if result.returncode == 0:
                print("MySQL is ready!")
                return True
            time.sleep(2)

        print("ERROR: MySQL failed to start within timeout")
        return False

    def create_test_database(self, db_name: str = "test_db",
                            table_name: str = "test_table",
                            num_rows: int = 100000) -> bool:
        """Create test database and table with data"""
        print(f"Creating test database: {db_name}")

        # Create database and table
        sql = f"""
        CREATE DATABASE IF NOT EXISTS {db_name};
        USE {db_name};
        CREATE TABLE {table_name} (
            id BIGINT NOT NULL AUTO_INCREMENT,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(255) NOT NULL,
            created_at DATETIME NOT NULL,
            data TEXT,
            PRIMARY KEY (id)
        ) ENGINE=InnoDB ROW_FORMAT=COMPACT;
        """
        self._execute_sql(sql)

        # Insert data in batches
        batch_size = 10000
        print(f"Inserting {num_rows} rows...")

        for batch_start in range(0, num_rows, batch_size):
            batch_end = min(batch_start + batch_size, num_rows)

            values = []
            for i in range(batch_start, batch_end):
                name = f"user_{i:07d}"
                email = f"{name}@test.com"
                values.append(f"('{name}', '{email}', NOW(), 'Sample data for user {i}')")

            sql = f"""
            USE {db_name};
            INSERT INTO {table_name} (name, email, created_at, data)
            VALUES {','.join(values)};
            """
            if not self._execute_sql(sql):
                print(f"Failed to insert batch {batch_start}-{batch_end}")
                return False

            if batch_start % 100000 == 0:
                print(f"  Inserted {batch_end}/{num_rows} rows...")

        print(f"Inserted {num_rows} rows")
        return True

    def get_table_info(self, db_name: str, table_name: str) -> dict:
        """Get table information"""
        # Get row count
        result = subprocess.run(
            [
                "podman", "exec", self.container_name,
                "mysql", "-uroot", f"-p{self.root_password}",
                "-e", f"USE {db_name}; SELECT COUNT(*) FROM {table_name};"
            ],
            capture_output=True, text=True
        )

        row_count = 0
        if result.returncode == 0:
            lines = result.stdout.strip().split('\n')
            if len(lines) >= 2:
                try:
                    row_count = int(lines[1])
                except ValueError:
                    pass

        # Get .ibd file path
        ibd_path = self.mysql_dir / db_name / f"{table_name}.ibd"
        ibd_size = ibd_path.stat().st_size if ibd_path.exists() else 0

        return {
            "row_count": row_count,
            "ibd_path": str(ibd_path),
            "ibd_size": ibd_size,
            "database": db_name,
            "table": table_name
        }

    def execute_truncate(self, db_name: str, table_name: str) -> bool:
        """Execute TRUNCATE TABLE"""
        print(f"Executing TRUNCATE TABLE {db_name}.{table_name}")
        return self._execute_sql(f"USE {db_name}; TRUNCATE TABLE {table_name};")

    def stop(self) -> bool:
        """Stop container"""
        if not self.container_id:
            return True

        print(f"Stopping container: {self.container_name}")
        subprocess.run(
            ["podman", "stop", self.container_name],
            capture_output=True
        )
        subprocess.run(
            ["podman", "rm", self.container_name],
            capture_output=True
        )
        return True

    def _execute_sql(self, sql: str) -> bool:
        """Execute SQL statement"""
        result = subprocess.run(
            [
                "podman", "exec", self.container_name,
                "mysql", "-uroot", f"-p{self.root_password}",
                "-e", sql
            ],
            capture_output=True, text=True
        )
        return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description="MySQL 5.6 Container Setup")
    parser.add_argument("command", choices=["setup", "create", "truncate", "stop", "info"],
                        help="Command to execute")
    parser.add_argument("--container", default="mysql56_test",
                        help="Container name")
    parser.add_argument("--mysql-dir", default="/tmp/mysql56_test_data",
                        help="MySQL data directory")
    parser.add_argument("--password", default="test_password",
                        help="Root password")
    parser.add_argument("--database", default="test_db",
                        help="Database name")
    parser.add_argument("--table", default="test_table",
                        help="Table name")
    parser.add_argument("--rows", type=int, default=100000,
                        help="Number of rows to insert")

    args = parser.parse_args()

    setup = MySQL56Setup(
        container_name=args.container,
        mysql_dir=args.mysql_dir,
        root_password=args.password
    )

    if args.command == "setup":
        if not setup.setup():
            sys.exit(1)
        print("MySQL 5.6 container is ready!")

    elif args.command == "create":
        setup.setup()
        if not setup.create_test_database(args.database, args.table, args.rows):
            sys.exit(1)
        print("Test database created!")

    elif args.command == "truncate":
        if not setup.execute_truncate(args.database, args.table):
            sys.exit(1)
        print("Table truncated!")

    elif args.command == "stop":
        setup.stop()
        print("Container stopped!")

    elif args.command == "info":
        info = setup.get_table_info(args.database, args.table)
        print(json.dumps(info, indent=2))


if __name__ == "__main__":
    main()