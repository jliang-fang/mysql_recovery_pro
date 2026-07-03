#!/usr/bin/env python3
"""
XFS 1000K Row Disaster Recovery Validation Test

This script performs comprehensive disaster recovery validation for MySQL 5.6-9.0
on XFS filesystem with 1 million row tables containing all field types.

Test Scenarios:
1. rm -rf datadir (file deletion)
2. DROP TABLE (table drop)
3. TRUNCATE TABLE (table truncation)

Recovery Methods:
- XFS AG (Allocation Group) scanning
- XFS free inode btree traversal
- XFS extent btree parsing for large files
- InnoDB page signature scanning
- FRM/SDI schema extraction

Author: jliang.fang
Date: 2026-07-03
"""

import os
import sys
import time
import json
import shutil
import subprocess
import argparse
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Optional, Tuple

# Configuration
MYSQL_VERSIONS = {
    "5.6": {"image": "mysql:5.6", "has_frm": True, "has_json": False, "has_sdi": False},
    "5.7": {"image": "mysql:5.7", "has_frm": True, "has_json": True, "has_sdi": False},
    "8.0": {"image": "mysql:8.0", "has_frm": False, "has_json": True, "has_sdi": True},
    "8.4": {"image": "mysql:8.4", "has_frm": False, "has_json": True, "has_sdi": True},
    "9.0": {"image": "mysql:9.0", "has_frm": False, "has_json": True, "has_sdi": True},
}

DISASTER_SCENARIOS = ["rm_rf", "drop_table", "truncate"]
NUM_ROWS = 1000000  # 1 million rows
RECOVERY_TOOL = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")
TEST_BASE = Path("/app/mysql_recovery_test/xfs_1000k_validation")

# All MySQL field types
ALL_FIELD_TYPES = """
    col_tinyint TINYINT,
    col_smallint SMALLINT,
    col_mediumint MEDIUMINT,
    col_int INT,
    col_bigint BIGINT,
    col_float FLOAT,
    col_double DOUBLE,
    col_decimal DECIMAL(15,4),
    col_char CHAR(50),
    col_varchar VARCHAR(255),
    col_text TEXT,
    col_mediumtext MEDIUMTEXT,
    col_blob BLOB,
    col_mediumblob MEDIUMBLOB,
    col_date DATE,
    col_time TIME,
    col_datetime DATETIME,
    col_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    col_year YEAR,
    col_binary BINARY(16),
    col_varbinary VARBINARY(100),
    col_bit BIT(8),
    col_bool BOOLEAN
"""

# Fields for MySQL 5.7+ (JSON support)
JSON_FIELD = "col_json JSON"

# Fields for MySQL 5.6/5.7 (ENUM/SET support)
ENUM_SET_FIELDS = """
    col_enum ENUM('active','inactive','pending','archived'),
    col_set SET('read','write','execute','admin')
"""


