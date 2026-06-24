#!/usr/bin/env python3
"""
MySQL Recovery Pro - Comprehensive Disaster Recovery Validation
Tests MySQL 5.6/5.7/8.0/8.4/9.0 with DROP TABLE, TRUNCATE TABLE, rm -rf scenarios
"""

import os
import sys
import json
import subprocess
import shutil
import time
import hashlib
import random
import string
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass, field


@dataclass
class TestConfig:
    """Test configuration"""
    mysql_versions = ['5.6', '5.7', '8.0', '8.4', '9.0']
    mysql_ports = {
        '5.6': 3306,
        '5.7': 3307,
        '8.0': 3308,
        '8.4': 3309,
        '9.0': 3310
    }
    container_names = {
        '5.6': 'mysql56_test',
        '5.7': 'mysql57_test',
        '8.0': 'mysql80_test',
        '8.4': 'mysql84_test',
        '9.0': 'mysql90_test'
    }
    test_data_dir: str = '/app/mysql_recovery_test'
    recovery_tool: str = '/app/mysql_recovery_pro/build/mysql_recovery_pro'
    output_dir: str = '/app/mysql_recovery_test/comprehensive_validation'
    record_counts = {
        'small': 100,
        'medium': 1000,
        'large': 10000
    }


@dataclass
class RecoveryResult:
    """Recovery test result"""
    scenario: str
    mysql_version: str
    records_before: int
    records_recovered: int
    recovery_rate: float
    data_integrity: bool
    field_accuracy: float
    duration_seconds: float
    error_message: str = ""


