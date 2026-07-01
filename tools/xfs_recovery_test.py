#!/usr/bin/env python3
"""
XFS Disaster Recovery Test Script for MySQL Recovery Pro

This script tests XFS filesystem recovery capabilities for MySQL databases:
- MySQL 5.6, 5.7, 8.0, 8.4, 9.0
- 1000k (1M) row table with all field types
- Disaster scenarios: rm -rf, DROP TABLE, TRUNCATE TABLE
- XFS filesystem block device mapping

Reference:
- xfs_undelete: https://codeberg.org/mateidibu/xfs_undelete
- XFS Algorithms & Data Structures: https://mirrors.edge.kernel.org/pub/linux/utils/fs/xfs/docs/xfs_filesystem_structure.pdf
"""

import os
import sys
import json
import time
import shutil
import subprocess
import argparse
import tempfile
import random
import uuid
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Optional, Tuple

# MySQL versions to test
MYSQL_VERSIONS = [
    "5.6",
    "5.7",
    "8.0",
    "8.4",
    "9.0"
]

# Disaster scenarios
DISASTER_SCENARIOS = [
    "rm_rf",      # rm -rf datadir
    "drop_table", # DROP TABLE
    "truncate",   # TRUNCATE TABLE
]

# Field types for comprehensive table
FIELD_TYPES = [
    ("id", "INT PRIMARY KEY AUTO_INCREMENT"),
    ("tinyint_col", "TINYINT"),
    ("smallint_col", "SMALLINT"),
    ("mediumint_col", "MEDIUMINT"),
    ("int_col", "INT"),
    ("bigint_col", "BIGINT"),
    ("float_col", "FLOAT"),
    ("double_col", "DOUBLE"),
    ("decimal_col", "DECIMAL(15,2)"),
    ("char_col", "CHAR(50)"),
    ("varchar_col", "VARCHAR(255)"),
    ("text_col", "TEXT"),
    ("mediumtext_col", "MEDIUMTEXT"),
    ("longtext_col", "LONGTEXT"),
    ("blob_col", "BLOB"),
    ("mediumblob_col", "MEDIUMBLOB"),
    ("longblob_col", "LONGBLOB"),
    ("date_col", "DATE"),
    ("time_col", "TIME"),
    ("datetime_col", "DATETIME"),
    ("timestamp_col", "TIMESTAMP"),
    ("year_col", "YEAR"),
    ("enum_col", "ENUM('active', 'inactive', 'pending')"),
    ("set_col", "SET('read', 'write', 'execute')"),
    ("json_col", "JSON"),
    ("binary_col", "BINARY(16)"),
    ("varbinary_col", "VARBINARY(255)"),
    ("bit_col", "BIT(8)"),
    ("bool_col", "BOOLEAN"),
]

# Container image mapping
CONTAINER_IMAGES = {
    "5.6": "mysql:5.6",
    "5.7": "mysql:5.7",
    "8.0": "mysql:8.0",
    "8.4": "mysql:8.4",  # May need custom image
    "9.0": "mysql:9.0",  # May need custom image
}


