#!/usr/bin/env python3
"""
MySQL Multi-Version Comprehensive Recovery Test Framework

Tests recovery for MySQL 5.6, 5.7, 8.0, 8.4, and 9.0 versions.
Supports incremental data volume testing up to 100GB.

Usage:
    python3 comprehensive_recovery_test.py --version 5.6 --size 1GB
    python3 comprehensive_recovery_test.py --version all --size 100GB
    python3 comprehensive_recovery_test.py --benchmark  # Run full benchmark suite
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import shutil
import signal
from pathlib import Path
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass, field
import tempfile
import threading
import queue


# ==================== Configuration ====================

@dataclass
class VersionConfig:
    """MySQL version configuration"""
    version: str
    container_image: str
    default_row_format: str
    supports_sdi: bool
    supports_instant_ddl: bool
    supports_encryption: bool
    supports_json: bool
    supports_invisible_columns: bool
    supports_temporary_pages: bool
    # Container settings
    container_port: int = 3306
    root_password: str = "test_password"
    # Performance settings
    innodb_buffer_pool_size: str = "1G"
    innodb_log_file_size: str = "256M"


VERSION_CONFIGS = {
    "5.6": VersionConfig(
        version="5.6",
        container_image="mysql:5.6",
        default_row_format="COMPACT",
        supports_sdi=False,
        supports_instant_ddl=False,
        supports_encryption=False,
        supports_json=False,
        supports_invisible_columns=False,
        supports_temporary_pages=False,
        container_port=3306,
    ),
    "5.7": VersionConfig(
        version="5.7",
        container_image="mysql:5.7",
        default_row_format="DYNAMIC",
        supports_sdi=False,
        supports_instant_ddl=True,
        supports_encryption=False,
        supports_json=True,
        supports_invisible_columns=False,
        supports_temporary_pages=False,
        container_port=3307,
    ),
    "8.0": VersionConfig(
        version="8.0",
        container_image="mysql:8.0",
        default_row_format="DYNAMIC",
        supports_sdi=True,
        supports_instant_ddl=True,
        supports_encryption=True,
        supports_json=True,
        supports_invisible_columns=True,
        supports_temporary_pages=False,
        container_port=3308,
    ),
    "8.4": VersionConfig(
        version="8.4",
        container_image="mysql:8.4",
        default_row_format="DYNAMIC",
        supports_sdi=True,
        supports_instant_ddl=True,
        supports_encryption=True,
        supports_json=True,
        supports_invisible_columns=True,
        supports_temporary_pages=True,
        container_port=3309,
    ),
    "9.0": VersionConfig(
        version="9.0",
        container_image="mysql:9.0",
        default_row_format="DYNAMIC",
        supports_sdi=True,
        supports_instant_ddl=True,
        supports_encryption=True,
        supports_json=True,
        supports_invisible_columns=True,
        supports_temporary_pages=True,
        container_port=3310,
    ),
}


@dataclass
class TestScale:
    """Test scale configuration"""
    name: str
    rows: int
    expected_size_gb: float
    batch_size: int = 10000
    timeout_seconds: int = 3600


# Scale configurations from 1GB to 100GB
SCALE_CONFIGS = {
    "100MB": TestScale("100MB", 500_000, 0.1, 5000, 600),
    "500MB": TestScale("500MB", 2_500_000, 0.5, 10000, 900),
    "1GB": TestScale("1GB", 5_000_000, 1.0, 10000, 1200),
    "5GB": TestScale("5GB", 25_000_000, 5.0, 20000, 2400),
    "10GB": TestScale("10GB", 50_000_000, 10.0, 25000, 3600),
    "25GB": TestScale("25GB", 125_000_000, 25.0, 50000, 5400),
    "50GB": TestScale("50GB", 250_000_000, 50.0, 50000, 7200),
    "75GB": TestScale("75GB", 375_000_000, 75.0, 100000, 9000),
    "100GB": TestScale("100GB", 500_000_000, 100.0, 100000, 10800),
}


@dataclass
class TestResult:
    """Test result"""
    version: str
    scale: str
    test_type: str
    success: bool
    recovery_rate: float
    rows_recovered: int
    rows_expected: int
    elapsed_seconds: float
    file_size_bytes: int
    scan_speed_mbps: float
    memory_usage_mb: float
    error_message: Optional[str] = None


# ==================== MySQL Container Manager ====================

class MySQLContainerManager:
    """Manages MySQL containers for testing"""

    def __init__(self, config: VersionConfig, work_dir: Path):
        self.config = config
        self.work_dir = work_dir
        self.container_name = f"mysql_{config.version}_test_{os.getpid()}"
        self.container_id: Optional[str] = None
        self.mysql_data_dir = work_dir / f"mysql_data_{config.version}"
        self.is_running = False

    def start(self) -> bool:
        """Start MySQL container"""
        print(f"\n{'='*70}")
        print(f"Starting MySQL {self.config.version} Container")
        print(f"{'='*70}")

        # Create data directory
        self.mysql_data_dir.mkdir(parents=True, exist_ok=True)

        # Pull image
        print(f"[1/3] Pulling MySQL {self.config.version} image...")
        result = subprocess.run(
            ["podman", "pull", self.config.container_image],
            capture_output=True, text=True, timeout=300
        )
        if result.returncode != 0:
            print(f"  ERROR: Failed to pull image: {result.stderr}")
            return False
        print(f"  OK: Image pulled successfully")

        # Remove existing container if any
        subprocess.run(
            ["podman", "rm", "-f", self.container_name],
            capture_output=True, text=True
        )

        # Start container
        print(f"[2/3] Starting container...")
        cmd = [
            "podman", "run", "-d",
            "--name", self.container_name,
            "-e", f"MYSQL_ROOT_PASSWORD={self.config.root_password}",
            "-v", f"{self.mysql_data_dir}:/var/lib/mysql",
            "-p", f"{self.config.container_port}:3306",
            # Performance tuning for large datasets
            "-e", f"INNODB_BUFFER_POOL_SIZE={self.config.innodb_buffer_pool_size}",
            "-e", f"INNODB_LOG_FILE_SIZE={self.config.innodb_log_file_size}",
            self.config.container_image,
            # MySQL server arguments for performance
            "--innodb-buffer-pool-size=1G",
            "--innodb-log-file-size=512M",
            "--innodb-flush-log-at-trx-commit=2",
            "--innodb-doublewrite=0",
            "--skip-log-bin",
        ]

        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  ERROR: Failed to start container: {result.stderr}")
            return False

        self.container_id = result.stdout.strip()
        print(f"  OK: Container started (ID: {self.container_id[:12]})")

        # Wait for MySQL to be ready
        print(f"[3/3] Waiting for MySQL to be ready...")
        for i in range(60):
            result = subprocess.run(
                [
                    "podman", "exec", self.container_name,
                    "mysql", "-uroot", f"-p{self.config.root_password}",
                    "-e", "SELECT 1"
                ],
                capture_output=True, text=True
            )
            if result.returncode == 0:
                self.is_running = True
                print(f"  OK: MySQL is ready!")
                return True
            time.sleep(2)

        print(f"  ERROR: MySQL failed to start within timeout")
        return False

    def stop(self) -> bool:
        """Stop MySQL container"""
        if not self.container_id:
            return True

        print(f"\nStopping MySQL {self.config.version} container...")

        # Stop container
        subprocess.run(
            ["podman", "stop", "-t", "30", self.container_name],
            capture_output=True, text=True, timeout=60
        )

        # Remove container
        subprocess.run(
            ["podman", "rm", "-f", self.container_name],
            capture_output=True, text=True
        )

        self.is_running = False
        self.container_id = None
        print(f"  OK: Container stopped and removed")
        return True

    def execute_sql(self, sql: str, timeout: int = 300) -> Tuple[bool, str]:
        """Execute SQL statement"""
        result = subprocess.run(
            [
                "podman", "exec", self.container_name,
                "mysql", "-uroot", f"-p{self.config.root_password}",
                "-e", sql
            ],
            capture_output=True, text=True, timeout=timeout
        )
        return result.returncode == 0, result.stdout + result.stderr

    def execute_sql_file(self, sql_file: Path, timeout: int = 3600) -> Tuple[bool, str]:
        """Execute SQL file"""
        result = subprocess.run(
            [
                "podman", "exec", "-i", self.container_name,
                "mysql", "-uroot", f"-p{self.config.root_password}"
            ],
            stdin=open(sql_file, 'r'),
            capture_output=True, text=True, timeout=timeout
        )
        return result.returncode == 0, result.stdout + result.stderr

    def get_ibd_path(self, database: str, table: str) -> Path:
        """Get path to .ibd file"""
        return self.mysql_data_dir / database / f"{table}.ibd"

    def get_frm_path(self, database: str, table: str) -> Path:
        """Get path to .frm file"""
        return self.mysql_data_dir / database / f"{table}.frm"

    def get_database_dir(self, database: str) -> Path:
        """Get database directory"""
        return self.mysql_data_dir / database


# ==================== Data Generator ====================

class DataGenerator:
    """Generate test data for MySQL versions"""

    def __init__(self, config: VersionConfig, container: MySQLContainerManager, work_dir: Path):
        self.config = config
        self.container = container
        self.work_dir = work_dir

    def create_database(self, database: str) -> bool:
        """Create database"""
        sql = f"CREATE DATABASE IF NOT EXISTS {database};"
        success, output = self.container.execute_sql(sql)
        return success

    def create_table(self, database: str, table: str) -> bool:
        """Create test table with version-specific features"""
        print(f"\n  Creating table {database}.{table}...")

        # Base table definition
        if self.config.version == "5.6":
            table_sql = f"""
            USE {database};
            CREATE TABLE {table} (
                id BIGINT NOT NULL AUTO_INCREMENT,
                name VARCHAR(100) NOT NULL,
                email VARCHAR(255) NOT NULL,
                created_at DATETIME NOT NULL,
                data TEXT,
                value1 INT,
                value2 DOUBLE,
                value3 DECIMAL(10,2),
                PRIMARY KEY (id),
                KEY idx_email (email),
                KEY idx_name (name)
            ) ENGINE=InnoDB ROW_FORMAT=COMPACT;
            """
        elif self.config.version == "5.7":
            table_sql = f"""
            USE {database};
            CREATE TABLE {table} (
                id BIGINT NOT NULL AUTO_INCREMENT,
                name VARCHAR(100) NOT NULL,
                email VARCHAR(255) NOT NULL,
                created_at DATETIME NOT NULL,
                data TEXT,
                metadata JSON,
                value1 INT,
                value2 DOUBLE,
                value3 DECIMAL(10,2),
                PRIMARY KEY (id),
                KEY idx_email (email),
                KEY idx_name (name)
            ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
            """
        else:  # 8.0, 8.4, 9.0
            table_sql = f"""
            USE {database};
            CREATE TABLE {table} (
                id BIGINT NOT NULL AUTO_INCREMENT,
                name VARCHAR(100) NOT NULL,
                email VARCHAR(255) NOT NULL,
                created_at DATETIME NOT NULL,
                data TEXT,
                metadata JSON,
                value1 INT,
                value2 DOUBLE,
                value3 DECIMAL(10,2),
                version INT INVISIBLE DEFAULT 1,
                PRIMARY KEY (id),
                KEY idx_email (email),
                KEY idx_name (name)
            ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
            """

        success, output = self.container.execute_sql(table_sql, timeout=60)
        if not success:
            print(f"    ERROR: {output}")
        else:
            print(f"    OK: Table created")
        return success

    def insert_data(self, database: str, table: str,
                    num_rows: int, batch_size: int = 10000) -> Tuple[bool, int]:
        """Insert test data in batches using SQL files"""
        print(f"\n  Inserting {num_rows:,} rows (batch size: {batch_size:,})...")

        total_inserted = 0
        start_time = time.time()

        for batch_start in range(0, num_rows, batch_size):
            batch_end = min(batch_start + batch_size, num_rows)

            # Generate batch data - use smaller chunks to avoid argument limit
            chunk_size = min(500, batch_size)  # Split into smaller chunks

            for chunk_start in range(batch_start, batch_end, chunk_size):
                chunk_end = min(chunk_start + chunk_size, batch_end)

                values = []
                for i in range(chunk_start, chunk_end):
                    name = f"user_{i:010d}"
                    email = f"user{i}@example.com"
                    created_at = "2024-01-01 00:00:00"
                    data = f"Data {i}"
                    value1 = i % 1000
                    value2 = i * 1.5
                    value3 = f"{i % 100}.{i % 100:02d}"

                    if self.config.supports_json:
                        metadata = json.dumps({"i": i})
                        values.append(
                            f"('{name}', '{email}', '{created_at}', '{data}', "
                            f"'{metadata}', {value1}, {value2}, {value3})"
                        )
                    else:
                        values.append(
                            f"('{name}', '{email}', '{created_at}', '{data}', "
                            f"{value1}, {value2}, {value3})"
                        )

                # Write to temp SQL file
                sql_file = self.work_dir / f"insert_{batch_start}_{chunk_start}.sql"
                with open(sql_file, 'w') as f:
                    if self.config.supports_json:
                        f.write(f"USE {database};\n")
                        f.write(f"INSERT INTO {table} ")
                        f.write(f"(name, email, created_at, data, metadata, value1, value2, value3) VALUES\n")
                        f.write(',\n'.join(values) + ";\n")
                    else:
                        f.write(f"USE {database};\n")
                        f.write(f"INSERT INTO {table} ")
                        f.write(f"(name, email, created_at, data, value1, value2, value3) VALUES\n")
                        f.write(',\n'.join(values) + ";\n")

                # Execute SQL file
                timeout = max(60, chunk_size // 50)
                success, output = self.container.execute_sql_file(sql_file, timeout=timeout)

                if not success:
                    print(f"    ERROR at chunk {chunk_start}: {output[:200]}")
                    return False, total_inserted

                total_inserted += chunk_end - chunk_start

            # Progress update
            if batch_start % (batch_size * 2) == 0 or batch_end == num_rows:
                elapsed = time.time() - start_time
                rate = total_inserted / elapsed if elapsed > 0 else 0
                eta = (num_rows - total_inserted) / rate if rate > 0 else 0
                print(f"    Progress: {total_inserted:,}/{num_rows:,} rows "
                      f"({total_inserted/num_rows*100:.1f}%) "
                      f"- {rate:.0f} rows/s - ETA: {eta:.0f}s")

        elapsed = time.time() - start_time
        print(f"    OK: Inserted {total_inserted:,} rows in {elapsed:.1f}s "
              f"({total_inserted/elapsed:.0f} rows/s)")
        return True, total_inserted

    def get_row_count(self, database: str, table: str) -> int:
        """Get row count"""
        sql = f"USE {database}; SELECT COUNT(*) FROM {table};"
        success, output = self.container.execute_sql(sql)
        if success:
            lines = output.strip().split('\n')
            if len(lines) >= 2:
                try:
                    return int(lines[1])
                except ValueError:
                    pass
        return 0

    def compute_checksum(self, database: str, table: str) -> str:
        """Compute data checksum for validation"""
        sql = f"""
        USE {database};
        SELECT
            MD5(GROUP_CONCAT(
                CONCAT(id, name, email, created_at)
                ORDER BY id
            )) as checksum
        FROM {table};
        """
        success, output = self.container.execute_sql(sql, timeout=600)
        if success:
            lines = output.strip().split('\n')
            if len(lines) >= 2:
                return lines[1].strip()
        return ""


# ==================== Recovery Tool Runner ====================

class RecoveryToolRunner:
    """Run mysql_recovery_pro tool"""

    def __init__(self, tool_path: str):
        self.tool_path = tool_path

    def run_truncate_recovery(self, input_path: str, output_path: str,
                              version: str, verbose: bool = True) -> Tuple[bool, Dict]:
        """Run TRUNCATE recovery"""
        cmd = [
            self.tool_path,
            "--ibd", input_path,
            "--truncate-recovery",
            "--mysql-version", version,
            "--output", output_path,
            "--format", "csv",
        ]
        if verbose:
            cmd.append("--verbose")

        return self._run_recovery(cmd, "TRUNCATE")

    def run_drop_recovery(self, input_path: str, output_path: str,
                          version: str, verbose: bool = True) -> Tuple[bool, Dict]:
        """Run DROP TABLE recovery"""
        cmd = [
            self.tool_path,
            "--disk", input_path,
            "--drop-recovery",
            "--mysql-version", version,
            "--output", output_path,
            "--format", "csv",
        ]
        if verbose:
            cmd.append("--verbose")

        return self._run_recovery(cmd, "DROP")

    def run_rmrf_recovery(self, input_path: str, output_path: str,
                          version: str, verbose: bool = True) -> Tuple[bool, Dict]:
        """Run rm -rf recovery"""
        cmd = [
            self.tool_path,
            "--disk", input_path,
            "--rmrf-recovery",
            "--mysql-version", version,
            "--output", output_path,
            "--format", "csv",
        ]
        if verbose:
            cmd.append("--verbose")

        return self._run_recovery(cmd, "RMRF")

    def _run_recovery(self, cmd: List[str], mode: str) -> Tuple[bool, Dict]:
        """Execute recovery command"""
        print(f"\n  Running {mode} recovery...")
        print(f"    Command: {' '.join(cmd[:6])}...")

        start_time = time.time()
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=7200  # 2 hour timeout
            )
            elapsed = time.time() - start_time

            if result.returncode != 0:
                print(f"    ERROR: Recovery failed")
                print(f"    stderr: {result.stderr[:500]}")
                return False, {
                    "error": result.stderr,
                    "elapsed": elapsed,
                    "mode": mode
                }

            # Parse statistics from output
            stats = {
                "elapsed": elapsed,
                "mode": mode,
                "stdout": result.stdout,
                "stderr": result.stderr
            }

            for line in result.stdout.split('\n'):
                if "Recovered" in line and "records" in line:
                    try:
                        stats["records_recovered"] = int(line.split()[1])
                    except:
                        pass
                elif "pages" in line.lower() and "processed" in line.lower():
                    print(f"    {line.strip()}")
                elif "version" in line.lower():
                    print(f"    {line.strip()}")

            print(f"    OK: Recovery completed in {elapsed:.2f}s")
            return True, stats

        except subprocess.TimeoutExpired:
            elapsed = time.time() - start_time
            print(f"    ERROR: Recovery timed out after {elapsed:.0f}s")
            return False, {"error": "Timeout", "elapsed": elapsed, "mode": mode}
        except Exception as e:
            elapsed = time.time() - start_time
            print(f"    ERROR: {str(e)}")
            return False, {"error": str(e), "elapsed": elapsed, "mode": mode}

    def count_recovered_records(self, output_dir: str) -> int:
        """Count records in recovered CSV files"""
        output_path = Path(output_dir)
        total_records = 0

        for csv_file in output_path.glob("*.csv"):
            with open(csv_file, 'r', errors='ignore') as f:
                content = f.read()
                # Count lines containing user_ pattern
                total_records += content.count('user_')

        return total_records


# ==================== Test Scenarios ====================

class RecoveryTestSuite:
    """Comprehensive recovery test suite"""

    def __init__(self, version: str, scale: str, work_dir: Path, tool_path: str):
        self.version = version
        self.scale = scale
        self.work_dir = work_dir
        self.tool_path = tool_path

        self.config = VERSION_CONFIGS[version]
        self.scale_config = SCALE_CONFIGS[scale]

        self.container = MySQLContainerManager(self.config, work_dir)
        self.generator = DataGenerator(self.config, self.container, work_dir)
        self.runner = RecoveryToolRunner(tool_path)

        self.database = "recovery_test"
        self.table = "test_table"

        self.results: List[TestResult] = []

    def setup(self) -> bool:
        """Setup test environment"""
        print(f"\n{'='*70}")
        print(f"Setting up test: MySQL {self.version} @ {self.scale}")
        print(f"{'='*70}")
        print(f"  Expected rows: {self.scale_config.rows:,}")
        print(f"  Expected size: ~{self.scale_config.expected_size_gb:.1f} GB")

        # Start container
        if not self.container.start():
            return False

        # Create database and table
        if not self.generator.create_database(self.database):
            return False

        if not self.generator.create_table(self.database, self.table):
            return False

        # Insert data
        success, rows = self.generator.insert_data(
            self.database,
            self.table,
            self.scale_config.rows,
            self.scale_config.batch_size
        )
        if not success:
            return False

        # Verify row count
        actual_rows = self.generator.get_row_count(self.database, self.table)
        print(f"\n  Verification: {actual_rows:,} rows in table")

        # Get file size
        ibd_path = self.container.get_ibd_path(self.database, self.table)
        if ibd_path.exists():
            file_size = ibd_path.stat().st_size
            print(f"  IBD file size: {file_size / (1024**3):.2f} GB ({file_size:,} bytes)")
        else:
            print(f"  WARNING: IBD file not found at {ibd_path}")

        return True

    def run_truncate_test(self) -> TestResult:
        """Run TRUNCATE TABLE recovery test"""
        print(f"\n{'='*70}")
        print(f"Test: TRUNCATE TABLE Recovery - MySQL {self.version} @ {self.scale}")
        print(f"{'='*70}")

        # Get baseline
        ibd_path = self.container.get_ibd_path(self.database, self.table)
        row_count = self.generator.get_row_count(self.database, self.table)

        # Backup IBD file
        backup_path = self.work_dir / f"truncate_backup_{self.version}_{self.scale}.ibd"
        print(f"\n[1/4] Backing up IBD file...")
        shutil.copy(ibd_path, backup_path)
        print(f"  OK: Backup created ({backup_path.stat().st_size / (1024**3):.2f} GB)")

        # TRUNCATE table
        print(f"\n[2/4] Executing TRUNCATE TABLE...")
        sql = f"USE {self.database}; TRUNCATE TABLE {self.table};"
        self.container.execute_sql(sql)

        count_after = self.generator.get_row_count(self.database, self.table)
        print(f"  OK: Rows after TRUNCATE: {count_after}")

        # Stop container to release file handles
        self.container.stop()

        # Run recovery
        print(f"\n[3/4] Running recovery...")
        output_dir = self.work_dir / f"truncate_recovery_{self.version}_{self.scale}"
        output_dir.mkdir(parents=True, exist_ok=True)

        start_time = time.time()
        success, stats = self.runner.run_truncate_recovery(
            str(backup_path),
            str(output_dir),
            self.version
        )
        elapsed = time.time() - start_time

        # Count recovered records
        print(f"\n[4/4] Validating recovery...")
        recovered_rows = self.runner.count_recovered_records(str(output_dir))
        recovery_rate = recovered_rows / row_count if row_count > 0 else 0

        file_size = backup_path.stat().st_size
        scan_speed = (file_size / (1024**2)) / elapsed if elapsed > 0 else 0

        print(f"  Original rows:  {row_count:,}")
        print(f"  Recovered rows: {recovered_rows:,}")
        print(f"  Recovery rate:  {recovery_rate*100:.2f}%")
        print(f"  Scan speed:     {scan_speed:.1f} MB/s")

        result = TestResult(
            version=self.version,
            scale=self.scale,
            test_type="TRUNCATE",
            success=(recovery_rate >= 0.999),
            recovery_rate=recovery_rate,
            rows_recovered=recovered_rows,
            rows_expected=row_count,
            elapsed_seconds=elapsed,
            file_size_bytes=file_size,
            scan_speed_mbps=scan_speed,
            memory_usage_mb=0
        )

        self.results.append(result)
        return result

    def run_drop_test(self) -> TestResult:
        """Run DROP TABLE recovery test"""
        print(f"\n{'='*70}")
        print(f"Test: DROP TABLE Recovery - MySQL {self.version} @ {self.scale}")
        print(f"{'='*70}")

        # Restart container if needed
        if not self.container.is_running:
            if not self.container.start():
                return TestResult(
                    version=self.version, scale=self.scale, test_type="DROP",
                    success=False, recovery_rate=0, rows_recovered=0,
                    rows_expected=0, elapsed_seconds=0, file_size_bytes=0,
                    scan_speed_mbps=0, memory_usage_mb=0,
                    error_message="Failed to start container"
                )

        # Get baseline
        db_dir = self.container.get_database_dir(self.database)
        row_count = self.generator.get_row_count(self.database, self.table)

        # Backup database directory
        backup_dir = self.work_dir / f"drop_backup_{self.version}_{self.scale}"
        print(f"\n[1/4] Backing up database directory...")
        shutil.copytree(db_dir, backup_dir)
        total_size = sum(f.stat().st_size for f in backup_dir.rglob('*') if f.is_file())
        print(f"  OK: Backup created ({total_size / (1024**3):.2f} GB)")

        # DROP table
        print(f"\n[2/4] Executing DROP TABLE...")
        sql = f"USE {self.database}; DROP TABLE {self.table};"
        self.container.execute_sql(sql)

        # Verify table is gone
        sql = f"USE {self.database}; SHOW TABLES;"
        success, output = self.container.execute_sql(sql)
        print(f"  OK: Table dropped")

        # Stop container
        self.container.stop()

        # Run recovery on backup directory
        print(f"\n[3/4] Running recovery...")
        output_dir = self.work_dir / f"drop_recovery_{self.version}_{self.scale}"
        output_dir.mkdir(parents=True, exist_ok=True)

        start_time = time.time()
        success, stats = self.runner.run_drop_recovery(
            str(backup_dir),
            str(output_dir),
            self.version
        )
        elapsed = time.time() - start_time

        # Count recovered records
        print(f"\n[4/4] Validating recovery...")
        recovered_rows = self.runner.count_recovered_records(str(output_dir))
        recovery_rate = recovered_rows / row_count if row_count > 0 else 0

        scan_speed = (total_size / (1024**2)) / elapsed if elapsed > 0 else 0

        print(f"  Original rows:  {row_count:,}")
        print(f"  Recovered rows: {recovered_rows:,}")
        print(f"  Recovery rate:  {recovery_rate*100:.2f}%")

        result = TestResult(
            version=self.version,
            scale=self.scale,
            test_type="DROP",
            success=(recovery_rate >= 0.999),
            recovery_rate=recovery_rate,
            rows_recovered=recovered_rows,
            rows_expected=row_count,
            elapsed_seconds=elapsed,
            file_size_bytes=total_size,
            scan_speed_mbps=scan_speed,
            memory_usage_mb=0
        )

        self.results.append(result)
        return result

    def run_rmrf_test(self) -> TestResult:
        """Run rm -rf recovery test"""
        print(f"\n{'='*70}")
        print(f"Test: rm -rf Recovery - MySQL {self.version} @ {self.scale}")
        print(f"{'='*70}")

        # This simulates complete data loss
        # For now, we use the same approach as DROP test
        return self.run_drop_test()

    def cleanup(self):
        """Cleanup test resources"""
        print(f"\nCleaning up test resources...")
        self.container.stop()
        print(f"  OK: Cleanup complete")


# ==================== Main Test Runner ====================

def run_single_test(version: str, scale: str, tool_path: str,
                    scenarios: List[str]) -> List[TestResult]:
    """Run test for single version and scale"""
    work_dir = Path(f"/tmp/recovery_test_{version}_{scale}_{os.getpid()}")
    work_dir.mkdir(parents=True, exist_ok=True)

    suite = RecoveryTestSuite(version, scale, work_dir, tool_path)
    results = []

    try:
        # Setup
        if not suite.setup():
            print(f"ERROR: Setup failed for {version} @ {scale}")
            return results

        # Run scenarios
        if "truncate" in scenarios:
            results.append(suite.run_truncate_test())

        if "drop" in scenarios:
            results.append(suite.run_drop_test())

        if "rmrf" in scenarios:
            results.append(suite.run_rmrf_test())

    except Exception as e:
        print(f"ERROR: Test failed with exception: {e}")
        import traceback
        traceback.print_exc()

    finally:
        suite.cleanup()

    return results


def run_benchmark(tool_path: str, max_scale: str = "100GB",
                  versions: List[str] = None,
                  scenarios: List[str] = None) -> List[TestResult]:
    """Run full benchmark suite"""
    if versions is None:
        versions = list(VERSION_CONFIGS.keys())

    if scenarios is None:
        scenarios = ["truncate", "drop", "rmrf"]

    # Determine scales to test
    scale_order = ["100MB", "500MB", "1GB", "5GB", "10GB", "25GB", "50GB", "75GB", "100GB"]
    max_idx = scale_order.index(max_scale) if max_scale in scale_order else len(scale_order) - 1
    scales = scale_order[:max_idx + 1]

    all_results = []

    print(f"\n{'='*70}")
    print(f"MySQL Recovery Pro - Comprehensive Benchmark Suite")
    print(f"{'='*70}")
    print(f"Versions: {', '.join(versions)}")
    print(f"Scales: {', '.join(scales)}")
    print(f"Scenarios: {', '.join(scenarios)}")

    for version in versions:
        for scale in scales:
            print(f"\n{'='*70}")
            print(f"Testing: MySQL {version} @ {scale}")
            print(f"{'='*70}")

            results = run_single_test(version, scale, tool_path, scenarios)
            all_results.extend(results)

            # Print intermediate summary
            print_summary(all_results)

    return all_results


def print_summary(results: List[TestResult]):
    """Print test summary"""
    print(f"\n{'='*70}")
    print(f"TEST SUMMARY")
    print(f"{'='*70}")

    # Group by version
    by_version: Dict[str, List[TestResult]] = {}
    for r in results:
        if r.version not in by_version:
            by_version[r.version] = []
        by_version[r.version].append(r)

    for version in VERSION_CONFIGS.keys():
        if version not in by_version:
            continue

        print(f"\nMySQL {version}:")
        version_results = by_version[version]

        for r in version_results:
            status = "PASS" if r.success else "FAIL"
            print(f"  {r.scale:8s} {r.test_type:8s}: "
                  f"{r.recovery_rate*100:6.2f}% "
                  f"({r.rows_recovered:,}/{r.rows_expected:,}) "
                  f"[{status}] "
                  f"- {r.elapsed_seconds:.1f}s "
                  f"- {r.scan_speed_mbps:.1f} MB/s")

    # Overall statistics
    total_tests = len(results)
    passed_tests = sum(1 for r in results if r.success)
    avg_recovery = sum(r.recovery_rate for r in results) / total_tests if total_tests > 0 else 0

    print(f"\n{'-'*70}")
    print(f"Total tests: {total_tests}")
    if total_tests > 0:
        print(f"Passed: {passed_tests} ({passed_tests/total_tests*100:.1f}%)")
        print(f"Average recovery rate: {avg_recovery*100:.2f}%")
    print(f"{'='*70}")


def save_results(results: List[TestResult], output_file: str):
    """Save results to JSON file"""
    data = []
    for r in results:
        data.append({
            "version": r.version,
            "scale": r.scale,
            "test_type": r.test_type,
            "success": r.success,
            "recovery_rate": r.recovery_rate,
            "rows_recovered": r.rows_recovered,
            "rows_expected": r.rows_expected,
            "elapsed_seconds": r.elapsed_seconds,
            "file_size_bytes": r.file_size_bytes,
            "scan_speed_mbps": r.scan_speed_mbps,
            "memory_usage_mb": r.memory_usage_mb,
            "error_message": r.error_message
        })

    with open(output_file, 'w') as f:
        json.dump(data, f, indent=2)

    print(f"\nResults saved to: {output_file}")


def main():
    parser = argparse.ArgumentParser(
        description="MySQL Multi-Version Comprehensive Recovery Test"
    )
    parser.add_argument(
        "--version",
        choices=["all", "5.6", "5.7", "8.0", "8.4", "9.0"],
        default="all",
        help="MySQL version to test"
    )
    parser.add_argument(
        "--size",
        choices=list(SCALE_CONFIGS.keys()),
        default="1GB",
        help="Data size to test"
    )
    parser.add_argument(
        "--scenario",
        choices=["all", "truncate", "drop", "rmrf"],
        default="all",
        help="Recovery scenario to test"
    )
    parser.add_argument(
        "--tool",
        default="./build/mysql_recovery_pro",
        help="Path to recovery tool"
    )
    parser.add_argument(
        "--benchmark",
        action="store_true",
        help="Run full benchmark suite"
    )
    parser.add_argument(
        "--max-size",
        choices=list(SCALE_CONFIGS.keys()),
        default="100GB",
        help="Maximum data size for benchmark"
    )
    parser.add_argument(
        "--output",
        default="recovery_test_results.json",
        help="Output file for results"
    )

    args = parser.parse_args()

    # Determine versions
    if args.version == "all":
        versions = list(VERSION_CONFIGS.keys())
    else:
        versions = [args.version]

    # Determine scenarios
    if args.scenario == "all":
        scenarios = ["truncate", "drop", "rmrf"]
    else:
        scenarios = [args.scenario]

    # Check tool exists
    if not os.path.exists(args.tool):
        print(f"ERROR: Recovery tool not found: {args.tool}")
        return 1

    # Run tests
    if args.benchmark:
        results = run_benchmark(args.tool, args.max_size, versions, scenarios)
    else:
        results = []
        for version in versions:
            r = run_single_test(version, args.size, args.tool, scenarios)
            results.extend(r)

    # Print final summary
    print_summary(results)

    # Save results
    save_results(results, args.output)

    # Return code
    all_passed = all(r.success for r in results) if results else False
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
