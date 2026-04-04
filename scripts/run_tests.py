#!/usr/bin/env python3
"""
Comprehensive Test Runner for MySQL Recovery Pro
Tests various recovery scenarios across different MySQL versions and corruption types
"""

import os
import sys
import subprocess
import json
import time
from pathlib import Path
from typing import Dict, List, Tuple, Optional

class TestResult:
    def __init__(self, name: str, passed: bool, duration: float, details: str = ""):
        self.name = name
        self.passed = passed
        self.duration = duration
        self.details = details

class TestRunner:
    def __init__(self, build_dir: str = "build", test_data_dir: str = "test_data"):
        self.build_dir = Path(build_dir)
        self.test_data_dir = Path(test_data_dir)
        self.results: List[TestResult] = []
        self.binaries = {
            'mysql_recovery_pro': self.build_dir / 'mysql_recovery_pro',
            'mysql_recover_cli': self.build_dir / 'mysql_recover_cli',
        }
    
    def run_command(self, cmd: List[str], timeout: int = 60) -> Tuple[bool, str, str]:
        """Run a command and return (success, stdout, stderr)"""
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout
            )
            return result.returncode == 0, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return False, "", "Command timed out"
        except Exception as e:
            return False, "", str(e)
    
    def check_binary(self, name: str) -> bool:
        """Check if a binary exists and is executable"""
        binary = self.binaries.get(name)
        if binary and binary.exists() and os.access(binary, os.X_OK):
            return True
        print(f"Warning: Binary not found or not executable: {name}")
        return False
    
    def test_basic_recovery(self) -> TestResult:
        """Test basic recovery from a valid table"""
        start_time = time.time()
        
        test_file = str(self.test_data_dir / "test_table_v80_ps16384_rec100.ibd")
        output_dir = Path("/tmp/test_basic_recovery")
        output_dir.mkdir(exist_ok=True)
        
        if not Path(test_file).exists():
            return TestResult("basic_recovery", False, time.time() - start_time,
                            f"Test file not found: {test_file}")
        
        cmd = [
            str(self.binaries['mysql_recovery_pro']),
            f"--ibd={test_file}",
            f"--output={output_dir}",
            "--format=csv"
        ]
        
        success, stdout, stderr = self.run_command(cmd)
        
        if success:
            csv_file = output_dir / "recovered.csv"
            if csv_file.exists():
                with open(csv_file) as f:
                    lines = f.readlines()
                    record_count = len(lines) - 1  # Exclude header
                    return TestResult("basic_recovery", True, time.time() - start_time,
                                    f"Recovered {record_count} records")
        
        return TestResult("basic_recovery", False, time.time() - start_time,
                         f"stdout: {stdout[:200]}, stderr: {stderr[:200]}")
    
    def test_truncate_recovery(self) -> TestResult:
        """Test truncate recovery mode"""
        start_time = time.time()
        
        test_file = str(self.test_data_dir / "test_table_v80_ps16384_rec100.ibd")
        output_dir = Path("/tmp/test_truncate_recovery")
        output_dir.mkdir(exist_ok=True)
        
        if not Path(test_file).exists():
            return TestResult("truncate_recovery", False, time.time() - start_time,
                            f"Test file not found: {test_file}")
        
        cmd = [
            str(self.binaries['mysql_recovery_pro']),
            f"--ibd={test_file}",
            f"--output={output_dir}",
            "--truncate-recovery",
            "--format=csv"
        ]
        
        success, stdout, stderr = self.run_command(cmd)
        
        if success:
            csv_file = output_dir / "recovered.csv"
            if csv_file.exists():
                with open(csv_file) as f:
                    lines = f.readlines()
                    record_count = len(lines) - 1
                    return TestResult("truncate_recovery", True, time.time() - start_time,
                                    f"Recovered {record_count} records")
        
        return TestResult("truncate_recovery", False, time.time() - start_time,
                         f"stdout: {stdout[:200]}, stderr: {stderr[:200]}")
    
    def test_rmrf_recovery(self) -> TestResult:
        """Test rm -rf recovery mode"""
        start_time = time.time()
        
        test_file = str(self.test_data_dir / "test_table_v80_ps16384_rec100.ibd")
        output_dir = Path("/tmp/test_rmrf_recovery")
        output_dir.mkdir(exist_ok=True)
        
        if not Path(test_file).exists():
            return TestResult("rmrf_recovery", False, time.time() - start_time,
                            f"Test file not found: {test_file}")
        
        cmd = [
            str(self.binaries['mysql_recovery_pro']),
            f"--ibd={test_file}",
            f"--output={output_dir}",
            "--rmrf-recovery",
            "--format=csv"
        ]
        
        success, stdout, stderr = self.run_command(cmd)
        
        if success:
            csv_file = output_dir / "recovered.csv"
            if csv_file.exists():
                with open(csv_file) as f:
                    lines = f.readlines()
                    record_count = len(lines) - 1
                    return TestResult("rmrf_recovery", True, time.time() - start_time,
                                    f"Recovered {record_count} records")
        
        return TestResult("rmrf_recovery", False, time.time() - start_time,
                         f"stdout: {stdout[:200]}, stderr: {stderr[:200]}")
    
    def test_header_repair(self) -> TestResult:
        """Test header repair mode"""
        start_time = time.time()
        
        test_file = str(self.test_data_dir / "test_corrupted_header_type_v80_ps16384.ibd")
        output_dir = Path("/tmp/test_header_repair")
        output_dir.mkdir(exist_ok=True)
        
        if not Path(test_file).exists():
            return TestResult("header_repair", False, time.time() - start_time,
                            f"Test file not found: {test_file}")
        
        cmd = [
            str(self.binaries['mysql_recovery_pro']),
            f"--ibd={test_file}",
            f"--output={output_dir}",
            "--check-headers"
        ]
        
        success, stdout, stderr = self.run_command(cmd)
        
        return TestResult("header_repair", success, time.time() - start_time,
                         f"stdout: {stdout[:200]}, stderr: {stderr[:200]}")
    
    def test_corruption_scenarios(self) -> TestResult:
        """Test various corruption scenarios"""
        start_time = time.time()
        
        test_files = [
            "test_corrupted_header_type_v80_ps16384.ibd",
            "test_corrupted_header_checksum_v80_ps16384.ibd",
            "test_corrupted_infimum_corrupt_v80_ps16384.ibd",
            "test_corrupted_supremum_corrupt_v80_ps16384.ibd",
            "test_corrupted_chain_broken_v80_ps16384.ibd",
            "test_corrupted_record_chain_broken_v80_ps16384.ibd",
        ]
        
        passed = 0
        failed = 0
        
        for test_file in test_files:
            file_path = str(self.test_data_dir / test_file)
            if not Path(file_path).exists():
                failed += 1
                continue
            
            output_dir = Path(f"/tmp/test_corruption_{test_file}")
            output_dir.mkdir(exist_ok=True)
            
            cmd = [
                str(self.binaries['mysql_recovery_pro']),
                f"--ibd={file_path}",
                f"--output={output_dir}",
                "--format=csv"
            ]
            
            success, _, _ = self.run_command(cmd)
            
            if success:
                passed += 1
            else:
                failed += 1
        
        total = passed + failed
        return TestResult("corruption_scenarios", passed > 0, time.time() - start_time,
                         f"Passed: {passed}/{total} corruption scenarios")
    
    def test_mysql_version_compatibility(self) -> TestResult:
        """Test recovery across different MySQL versions"""
        start_time = time.time()
        
        versions = [
            (56, "test_table_v56_ps16384_rec50.ibd"),
            (80, "test_table_v80_ps16384_rec100.ibd"),
            (84, "test_table_v84_ps16384_rec100.ibd"),
            (90, "test_table_v90_ps16384_rec100.ibd"),
        ]
        
        passed = 0
        failed = 0
        
        for version, test_file in versions:
            file_path = str(self.test_data_dir / test_file)
            if not Path(file_path).exists():
                failed += 1
                continue
            
            output_dir = Path(f"/tmp/test_version_{version}")
            output_dir.mkdir(exist_ok=True)
            
            cmd = [
                str(self.binaries['mysql_recovery_pro']),
                f"--ibd={file_path}",
                f"--output={output_dir}",
                "--format=csv"
            ]
            
            success, _, _ = self.run_command(cmd)
            
            if success:
                passed += 1
            else:
                failed += 1
        
        total = passed + failed
        return TestResult("version_compatibility", passed > 0, time.time() - start_time,
                         f"Passed: {passed}/{total} MySQL versions")
    
    def test_sql_export(self) -> TestResult:
        """Test SQL export functionality"""
        start_time = time.time()
        
        test_file = str(self.test_data_dir / "test_table_v80_ps16384_rec100.ibd")
        output_dir = Path("/tmp/test_sql_export")
        output_dir.mkdir(exist_ok=True)
        
        if not Path(test_file).exists():
            return TestResult("sql_export", False, time.time() - start_time,
                            f"Test file not found: {test_file}")
        
        cmd = [
            str(self.binaries['mysql_recovery_pro']),
            f"--ibd={test_file}",
            f"--output={output_dir}",
            "--format=sql"
        ]
        
        success, stdout, stderr = self.run_command(cmd)
        
        if success:
            sql_file = output_dir / "recovered.sql"
            if sql_file.exists():
                with open(sql_file) as f:
                    content = f.read()
                    insert_count = content.count("INSERT INTO")
                    return TestResult("sql_export", True, time.time() - start_time,
                                    f"Generated {insert_count} INSERT statements")
        
        return TestResult("sql_export", False, time.time() - start_time,
                         f"stdout: {stdout[:200]}, stderr: {stderr[:200]}")
    
    def test_blob_recovery(self) -> TestResult:
        """Test BLOB recovery mode"""
        start_time = time.time()
        
        test_file = str(self.test_data_dir / "test_table_v80_ps16384_rec100.ibd")
        output_dir = Path("/tmp/test_blob_recovery")
        output_dir.mkdir(exist_ok=True)
        
        if not Path(test_file).exists():
            return TestResult("blob_recovery", False, time.time() - start_time,
                            f"Test file not found: {test_file}")
        
        cmd = [
            str(self.binaries['mysql_recovery_pro']),
            f"--ibd={test_file}",
            f"--output={output_dir}",
            "--blob-recovery",
            "--format=csv"
        ]
        
        success, stdout, stderr = self.run_command(cmd)
        
        return TestResult("blob_recovery", success, time.time() - start_time,
                         f"stdout: {stdout[:200]}, stderr: {stderr[:200]}")
    
    def run_all_tests(self) -> Dict[str, TestResult]:
        """Run all tests and return results"""
        print("=" * 60)
        print("MySQL Recovery Pro - Comprehensive Test Suite")
        print("=" * 60)
        print()
        
        # Check binaries
        for name, binary in self.binaries.items():
            if not self.check_binary(name):
                print(f"Warning: {name} binary not found")
        
        # Run tests
        tests = [
            ("Basic Recovery", self.test_basic_recovery),
            ("Truncate Recovery", self.test_truncate_recovery),
            ("rm -rf Recovery", self.test_rmrf_recovery),
            ("Header Repair", self.test_header_repair),
            ("Corruption Scenarios", self.test_corruption_scenarios),
            ("MySQL Version Compatibility", self.test_mysql_version_compatibility),
            ("SQL Export", self.test_sql_export),
            ("BLOB Recovery", self.test_blob_recovery),
        ]
        
        for name, test_func in tests:
            print(f"Running: {name}...", end=" ")
            result = test_func()
            self.results.append(result)
            
            status = "PASS" if result.passed else "FAIL"
            print(f"[{status}] ({result.duration:.2f}s)")
            
            if result.details:
                print(f"  Details: {result.details}")
            print()
        
        return {r.name: r for r in self.results}
    
    def print_summary(self):
        """Print test summary"""
        print("=" * 60)
        print("Test Summary")
        print("=" * 60)
        
        total = len(self.results)
        passed = sum(1 for r in self.results if r.passed)
        failed = total - passed
        
        print(f"Total tests: {total}")
        print(f"Passed: {passed}")
        print(f"Failed: {failed}")
        print()
        
        if failed > 0:
            print("Failed tests:")
            for result in self.results:
                if not result.passed:
                    print(f"  - {result.name}")
            print()
        
        # Print detailed results
        print("Detailed Results:")
        print("-" * 60)
        for result in self.results:
            status = "PASS" if result.passed else "FAIL"
            print(f"{status:4} | {result.name:30} | {result.duration:.2f}s")
        
        # Save results to JSON
        results_file = Path("test_results.json")
        with open(results_file, 'w') as f:
            json.dump({
                "summary": {
                    "total": total,
                    "passed": passed,
                    "failed": failed
                },
                "results": [
                    {
                        "name": r.name,
                        "passed": r.passed,
                        "duration": r.duration,
                        "details": r.details
                    }
                    for r in self.results
                ]
            }, f, indent=2)
        
        print()
        print(f"Results saved to: {results_file}")


def main():
    runner = TestRunner()
    runner.run_all_tests()
    runner.print_summary()


if __name__ == "__main__":
    main()