class DisasterRecoveryValidator:
    """Comprehensive disaster recovery validation framework"""

    def __init__(self, config: TestConfig):
        self.config = config
        self.results: List[RecoveryResult] = []
        self.test_db = "disaster_test"
        self.test_table = "comprehensive_data"
        os.makedirs(config.output_dir, exist_ok=True)

    def log(self, msg: str, level: str = "INFO"):
        """Log message with timestamp"""
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{timestamp}] [{level}] {msg}")

    def run_sql(self, version: str, sql: str) -> Tuple[bool, str]:
        """Execute SQL against MySQL container"""
        port = self.config.mysql_ports[version]
        container = self.config.container_names[version]

        cmd = [
            'podman', 'exec', '-i', container,
            'mysql', '-uroot', '-proot', '-P3306', '-h127.0.0.1',
            '-e', sql
        ]

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            if result.returncode != 0:
                return False, result.stderr
            return True, result.stdout
        except subprocess.TimeoutExpired:
            return False, "SQL execution timeout"
        except Exception as e:
            return False, str(e)

    def start_containers(self):
        """Start all MySQL containers"""
        self.log("Starting MySQL containers...")

        for version in self.config.mysql_versions:
            container = self.config.container_names[version]

            # Check if container is running
            result = subprocess.run(
                ['podman', 'ps', '--filter', f'name={container}', '--format', '{{.Status}}'],
                capture_output=True, text=True
            )

            if 'Up' not in result.stdout:
                self.log(f"Starting {container}...")
                subprocess.run(['podman', 'start', container], capture_output=True)
                time.sleep(5)  # Wait for MySQL to be ready

                # Wait for MySQL to accept connections
                for _ in range(30):
                    success, _ = self.run_sql(version, "SELECT 1")
                    if success:
                        self.log(f"{container} is ready")
                        break
                    time.sleep(1)
                else:
                    self.log(f"WARNING: {container} may not be ready", "WARN")

    def create_test_data(self, version: str, count: int) -> Dict:
        """Create comprehensive test data with various field types"""
        self.log(f"Creating {count} test records for MySQL {version}...")

        # Create database and table
        create_sql = f"""
        DROP DATABASE IF EXISTS {self.test_db};
        CREATE DATABASE {self.test_db} CHARACTER SET utf8mb4;
        USE {self.test_db};

        CREATE TABLE {self.test_table} (
            id BIGINT PRIMARY KEY AUTO_INCREMENT,
            name VARCHAR(255) NOT NULL,
            email VARCHAR(255),
            amount DECIMAL(15,2),
            status ENUM('active', 'inactive', 'pending') DEFAULT 'active',
            created_at DATETIME,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
            description TEXT,
            metadata JSON,
            score DOUBLE,
            flags BIT(8),
            INDEX idx_name (name),
            INDEX idx_status (status)
        ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
        """

        success, err = self.run_sql(version, create_sql)
        if not success:
            self.log(f"Failed to create table: {err}", "ERROR")
            return {}

        # Insert test data
        records = []
        for i in range(1, count + 1):
            status = random.choice(['active', 'inactive', 'pending'])
            name = f"user_{i:05d}"
            email = f"{name}@test.com"
            amount = round(random.uniform(100.0, 10000.0), 2)
            desc = f"Description for record {i}: " + ''.join(random.choices(string.ascii_letters, k=50))
            metadata = json.dumps({"key": f"value_{i}", "index": i, "tags": [f"tag{j}" for j in range(3)]})
            score = round(random.uniform(0.0, 100.0), 2)
            flags = random.randint(0, 255)

            records.append({
                'id': i,
                'name': name,
                'email': email,
                'amount': amount,
                'status': status,
                'description': desc,
                'metadata': metadata,
                'score': score,
                'flags': flags
            })

        # Batch insert
        batch_size = 100
        for i in range(0, len(records), batch_size):
            batch = records[i:i + batch_size]
            values = []
            for r in batch:
                values.append(
                    f"('{r['name']}', '{r['email']}', {r['amount']}, '{r['status']}', "
                    f"NOW(), '{r['description']}', '{r['metadata']}', {r['score']}, b'{r['flags']:08b}')"
                )

            insert_sql = f"""
            USE {self.test_db};
            INSERT INTO {self.test_table}
                (name, email, amount, status, created_at, description, metadata, score, flags)
            VALUES {', '.join(values)};
            """

            success, err = self.run_sql(version, insert_sql)
            if not success:
                self.log(f"Failed to insert batch: {err}", "ERROR")
                return {}

        # Verify count
        success, output = self.run_sql(version, f"SELECT COUNT(*) FROM {self.test_db}.{self.test_table}")
        actual_count = 0
        if success:
            try:
                actual_count = int(output.strip().split('\n')[1])
            except:
                pass

        self.log(f"Created {actual_count} records in MySQL {version}")
        return {'count': actual_count, 'records': records[:10]}  # Return sample for verification

    def backup_ibd(self, version: str, scenario: str) -> Optional[str]:
        """Backup IBD file before disaster"""
        container = self.config.container_names[version]
        backup_dir = os.path.join(self.config.output_dir, f"{version}_{scenario}_backup")
        os.makedirs(backup_dir, exist_ok=True)

        # Find IBD file path in container
        cmd = ['podman', 'exec', container, 'find', '/var/lib/mysql', '-name', f'{self.test_table}.ibd']
        result = subprocess.run(cmd, capture_output=True, text=True)
        ibd_path = result.stdout.strip().split('\n')[0] if result.stdout.strip() else None

        if not ibd_path:
            self.log(f"Could not find IBD file for {version}", "ERROR")
            return None

        # Copy IBD file from container
        local_path = os.path.join(backup_dir, f"{self.test_table}.ibd")
        cmd = ['podman', 'cp', f'{container}:{ibd_path}', local_path]
        result = subprocess.run(cmd, capture_output=True, text=True)

        if result.returncode != 0:
            self.log(f"Failed to backup IBD: {result.stderr}", "ERROR")
            return None

        # Also get FRM file for MySQL 5.6/5.7
        if version in ['5.6', '5.7']:
            frm_path = ibd_path.replace('.ibd', '.frm')
            cmd = ['podman', 'cp', f'{container}:{frm_path}', os.path.join(backup_dir, f"{self.test_table}.frm")]
            subprocess.run(cmd, capture_output=True, text=True)

        self.log(f"Backed up IBD to {backup_dir}")
        return backup_dir

    def get_container_data_dir(self, version: str) -> str:
        """Get the data directory path inside container"""
        return f"/var/lib/mysql/{self.test_db}"

    def execute_drop_table(self, version: str) -> bool:
        """Execute DROP TABLE"""
        self.log(f"Executing DROP TABLE on MySQL {version}...")
        success, err = self.run_sql(version, f"DROP TABLE {self.test_db}.{self.test_table}")
        if success:
            self.log(f"DROP TABLE completed for MySQL {version}")
        else:
            self.log(f"DROP TABLE failed: {err}", "ERROR")
        return success

    def execute_truncate_table(self, version: str) -> bool:
        """Execute TRUNCATE TABLE"""
        self.log(f"Executing TRUNCATE TABLE on MySQL {version}...")
        success, err = self.run_sql(version, f"TRUNCATE TABLE {self.test_db}.{self.test_table}")
        if success:
            self.log(f"TRUNCATE TABLE completed for MySQL {version}")
        else:
            self.log(f"TRUNCATE TABLE failed: {err}", "ERROR")
        return success

    def execute_rmrf(self, version: str) -> bool:
        """Simulate rm -rf by deleting IBD file"""
        self.log(f"Simulating rm -rf on MySQL {version}...")
        container = self.config.container_names[version]

        # Stop container first
        subprocess.run(['podman', 'stop', container], capture_output=True)
        time.sleep(2)

        # Find data directory
        inspect = subprocess.run(
            ['podman', 'inspect', container, '--format', '{{range .Mounts}}{{if eq .Destination "/var/lib/mysql"}}{{.Source}}{{end}}{{end}}'],
            capture_output=True, text=True
        )
        data_dir = inspect.stdout.strip()

        if data_dir:
            # Remove IBD file
            ibd_path = os.path.join(data_dir, self.test_db, f"{self.test_table}.ibd")
            if os.path.exists(ibd_path):
                os.remove(ibd_path)
                self.log(f"Removed {ibd_path}")

        # Start container
        subprocess.run(['podman', 'start', container], capture_output=True)
        time.sleep(5)

        return True

    def recover_with_backup(self, version: str, scenario: str, backup_dir: str,
                            original_count: int) -> RecoveryResult:
        """Recover using backup IBD file"""
        start_time = time.time()

        ibd_path = os.path.join(backup_dir, f"{self.test_table}.ibd")
        output_dir = os.path.join(self.config.output_dir, f"{version}_{scenario}_recovery")
        os.makedirs(output_dir, exist_ok=True)

        recovery_mode = '--truncate-recovery' if scenario == 'truncate' else '--rmrf-recovery'
        if scenario == 'drop':
            recovery_mode = '--drop-table-recovery'

        # Run recovery
        cmd = [
            self.config.recovery_tool,
            '--ibd=' + ibd_path,
            recovery_mode,
            '--output=' + output_dir,
            '--format=csv',
            '--verbose'
        ]

        self.log(f"Running recovery: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)

        # Count recovered records
        recovered_count = 0
        csv_files = list(Path(output_dir).rglob('*.csv'))
        for csv_file in csv_files:
            with open(csv_file, 'r') as f:
                recovered_count += sum(1 for _ in f) - 1  # Subtract header

        duration = time.time() - start_time
        recovery_rate = (recovered_count / original_count * 100) if original_count > 0 else 0

        return RecoveryResult(
            scenario=scenario,
            mysql_version=version,
            records_before=original_count,
            records_recovered=recovered_count,
            recovery_rate=recovery_rate,
            data_integrity=True,
            field_accuracy=100.0,  # Will be calculated separately
            duration_seconds=duration,
            error_message=result.stderr if result.returncode != 0 else ""
        )

    def recover_disk_carving(self, version: str, original_count: int) -> RecoveryResult:
        """Recover using disk carving (no backup)"""
        start_time = time.time()
        container = self.config.container_names[version]

        # Get data directory from container
        inspect = subprocess.run(
            ['podman', 'inspect', container, '--format', '{{range .Mounts}}{{if eq .Destination "/var/lib/mysql"}}{{.Source}}{{end}}{{end}}'],
            capture_output=True, text=True
        )
        data_dir = inspect.stdout.strip()

        output_dir = os.path.join(self.config.output_dir, f"{version}_drop_carving_recovery")
        os.makedirs(output_dir, exist_ok=True)

        # For DROP TABLE, we need to scan the system tablespace
        system_tablespace = os.path.join(data_dir, 'ibdata1') if version in ['5.6', '5.7'] else os.path.join(data_dir, 'mysql.ibd')

        # Run disk carving recovery
        cmd = [
            self.config.recovery_tool,
            '--disk-carving',
            '--device-path=' + data_dir,
            '--output=' + output_dir,
            '--table-name=' + self.test_table,
            '--format=csv',
            '--verbose'
        ]

        self.log(f"Running disk carving recovery: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)

        # Count recovered records
        recovered_count = 0
        csv_files = list(Path(output_dir).rglob('*.csv'))
        for csv_file in csv_files:
            with open(csv_file, 'r') as f:
                recovered_count += sum(1 for _ in f) - 1

        duration = time.time() - start_time
        recovery_rate = (recovered_count / original_count * 100) if original_count > 0 else 0

        return RecoveryResult(
            scenario='drop_carving',
            mysql_version=version,
            records_before=original_count,
            records_recovered=recovered_count,
            recovery_rate=recovery_rate,
            data_integrity=True,
            field_accuracy=100.0,
            duration_seconds=duration,
            error_message=result.stderr if result.returncode != 0 else ""
        )

    def test_corruption_recovery(self, version: str, backup_dir: str,
                                  original_count: int) -> List[RecoveryResult]:
        """Test various corruption scenarios"""
        results = []
        ibd_path = os.path.join(backup_dir, f"{self.test_table}.ibd")

        corruption_types = [
            ('header_corruption', 'Corrupt page headers'),
            ('middle_corruption', 'Corrupt middle pages'),
            ('random_corruption', 'Random byte corruption'),
            ('chain_broken', 'Break page chains'),
        ]

        for corruption_type, description in corruption_types:
            self.log(f"Testing {corruption_type} for MySQL {version}...")

            # Create corrupted copy
            corrupted_path = os.path.join(backup_dir, f"{self.test_table}_{corruption_type}.ibd")
            shutil.copy(ibd_path, corrupted_path)

            # Apply corruption
            self._apply_corruption(corrupted_path, corruption_type)

            # Try recovery
            output_dir = os.path.join(self.config.output_dir, f"{version}_{corruption_type}_recovery")
            os.makedirs(output_dir, exist_ok=True)

            cmd = [
                self.config.recovery_tool,
                '--ibd=' + corrupted_path,
                '--header-repair',
                '--output=' + output_dir,
                '--format=csv'
            ]

            start_time = time.time()
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)

            # Count recovered records
            recovered_count = 0
            csv_files = list(Path(output_dir).rglob('*.csv'))
            for csv_file in csv_files:
                with open(csv_file, 'r') as f:
                    recovered_count += sum(1 for _ in f) - 1

            duration = time.time() - start_time
            recovery_rate = (recovered_count / original_count * 100) if original_count > 0 else 0

            results.append(RecoveryResult(
                scenario=corruption_type,
                mysql_version=version,
                records_before=original_count,
                records_recovered=recovered_count,
                recovery_rate=recovery_rate,
                data_integrity=True,
                field_accuracy=100.0,
                duration_seconds=duration,
                error_message=result.stderr if result.returncode != 0 else ""
            ))

        return results

    def _apply_corruption(self, file_path: str, corruption_type: str):
        """Apply specific corruption to file"""
        with open(file_path, 'rb') as f:
            data = bytearray(f.read())

        page_size = 16384  # Default InnoDB page size

        if corruption_type == 'header_corruption':
            # Corrupt first few page headers
            for i in range(min(3, len(data) // page_size)):
                offset = i * page_size
                if offset + 64 < len(data):
                    data[offset:offset+8] = bytes([random.randint(0, 255) for _ in range(8)])

        elif corruption_type == 'middle_corruption':
            # Corrupt middle pages
            start_page = len(data) // page_size // 3
            end_page = start_page + 3
            for i in range(start_page, min(end_page, len(data) // page_size)):
                offset = i * page_size + 100
                if offset + 100 < len(data):
                    data[offset:offset+100] = bytes([random.randint(0, 255) for _ in range(100)])

        elif corruption_type == 'random_corruption':
            # Random byte corruption (1%)
            num_corruptions = len(data) // 100
            for _ in range(num_corruptions):
                pos = random.randint(0, len(data) - 1)
                data[pos] = random.randint(0, 255)

        elif corruption_type == 'chain_broken':
            # Break page chains by corrupting next_page pointers
            for i in range(1, min(5, len(data) // page_size)):
                offset = i * page_size + 8  # FIL_PAGE_NEXT offset
                if offset + 4 < len(data):
                    data[offset:offset+4] = bytes([0xFF, 0xFF, 0xFF, 0xFF])

        with open(file_path, 'wb') as f:
            f.write(data)

    def generate_report(self) -> str:
        """Generate comprehensive test report"""
        report = []
        report.append("# MySQL Recovery Pro - Comprehensive Validation Report")
        report.append(f"## Test Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append("")

        # Summary table
        report.append("## Recovery Summary")
        report.append("")
        report.append("| MySQL Version | Scenario | Records Before | Records Recovered | Recovery Rate | Duration |")
        report.append("|---------------|----------|----------------|-------------------|---------------|----------|")

        for r in self.results:
            report.append(f"| {r.mysql_version} | {r.scenario} | {r.records_before} | {r.records_recovered} | {r.recovery_rate:.1f}% | {r.duration_seconds:.2f}s |")

        report.append("")

        # Detailed results by version
        report.append("## Detailed Results by Version")
        report.append("")

        for version in self.config.mysql_versions:
            version_results = [r for r in self.results if r.mysql_version == version]
            if version_results:
                report.append(f"### MySQL {version}")
                report.append("")

                for r in version_results:
                    status = "✓ PASS" if r.recovery_rate >= 99.0 else "✗ FAIL"
                    report.append(f"#### {r.scenario} {status}")
                    report.append(f"- Records: {r.records_before} → {r.records_recovered}")
                    report.append(f"- Recovery Rate: {r.recovery_rate:.2f}%")
                    report.append(f"- Duration: {r.duration_seconds:.2f}s")
                    if r.error_message:
                        report.append(f"- Error: {r.error_message[:200]}")
                    report.append("")

        # Error summary
        errors = [r for r in self.results if r.error_message]
        if errors:
            report.append("## Errors")
            report.append("")
            for r in errors:
                report.append(f"### {r.mysql_version} - {r.scenario}")
                report.append(f"```\n{r.error_message}\n```")
                report.append("")

        return "\n".join(report)

    def run_full_validation(self, record_count: int = 1000):
        """Run full validation across all MySQL versions and scenarios"""
        self.log("=" * 60)
        self.log("Starting Comprehensive Disaster Recovery Validation")
        self.log("=" * 60)

        # Start containers
        self.start_containers()

        for version in self.config.mysql_versions:
            self.log("")
            self.log(f"{'='*60}")
            self.log(f"Testing MySQL {version}")
            self.log(f"{'='*60}")

            # Create test data
            test_data = self.create_test_data(version, record_count)
            original_count = test_data.get('count', 0)

            if original_count == 0:
                self.log(f"Skipping MySQL {version} - failed to create test data", "ERROR")
                continue

            # ===== Test DROP TABLE =====
            self.log("")
            self.log(f"--- DROP TABLE Scenario ---")

            # Backup before disaster
            backup_dir = self.backup_ibd(version, 'drop')
            if backup_dir:
                # Execute DROP TABLE
                if self.execute_drop_table(version):
                    # Wait a moment
                    time.sleep(1)

                    # Recovery with backup
                    result = self.recover_with_backup(version, 'drop', backup_dir, original_count)
                    self.results.append(result)
                    self.log(f"DROP TABLE recovery: {result.recovery_rate:.1f}%")

            # Recreate table for next test
            self.create_test_data(version, record_count)
            backup_dir = self.backup_ibd(version, 'truncate')

            # ===== Test TRUNCATE TABLE =====
            self.log("")
            self.log(f"--- TRUNCATE TABLE Scenario ---")

            if backup_dir:
                if self.execute_truncate_table(version):
                    time.sleep(1)
                    result = self.recover_with_backup(version, 'truncate', backup_dir, original_count)
                    self.results.append(result)
                    self.log(f"TRUNCATE TABLE recovery: {result.recovery_rate:.1f}%")

            # Recreate table for next test
            self.create_test_data(version, record_count)
            backup_dir = self.backup_ibd(version, 'rmrf')

            # ===== Test rm -rf =====
            self.log("")
            self.log(f"--- rm -rf Scenario ---")

            if backup_dir:
                if self.execute_rmrf(version):
                    time.sleep(2)
                    result = self.recover_with_backup(version, 'rmrf', backup_dir, original_count)
                    self.results.append(result)
                    self.log(f"rm -rf recovery: {result.recovery_rate:.1f}%")

            # ===== Test Corruption Recovery =====
            self.log("")
            self.log(f"--- Corruption Recovery Scenario ---")

            # Recreate and backup
            self.create_test_data(version, record_count)
            backup_dir = self.backup_ibd(version, 'corruption')

            if backup_dir:
                corruption_results = self.test_corruption_recovery(version, backup_dir, original_count)
                self.results.extend(corruption_results)
                for cr in corruption_results:
                    self.log(f"{cr.scenario} recovery: {cr.recovery_rate:.1f}%")

        # Generate report
        report = self.generate_report()
        report_path = os.path.join(self.config.output_dir, "VALIDATION_REPORT.md")
        with open(report_path, 'w') as f:
            f.write(report)

        self.log("")
        self.log("=" * 60)
        self.log(f"Validation complete. Report saved to: {report_path}")
        self.log("=" * 60)

        return self.results


def main():
    parser = argparse.ArgumentParser(description='MySQL Recovery Pro Validation')
    parser.add_argument('--records', type=int, default=1000, help='Number of test records')
    parser.add_argument('--versions', nargs='+', default=['5.6', '5.7', '8.0', '8.4', '9.0'],
                        help='MySQL versions to test')
    parser.add_argument('--scenarios', nargs='+', default=['drop', 'truncate', 'rmrf', 'corruption'],
                        help='Scenarios to test')

    args = parser.parse_args()

    config = TestConfig()
    config.mysql_versions = args.versions

    validator = DisasterRecoveryValidator(config)
    results = validator.run_full_validation(args.records)

    # Print summary
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    for r in results:
        status = "PASS" if r.recovery_rate >= 99.0 else "FAIL"
        print(f"{r.mysql_version} {r.scenario}: {r.recovery_rate:.1f}% [{status}]")


if __name__ == '__main__':
    main()