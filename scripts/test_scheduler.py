#!/usr/bin/env python3
"""
Main Test Scheduler and Report Generator for MySQL Recovery Pro
Orchestrates the complete validation pipeline
"""

import os
import sys
import json
import subprocess
import argparse
import shutil
from datetime import datetime
from typing import List, Dict, Tuple, Optional
from pathlib import Path
import threading
import time
import signal


class TestScheduler:
    """Main test scheduler orchestrating the validation pipeline"""
    
    def __init__(self, config: Dict = None):
        self.results: List[Dict] = []
        self.start_time = None
        self.end_time = None
        self.interrupted = False
        
        # Setup paths first
        self.project_dir = Path(__file__).parent.parent
        self.scripts_dir = self.project_dir / "scripts"
        self.test_data_dir = self.project_dir / "test_data"
        self.results_dir = self.project_dir / "test_results"
        self.build_dir = self.project_dir / "build"
        
        # Ensure directories exist
        self.results_dir.mkdir(parents=True, exist_ok=True)
        self.test_data_dir.mkdir(parents=True, exist_ok=True)
        
        # Load config after paths are set
        self.config = config or self.load_config()
        
        # Initialize components
        self.test_components = {}
    
    def load_config(self) -> Dict:
        """Load test configuration"""
        config_file = self.scripts_dir / "test_config.json"
        if config_file.exists():
            with open(config_file) as f:
                return json.load(f)
        
        # Default configuration
        return {
            "mysql_versions": ["5.6", "5.7", "8.0", "8.4", "9.0"],
            "corruption_scenarios": [
                "random_3_pages",
                "random_5_pages",
                "all_headers",
                "btree_chain_broken",
                "metadata_lost",
                "file_fragments"
            ],
            "record_counts": [1000, 10000, 100000],
            "page_sizes": [4096, 8192, 16384, 32768],
            "output_formats": ["csv", "sql", "json"],
            "docker_enabled": True,
            "parallel_tests": 4,
            "timeout_seconds": 300
        }
    
    def run_command(self, cmd: List[str], timeout: int = None, 
                   cwd: str = None) -> Tuple[bool, str, str]:
        """Run a command with timeout"""
        if timeout is None:
            timeout = self.config.get("timeout_seconds", 300)
        
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout,
                cwd=cwd or str(self.project_dir)
            )
            return result.returncode == 0, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return False, "", "Command timed out"
        except Exception as e:
            return False, "", str(e)
    
    def setup_docker_environment(self) -> bool:
        """Setup Docker test environment"""
        if not self.config.get("docker_enabled", True):
            print("Docker testing disabled")
            return True
        
        print("Setting up Docker environment...")
        
        # Run Docker setup script
        docker_script = self.scripts_dir / "docker_test_env.sh"
        if not docker_script.exists():
            print("Docker script not found, skipping Docker setup")
            return True
        
        success, stdout, stderr = self.run_command([
            "bash", str(docker_script), "setup"
        ])
        
        if success:
            print("Docker environment setup complete")
        else:
            print(f"Docker setup failed: {stderr}")
        
        return success
    
    def teardown_docker_environment(self):
        """Teardown Docker test environment"""
        if not self.config.get("docker_enabled", True):
            return
        
        print("Teardown Docker environment...")
        docker_script = self.scripts_dir / "docker_test_env.sh"
        
        if docker_script.exists():
            self.run_command(["bash", str(docker_script), "teardown"])
    
    def generate_test_data(self, version: str, num_records: int = 100000) -> bool:
        """Generate test data for a specific MySQL version"""
        print(f"Generating test data for MySQL {version} ({num_records} records)...")
        
        data_generator = self.scripts_dir / "generate_test_data.py"
        if not data_generator.exists():
            print("Data generator not found")
            return False
        
        output_file = self.test_data_dir / f"test_table_v{version.replace('.', '')}_ps16384_rec{num_records}.ibd"
        
        success, stdout, stderr = self.run_command([
            "python3", str(data_generator),
            "--output-dir", str(self.test_data_dir),
            "--mysql-version", version.replace(".", ""),
            "--num-records", str(num_records),
            "--output", str(output_file)
        ])
        
        if success and output_file.exists():
            print(f"Generated: {output_file}")
            return True
        else:
            print(f"Failed to generate test data: {stderr}")
            return False
    
    def corrupt_test_data(self, input_file: str, scenario: str) -> Tuple[bool, Dict]:
        """Corrupt test data using chaos monkey"""
        print(f"Corrupting data with scenario: {scenario}...")
        
        chaos_monkey = self.scripts_dir / "chaos_monkey.py"
        if not chaos_monkey.exists():
            print("Chaos monkey not found")
            return False, {}
        
        output_file = self.test_data_dir / f"test_corrupted_{scenario}_{Path(input_file).stem}.ibd"
        
        success, stdout, stderr = self.run_command([
            "python3", str(chaos_monkey),
            "--input", input_file,
            "--output", str(output_file),
            "--scenario", scenario,
            "--seed", "42"  # Fixed seed for reproducibility
        ])
        
        if success and output_file.exists():
            # Parse output for hash information
            output_lines = stdout.strip().split('\n')
            result = {"output_file": str(output_file)}
            for line in output_lines:
                if line.startswith("HASH_INPUT:"):
                    result["input_hash"] = line.split(":")[1]
                elif line.startswith("HASH_OUTPUT:"):
                    result["output_hash"] = line.split(":")[1]
            
            print(f"Corrupted: {output_file}")
            return True, result
        else:
            print(f"Failed to corrupt data: {stderr}")
            return False, {}
    
    def run_recovery(self, ibd_file: str, output_dir: str, 
                    format_type: str = "csv", **kwargs) -> Tuple[bool, Dict]:
        """Run recovery on a test file"""
        binary = self.build_dir / "mysql_recovery_pro"
        if not binary.exists():
            print(f"Recovery binary not found: {binary}")
            return False, {}
        
        output_path = Path(output_dir)
        output_path.mkdir(parents=True, exist_ok=True)
        
        cmd = [
            str(binary),
            f"--ibd={ibd_file}",
            f"--output={output_dir}",
            f"--format={format_type}"
        ]
        
        # Add additional arguments
        for key, value in kwargs.items():
            if value is True:
                cmd.append(f"--{key.replace('_', '-')}")
            elif value is not False and value is not None:
                cmd.append(f"--{key.replace('_', '-')}={value}")
        
        success, stdout, stderr = self.run_command(cmd)
        
        # Check output file
        csv_file = output_path / "recovered.csv"
        json_file = output_path / "recovered.json"
        sql_file = output_path / "recovered.sql"
        
        result = {
            "success": success,
            "stdout": stdout[:500],
            "stderr": stderr[:500],
            "output_files": {}
        }
        
        if csv_file.exists():
            result["output_files"]["csv"] = str(csv_file)
            result["csv_record_count"] = self.count_csv_records(str(csv_file))
        if json_file.exists():
            result["output_files"]["json"] = str(json_file)
        if sql_file.exists():
            result["output_files"]["sql"] = str(sql_file)
        
        return success, result
    
    def count_csv_records(self, csv_file: str) -> int:
        """Count records in CSV file"""
        try:
            with open(csv_file, 'r') as f:
                return sum(1 for _ in f) - 1  # Exclude header
        except:
            return 0
    
    def validate_recovery(self, recovered_file: str, 
                         expected_hash: str = None) -> Dict:
        """Validate recovery results"""
        import hashlib
        
        result = {
            "file": recovered_file,
            "exists": os.path.exists(recovered_file),
            "hash": None,
            "valid": False
        }
        
        if not result["exists"]:
            return result
        
        # Calculate hash
        with open(recovered_file, 'rb') as f:
            content = f.read()
            result["hash"] = hashlib.sha256(content).hexdigest()
        
        # Validate against expected hash
        if expected_hash:
            result["hash_match"] = result["hash"] == expected_hash
            result["valid"] = result["hash_match"]
        else:
            # Basic validation: file exists and has content
            result["valid"] = len(content) > 0
        
        return result
    
    def run_corruption_scenario(self, scenario: str, version: str, 
                                 num_records: int) -> Dict:
        """Run a complete corruption scenario test"""
        print(f"\n{'='*60}")
        print(f"Running scenario: {scenario} (MySQL {version}, {num_records} records)")
        print('='*60)
        
        result = {
            "scenario": scenario,
            "mysql_version": version,
            "num_records": num_records,
            "timestamp": datetime.now().isoformat(),
            "steps": {}
        }
        
        # Step 1: Generate test data
        print("\n[1/4] Generating test data...")
        data_file = self.test_data_dir / f"test_table_v{version.replace('.', '')}_ps16384_rec{num_records}.ibd"
        
        if not data_file.exists():
            if not self.generate_test_data(version, num_records):
                result["steps"]["generate_data"] = {"success": False}
                return result
        
        result["steps"]["generate_data"] = {"success": True, "file": str(data_file)}
        
        # Step 2: Corrupt data
        print("\n[2/4] Corrupting data...")
        success, corruption_result = self.corrupt_test_data(str(data_file), scenario)
        if not success:
            result["steps"]["corrupt_data"] = {"success": False}
            return result
        
        result["steps"]["corrupt_data"] = {
            "success": True,
            "corrupted_file": corruption_result.get("output_file"),
            "input_hash": corruption_result.get("input_hash"),
            "output_hash": corruption_result.get("output_hash")
        }
        
        # Step 3: Run recovery
        print("\n[3/4] Running recovery...")
        output_dir = self.results_dir / f"{scenario}_{version}_{num_records}"
        success, recovery_result = self.run_recovery(
            corruption_result.get("output_file"),
            str(output_dir),
            format_type="csv"
        )
        
        result["steps"]["recovery"] = {
            "success": success,
            **recovery_result
        }
        
        # Step 4: Validate results
        print("\n[4/4] Validating results...")
        csv_file = output_dir / "recovered.csv"
        validation_result = self.validate_recovery(
            str(csv_file),
            expected_hash=corruption_result.get("input_hash")
        )
        
        result["steps"]["validation"] = validation_result
        result["overall_passed"] = (
            result["steps"]["generate_data"]["success"] and
            result["steps"]["corrupt_data"]["success"] and
            result["steps"]["recovery"]["success"] and
            validation_result.get("valid", False)
        )
        
        return result
    
    def run_version_compatibility_test(self, version: str) -> Dict:
        """Run version compatibility test"""
        print(f"\n{'='*60}")
        print(f"Version Compatibility Test: MySQL {version}")
        print('='*60)
        
        result = {
            "test_type": "version_compatibility",
            "mysql_version": version,
            "timestamp": datetime.now().isoformat(),
            "scenarios_passed": 0,
            "scenarios_total": 0
        }
        
        # Test with different scenarios
        scenarios = ["basic_recovery", "truncate_recovery", "rmrf_recovery"]
        
        for scenario in scenarios:
            scenario_result = self.run_corruption_scenario(
                scenario, version, 1000
            )
            result["scenarios_total"] += 1
            if scenario_result.get("overall_passed", False):
                result["scenarios_passed"] += 1
            
            self.results.append(scenario_result)
        
        result["passed"] = result["scenarios_passed"] == result["scenarios_total"]
        return result
    
    def run_full_validation(self) -> Dict:
        """Run complete validation pipeline"""
        self.start_time = datetime.now()
        
        print("="*60)
        print("MySQL Recovery Pro - Full Validation Pipeline")
        print("="*60)
        print(f"Started: {self.start_time.isoformat()}")
        print()
        
        # Setup Docker environment
        self.setup_docker_environment()
        
        # Build project
        print("\n" + "="*60)
        print("Building Project")
        print("="*60)
        build_success, build_stdout, build_stderr = self.run_command([
            "bash", str(self.scripts_dir / "build_linux.sh")
        ])
        
        if not build_success:
            print(f"Build failed: {build_stderr}")
            return {"error": "Build failed", "details": build_stderr}
        
        print("Build successful")
        
        # Run version compatibility tests
        versions = self.config.get("mysql_versions", ["5.6", "5.7", "8.0", "8.4", "9.0"])
        
        for version in versions:
            result = self.run_version_compatibility_test(version)
            self.results.append(result)
        
        # Run corruption scenario tests
        scenarios = self.config.get("corruption_scenarios", [
            "random_3_pages", "random_5_pages", "all_headers"
        ])
        
        for scenario in scenarios:
            result = self.run_corruption_scenario(scenario, "8.0", 10000)
            self.results.append(result)
        
        # Teardown Docker environment
        self.teardown_docker_environment()
        
        self.end_time = datetime.now()
        
        # Generate report
        report = self.generate_report()
        
        return report
    
    def generate_report(self) -> Dict:
        """Generate test report"""
        report = {
            "timestamp": datetime.now().isoformat(),
            "start_time": self.start_time.isoformat() if self.start_time else None,
            "end_time": self.end_time.isoformat() if self.end_time else None,
            "duration_seconds": None,
            "summary": {
                "total_tests": len(self.results),
                "passed": 0,
                "failed": 0
            },
            "results": self.results
        }
        
        if self.start_time and self.end_time:
            report["duration_seconds"] = (
                self.end_time - self.start_time
            ).total_seconds()
        
        # Calculate summary
        for result in self.results:
            if result.get("overall_passed", False) or result.get("passed", False):
                report["summary"]["passed"] += 1
            else:
                report["summary"]["failed"] += 1
        
        # Save report
        report_file = self.results_dir / "validation_report.json"
        with open(report_file, 'w') as f:
            json.dump(report, f, indent=2)
        
        # Generate markdown summary
        markdown_file = self.results_dir / "validation_report.md"
        with open(markdown_file, 'w') as f:
            f.write(self.generate_markdown_report(report))
        
        print(f"\nReport saved to: {report_file}")
        print(f"Markdown report saved to: {markdown_file}")
        
        return report
    
    def generate_markdown_report(self, report: Dict) -> str:
        """Generate markdown report"""
        lines = [
            "# MySQL Recovery Pro - Validation Report",
            "",
            f"**Generated:** {report['timestamp']}",
            f"**Duration:** {report.get('duration_seconds', 'N/A')} seconds",
            "",
            "## Summary",
            "",
            f"| Metric | Value |",
            f"|--------|-------|",
            f"| Total Tests | {report['summary']['total_tests']} |",
            f"| Passed | {report['summary']['passed']} |",
            f"| Failed | {report['summary']['failed']} |",
            f"| Pass Rate | {report['summary']['passed']/max(1, report['summary']['total_tests'])*100:.1f}% |",
            "",
            "## Test Results",
            ""
        ]
        
        for i, result in enumerate(report['results'], 1):
            status = "✅ PASS" if result.get("overall_passed", False) else "❌ FAIL"
            lines.append(f"### Test {i}: {result.get('scenario', 'N/A')}")
            lines.append("")
            lines.append(f"- **Status:** {status}")
            lines.append(f"- **MySQL Version:** {result.get('mysql_version', 'N/A')}")
            lines.append(f"- **Records:** {result.get('num_records', 'N/A')}")
            lines.append(f"- **Timestamp:** {result.get('timestamp', 'N/A')}")
            lines.append("")
        
        return "\n".join(lines)