class XfsRecoveryTest:
    def __init__(self, test_dir: str = "/app/mysql_recovery_test/xfs_test"):
        self.test_dir = Path(test_dir)
        self.test_dir.mkdir(parents=True, exist_ok=True)
        self.results_dir = self.test_dir / "results"
        self.results_dir.mkdir(parents=True, exist_ok=True)
        self.results = {}

    def create_xfs_disk_image(self, size_mb: int = 500) -> Path:
        """Create a XFS-formatted disk image for testing"""
        disk_path = self.test_dir / "xfs_disk.img"

        if disk_path.exists():
            print(f"[INFO] Using existing XFS disk image: {disk_path}")
            return disk_path

        print(f"[INFO] Creating XFS disk image ({size_mb}MB)...")

        # Create empty disk image
        subprocess.run(
            ["dd", "if=/dev/zero", "of=str(disk_path)", "bs=1M", f"count={size_mb}"],
            check=True, capture_output=True
        )

        # Format as XFS
        subprocess.run(
            ["mkfs.xfs", "-f", str(disk_path)],
            check=True, capture_output=True
        )

        print(f"[INFO] XFS disk image created: {disk_path}")
        return disk_path

    def mount_xfs_disk(self, disk_path: Path) -> Path:
        """Mount XFS disk image"""
        mount_dir = self.test_dir / "xfs_mount"
        mount_dir.mkdir(parents=True, exist_ok=True)

        # Check if already mounted
        mount_check = subprocess.run(
            ["mountpoint", str(mount_dir)],
            capture_output=True
        )

        if mount_check.returncode == 0:
            print(f"[INFO] Already mounted at: {mount_dir}")
            return mount_dir

        print(f"[INFO] Mounting XFS disk...")

        # Mount the XFS disk (requires root)
        subprocess.run(
            ["mount", "-o", "loop", str(disk_path), str(mount_dir)],
            check=True, capture_output=True
        )

        print(f"[INFO] XFS disk mounted at: {mount_dir}")
        return mount_dir

    def start_mysql_container(self, version: str, data_dir: Path) -> str:
        """Start MySQL container with XFS-mounted data directory"""
        container_name = f"xfs_mysql_test_{version.replace('.', '_')}"
        image = CONTAINER_IMAGES.get(version, f"mysql:{version}")

        # Stop existing container if running
        subprocess.run(
            ["docker", "stop", container_name],
            capture_output=True, timeout=30
        )
        subprocess.run(
            ["docker", "rm", container_name],
            capture_output=True, timeout=30
        )

        # Create data directory for this version
        version_data_dir = data_dir / f"mysql_{version}"
        version_data_dir.mkdir(parents=True, exist_ok=True)

        print(f"[INFO] Starting MySQL {version} container...")

        # Start MySQL container with data directory mapped
        cmd = [
            "docker", "run", "-d",
            "--name", container_name,
            "-e", "MYSQL_ROOT_PASSWORD=testpass",
            "-e", "MYSQL_DATABASE=testdb",
            "-v", f"{version_data_dir}:/var/lib/mysql",
            "-p", f"330{version.replace('.', '')[-2:]}:3306",
            image
        ]

        subprocess.run(cmd, check=True, capture_output=True)

        # Wait for MySQL to be ready
        print(f"[INFO] Waiting for MySQL {version} to be ready...")
        time.sleep(30)  # Initial wait

        for i in range(60):  # Wait up to 60 seconds
            try:
                result = subprocess.run(
                    ["docker", "exec", container_name,
                     "mysql", "-uroot", "-ptestpass", "-e", "SELECT 1"],
                    capture_output=True, timeout=5
                )
                if result.returncode == 0:
                    print(f"[INFO] MySQL {version} ready!")
                    break
            except:
                pass
            time.sleep(1)

        return container_name

    def create_test_table(self, container_name: str, num_rows: int = 1000) -> bool:
        """Create test table with all field types"""
        print(f"[INFO] Creating test table with {num_rows} rows...")

        # Build CREATE TABLE statement
        columns = ", ".join([f"{name} {type_def}" for name, type_def in FIELD_TYPES])
        create_sql = f"CREATE TABLE IF NOT EXISTS test_table ({columns})"

        # Execute CREATE TABLE
        subprocess.run(
            ["docker", "exec", container_name,
             "mysql", "-uroot", "-ptestpass", "testdb", "-e", create_sql],
            check=True, capture_output=True
        )

        # Insert test data
        insert_sql = self._generate_insert_sql(num_rows)

        subprocess.run(
            ["docker", "exec", container_name,
             "mysql", "-uroot", "-ptestpass", "testdb", "-e", insert_sql],
            check=True, capture_output=True
        )

        # FLUSH TABLES to ensure data is on disk
        subprocess.run(
            ["docker", "exec", container_name,
             "mysql", "-uroot", "-ptestpass", "testdb", "-e", "FLUSH TABLES"],
            check=True, capture_output=True
        )

        # Verify row count
        result = subprocess.run(
            ["docker", "exec", container_name,
             "mysql", "-uroot", "-ptestpass", "testdb", "-N", "-e",
             "SELECT COUNT(*) FROM test_table"],
            check=True, capture_output=True, text=True
        )

        actual_count = int(result.stdout.strip())
        print(f"[INFO] Created table with {actual_count} rows")

        return actual_count >= num_rows

    def _generate_insert_sql(self, num_rows: int) -> str:
        """Generate INSERT SQL with test data"""
        values = []
        for i in range(num_rows):
            row = self._generate_test_row(i)
            values.append(f"({row})")

        # Split into batches to avoid SQL length limits
        batch_size = 1000
        inserts = []

        for i in range(0, len(values), batch_size):
            batch = values[i:i+batch_size]
            insert = f"INSERT INTO test_table VALUES {','.join(batch)}"
            inserts.append(insert)

        return "; ".join(inserts)

    def _generate_test_row(self, row_id: int) -> str:
        """Generate a single test row"""
        parts = []

        # id - auto-generated
        parts.append(str(row_id + 1))

        # TinyInt
        parts.append(str(random.randint(-128, 127)))

        # SmallInt
        parts.append(str(random.randint(-32768, 32767)))

        # MediumInt
        parts.append(str(random.randint(-8388608, 8388607)))

        # Int
        parts.append(str(random.randint(-2147483648, 2147483647)))

        # BigInt
        parts.append(str(random.randint(-9223372036854775808, 9223372036854775807)))

        # Float
        parts.append(str(random.uniform(-1000.0, 1000.0)))

        # Double
        parts.append(str(random.uniform(-1000000.0, 1000000.0)))

        # Decimal
        parts.append(str(random.uniform(-999999999999.99, 999999999999.99)))

        # Char
        parts.append(f"'char_{uuid.uuid4().hex[:50]}'")

        # VarChar
        parts.append(f"'varchar_{uuid.uuid4().hex}'")

        # Text
        parts.append(f"'text_{uuid.uuid4().hex * 4}'")

        # MediumText
        parts.append(f"'mediumtext_{uuid.uuid4().hex * 16}'")

        # LongText
        parts.append(f"'longtext_{uuid.uuid4().hex * 64}'")

        # Blob (hex encoded)
        parts.append(f"UNHEX('{uuid.uuid4().hex}')")

        # MediumBlob
        parts.append(f"UNHEX('{uuid.uuid4().hex * 4}')")

        # LongBlob
        parts.append(f"UNHEX('{uuid.uuid4().hex * 16}')")

        # Date
        parts.append(f"'{datetime.now().strftime('%Y-%m-%d')}'")

        # Time
        parts.append(f"'{datetime.now().strftime('%H:%M:%S')}'")

        # DateTime
        parts.append(f"'{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}'")

        # Timestamp
        parts.append("NOW()")

        # Year
        parts.append(str(random.randint(1901, 2155)))

        # Enum
        parts.append(f"'{random.choice(['active', 'inactive', 'pending'])}'")

        # Set
        parts.append(f"'{random.choice(['read', 'write', 'execute', 'read,write', 'read,execute', 'write,execute', 'read,write,execute'])}'")

        # JSON
        json_data = json.dumps({
            "id": row_id,
            "name": f"test_{row_id}",
            "values": [random.random() for _ in range(5)],
            "nested": {"key": "value", "number": random.randint(1, 100)}
        })
        parts.append(f"'{json_data}'")

        # Binary
        parts.append(f"UNHEX('{uuid.uuid4().hex[:16]}')")

        # VarBinary
        parts.append(f"UNHEX('{uuid.uuid4().hex}')")

        # Bit
        parts.append(f"b'{random.randint(0, 255):08b}'")

        # Bool
        parts.append(str(random.choice([0, 1])))

        return ", ".join(parts)

    def execute_disaster_scenario(self, scenario: str, container_name: str, data_dir: Path) -> Dict:
        """Execute disaster scenario"""
        version_data_dir = data_dir / f"mysql_{container_name.split('_')[-1]}"

        print(f"[INFO] Executing disaster scenario: {scenario}")

        result = {
            "scenario": scenario,
            "timestamp": datetime.now().isoformat(),
            "success": False
        }

        if scenario == "rm_rf":
            # Stop container first
            subprocess.run(["docker", "stop", container_name], capture_output=True)
            time.sleep(5)

            # rm -rf the data directory
            subprocess.run(["rm", "-rf", str(version_data_dir)], check=True)

            result["description"] = "Removed data directory via rm -rf"
            result["success"] = True

        elif scenario == "drop_table":
            # DROP TABLE while container running
            subprocess.run(
                ["docker", "exec", container_name,
                 "mysql", "-uroot", "-ptestpass", "testdb", "-e",
                 "DROP TABLE test_table"],
                check=True, capture_output=True
            )

            # FLUSH to ensure deletion is on disk
            subprocess.run(
                ["docker", "exec", container_name,
                 "mysql", "-uroot", "-ptestpass", "testdb", "-e",
                 "FLUSH TABLES"],
                check=True, capture_output=True
            )

            result["description"] = "Executed DROP TABLE test_table"
            result["success"] = True

        elif scenario == "truncate":
            # TRUNCATE TABLE while container running
            subprocess.run(
                ["docker", "exec", container_name,
                 "mysql", "-uroot", "-ptestpass", "testdb", "-e",
                 "TRUNCATE TABLE test_table"],
                check=True, capture_output=True
            )

            # FLUSH to ensure truncation is on disk
            subprocess.run(
                ["docker", "exec", container_name,
                 "mysql", "-uroot", "-ptestpass", "testdb", "-e",
                 "FLUSH TABLES"],
                check=True, capture_output=True
            )

            result["description"] = "Executed TRUNCATE TABLE test_table"
            result["success"] = True

        return result

    def run_xfs_recovery(self, disk_path: Path, output_dir: Path, scenario: str, version: str) -> Dict:
        """Run XFS disk carving recovery"""
        print(f"[INFO] Running XFS recovery for {scenario} on MySQL {version}...")

        recovery_output = output_dir / f"{version}_{scenario}_recovery"
        recovery_output.mkdir(parents=True, exist_ok=True)

        # Build recovery command
        recovery_cmd = [
            "/app/mysql_recovery_pro/build/mysql_recovery_pro",
            "--mode", "disk-carve",
            "--device", str(disk_path),
            "--filesystem", "xfs",
            "--output", str(recovery_output),
            "--page-size", "16384",
            "--workers", "8",
            "--recover-deleted",
            "--verbose"
        ]

        # If we have schema file, use it
        schema_file = self.test_dir / "schema.json"
        if schema_file.exists():
            recovery_cmd.extend(["--schema-file", str(schema_file)])

        result = {
            "command": " ".join(recovery_cmd),
            "output_dir": str(recovery_output),
            "timestamp": datetime.now().isoformat()
        }

        try:
            proc = subprocess.run(
                recovery_cmd,
                capture_output=True,
                text=True,
                timeout=600  # 10 minutes max
            )

            result["stdout"] = proc.stdout
            result["stderr"] = proc.stderr
            result["return_code"] = proc.returncode
            result["success"] = proc.returncode == 0

            # Count recovered records
            csv_files = list(recovery_output.glob("*.csv"))
            total_records = 0

            for csv_file in csv_files:
                with open(csv_file, 'r') as f:
                    lines = f.readlines()
                    if lines:
                        total_records += len(lines) - 1  # Subtract header

            result["csv_files"] = [str(f) for f in csv_files]
            result["records_recovered"] = total_records

        except subprocess.TimeoutExpired:
            result["error"] = "Recovery timed out after 600 seconds"
            result["success"] = False
        except Exception as e:
            result["error"] = str(e)
            result["success"] = False

        return result

    def save_results(self, results: Dict):
        """Save test results to JSON file"""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        result_file = self.results_dir / f"xfs_recovery_test_{timestamp}.json"

        with open(result_file, 'w') as f:
            json.dump(results, f, indent=2, default=str)

        print(f"[INFO] Results saved to: {result_file}")

        return result_file

    def cleanup(self):
        """Cleanup test resources"""
        print("[INFO] Cleaning up...")

        # Stop all test containers
        for version in MYSQL_VERSIONS:
            container_name = f"xfs_mysql_test_{version.replace('.', '_')}"
            subprocess.run(["docker", "stop", container_name], capture_output=True, timeout=30)
            subprocess.run(["docker", "rm", container_name], capture_output=True, timeout=30)

        # Unmount XFS disk
        mount_dir = self.test_dir / "xfs_mount"
        subprocess.run(["umount", str(mount_dir)], capture_output=True)

    def run_full_test(self, versions: List[str] = None, scenarios: List[str] = None,
                      num_rows: int = 10000, disk_size_mb: int = 500) -> Dict:
        """Run full test suite"""
        if versions is None:
            versions = MYSQL_VERSIONS
        if scenarios is None:
            scenarios = DISASTER_SCENARIOS

        results = {
            "test_config": {
                "versions": versions,
                "scenarios": scenarios,
                "num_rows": num_rows,
                "disk_size_mb": disk_size_mb,
                "filesystem": "xfs",
                "timestamp": datetime.now().isoformat()
            },
            "tests": []
        }

        try:
            # Create and mount XFS disk
            disk_path = self.create_xfs_disk_image(disk_size_mb)
            mount_dir = self.mount_xfs_disk(disk_path)

            for version in versions:
                print(f"\n{'='*60}")
                print(f"[TEST] MySQL {version} on XFS")
                print(f"{'='*60}")

                version_tests = {
                    "version": version,
                    "scenarios": []
                }

                # Start MySQL container
                container_name = self.start_mysql_container(version, mount_dir)

                # Create test table
                if not self.create_test_table(container_name, num_rows):
                    print(f"[ERROR] Failed to create test table for MySQL {version}")
                    continue

                # Export schema
                self._export_schema(container_name)

                for scenario in scenarios:
                    print(f"\n[TEST] Scenario: {scenario}")

                    # Create fresh table for each scenario
                    if scenario != "rm_rf":
                        # Re-create table if it was dropped/truncated
                        subprocess.run(
                            ["docker", "exec", container_name,
                             "mysql", "-uroot", "-ptestpass", "testdb", "-e",
                             "DROP TABLE IF EXISTS test_table"],
                            capture_output=True
                        )
                        self.create_test_table(container_name, num_rows)

                    # Execute disaster
                    disaster_result = self.execute_disaster_scenario(
                        scenario, container_name, mount_dir
                    )

                    # Run recovery
                    recovery_result = self.run_xfs_recovery(
                        disk_path, self.results_dir, scenario, version
                    )

                    version_tests["scenarios"].append({
                        "scenario": scenario,
                        "disaster": disaster_result,
                        "recovery": recovery_result,
                        "recovery_rate": recovery_result.get("records_recovered", 0) / num_rows if recovery_result.get("success") else 0
                    })

                results["tests"].append(version_tests)

        finally:
            self.cleanup()

        # Save results
        self.save_results(results)

        # Print summary
        self._print_summary(results)

        return results

    def _export_schema(self, container_name: str):
        """Export table schema for recovery"""
        schema_file = self.test_dir / "schema.json"

        # Get column info
        result = subprocess.run(
            ["docker", "exec", container_name,
             "mysql", "-uroot", "-ptestpass", "testdb", "-e",
             "SHOW CREATE TABLE test_table"],
            capture_output=True, text=True
        )

        # Parse and save schema (simplified)
        schema = {
            "table_name": "test_table",
            "columns": []
        }

        for name, type_def in FIELD_TYPES:
            schema["columns"].append({
                "name": name,
                "type": type_def,
                "ordinal": len(schema["columns"]) + 1
            })

        with open(schema_file, 'w') as f:
            json.dump(schema, f, indent=2)

    def _print_summary(self, results: Dict):
        """Print test summary"""
        print("\n" + "="*60)
        print("XFS RECOVERY TEST SUMMARY")
        print("="*60)

        for version_test in results.get("tests", []):
            version = version_test["version"]
            print(f"\nMySQL {version}:")

            for scenario in version_test.get("scenarios", []):
                name = scenario["scenario"]
                rate = scenario.get("recovery_rate", 0)
                records = scenario.get("recovery", {}).get("records_recovered", 0)
                success = scenario.get("recovery", {}).get("success", False)

                status = "✓" if success else "✗"
                print(f"  {name}: {status} ({records} records, {rate:.1%} recovery rate)")

        print("\n" + "="*60)


