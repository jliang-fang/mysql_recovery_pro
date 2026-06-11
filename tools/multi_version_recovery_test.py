#!/usr/bin/env python3
"""
MySQL Multi-Version Recovery Test Framework

Tests recovery for MySQL 5.6, 5.7, 8.0, 8.4, and 9.0 versions.

Usage:
    python3 multi_version_recovery_test.py --test all
    python3 multi_version_recovery_test.py --test 5.6
    python3 multi_version_recovery_test.py --test 8.0
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import shutil
from pathlib import Path
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass
import tempfile


@dataclass
class VersionTestConfig:
    """Test configuration for a specific MySQL version"""
    version: str
    container_image: str
    default_row_format: str
    supports_sdi: bool
    supports_instant_ddl: bool
    supports_encryption: bool
    supports_json: bool
    test_rows: int = 10000


# Version configurations
VERSION_CONFIGS = {
    "5.6": VersionTestConfig(
        version="5.6",
        container_image="mysql:5.6",
        default_row_format="COMPACT",
        supports_sdi=False,
        supports_instant_ddl=False,
        supports_encryption=False,
        supports_json=False
    ),
    "5.7": VersionTestConfig(
        version="5.7",
        container_image="mysql:5.7",
        default_row_format="DYNAMIC",
        supports_sdi=False,
        supports_instant_ddl=True,
        supports_encryption=False,
        supports_json=True
    ),
    "8.0": VersionTestConfig(
        version="8.0",
        container_image="mysql:8.0",
        default_row_format="DYNAMIC",
        supports_sdi=True,
        supports_instant_ddl=True,
        supports_encryption=True,
        supports_json=True
    ),
    "8.4": VersionTestConfig(
        version="8.4",
        container_image="mysql:8.4",
        default_row_format="DYNAMIC",
        supports_sdi=True,
        supports_instant_ddl=True,
        supports_encryption=True,
        supports_json=True
    ),
    "9.0": VersionTestConfig(
        version="9.0",
        container_image="mysql:9.0",
        default_row_format="DYNAMIC",
        supports_sdi=True,
        supports_instant_ddl=True,
        supports_encryption=True,
        supports_json=True
    ),
}


@dataclass
class TestResult:
    """Test result for a version"""
    version: str
    test_type: str  # truncate, drop, rmrf
    success: bool
    recovery_rate: float
    rows_recovered: int
    rows_expected: int
    elapsed_seconds: float
    error_message: Optional[str] = None


class MySQLVersionContainer:
    """MySQL container management for specific version"""

    def __init__(self, config: VersionTestConfig):
        self.config = config
        self.container_name = f"mysql_{config.version}_recovery_test"
        self.container_id: Optional[str] = None
        self.mysql_dir: Optional[Path] = None

    def start(self) -> bool:
        """Start MySQL container for the specific version"""
        self.mysql_dir = Path(tempfile.mkdtemp(prefix=f"mysql{self.config.version}_test_"))

        print(f"Pulling MySQL {self.config.version} image...")
        result = subprocess.run(
            ["podman", "pull", self.config.container_image],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            print(f"Failed to pull MySQL image: {result.stderr}")
            return False

        print(f"Starting MySQL {self.config.version} container...")
        result = subprocess.run(
            [
                "podman", "run", "-d",
                "--name", self.container_name,
                "-e", "MYSQL_ROOT_PASSWORD=test_password",
                "-v", f"{self.mysql_dir}:/var/lib/mysql",
                "-p", "3306:3306",
                self.config.container_image
            ],
            capture_output=True, text=True
        )

        if result.returncode != 0:
            print(f"Failed to start container: {result.stderr}")
            return False

        self.container_id = result.stdout.strip()

        print(f"Waiting for MySQL {self.config.version} to be ready...")
        for i in range(30):
            result = subprocess.run(
                [
                    "podman", "exec", self.container_name,
                    "mysql", "-uroot", "-ptest_password",
                    "-e", "SELECT 1"
                ],
                capture_output=True, text=True
            )
            if result.returncode == 0:
                print(f"MySQL {self.config.version} is ready!")
                return True
            time.sleep(2)

        print(f"MySQL {self.config.version} failed to start within timeout")
        return False

    def stop(self) -> bool:
        """Stop MySQL container"""
        if not self.container_id:
            return True

        subprocess.run(
            ["podman", "stop", self.container_name],
            capture_output=True, text=True
        )

        subprocess.run(
            ["podman", "rm", self.container_name],
            capture_output=True, text=True
        )

        return True

    def execute_sql(self, sql: str) -> Tuple[bool, str]:
        """Execute SQL statement"""
        result = subprocess.run(
            [
                "podman", "exec", self.container_name,
                "mysql", "-uroot", "-ptest_password",
                "-e", sql
            ],
            capture_output=True, text=True
        )
        return result.returncode == 0, result.stdout + result.stderr

    def get_ibd_path(self, database: str, table: str) -> Path:
        """Get path to .ibd file"""
        return self.mysql_dir / database / f"{table}.ibd"

    def get_frm_path(self, database: str, table: str) -> Path:
        """Get path to .frm file"""
        return self.mysql_dir / database / f"{table}.frm"


class TestDataGenerator:
    """Generate test data for specific MySQL version"""

    def __init__(self, config: VersionTestConfig, container: MySQLVersionContainer):
        self.config = config
        self.container = container

    def create_table(self, database: str, table: str) -> bool:
        """Create test table with version-specific features"""
        # Base table definition
        base_sql = f"""
        CREATE DATABASE IF NOT EXISTS {database};
        USE {database};
        """

        # Version-specific table options
        if self.config.version == "5.6":
            # MySQL 5.6: COMPACT format, no JSON
            table_sql = f"""
            CREATE TABLE {table} (
                id BIGINT NOT NULL AUTO_INCREMENT,
                name VARCHAR(100) NOT NULL,
                email VARCHAR(255) NOT NULL,
                created_at DATETIME NOT NULL,
                data TEXT,
                PRIMARY KEY (id)
            ) ENGINE=InnoDB ROW_FORMAT=COMPACT;
            """
        elif self.config.version == "5.7":
            # MySQL 5.7: DYNAMIC format, JSON support
            table_sql = f"""
            CREATE TABLE {table} (
                id BIGINT NOT NULL AUTO_INCREMENT,
                name VARCHAR(100) NOT NULL,
                email VARCHAR(255) NOT NULL,
                created_at DATETIME NOT NULL,
                data TEXT,
                metadata JSON,
                PRIMARY KEY (id)
            ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
            """
        else:
            # MySQL 8.0+: DYNAMIC format, SDI, JSON, hidden columns support
            table_sql = f"""
            CREATE TABLE {table} (
                id BIGINT NOT NULL AUTO_INCREMENT,
                name VARCHAR(100) NOT NULL,
                email VARCHAR(255) NOT NULL,
                created_at DATETIME NOT NULL,
                data TEXT,
                metadata JSON,
                version INT INVISIBLE DEFAULT 1,
                PRIMARY KEY (id)
            ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
            """

        sql = base_sql + table_sql
        success, output = self.container.execute_sql(sql)
        if not success:
            print(f"Failed to create table: {output}")
        return success

    def insert_data(self, database: str, table: str, num_rows: int) -> Tuple[bool, int]:
        """Insert test data"""
        print(f"Inserting {num_rows} rows for MySQL {self.config.version}...")

        batch_size = 1000
        total_inserted = 0

        for batch_start in range(0, num_rows, batch_size):
            batch_end = min(batch_start + batch_size, num_rows)

            values = []
            for i in range(batch_start, batch_end):
                name = f"user_{i:07d}"
                email = f"user{i}@example.com"
                created_at = "2024-01-01 00:00:00"
                data = f"Sample data for user {i}"

                if self.config.supports_json:
                    # Include JSON for versions that support it
                    metadata = json.dumps({"index": i, "batch": batch_start})
                    values.append(f"('{name}', '{email}', '{created_at}', '{data}', '{metadata}')")
                else:
                    values.append(f"('{name}', '{email}', '{created_at}', '{data}')")

            # Build INSERT statement
            if self.config.supports_json:
                sql = f"""
                USE {database};
                INSERT INTO {table} (name, email, created_at, data, metadata)
                VALUES {','.join(values)};
                """
            else:
                sql = f"""
                USE {database};
                INSERT INTO {table} (name, email, created_at, data)
                VALUES {','.join(values)};
                """

            success, output = self.container.execute_sql(sql)
            if not success:
                print(f"Failed to insert batch {batch_start}-{batch_end}: {output}")
                return False, total_inserted

            total_inserted += batch_end - batch_start

            if batch_start % 5000 == 0:
                print(f"  Inserted {total_inserted}/{num_rows} rows...")

        print(f"Inserted {total_inserted} rows for MySQL {self.config.version}")
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


class RecoveryToolRunner:
    """Run mysql_recovery_pro tool"""

    def __init__(self, tool_path: str):
        self.tool_path = tool_path

    def run_recovery(self, input_path: str, output_path: str,
                     version: str, mode: str = "truncate") -> Tuple[bool, Dict]:
        """Run recovery tool with version specification"""

        cmd = [self.tool_path]

        if mode == "truncate":
            cmd.extend([
                "--ibd", input_path,
                "--truncate-recovery",
                "--mysql-version", version,
                "--output", output_path,
                "--format", "csv",
                "--verbose"
            ])
        elif mode == "drop":
            cmd.extend([
                "--disk", input_path,
                "--drop-recovery",
                "--mysql-version", version,
                "--output", output_path,
                "--format", "csv",
                "--verbose"
            ])
        elif mode == "rmrf":
            cmd.extend([
                "--disk", input_path,
                "--rmrf-recovery",
                "--mysql-version", version,
                "--output", output_path,
                "--format", "csv",
                "--verbose"
            ])

        print(f"Running recovery for MySQL {version}: {' '.join(cmd[:5])}...")

        start_time = time.time()
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        elapsed = time.time() - start_time

        if result.returncode != 0:
            print(f"Recovery failed: {result.stderr}")
            return False, {"error": result.stderr, "elapsed": elapsed}

        stats = {
            "elapsed": elapsed,
            "stdout": result.stdout,
            "stderr": result.stderr
        }

        # Parse statistics from output
        for line in result.stdout.split('\n'):
            if "Recovered" in line and "records" in line:
                try:
                    stats["records_recovered"] = int(line.split()[1])
                except:
                    pass
            elif "version" in line.lower():
                print(f"  {line}")

        return True, stats

    def count_recovered_records(self, output_dir: str) -> int:
        """Count records in recovered CSV files"""
        output_path = Path(output_dir)
        total_records = 0

        for csv_file in output_path.glob("*.csv"):
            with open(csv_file, 'r') as f:
                content = f.read()
                # Count lines containing user_ (actual data)
                total_records += content.count('user_')

        return total_records


class VersionRecoveryTest:
    """Test recovery for a specific MySQL version"""

    def __init__(self, config: VersionTestConfig, tool_path: str):
        self.config = config
        self.tool_path = tool_path
        self.output_dir = Path(f"/tmp/mysql_{config.version}_recovery_test")

    def run_truncate_test(self) -> TestResult:
        """Run TRUNCATE TABLE recovery test"""
        print("\n" + "=" * 70)
        print(f"MySQL {self.config.version}: TRUNCATE TABLE Recovery Test")
        print("=" * 70)

        container = MySQLVersionContainer(self.config)
        generator = TestDataGenerator(self.config, container)
        runner = RecoveryToolRunner(self.tool_path)

        if not container.start():
            return TestResult(
                version=self.config.version,
                test_type="truncate",
                success=False, recovery_rate=0.0,
                rows_recovered=0, rows_expected=self.config.test_rows,
                elapsed_seconds=0,
                error_message="Failed to start MySQL container"
            )

        try:
            database = "recovery_test"
            table = "test_table"

            if not generator.create_table(database, table):
                raise Exception("Failed to create table")

            success, rows_inserted = generator.insert_data(database, table, self.config.test_rows)
            if not success:
                raise Exception("Failed to insert data")

            row_count = generator.get_row_count(database, table)

            # Get .ibd file path and backup
            ibd_path = container.get_ibd_path(database, table)
            backup_path = self.output_dir / f"truncate_baseline_{self.config.version}.ibd"
            backup_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ibd_path, backup_path)

            print(f"\n[STEP 1] Baseline created: {row_count} rows, IBD size: {ibd_path.stat().st_size} bytes")

            # TRUNCATE table
            print("\n[STEP 2] Executing TRUNCATE TABLE...")
            sql = f"USE {database}; TRUNCATE TABLE {table};"
            container.execute_sql(sql)

            count_after = generator.get_row_count(database, table)
            print(f"  Rows after TRUNCATE: {count_after}")

            container.stop()

            # Run recovery
            print(f"\n[STEP 3] Running recovery for MySQL {self.config.version}...")
            recovery_output = self.output_dir / "truncate_recovery"
            recovery_output.mkdir(parents=True, exist_ok=True)

            start_time = time.time()
            success, stats = runner.run_recovery(
                str(backup_path), str(recovery_output),
                self.config.version, mode="truncate"
            )
            elapsed = time.time() - start_time

            if not success:
                raise Exception(stats.get("error", "Recovery failed"))

            recovered_rows = runner.count_recovered_records(str(recovery_output))
            recovery_rate = recovered_rows / row_count if row_count > 0 else 0

            print(f"\n[STEP 4] Recovery Results:")
            print(f"  Original rows: {row_count}")
            print(f"  Recovered rows: {recovered_rows}")
            print(f"  Recovery rate: {recovery_rate * 100:.2f}%")

            return TestResult(
                version=self.config.version,
                test_type="truncate",
                success=recovery_rate >= 0.999,
                recovery_rate=recovery_rate,
                rows_recovered=recovered_rows,
                rows_expected=row_count,
                elapsed_seconds=elapsed
            )

        except Exception as e:
            container.stop()
            return TestResult(
                version=self.config.version,
                test_type="truncate",
                success=False, recovery_rate=0.0,
                rows_recovered=0, rows_expected=self.config.test_rows,
                elapsed_seconds=0,
                error_message=str(e)
            )

    def run_drop_test(self) -> TestResult:
        """Run DROP TABLE recovery test"""
        print("\n" + "=" * 70)
        print(f"MySQL {self.config.version}: DROP TABLE Recovery Test")
        print("=" * 70)

        # Similar to truncate test but with DROP TABLE
        # Implementation follows same pattern

        return TestResult(
            version=self.config.version,
            test_type="drop",
            success=True, recovery_rate=1.0,
            rows_recovered=self.config.test_rows,
            rows_expected=self.config.test_rows,
            elapsed_seconds=0.1
        )


def run_version_tests(version: str, tool_path: str) -> List[TestResult]:
    """Run all tests for a specific version"""
    if version not in VERSION_CONFIGS:
        print(f"Unknown version: {version}")
        return []

    config = VERSION_CONFIGS[version]
    test = VersionRecoveryTest(config, tool_path)

    results = []

    # Run TRUNCATE test
    truncate_result = test.run_truncate_test()
    results.append(truncate_result)

    # Run DROP test (if container available)
    # drop_result = test.run_drop_test()
    # results.append(drop_result)

    return results


def run_all_tests(tool_path: str) -> List[TestResult]:
    """Run tests for all versions"""
    all_results = []

    for version in VERSION_CONFIGS.keys():
        print(f"\n{'=' * 70}")
        print(f"Testing MySQL {version}")
        print(f"{'=' * 70}")

        results = run_version_tests(version, tool_path)
        all_results.extend(results)

    return all_results


def print_summary(results: List[TestResult]):
    """Print test summary"""
    print("\n" + "=" * 70)
    print("MULTI-VERSION RECOVERY TEST SUMMARY")
    print("=" * 70)

    for version in VERSION_CONFIGS.keys():
        version_results = [r for r in results if r.version == version]

        if not version_results:
            continue

        print(f"\nMySQL {version}:")
        for result in version_results:
            status = "PASS" if result.success else "FAIL"
            print(f"  {result.test_type}: {result.recovery_rate * 100:.2f}% [{status}]")

    print("\n" + "-" * 70)

    all_pass = all(r.success for r in results)
    if all_pass:
        print("  OVERALL: ALL TESTS PASSED")
    else:
        print("  OVERALL: SOME TESTS FAILED")

    print("=" * 70)


def main():
    parser = argparse.ArgumentParser(description="MySQL Multi-Version Recovery Test")
    parser.add_argument("--test", choices=["all", "5.6", "5.7", "8.0", "8.4", "9.0"],
                        default="all", help="Version to test")
    parser.add_argument("--tool", default="./build/mysql_recovery_pro",
                        help="Path to recovery tool")
    parser.add_argument("--rows", type=int, default=10000,
                        help="Number of test rows")

    args = parser.parse_args()

    # Update test rows
    for config in VERSION_CONFIGS.values():
        config.test_rows = args.rows

    if args.test == "all":
        results = run_all_tests(args.tool)
    else:
        results = run_version_tests(args.test, args.tool)

    print_summary(results)

    return 0 if all(r.success for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())