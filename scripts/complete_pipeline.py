#!/usr/bin/env python3
"""
Complete Test Pipeline for MySQL Recovery Pro
End-to-end validation with Docker, data injection, chaos testing, and validation
"""

import os
import sys
import json
import subprocess
import argparse
import hashlib
from datetime import datetime
from typing import List, Dict, Tuple, Optional
from pathlib import Path
import time


class CompleteTestPipeline:
    """Complete end-to-end test pipeline"""
    
    def __init__(self, config: Dict = None):
        self.results: List[Dict] = []
        self.start_time = None
        self.end_time = None
        
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
    
    def load_config(self) -> Dict:
        """Load test configuration"""
        config_file = self.scripts_dir / "test_config.json"
        if config_file.exists():
            with open(config_file) as f:
                return json.load(f)
        
        return {
            "mysql_versions": ["5.6", "5.7", "8.0", "8.4", "9.0"],
            "corruption_scenarios": [
                "random_3_pages", "random_5_pages", "random_10_pages",
                "all_headers", "btree_chain_broken", "metadata_lost",
                "file_fragments", "undo_log_corrupt"
            ],
            "record_counts": [1000, 10000, 100000],
            "page_sizes": [16384],
            "output_formats": ["csv", "sql", "json"],
            "docker_enabled": True,
            "timeout_seconds": 600
        }
    
    def log(self, message: str, level: str = "INFO"):
        """Log a message"""
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{timestamp}] [{level}] {message}")
    
    def run_command(self, cmd: List[str], timeout: int = None,
                   cwd: str = None, capture: bool = True) -> Tuple[bool, str, str]:
        """Run a command"""
        if timeout is None:
            timeout = self.config.get("timeout_seconds", 600)
        
        try:
            result = subprocess.run(
                cmd,
                capture_output=capture,
                text=capture,
                timeout=timeout,
                cwd=cwd or str(self.project_dir)
            )
            return result.returncode == 0, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return False, "", "Command timed out"
        except Exception as e:
            return False, "", str(e)
    
    def build_project(self) -> Tuple[bool, str]:
        """Build the project"""
        self.log("Building project...")
        
        build_script = self.scripts_dir / "build_linux.sh"
        if not build_script.exists():
            return False, "Build script not found"
        
        success, stdout, stderr = self.run_command([
            "bash", str(build_script)
        ])
        
        if success:
            self.log("Build successful")
        else:
            self.log(f"Build failed: {stderr}", "ERROR")
        
        return success, stdout + stderr
    
    def setup_docker(self) -> bool:
        """Setup Docker environment"""
        if not self.config.get("docker_enabled", True):
            self.log("Docker testing disabled")
            return True
        
        self.log("Setting up Docker environment...")
        
        docker_script = self.scripts_dir / "docker_test_env.sh"
        if not docker_script.exists():
            self.log("Docker script not found, skipping")
            return True
        
        success, stdout, stderr = self.run_command([
            "bash", str(docker_script), "setup"
        ])
        
        if success:
            self.log("Docker environment ready")
        else:
            self.log(f"Docker setup failed: {stderr}", "WARN")
        
        return success
    
    def teardown_docker(self):
        """Teardown Docker environment"""
        if not self.config.get("docker_enabled", True):
            return
        
        self.log("Teardown Docker environment...")
        
        docker_script = self.scripts_dir / "docker_test_env.sh"
        if docker_script.exists():
            self.run_command(["bash", str(docker_script), "teardown"])
    
    def generate_test_data(self, version: str, num_records: int = 100000,
                          include_vector: bool = False) -> Optional[Dict]:
        """Generate test data using data injector"""
        self.log(f"Generating {num_records} records for MySQL {version}...")
        
        data_injector = self.scripts_dir / "data_injector.py"
        if not data_injector.exists():
            self.log("Data injector not found, using fallback generator")
            return self.generate_fallback_test_data(version, num_records)
        
        # Generate hash file
        hash_file = self.results_dir / f"ground_truth_{version}_{num_records}.json"
        
        cmd = [
            "python3", str(data_injector),
            "--num-records", str(num_records),
            "--hash",
            "--output", str(hash_file)
        ]
        
        if include_vector:
            cmd.append("--vector")
        
        success, stdout, stderr = self.run_command(cmd)
        
        if success:
            self.log(f"Generated test data: {hash_file}")
            return {"hash_file": str(hash_file), "success": True}
        else:
            self.log(f"Data generation failed: {stderr}", "WARN")
            return None
    
    def generate_fallback_test_data(self, version: str, num_records: int = 100000) -> Optional[Dict]:
        """Generate test data using fallback generator"""
        self.log(f"Generating fallback test data for MySQL {version}...")
        
        generator = self.scripts_dir / "generate_test_data.py"
        if not generator.exists():
            return None
        
        output_file = self.test_data_dir / f"test_table_v{version.replace('.', '')}_ps16384_rec{num_records}.ibd"
        
        success, stdout, stderr = self.run_command([
            "python3", str(generator),
            "--output-dir", str(self.test_data_dir),
            "--mysql-version", version.replace(".", ""),
            "--num-records", str(num_records),
            "--output", str(output_file)
        ])
        
        if success and output_file.exists():
            # Calculate hash
            with open(output_file, 'rb') as f:
                file_hash = hashlib.sha256(f.read()).hexdigest()
            
            hash_file = self.results_dir / f"ground_truth_{version}_{num_records}.json"
            with open(hash_file, 'w') as f:
                json.dump({
                    "file": str(output_file),
                    "hash": file_hash,
                    "records": num_records,
                    "mysql_version": version
                }, f, indent=2)
            
            self.log(f"Generated: {output_file} (hash: {file_hash[:16]}...)")
            return {"file": str(output_file), "hash": file_hash, "success": True}
        
        return None
    
    def corrupt_data(self, input_file: str, scenario: str) -> Optional[Dict]:
        """Corrupt test data using chaos monkey"""
        self.log(f"Corrupting data with scenario: {scenario}...")
        
        chaos_monkey = self.scripts_dir / "chaos_monkey.py"
        if not chaos_monkey.exists():
            self.log("Chaos monkey not found", "ERROR")
            return None
        
        output_file = self.test_data_dir / f"test_corrupted_{scenario}_{Path(input_file).stem}.ibd"
        
        success, stdout, stderr = self.run_command([
            "python3", str(chaos_monkey),
            "--input", input_file,
            "--output", str(output_file),
            "--scenario", scenario,
            "--seed", "42"
        ])
        
        if success and output_file.exists():
            # Parse output
            input_hash = None
            output_hash = None
            for line in stdout.strip().split('\n'):
                if line.startswith("HASH_INPUT:"):
                    input_hash = line.split(":")[1]
                elif line.startswith("HASH_OUTPUT:"):
                    output_hash = line.split(":")[1]
            
            self.log(f"Corrupted: {output_file}")
            return {
                "input_file": input_file,
                "output_file": str(output_file),
                "input_hash": input_hash,
                "output_hash": output_hash,
                "success": True
            }
        
        self.log(f"Corruption failed: {stderr}", "ERROR")
        return None
    
    def run_recovery(self, ibd_file: str, output_dir: str,
                    format_type: str = "csv", **kwargs) -> Dict:
        """Run recovery on a test file"""
        binary = self.build_dir / "mysql_recovery_pro"
        if not binary.exists():
            self.log(f"Recovery binary not found: {binary}", "ERROR")
            return {"success": False, "error": "Binary not found"}
        
        output_path = Path(output_dir)
        output_path.mkdir(parents=True, exist_ok=True)
        
        cmd = [
            str(binary),
            f"--ibd={ibd_file}",
            f"--output={output_dir}",
            f"--format={format_type}"
        ]
        
        for key, value in kwargs.items():
            if value is True:
                cmd.append(f"--{key.replace('_', '-')}")
            elif value is not False and value is not None:
                cmd.append(f"--{key.replace('_', '-')}={value}")
        
        success, stdout, stderr = self.run_command(cmd)
        
        # Check output files
        result = {
            "success": success,
            "stdout": stdout[:500],
            "stderr": stderr[:500],
            "output_files": {}
        }
        
        for ext in ["csv", "json", "sql"]:
            file_path = output_path / f"recovered.{ext}"
            if file_path.exists():
                result["output_files"][ext] = str(file_path)
                result[f"{ext}_record_count"] = self.count_records(str(file_path), ext)
        
        return result
    
    def count_records(self, file_path: str, ext: str) -> int:
        """Count records in output file"""
        try:
            if ext == "csv":
                with open(file_path, 'r') as f:
                    return sum(1 for _ in f) - 1
            elif ext == "sql":
                with open(file_path, 'r') as f:
                    return f.read().count("INSERT INTO")
            elif ext == "json":
                with open(file_path, 'r') as f:
                    data = json.load(f)
                    return len(data) if isinstance(data, list) else 0
        except:
            pass
        return 0
    
    def validate_recovery(self, recovered_file: str, expected_hash: str = None) -> Dict:
        """Validate recovery results"""
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
        
        # Validate
        if expected_hash:
            result["hash_match"] = result["hash"] == expected_hash
            result["valid"] = result["hash_match"]
        else:
            result["valid"] = len(content) > 0
        
        return result
    
    def run_full_scenario(self, scenario: str, version: str,
                         num_records: int = 100000) -> Dict:
        """Run a complete scenario test"""
        self.log(f"\n{'='*60}")
        self.log(f"Scenario: {scenario} | MySQL {version} | {num_records} records")
        self.log('='*60)
        
        result = {
            "scenario": scenario,
            "mysql_version": version,
            "num_records": num_records,
            "timestamp": datetime.now().isoformat(),
            "steps": {}
        }
        
        # Step 1: Generate test data
        self.log("\n[1/4] Generating test data...")
        data_result = self.generate_test_data(version, num_records)
        
        if not data_result or not data_result.get("success"):
            result["steps"]["generate_data"] = {"success": False, "error": "Failed to generate data"}
            return result
        
        data_file = data_result.get("file") or data_result.get("hash_file", "")
        result["steps"]["generate_data"] = {
            "success": True,
            "file": data_file,
            "hash": data_result.get("hash", "N/A")
        }
        
        # Step 2: Corrupt data
        self.log("\n[2/4] Corrupting data...")
        corrupt_result = self.corrupt_data(data_file, scenario)
        
        if not corrupt_result or not corrupt_result.get("success"):
            result["steps"]["corrupt_data"] = {"success": False, "error": "Failed to corrupt data"}
            return result
        
        result["steps"]["corrupt_data"] = {
            "success": True,
            "corrupted_file": corrupt_result.get("output_file"),
            "input_hash": corrupt_result.get("input_hash"),
            "output_hash": corrupt_result.get("output_hash")
        }
        
        # Step 3: Run recovery
        self.log("\n[3/4] Running recovery...")
        output_dir = self.results_dir / f"{scenario}_{version}_{num_records}"
        recovery_result = self.run_recovery(
            corrupt_result.get("output_file"),
            str(output_dir),
            format_type="csv"
        )
        
        result["steps"]["recovery"] = recovery_result
        
        # Step 4: Validate
        self.log("\n[4/4] Validating results...")
        csv_file = output_dir / "recovered.csv"
        validate_result = self.validate_recovery(
            str(csv_file),
            expected_hash=corrupt_result.get("input_hash")
        )
        
        result["steps"]["validation"] = validate_result
        result["overall_passed"] = (
            result["steps"]["generate_data"]["success"] and
            result["steps"]["corrupt_data"]["success"] and
            recovery_result["success"] and
            validate_result.get("valid", False)
        )
        
        return result
    
    def run_version_test(self, version: str) -> Dict:
        """Run version compatibility test"""
        self.log(f"\n{'='*60}")
        self.log(f"Version Compatibility Test: MySQL {version}")
        self.log('='*60)
        
        result = {
            "test_type": "version_compatibility",
            "mysql_version": version,
            "timestamp": datetime.now().isoformat(),
            "scenarios": []
        }
        
        scenarios = ["basic_recovery", "truncate_recovery", "rmrf_recovery"]
        
        for scenario in scenarios:
            scenario_result = self.run_full_scenario(scenario, version, 1000)
            result["scenarios"].append(scenario_result)
        
        result["passed"] = all(s.get("overall_passed", False) for s in result["scenarios"])
        return result
    
    def run_corruption_test(self, scenario: str) -> Dict:
        """Run corruption scenario test"""
        self.log(f"\n{'='*60}")
        self.log(f"Corruption Scenario Test: {scenario}")
        self.log('='*60)
        
        result = {
            "test_type": "corruption_scenario",
            "scenario": scenario,
            "timestamp": datetime.now().isoformat(),
            "versions": []
        }
        
        versions = ["5.6", "5.7", "8.0", "8.4", "9.0"]
        
        for version in versions:
            version_result = self.run_full_scenario(scenario, version, 10000)
            result["versions"].append(version_result)
        
        result["passed"] = all(v.get("overall_passed", False) for v in result["versions"])
        return result
    
    def run_full_pipeline(self) -> Dict:
        """Run complete test pipeline"""
        self.start_time = datetime.now()
        
        self.log("="*60)
        self.log("MySQL Recovery Pro - Complete Test Pipeline")
        self.log("="*60)
        self.log(f"Started: {self.start_time.isoformat()}")
        
        # Build project
        self.log("\n" + "="*60)
        self.log("BUILD PHASE")
        self.log("="*60)
        build_success, build_output = self.build_project()
        
        if not build_success:
            self.log("Build failed, stopping pipeline", "ERROR")
            return {"error": "Build failed", "details": build_output}
        
        # Setup Docker
        self.log("\n" + "="*60)
        self.log("ENVIRONMENT SETUP")
        self.log("="*60)
        self.setup_docker()
        
        # Run tests
        self.log("\n" + "="*60)
        self.log("TEST EXECUTION")
        self.log("="*60)
        
        # Version compatibility tests
        versions = ["5.6", "5.7", "8.0", "8.4", "9.0"]
        for version in versions:
            result = self.run_version_test(version)
            self.results.append(result)
        
        # Corruption scenario tests
        scenarios = ["random_3_pages", "random_5_pages", "all_headers", "btree_chain_broken"]
        for scenario in scenarios:
            result = self.run_corruption_test(scenario)
            self.results.append(result)
        
        # Teardown
        self.teardown_docker()
        
        self.end_time = datetime.now()
        
        # Generate report
        report = self.generate_report()
        
        return report
    
    def generate_report(self) -> Dict:
        """Generate comprehensive test report"""
        report = {
            "timestamp": datetime.now().isoformat(),
            "start_time": self.start_time.isoformat() if self.start_time else None,
            "end_time": self.end_time.isoformat() if self.end_time else None,
            "duration_seconds": None,
            "summary": {
                "total_tests": len(self.results),
                "passed": 0,
                "failed": 0,
                "pass_rate": 0.0
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
        
        total = report["summary"]["total_tests"]
        report["summary"]["pass_rate"] = (
            report["summary"]["passed"] / max(1, total) * 100
        )
        
        # Save JSON report
        json_report = self.results_dir / "validation_report.json"
        with open(json_report, 'w') as f:
            json.dump(report, f, indent=2)
        
        # Save Markdown report
        md_report = self.results_dir / "validation_report.md"
        with open(md_report, 'w') as f:
            f.write(self.generate_markdown_report(report))
        
        self.log(f"\nReports saved:")
        self.log(f"  JSON: {json_report}")
        self.log(f"  Markdown: {md_report}")
        
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
            f"| Pass Rate | {report['summary']['pass_rate']:.1f}% |",
            "",
            "## Test Results",
            ""
        ]
        
        for i, result in enumerate(report['results'], 1):
            status = "✅ PASS" if result.get("overall_passed", False) or result.get("passed", False) else "❌ FAIL"
            test_type = result.get("test_type", "N/A")
            test_name = result.get("scenario", result.get("mysql_version", "N/A"))
            
            lines.append(f"### Test {i}: {test_name} ({test_type})")
            lines.append("")
            lines.append(f"- **Status:** {status}")
            lines.append(f"- **Timestamp:** {result.get('timestamp', 'N/A')}")
            
            if "steps" in result:
                for step_name, step_result in result["steps"].items():
                    step_status = "✅" if step_result.get("success", False) else "❌"
                    lines.append(f"- **{step_name}:** {step_status}")
            
            lines.append("")
        
        return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Complete Test Pipeline for MySQL Recovery Pro'
    )
    parser.add_argument('--config', '-c',
                       help='Configuration file')
    parser.add_argument('--scenario', '-s',
                       choices=['full', 'version', 'corruption', 'build'],
                       default='full',
                       help='Test scenario')
    parser.add_argument('--version', '-v',
                       help='MySQL version')
    parser.add_argument('--corruption', '-C',
                       help='Corruption scenario')
    parser.add_argument('--build-only', action='store_true',
                       help='Only build the project')
    parser.add_argument('--output', '-o',
                       help='Output directory')
    
    args = parser.parse_args()
    
    # Load configuration
    config = {}
    if args.config:
        with open(args.config) as f:
            config = json.load(f)
    
    # Create pipeline
    pipeline = CompleteTestPipeline(config)
    
    if args.output:
        pipeline.results_dir = Path(args.output)
        pipeline.results_dir.mkdir(parents=True, exist_ok=True)
    
    if args.build_only:
        success, output = pipeline.build_project()
        if success:
            print("Build successful!")
        else:
            print(f"Build failed: {output}")
            sys.exit(1)
    
    elif args.scenario == 'full':
        report = pipeline.run_full_pipeline()
        
        print("\n" + "="*60)
        print("Pipeline Complete!")
        print("="*60)
        print(f"Total tests: {report['summary']['total_tests']}")
        print(f"Passed: {report['summary']['passed']}")
        print(f"Failed: {report['summary']['failed']}")
        print(f"Pass rate: {report['summary']['pass_rate']:.1f}%")
        print(f"Duration: {report.get('duration_seconds', 'N/A')} seconds")
        
        if report['summary']['failed'] > 0:
            sys.exit(1)
    
    elif args.scenario == 'version':
        version = args.version or "8.0"
        result = pipeline.run_version_test(version)
        
        print("\n" + "="*60)
        print("Version Test Complete!")
        print("="*60)
        print(f"Version: {version}")
        print(f"Passed: {result['passed']}")
    
    elif args.scenario == 'corruption':
        scenario = args.corruption or "random_3_pages"
        result = pipeline.run_corruption_test(scenario)
        
        print("\n" + "="*60)
        print("Corruption Test Complete!")
        print("="*60)
        print(f"Scenario: {scenario}")
        print(f"Passed: {result['passed']}")
        
        if not result['passed']:
            sys.exit(1)
    
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