class XfsDisasterRecoveryTest:
    def __init__(self, block_device: str = "/dev/nvme0n2"):
        self.block_device = block_device
        self.mount_point = Path("/mnt/xfs_mysql_test")
        self.test_dir = TEST_BASE
        self.test_dir.mkdir(parents=True, exist_ok=True)
        self.results = {}
        self.log_file = self.test_dir / f"test_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"

    def log(self, msg: str, level: str = "INFO"):
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        colors = {"INFO": "\033[0m", "WARN": "\033[93m", "ERROR": "\033[91m",
                  "PASS": "\033[92m", "FAIL": "\033[91m"}
        line = f"[{timestamp}] [{level}] {msg}"
        print(f"{colors.get(level, '')}{line}\033[0m", flush=True)
        with open(self.log_file, "a") as f:
            f.write(line + "\n")

    def run_cmd(self, cmd: List[str], timeout: int = 300, check: bool = False) -> Tuple[bool, str, str]:
        """Run command with timeout"""
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout
            )
            success = result.returncode == 0
            if check and not success:
                self.log(f"Command failed: {' '.join(cmd)}", "ERROR")
                self.log(f"stderr: {result.stderr}", "ERROR")
            return success, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            self.log(f"Command timeout: {' '.join(cmd)}", "ERROR")
            return False, "", "Timeout"

    def setup_xfs_filesystem(self, size_gb: int = 100) -> bool:
        """Setup XFS filesystem on block device or loop device"""
        self.log(f"Setting up {size_gb}GB XFS filesystem...")

        # Check if block device exists
        if not Path(self.block_device).exists():
            self.log(f"Block device {self.block_device} not found, creating loop device")
            # Create loop device with XFS
            disk_img = self.test_dir / "xfs_disk.img"
            success, _, _ = self.run_cmd(
                ["fallocate", "-l", f"{size_gb}G", str(disk_img)]
            )
            if not success:
                return False

            # Setup loop device
            success, stdout, _ = self.run_cmd(["losetup", "-f", "--show", str(disk_img)])
            if not success:
                return False
            self.block_device = stdout.strip()
            self.log(f"Loop device: {self.block_device}")

        # Format as XFS
        self.run_cmd(["wipefs", "-a", self.block_device], timeout=60)
        success, _, _ = self.run_cmd(
            ["mkfs.xfs", "-f", "-b", "size=4096", self.block_device],
            timeout=120
        )
        if not success:
            self.log("Failed to create XFS filesystem", "ERROR")
            return False

        # Mount XFS
        self.mount_point.mkdir(parents=True, exist_ok=True)
        success, _, _ = self.run_cmd(
            ["mount", "-t", "xfs", self.block_device, str(self.mount_point)]
        )
        if not success:
            self.log("Failed to mount XFS filesystem", "ERROR")
            return False

        self.log(f"XFS filesystem mounted at {self.mount_point}", "PASS")
        return True

    def cleanup_filesystem(self):
        """Cleanup XFS filesystem"""
        self.log("Cleaning up filesystem...")

        # Unmount
        if self.mount_point.is_mount():
            self.run_cmd(["umount", str(self.mount_point)])

        # Remove loop device if used
        if "loop" in self.block_device:
            self.run_cmd(["losetup", "-d", self.block_device])

        self.log("Filesystem cleanup complete")

    def start_mysql_container(self, version: str, container_name: str) -> bool:
        """Start MySQL container with XFS volume mapping"""
        config = MYSQL_VERSIONS[version]
        data_dir = self.mount_point / f"mysql_{version.replace('.', '')}"
        data_dir.mkdir(parents=True, exist_ok=True)

        self.log(f"Starting MySQL {version} container...")

        # Remove existing container
        self.run_cmd(["podman", "rm", "-f", container_name])

        # Start new container with volume mapping
        cmd = [
            "podman", "run", "-d",
            "--name", container_name,
            "-e", "MYSQL_ROOT_PASSWORD=root123",
            "-e", "MYSQL_DATABASE=testdb",
            "-v", f"{data_dir}:/var/lib/mysql",
            config["image"]
        ]

        success, _, stderr = self.run_cmd(cmd, timeout=120)
        if not success:
            self.log(f"Failed to start MySQL {version}: {stderr}", "ERROR")
            return False

        # Wait for MySQL to be ready
        self.log("Waiting for MySQL to be ready...")
        for i in range(180):
            success, stdout, _ = self.run_cmd(
                ["podman", "exec", container_name, "mysqladmin", "-uroot", "-proot123", "ping"],
                timeout=10
            )
            if success and "alive" in stdout.lower():
                self.log(f"MySQL {version} is ready", "PASS")
                return True
            time.sleep(1)

        self.log(f"MySQL {version} failed to start within 180s", "ERROR")
        return False

    def create_test_table(self, container_name: str, version: str) -> bool:
        """Create test table with all field types"""
        config = MYSQL_VERSIONS[version]

        self.log("Creating test table with all field types...")

        # Build CREATE TABLE statement
        columns = ALL_FIELD_TYPES
        if config["has_json"]:
            columns += f",\n    {JSON_FIELD}"
        if version in ["5.6", "5.7"]:
            columns += f",\n    {ENUM_SET_FIELDS}"

        create_sql = f"""
        DROP TABLE IF EXISTS testdb.full_types_table;
        CREATE TABLE testdb.full_types_table (
            id INT PRIMARY KEY AUTO_INCREMENT,
            {columns}
        ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
        """

        success, _, stderr = self.run_cmd(
            ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123"],
            input_data=create_sql
        )

        if not success:
            self.log(f"Failed to create table: {stderr}", "ERROR")
            return False

        self.log("Test table created", "PASS")
        return True

    def insert_test_data(self, container_name: str, num_rows: int = NUM_ROWS) -> bool:
        """Insert test data in batches"""
        self.log(f"Inserting {num_rows:,} rows of test data...")

        batch_size = 10000
        total_batches = (num_rows + batch_size - 1) // batch_size

        for batch in range(total_batches):
            start_id = batch * batch_size + 1
            end_id = min((batch + 1) * batch_size, num_rows)

            # Generate batch insert
            values = []
            for i in range(start_id, end_id + 1):
                values.append(
                    f"({i % 128}, {i % 32768}, {i % 8388607}, {i}, {i * 1000}, "
                    f"{i * 1.5}, {i * 2.5}, {i * 100.1234}, "
                    f"'User_{i}', 'Description_{i}', 'Text content {i}', "
                    f"'Medium text {i}', X'{'AB' * 100}', X'{'CD' * 500}', "
                    f"'2026-07-03', '12:30:45', '2026-07-03 12:30:45', "
                    f"'2026', X'{'00' * 16}', X'{'FF' * 50}', b'{i % 256:08b}', "
                    f"{i % 2}"
                    f")"
                )

            insert_sql = f"""
            INSERT INTO testdb.full_types_table (
                col_tinyint, col_smallint, col_mediumint, col_int, col_bigint,
                col_float, col_double, col_decimal,
                col_char, col_varchar, col_text,
                col_mediumtext, col_blob, col_mediumblob,
                col_date, col_time, col_datetime,
                col_year, col_binary, col_varbinary, col_bit, col_bool
            ) VALUES {','.join(values)};
            """

            success, _, _ = self.run_cmd(
                ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-proot123"],
                input_data=insert_sql,
                timeout=300
            )

            if not success:
                self.log(f"Failed to insert batch {batch + 1}/{total_batches}", "ERROR")
                return False

            if (batch + 1) % 10 == 0:
                self.log(f"Inserted {(batch + 1) * batch_size:,} rows...")

        self.log(f"All {num_rows:,} rows inserted", "PASS")
        return True

    def flush_to_disk(self, container_name: str) -> bool:
        """Force flush all data to disk"""
        self.log("Flushing data to disk...")

        flush_cmds = [
            ["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "SET GLOBAL innodb_flush_log_at_trx_commit=1;"],
            ["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "SET GLOBAL innodb_max_dirty_pages_pct=0;"],
            ["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "FLUSH NO_WRITE_TO_BINLOG ENGINE INNODB;"],
            ["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
             "FLUSH NO_WRITE_TO_BINLOG TABLES;"],
            ["podman", "exec", container_name, "sync"],
        ]

        for cmd in flush_cmds:
            self.run_cmd(cmd, timeout=60)

        time.sleep(10)  # Wait for I/O to complete

        self.log("Data flushed to disk", "PASS")
        return True

    def execute_disaster(self, container_name: str, scenario: str, version: str) -> bool:
        """Execute disaster scenario"""
        self.log(f"Executing disaster scenario: {scenario}")

        if scenario == "rm_rf":
            # Stop container and delete data directory
            self.run_cmd(["podman", "stop", container_name], timeout=60)
            data_dir = self.mount_point / f"mysql_{version.replace('.', '')}"
            success, _, _ = self.run_cmd(["rm", "-rf", str(data_dir)], timeout=120)
            if not success:
                return False
            self.log("Data directory deleted (rm -rf)", "PASS")

        elif scenario == "drop_table":
            # Execute DROP TABLE
            success, _, _ = self.run_cmd(
                ["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
                 "DROP TABLE testdb.full_types_table;"],
                timeout=60
            )
            if not success:
                return False
            self.flush_to_disk(container_name)
            self.log("Table dropped", "PASS")

        elif scenario == "truncate":
            # Execute TRUNCATE TABLE
            success, _, _ = self.run_cmd(
                ["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-e",
                 "TRUNCATE TABLE testdb.full_types_table;"],
                timeout=60
            )
            if not success:
                return False
            self.flush_to_disk(container_name)
            self.log("Table truncated", "PASS")

        return True

    def run_recovery(self, version: str, scenario: str) -> Dict:
        """Run XFS disk carving recovery"""
        output_dir = self.test_dir / f"recovery_{version}_{scenario}"
        output_dir.mkdir(parents=True, exist_ok=True)

        self.log(f"Running XFS disk carving recovery...")

        start_time = time.time()

        cmd = [
            str(RECOVERY_TOOL),
            "--disk-carving",
            "--device-path", self.block_device,
            "--filesystem", "xfs",
            "--output", str(output_dir),
            "--num-workers", "8",
            "--verbose"
        ]

        success, stdout, stderr = self.run_cmd(cmd, timeout=3600)

        elapsed = time.time() - start_time

        result = {
            "success": success,
            "elapsed_time": elapsed,
            "output_dir": str(output_dir),
            "stdout": stdout,
            "stderr": stderr
        }

        if success:
            # Analyze recovery results
            result.update(self.analyze_recovery_output(output_dir))
            self.log(f"Recovery completed in {elapsed:.1f}s", "PASS")
        else:
            self.log(f"Recovery failed: {stderr}", "ERROR")

        return result

    def analyze_recovery_output(self, output_dir: Path) -> Dict:
        """Analyze recovered data"""
        csv_files = list(output_dir.glob("*.csv"))
        total_rows = 0
        total_size = 0

        for csv_file in csv_files:
            total_size += csv_file.stat().st_size
            with open(csv_file, "r") as f:
                total_rows += sum(1 for _ in f) - 1  # Exclude header

        return {
            "csv_files": len(csv_files),
            "total_rows": total_rows,
            "total_size_mb": total_size / (1024 * 1024)
        }

    def verify_data_quality(self, output_dir: Path, expected_rows: int) -> Dict:
        """Verify recovered data quality"""
        self.log("Verifying data quality...")

        quality = {
            "row_count_match": False,
            "data_integrity": False,
            "field_types_valid": False
        }

        # Check row count
        csv_files = list(output_dir.glob("*.csv"))
        total_rows = 0
        for csv_file in csv_files:
            with open(csv_file, "r") as f:
                total_rows += sum(1 for _ in f) - 1

        quality["row_count_match"] = total_rows >= expected_rows * 0.95  # Allow 5% loss
        quality["actual_rows"] = total_rows
        quality["expected_rows"] = expected_rows
        quality["recovery_rate"] = total_rows / expected_rows if expected_rows > 0 else 0

        if quality["row_count_match"]:
            self.log(f"Row count verification: {total_rows:,}/{expected_rows:,} ({quality['recovery_rate']*100:.1f}%)", "PASS")
        else:
            self.log(f"Row count mismatch: {total_rows:,}/{expected_rows:,} ({quality['recovery_rate']*100:.1f}%)", "WARN")

        return quality

    def run_single_test(self, version: str, scenario: str) -> Dict:
        """Run single test case"""
        container_name = f"mysql_xfs_{version.replace('.', '')}"

        self.log(f"\n{'='*60}", "INFO")
        self.log(f"Testing MySQL {version} - {scenario}", "INFO")
        self.log(f"{'='*60}", "INFO")

        result = {
            "version": version,
            "scenario": scenario,
            "start_time": datetime.now().isoformat(),
            "success": False
        }

        try:
            # Setup XFS
            if not self.setup_xfs_filesystem(size_gb=100):
                return result

            # Start MySQL
            if not self.start_mysql_container(version, container_name):
                return result

            # Create table
            if not self.create_test_table(container_name, version):
                return result

            # Insert data
            if not self.insert_test_data(container_name):
                return result

            # Flush to disk
            if not self.flush_to_disk(container_name):
                return result

            # Get actual row count
            success, stdout, _ = self.run_cmd(
                ["podman", "exec", container_name, "mysql", "-uroot", "-proot123", "-Nse",
                 "SELECT COUNT(*) FROM testdb.full_types_table;"]
            )
            if success:
                result["inserted_rows"] = int(stdout.strip())

            # Execute disaster
            if not self.execute_disaster(container_name, scenario, version):
                return result

            # Run recovery
            recovery_result = self.run_recovery(version, scenario)
            result.update(recovery_result)

            # Verify data quality
            if recovery_result["success"]:
                quality = self.verify_data_quality(
                    Path(recovery_result["output_dir"]),
                    result.get("inserted_rows", NUM_ROWS)
                )
                result["quality"] = quality
                result["success"] = quality.get("row_count_match", False)

        except Exception as e:
            self.log(f"Test failed with exception: {e}", "ERROR")
            result["error"] = str(e)

        finally:
            # Cleanup
            self.run_cmd(["podman", "rm", "-f", container_name], timeout=60)
            self.cleanup_filesystem()

        result["end_time"] = datetime.now().isoformat()
        return result

    def run_all_tests(self, versions: List[str] = None, scenarios: List[str] = None) -> Dict:
        """Run all test combinations"""
        versions = versions or list(MYSQL_VERSIONS.keys())
        scenarios = scenarios or DISASTER_SCENARIOS

        self.log(f"\nStarting XFS 1000K Disaster Recovery Validation")
        self.log(f"Versions: {', '.join(versions)}")
        self.log(f"Scenarios: {', '.join(scenarios)}")
        self.log(f"Rows per test: {NUM_ROWS:,}")

        all_results = {}

        for version in versions:
            all_results[version] = {}
            for scenario in scenarios:
                result = self.run_single_test(version, scenario)
                all_results[version][scenario] = result

                # Save intermediate results
                with open(self.test_dir / "results.json", "w") as f:
                    json.dump(all_results, f, indent=2)

        # Generate summary
        self.generate_summary(all_results)

        return all_results

    def generate_summary(self, results: Dict):
        """Generate test summary"""
        self.log("\n" + "="*60, "INFO")
        self.log("TEST SUMMARY", "INFO")
        self.log("="*60, "INFO")

        total_tests = 0
        passed_tests = 0

        for version, scenarios in results.items():
            self.log(f"\nMySQL {version}:")
            for scenario, result in scenarios.items():
                total_tests += 1
                status = "PASS" if result.get("success") else "FAIL"
                if result.get("success"):
                    passed_tests += 1

                recovery_rate = result.get("quality", {}).get("recovery_rate", 0) * 100
                elapsed = result.get("elapsed_time", 0)

                self.log(
                    f"  {scenario}: {status} - "
                    f"Recovery: {recovery_rate:.1f}%, Time: {elapsed:.1f}s",
                    status
                )

        self.log(f"\nTotal: {passed_tests}/{total_tests} tests passed",
                 "PASS" if passed_tests == total_tests else "WARN")


def main():
    parser = argparse.ArgumentParser(description="XFS 1000K Disaster Recovery Test")
    parser.add_argument("--device", default="/dev/nvme0n2", help="Block device for XFS")
    parser.add_argument("--versions", nargs="+", choices=list(MYSQL_VERSIONS.keys()),
                        default=list(MYSQL_VERSIONS.keys()), help="MySQL versions to test")
    parser.add_argument("--scenarios", nargs="+", choices=DISASTER_SCENARIOS,
                        default=DISASTER_SCENARIOS, help="Disaster scenarios")
    parser.add_argument("--rows", type=int, default=NUM_ROWS, help="Number of rows")

    args = parser.parse_args()

    global NUM_ROWS
    NUM_ROWS = args.rows

    tester = XfsDisasterRecoveryTest(block_device=args.device)

    try:
        results = tester.run_all_tests(versions=args.versions, scenarios=args.scenarios)
    except KeyboardInterrupt:
        tester.log("Test interrupted by user", "WARN")
        tester.cleanup_filesystem()
        sys.exit(1)

    # Exit with error if any test failed
    all_passed = all(
        result.get("success", False)
        for version in results.values()
        for result in version.values()
    )

    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
