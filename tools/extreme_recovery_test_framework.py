#!/usr/bin/env python3
"""
MySQL 5.6 Extreme Recovery Test Framework

This framework provides automated validation for the mysql_recovery_pro tool,
testing recovery scenarios including:
- TRUNCATE TABLE recovery
- rm -rf .ibd/.frm recovery

Requirements:
- Podman (for MySQL 5.6 container)
- Python 3.8+
- mysql_recovery_pro binary

Usage:
    python3 extreme_recovery_test_framework.py --test truncate
    python3 extreme_recovery_test_framework.py --test rmrf
    python3 extreme_recovery_test_framework.py --test all
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
class TestConfig:
    """Test configuration"""
    mysql_version: str = "5.6"
    container_name: str = "mysql_recovery_test"
    mysql_root_password: str = "test_password"
    test_database: str = "recovery_test_db"
    test_table: str = "test_table"
    num_rows: int = 5_000_000
    output_dir: str = "/tmp/mysql_recovery_test_output"
    recovery_tool_path: str = "./build/mysql_recovery_pro"
    page_size: int = 16384


@dataclass
class TestResult:
    """Test result"""
    test_name: str
    success: bool
    recovery_rate: float
    rows_recovered: int
    rows_expected: int
    hash_match: bool
    baseline_hash: str
    recovered_hash: str
    elapsed_seconds: float
    scan_speed_mb_s: float
    memory_usage_mb: float
    error_message: Optional[str] = None


class MySQL56Container:
    """MySQL 5.6 container management"""

    def __init__(self, config: TestConfig):
        self.config = config
        self.container_id: Optional[str] = None
        self.mysql_dir: Optional[Path] = None

    def start(self) -> bool:
        """Start MySQL 5.6 container"""
        # Create temporary directory for MySQL data
        self.mysql_dir = Path(tempfile.mkdtemp(prefix="mysql_recovery_test_"))

        # Pull MySQL 5.6 image
        print(f"Pulling MySQL {self.config.mysql_version} image...")
        result = subprocess.run(
            ["podman", "pull", f"mysql:{self.config.mysql_version}"],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            print(f"Failed to pull MySQL image: {result.stderr}")
            return False

        # Start container
        print(f"Starting MySQL container...")
        result = subprocess.run(
            [
                "podman", "run", "-d",
                "--name", self.config.container_name,
                "-e", f"MYSQL_ROOT_PASSWORD={self.config.mysql_root_password}",
                "-v", f"{self.mysql_dir}:/var/lib/mysql",
                "-p", "3306:3306",
                f"mysql:{self.config.mysql_version}"
            ],
            capture_output=True, text=True
        )

        if result.returncode != 0:
            print(f"Failed to start container: {result.stderr}")
            return False

        self.container_id = result.stdout.strip()

        # Wait for MySQL to be ready
        print("Waiting for MySQL to be ready...")
        for i in range(30):
            result = subprocess.run(
                [
                    "podman", "exec", self.config.container_name,
                    "mysql", "-uroot", f"-p{self.config.mysql_root_password}",
                    "-e", "SELECT 1"
                ],
                capture_output=True, text=True
            )
            if result.returncode == 0:
                print("MySQL is ready!")
                return True
            time.sleep(2)

        print("MySQL failed to start within timeout")
        return False

    def stop(self) -> bool:
        """Stop MySQL container"""
        if not self.container_id:
            return True

        result = subprocess.run(
            ["podman", "stop", self.config.container_name],
            capture_output=True, text=True
        )

        result = subprocess.run(
            ["podman", "rm", self.config.container_name],
            capture_output=True, text=True
        )

        # Clean up temporary directory
        if self.mysql_dir and self.mysql_dir.exists():
            shutil.rmtree(self.mysql_dir)

        return True

    def execute_sql(self, sql: str) -> Tuple[bool, str]:
        """Execute SQL statement"""
        result = subprocess.run(
            [
                "podman", "exec", self.config.container_name,
                "mysql", "-uroot", f"-p{self.config.mysql_root_password}",
                "-e", sql
            ],
            capture_output=True, text=True
        )
        return result.returncode == 0, result.stdout + result.stderr

    def get_ibd_path(self) -> Path:
        """Get path to .ibd file"""
        return self.mysql_dir / self.config.test_database / f"{self.config.test_table}.ibd"


class TestDataGenerator:
    """Generate test data for recovery testing"""

    def __init__(self, config: TestConfig, container: MySQL56Container):
        self.config = config
        self.container = container

    def create_table(self) -> bool:
        """Create test table"""
        sql = f"""
        CREATE DATABASE IF NOT EXISTS {self.config.test_database};
        USE {self.config.test_database};
        CREATE TABLE {self.config.test_table} (
            id BIGINT NOT NULL AUTO_INCREMENT,
            name VARCHAR(100) NOT NULL,
            email VARCHAR(255) NOT NULL,
            created_at DATETIME NOT NULL,
            data TEXT,
            PRIMARY KEY (id)
        ) ENGINE=InnoDB ROW_FORMAT=COMPACT;
        """
        success, output = self.container.execute_sql(sql)
        if not success:
            print(f"Failed to create table: {output}")
        return success

    def insert_data(self, num_rows: int) -> Tuple[bool, int]:
        """Insert test data"""
        print(f"Inserting {num_rows} rows...")

        batch_size = 10000
        total_inserted = 0

        for batch_start in range(0, num_rows, batch_size):
            batch_end = min(batch_start + batch_size, num_rows)

            # Generate batch insert
            values = []
            for i in range(batch_start, batch_end):
                name = f"user_{i:07d}"
                email = f"{name}@test.com"
                created_at = "2024-01-01 00:00:00"
                data = f"Sample data for user {i}"
                values.append(f"('{name}', '{email}', '{created_at}', '{data}')")

            sql = f"""
            USE {self.config.test_database};
            INSERT INTO {self.config.test_table} (name, email, created_at, data)
            VALUES {','.join(values)};
            """

            success, output = self.container.execute_sql(sql)
            if not success:
                print(f"Failed to insert batch {batch_start}-{batch_end}: {output}")
                return False, total_inserted

            total_inserted += batch_end - batch_start

            if batch_start % 100000 == 0:
                print(f"  Inserted {total_inserted}/{num_rows} rows...")

        print(f"Inserted {total_inserted} rows")
        return True, total_inserted

    def compute_hash(self) -> str:
        """Compute SHA-256 hash of all data"""
        print("Computing baseline hash...")

        # Export data to CSV
        sql = f"""
        USE {self.config.test_database};
        SELECT id, name, email, created_at, data
        FROM {self.config.test_table}
        ORDER BY id
        INTO OUTFILE '/tmp/baseline.csv'
        FIELDS TERMINATED BY ',' ENCLOSED BY '"' LINES TERMINATED BY '\\n';
        """

        success, output = self.container.execute_sql(sql)
        if not success:
            # Try alternative method
            result = subprocess.run(
                [
                    "podman", "exec", self.config.container_name,
                    "mysql", "-uroot", f"-p{self.config.mysql_root_password}",
                    "-e", f"USE {self.config.test_database}; SELECT id, name, email, created_at, data FROM {self.config.test_table} ORDER BY id;"
                ],
                capture_output=True, text=True
            )
            data = result.stdout
        else:
            # Read CSV file
            result = subprocess.run(
                [
                    "podman", "exec", self.config.container_name,
                    "cat", "/tmp/baseline.csv"
                ],
                capture_output=True, text=True
            )
            data = result.stdout

        # Compute hash
        hash_value = hashlib.sha256(data.encode()).hexdigest()
        print(f"Baseline hash: {hash_value}")
        return hash_value

    def get_row_count(self) -> int:
        """Get row count"""
        sql = f"USE {self.config.test_database}; SELECT COUNT(*) FROM {self.config.test_table};"
        success, output = self.container.execute_sql(sql)
        if success:
            # Parse count from output
            lines = output.strip().split('\n')
            if len(lines) >= 2:
                try:
                    return int(lines[1])
                except ValueError:
                    pass
        return 0


class RecoveryToolRunner:
    """Run mysql_recovery_pro tool"""

    def __init__(self, config: TestConfig):
        self.config = config

    def run_recovery(self, input_path: str, output_path: str,
                     schema_path: Optional[str] = None) -> Tuple[bool, Dict]:
        """Run recovery tool"""
        cmd = [
            self.config.recovery_tool_path,
            "extreme-recovery",
            "-d", input_path,
            "-o", output_path,
            "-t", "4"
        ]

        if schema_path:
            cmd.extend(["-s", schema_path])

        print(f"Running recovery: {' '.join(cmd)}")

        start_time = time.time()
        result = subprocess.run(cmd, capture_output=True, text=True)
        elapsed = time.time() - start_time

        if result.returncode != 0:
            print(f"Recovery failed: {result.stderr}")
            return False, {"error": result.stderr, "elapsed": elapsed}

        # Parse output for statistics
        stats = {
            "elapsed": elapsed,
            "stdout": result.stdout,
            "stderr": result.stderr
        }

        # Try to parse recovery statistics from output
        for line in result.stdout.split('\n'):
            if "Pages scanned:" in line:
                stats["pages_scanned"] = int(line.split(':')[1].strip())
            elif "Records recovered:" in line:
                stats["records_recovered"] = int(line.split(':')[1].strip())
            elif "Recovery rate:" in line:
                stats["recovery_rate"] = float(line.split(':')[1].strip().replace('%', ''))

        return True, stats

    def compute_recovered_hash(self, output_path: str) -> str:
        """Compute hash of recovered data"""
        recovered_file = Path(output_path) / "recovered_data.csv"
        if not recovered_file.exists():
            # Try SQL file
            recovered_file = Path(output_path) / "recovered_data.sql"
            if not recovered_file.exists():
                return ""

        with open(recovered_file, 'r') as f:
            data = f.read()

        # For SQL files, extract VALUES
        if recovered_file.suffix == '.sql':
            # Parse INSERT statements
            values = []
            for line in data.split('\n'):
                if 'INSERT' in line:
                    # Extract values
                    start = line.find('VALUES (')
                    if start >= 0:
                        values.append(line[start + 8:-2])
            data = '\n'.join(values)

        hash_value = hashlib.sha256(data.encode()).hexdigest()
        return hash_value


class TruncateRecoveryTest:
    """Test TRUNCATE TABLE recovery"""

    def __init__(self, config: TestConfig):
        self.config = config

    def run(self) -> TestResult:
        """Run TRUNCATE recovery test"""
        print("\n=== TRUNCATE Recovery Test ===")

        container = MySQL56Container(self.config)
        generator = TestDataGenerator(self.config, container)
        runner = RecoveryToolRunner(self.config)

        # Start container
        if not container.start():
            return TestResult(
                test_name="truncate_recovery",
                success=False,
                recovery_rate=0.0,
                rows_recovered=0,
                rows_expected=self.config.num_rows,
                hash_match=False,
                baseline_hash="",
                recovered_hash="",
                elapsed_seconds=0,
                scan_speed_mb_s=0,
                memory_usage_mb=0,
                error_message="Failed to start MySQL container"
            )

        try:
            # Create table and insert data
            if not generator.create_table():
                raise Exception("Failed to create table")

            success, rows_inserted = generator.insert_data(self.config.num_rows)
            if not success:
                raise Exception("Failed to insert data")

            baseline_hash = generator.compute_hash()
            row_count = generator.get_row_count()

            # Get .ibd file path
            ibd_path = container.get_ibd_path()

            # TRUNCATE table
            print("Executing TRUNCATE TABLE...")
            sql = f"USE {self.config.test_database}; TRUNCATE TABLE {self.config.test_table};"
            container.execute_sql(sql)

            # Stop container to ensure data is flushed
            container.stop()

            # Run recovery
            output_dir = Path(self.config.output_dir) / "truncate_test"
            output_dir.mkdir(parents=True, exist_ok=True)

            success, stats = runner.run_recovery(
                str(ibd_path), str(output_dir)
            )

            if not success:
                raise Exception(stats.get("error", "Recovery failed"))

            # Compute recovered hash
            recovered_hash = runner.compute_recovered_hash(str(output_dir))

            # Calculate results
            rows_recovered = stats.get("records_recovered", 0)
            recovery_rate = rows_recovered / row_count if row_count > 0 else 0
            hash_match = baseline_hash == recovered_hash

            return TestResult(
                test_name="truncate_recovery",
                success=recovery_rate >= 0.999,
                recovery_rate=recovery_rate,
                rows_recovered=rows_recovered,
                rows_expected=row_count,
                hash_match=hash_match,
                baseline_hash=baseline_hash,
                recovered_hash=recovered_hash,
                elapsed_seconds=stats.get("elapsed", 0),
                scan_speed_mb_s=ibd_path.stat().st_size / (1024 * 1024) / stats.get("elapsed", 1),
                memory_usage_mb=0
            )

        except Exception as e:
            container.stop()
            return TestResult(
                test_name="truncate_recovery",
                success=False,
                recovery_rate=0.0,
                rows_recovered=0,
                rows_expected=self.config.num_rows,
                hash_match=False,
                baseline_hash="",
                recovered_hash="",
                elapsed_seconds=0,
                scan_speed_mb_s=0,
                memory_usage_mb=0,
                error_message=str(e)
            )


class RmrfRecoveryTest:
    """Test rm -rf recovery"""

    def __init__(self, config: TestConfig):
        self.config = config

    def run(self) -> TestResult:
        """Run rm -rf recovery test"""
        print("\n=== rm -rf Recovery Test ===")

        container = MySQL56Container(self.config)
        generator = TestDataGenerator(self.config, container)
        runner = RecoveryToolRunner(self.config)

        # Start container
        if not container.start():
            return TestResult(
                test_name="rmrf_recovery",
                success=False,
                recovery_rate=0.0,
                rows_recovered=0,
                rows_expected=self.config.num_rows,
                hash_match=False,
                baseline_hash="",
                recovered_hash="",
                elapsed_seconds=0,
                scan_speed_mb_s=0,
                memory_usage_mb=0,
                error_message="Failed to start MySQL container"
            )

        try:
            # Create table and insert data
            if not generator.create_table():
                raise Exception("Failed to create table")

            success, rows_inserted = generator.insert_data(self.config.num_rows)
            if not success:
                raise Exception("Failed to insert data")

            baseline_hash = generator.compute_hash()
            row_count = generator.get_row_count()

            # Get .ibd and .frm file paths
            ibd_path = container.get_ibd_path()
            frm_path = container.mysql_dir / self.config.test_database / f"{self.config.test_table}.frm"

            # Stop container
            container.stop()

            # Delete files (rm -rf simulation)
            print("Deleting .ibd and .frm files...")
            if ibd_path.exists():
                ibd_path.unlink()
            if frm_path.exists():
                frm_path.unlink()

            # Run recovery on disk image
            # For testing, we use the MySQL data directory
            output_dir = Path(self.config.output_dir) / "rmrf_test"
            output_dir.mkdir(parents=True, exist_ok=True)

            success, stats = runner.run_recovery(
                str(container.mysql_dir), str(output_dir)
            )

            if not success:
                raise Exception(stats.get("error", "Recovery failed"))

            # Compute recovered hash
            recovered_hash = runner.compute_recovered_hash(str(output_dir))

            # Calculate results
            rows_recovered = stats.get("records_recovered", 0)
            recovery_rate = rows_recovered / row_count if row_count > 0 else 0
            hash_match = baseline_hash == recovered_hash

            return TestResult(
                test_name="rmrf_recovery",
                success=recovery_rate >= 0.999,
                recovery_rate=recovery_rate,
                rows_recovered=rows_recovered,
                rows_expected=row_count,
                hash_match=hash_match,
                baseline_hash=baseline_hash,
                recovered_hash=recovered_hash,
                elapsed_seconds=stats.get("elapsed", 0),
                scan_speed_mb_s=0,
                memory_usage_mb=0
            )

        except Exception as e:
            container.stop()
            return TestResult(
                test_name="rmrf_recovery",
                success=False,
                recovery_rate=0.0,
                rows_recovered=0,
                rows_expected=self.config.num_rows,
                hash_match=False,
                baseline_hash="",
                recovered_hash="",
                elapsed_seconds=0,
                scan_speed_mb_s=0,
                memory_usage_mb=0,
                error_message=str(e)
            )


class TestReport:
    """Generate test report"""

    def __init__(self, results: List[TestResult]):
        self.results = results

    def print_summary(self):
        """Print test summary"""
        print("\n" + "=" * 60)
        print("RECOVERY TEST SUMMARY")
        print("=" * 60)

        for result in self.results:
            status = "PASS" if result.success else "FAIL"
            print(f"\n{result.test_name}: {status}")
            print(f"  Recovery rate: {result.recovery_rate * 100:.2f}%")
            print(f"  Rows recovered: {result.rows_recovered}/{result.rows_expected}")
            print(f"  Hash match: {result.hash_match}")
            print(f"  Elapsed time: {result.elapsed_seconds:.2f}s")
            if result.scan_speed_mb_s > 0:
                print(f"  Scan speed: {result.scan_speed_mb_s:.2f} MB/s")
            if result.error_message:
                print(f"  Error: {result.error_message}")

        # Overall summary
        passed = sum(1 for r in self.results if r.success)
        total = len(self.results)

        print("\n" + "-" * 60)
        print(f"Overall: {passed}/{total} tests passed")

        if passed == total:
            print("All tests passed! Recovery rate >= 99.9% achieved.")
        else:
            print("Some tests failed. Recovery rate target not met.")

    def save_json(self, path: str):
        """Save results as JSON"""
        data = {
            "results": [
                {
                    "test_name": r.test_name,
                    "success": r.success,
                    "recovery_rate": r.recovery_rate,
                    "rows_recovered": r.rows_recovered,
                    "rows_expected": r.rows_expected,
                    "hash_match": r.hash_match,
                    "elapsed_seconds": r.elapsed_seconds,
                    "scan_speed_mb_s": r.scan_speed_mb_s,
                    "error_message": r.error_message
                }
                for r in self.results
            ],
            "summary": {
                "passed": sum(1 for r in self.results if r.success),
                "total": len(self.results),
                "target_recovery_rate": 0.999
            }
        }

        with open(path, 'w') as f:
            json.dump(data, f, indent=2)

        print(f"Results saved to {path}")


def main():
    parser = argparse.ArgumentParser(description="MySQL 5.6 Extreme Recovery Test Framework")
    parser.add_argument("--test", choices=["truncate", "rmrf", "all"],
                        default="all", help="Test to run")
    parser.add_argument("--num-rows", type=int, default=5_000_000,
                        help="Number of rows to insert")
    parser.add_argument("--output-dir", default="/tmp/mysql_recovery_test_output",
                        help="Output directory")
    parser.add_argument("--recovery-tool", default="./build/mysql_recovery_pro",
                        help="Path to recovery tool")
    parser.add_argument("--report", default="recovery_test_report.json",
                        help="Report output file")

    args = parser.parse_args()

    config = TestConfig(
        num_rows=args.num_rows,
        output_dir=args.output_dir,
        recovery_tool_path=args.recovery_tool
    )

    results = []

    if args.test in ["truncate", "all"]:
        test = TruncateRecoveryTest(config)
        results.append(test.run())

    if args.test in ["rmrf", "all"]:
        test = RmrfRecoveryTest(config)
        results.append(test.run())

    report = TestReport(results)
    report.print_summary()
    report.save_json(args.report)

    # Return exit code
    passed = sum(1 for r in results if r.success)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())