def main():
    parser = argparse.ArgumentParser(description="XFS Disaster Recovery Test")
    parser.add_argument("--versions", nargs="+", default=MYSQL_VERSIONS,
                        help="MySQL versions to test")
    parser.add_argument("--scenarios", nargs="+", default=DISASTER_SCENARIOS,
                        help="Disaster scenarios to test")
    parser.add_argument("--rows", type=int, default=10000,
                        help="Number of test rows (default: 10000)")
    parser.add_argument("--disk-size", type=int, default=500,
                        help="XFS disk size in MB (default: 500)")
    parser.add_argument("--test-dir", type=str, default="/app/mysql_recovery_test/xfs_test",
                        help="Test directory")
    parser.add_argument("--single", type=str, nargs=2,
                        help="Run single test: version scenario")

    args = parser.parse_args()

    test = XfsRecoveryTest(args.test_dir)

    if args.single:
        # Run single test
        version, scenario = args.single
        test.run_full_test(
            versions=[version],
            scenarios=[scenario],
            num_rows=args.rows,
            disk_size_mb=args.disk_size
        )
    else:
        # Run full test suite
        test.run_full_test(
            versions=args.versions,
            scenarios=args.scenarios,
            num_rows=args.rows,
            disk_size_mb=args.disk_size
        )


if __name__ == "__main__":
    main()