class ParallelTestRunner:
    """Run tests in parallel"""
    
    def __init__(self, max_workers: int = 4):
        self.max_workers = max_workers
        self.results: List[Dict] = []
        self.lock = threading.Lock()
    
    def run_test_worker(self, test_func, *args):
        """Worker function to run a test"""
        try:
            result = test_func(*args)
            with self.lock:
                self.results.append(result)
            return result
        except Exception as e:
            with self.lock:
                self.results.append({"error": str(e)})
            return {"error": str(e)}
    
    def run_tests_parallel(self, test_func, test_args_list: List[Tuple]):
        """Run multiple tests in parallel"""
        threads = []
        
        for args in test_args_list:
            while threading.active_count() > self.max_workers + 1:
                time.sleep(0.1)
            
            thread = threading.Thread(
                target=self.run_test_worker,
                args=(test_func,) + args
            )
            thread.start()
            threads.append(thread)
        
        # Wait for all threads
        for thread in threads:
            thread.join()


def signal_handler(signum, frame):
    """Handle Ctrl+C gracefully"""
    print("\n\nTest interrupted by user")
    sys.exit(1)


def main():
    # Set up signal handler
    signal.signal(signal.SIGINT, signal_handler)
    
    parser = argparse.ArgumentParser(
        description='MySQL Recovery Pro - Test Scheduler'
    )
    parser.add_argument('--config', '-c',
                       help='Configuration file')
    parser.add_argument('--scenario', '-s',
                       choices=['full', 'version', 'corruption', 'build'],
                       default='full',
                       help='Test scenario')
    parser.add_argument('--version', '-v',
                       help='MySQL version for version test')
    parser.add_argument('--corruption', '-C',
                       help='Corruption scenario')
    parser.add_argument('--build-only', action='store_true',
                       help='Only build the project')
    parser.add_argument('--parallel', '-p', type=int, default=4,
                       help='Number of parallel tests')
    parser.add_argument('--output', '-o',
                       help='Output directory')
    
    args = parser.parse_args()
    
    # Load configuration
    config = {}
    if args.config:
        with open(args.config) as f:
            config = json.load(f)
    
    # Create scheduler
    scheduler = TestScheduler(config)
    
    if args.output:
        scheduler.results_dir = Path(args.output)
        scheduler.results_dir.mkdir(parents=True, exist_ok=True)
    
    if args.build_only:
        # Only build
        print("Building project...")
        success, stdout, stderr = scheduler.run_command([
            "bash", str(scheduler.scripts_dir / "build_linux.sh")
        ])
        
        if success:
            print("Build successful!")
        else:
            print(f"Build failed: {stderr}")
            sys.exit(1)
    
    elif args.scenario == 'full':
        # Run full validation
        report = scheduler.run_full_validation()
        
        print("\n" + "="*60)
        print("Validation Complete!")
        print("="*60)
        print(f"Total tests: {report['summary']['total_tests']}")
        print(f"Passed: {report['summary']['passed']}")
        print(f"Failed: {report['summary']['failed']}")
        print(f"Duration: {report.get('duration_seconds', 'N/A')} seconds")
        
        if report['summary']['failed'] > 0:
            sys.exit(1)
    
    elif args.scenario == 'version':
        # Run version compatibility test
        version = args.version or "8.0"
        result = scheduler.run_version_compatibility_test(version)
        
        print("\n" + "="*60)
        print("Version Compatibility Test Complete!")
        print("="*60)
        print(f"Version: {version}")
        print(f"Passed: {result['scenarios_passed']}/{result['scenarios_total']}")
    
    elif args.scenario == 'corruption':
        # Run corruption scenario test
        scenario = args.corruption or "random_3_pages"
        version = args.version or "8.0"
        result = scheduler.run_corruption_scenario(scenario, version, 10000)
        
        print("\n" + "="*60)
        print("Corruption Scenario Test Complete!")
        print("="*60)
        print(f"Scenario: {scenario}")
        print(f"Status: {'PASS' if result.get('overall_passed', False) else 'FAIL'}")
        
        if not result.get('overall_passed', False):
            sys.exit(1)
    
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
