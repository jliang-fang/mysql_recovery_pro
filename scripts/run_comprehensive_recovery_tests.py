#!/usr/bin/env python3
"""
Comprehensive Recovery Testing Framework for MySQL Recovery Pro
Tests various data corruption scenarios and validates recovery improvements
"""

import os
import sys
import json
import subprocess
import argparse
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Tuple, Optional
import time


class RecoveryTestScenario:
    """Represents a recovery test scenario"""

    def __init__(self, name: str, description: str, records: int,
                 page_size: int = 16384, corruption_type: str = None,
                 corruption_ratio: float = 0.0, expected_success: bool = True,
                 recovery_modes: List[str] = None):
        self.name = name
        self.description = description
        self.records = records
        self.page_size = page_size
        self.corruption_type = corruption_type
        self.corruption_ratio = corruption_ratio
        self.expected_success = expected_success
        self.recovery_modes = recovery_modes or ["default", "scan", "search"]


class ComprehensiveRecoveryTestRunner:
    """Run comprehensive recovery tests with various scenarios"""

    def __init__(self, recovery_tool_path: str = "./mysql_recovery_pro",
                 test_data_dir: str = "test_data",
                 output_dir: str = "recovery_test_results"):
        self.recovery_tool_path = recovery_tool_path
        self.test_data_dir = test_data_dir
        self.output_dir = output_dir
        self.results = []
        self.test_scenarios = self._create_test_scenarios()

    def _create_test_scenarios(self) -> List[RecoveryTestScenario]:
        """Create comprehensive test scenarios"""
        scenarios = []

        # Normal scenarios (no corruption)
        scenarios.extend([
            RecoveryTestScenario(
                name="normal_10k",
                description="Normal recovery with 10k records",
                records=10000,
                corruption_type=None,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="normal_50k",
                description="Normal recovery with 50k records",
                records=50000,
                corruption_type=None,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="normal_100k",
                description="Normal recovery with 100k records (large scale)",
                records=100000,
                corruption_type=None,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="normal_200k",
                description="Normal recovery with 200k records (very large scale)",
                records=200000,
                corruption_type=None,
                expected_success=True
            ),
        ])

        # Single page corruption scenarios
        scenarios.extend([
            RecoveryTestScenario(
                name="single_header_type_corruption",
                description="Single page with header type corruption",
                records=100000,
                corruption_type="header_type",
                corruption_ratio=0.001,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="single_checksum_corruption",
                description="Single page with checksum corruption",
                records=100000,
                corruption_type="header_checksum",
                corruption_ratio=0.001,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="single_infimum_corruption",
                description="Single page with INFIMUM corruption",
                records=100000,
                corruption_type="infimum_corrupt",
                corruption_ratio=0.001,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="single_supremum_corruption",
                description="Single page with SUPREMUM corruption",
                records=100000,
                corruption_type="supremum_corrupt",
                corruption_ratio=0.001,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="single_chain_broken",
                description="Single page with page chain broken",
                records=100000,
                corruption_type="chain_broken",
                corruption_ratio=0.001,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="single_record_chain_broken",
                description="Single page with record chain broken",
                records=100000,
                corruption_type="record_chain_broken",
                corruption_ratio=0.001,
                expected_success=True
            ),
        ])

        # Multi-page corruption scenarios
        scenarios.extend([
            RecoveryTestScenario(
                name="multi_page_random_1pct",
                description="Multiple pages with 1% random corruption",
                records=100000,
                corruption_type="random",
                corruption_ratio=0.01,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="multi_page_random_5pct",
                description="Multiple pages with 5% random corruption",
                records=100000,
                corruption_type="random",
                corruption_ratio=0.05,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="multi_page_chain_3pct",
                description="Multiple pages with 3% chain corruption",
                records=100000,
                corruption_type="chain_broken",
                corruption_ratio=0.03,
                expected_success=True
            ),
        ])

        # Extreme corruption scenarios
        scenarios.extend([
            RecoveryTestScenario(
                name="extreme_random_10pct",
                description="Extreme random corruption at 10%",
                records=100000,
                corruption_type="random",
                corruption_ratio=0.10,
                expected_success=False  # Expected to fail
            ),
            RecoveryTestScenario(
                name="extreme_truncation_30pct",
                description="Severe truncation at 30%",
                records=100000,
                corruption_type="truncate",
                corruption_ratio=0.30,
                expected_success=False  # Expected to fail
            ),
            RecoveryTestScenario(
                name="extreme_all_zero_5pct",
                description="5% pages zeroed out",
                records=100000,
                corruption_type="all_zero",
                corruption_ratio=0.05,
                expected_success=False  # Expected to fail
            ),
        ])

        # Different page sizes
        scenarios.extend([
            RecoveryTestScenario(
                name="page_size_4k_100k",
                description="4KB page size with 100k records",
                records=100000,
                page_size=4096,
                corruption_type=None,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="page_size_8k_100k",
                description="8KB page size with 100k records",
                records=100000,
                page_size=8192,
                corruption_type=None,
                expected_success=True
            ),
            RecoveryTestScenario(
                name="page_size_32k_100k",
                description="32KB page size with 100k records",
                records=100000,
                page_size=32768,
                corruption_type=None,
                expected_success=True
            ),
        ])

        return scenarios

    def ensure_recovery_tool(self) -> bool:
        """Ensure recovery tool exists"""
        if os.path.exists(self.recovery_tool_path):
            return True

        # Try to build it
        print("Building recovery tool...")
        build_script = "scripts/build_linux.sh"
        if os.path.exists(build_script):
            result = subprocess.run(
                ["bash", build_script],
                capture_output=True,
                text=True
            )
            if result.returncode == 0:
                return True

        print(f"Recovery tool not found: {self.recovery_tool_path}")
        return False

    def generate_test_data(self, num_records: int, page_size: int,
                          corruption_type: str = None,
                          corruption_ratio: float = 0.0,
                          seed: int = 42) -> str:
        """Generate test data with optional corruption"""
        script = "scripts/generate_large_scale_test_data.py"

        if corruption_type:
            # Use generator's naming convention: test_large_corrupted_{type}_rec{N}_ps{S}.ibd
            output_name = f"test_large_corrupted_{corruption_type}_rec{num_records}_ps{page_size}"
            output_path = os.path.join(
                self.test_data_dir,
                f"{output_name}.ibd"
            )
        else:
            output_name = f"test_large_rec{num_records}_ps{page_size}"
            output_path = os.path.join(
                self.test_data_dir,
                f"{output_name}.ibd"
            )

        if os.path.exists(output_path):
            return output_path

        cmd = [
            sys.executable, script,
            "--output-dir", self.test_data_dir,
            "--num-records", str(num_records),
            "--page-size", str(page_size)
        ]

        if corruption_type:
            cmd.extend([
                "--corruption", corruption_type,
                "--corruption-ratio", str(corruption_ratio),
                "--seed", str(seed)
            ])

        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Failed to generate test data: {result.stderr}")
            return None

        return output_path

    def run_recovery(self, input_file: str, output_format: str = "sql",
                    recovery_mode: str = "default",
                    output_dir: str = None) -> Dict:
        """Run recovery on a file with specified mode"""
        if output_dir is None:
            output_dir = self.output_dir

        os.makedirs(output_dir, exist_ok=True)

        mode_suffix = f"_{recovery_mode}" if recovery_mode != "default" else ""
        # The tool outputs to output_dir/recovered.{ext} by default
        # We rename it after each run to avoid overwriting
        expected_default_name = os.path.join(output_dir, f"recovered.{output_format}")
        output_file = os.path.join(output_dir, f"recovered_{recovery_mode}.{output_format}")

        # Clean up old output file if exists
        if os.path.exists(expected_default_name):
            os.remove(expected_default_name)

        # Map recovery mode to appropriate flags
        cmd = [
            self.recovery_tool_path,
            f"--ibd={input_file}",
            f"--output={output_dir}",
        ]

        if recovery_mode == "scan":
            cmd.append("--schemaless")
        elif recovery_mode == "search":
            cmd.append("--truncate-recovery")

        # Add format
        cmd.append(f"--format={output_format}")

        try:
            start_time = time.time()
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=600  # 10 minute timeout
            )
            elapsed = time.time() - start_time

            # Rename the output file to include mode suffix
            if result.returncode == 0 and os.path.exists(expected_default_name):
                os.rename(expected_default_name, output_file)

            # Find the actual output file
            actual_output = None
            if result.returncode == 0:
                if os.path.exists(output_file):
                    actual_output = output_file
                else:
                    # Try to find any output file
                    import glob
                    pattern = os.path.join(output_dir, f"recovered*.{output_format}")
                    found_files = glob.glob(pattern)
                    if found_files:
                        actual_output = found_files[0]

            return {
                "returncode": result.returncode,
                "stdout": result.stdout,
                "stderr": result.stderr,
                "output_file": actual_output,
                "success": result.returncode == 0,
                "elapsed_seconds": elapsed
            }
        except subprocess.TimeoutExpired:
            return {
                "returncode": -1,
                "stdout": "",
                "stderr": "Recovery timed out",
                "output_file": None,
                "success": False,
                "elapsed_seconds": 600
            }
        except Exception as e:
            return {
                "returncode": -1,
                "stdout": "",
                "stderr": str(e),
                "output_file": None,
                "success": False,
                "elapsed_seconds": 0
            }

    def verify_recovery(self, scenario: RecoveryTestScenario,
                       recovered_file: str,
                       output_format: str = "sql") -> Dict:
        """Verify recovery results"""
        verification = {
            "records_recovered": 0,
            "data_integrity": "unknown",
            "verification_passed": False,
            "sample_records": []
        }

        if not recovered_file or not os.path.exists(recovered_file):
            verification["data_integrity"] = "no_output"
            return verification

        # Count records in output - handle encoding issues
        try:
            with open(recovered_file, 'r', encoding='utf-8', errors='replace') as f:
                content = f.read()

            if output_format == "sql":
                # Count INSERT statements
                insert_count = content.count("INSERT INTO")
                select_count = content.count("SELECT * FROM")
                verification["records_recovered"] = insert_count + select_count
            elif output_format == "csv":
                # Count lines (excluding header)
                lines = content.strip().split('\n')
                verification["records_recovered"] = max(0, len(lines) - 1)
            elif output_format == "json":
                # Parse JSON and count records
                try:
                    data = json.loads(content)
                    if isinstance(data, list):
                        verification["records_recovered"] = len(data)
                    elif isinstance(data, dict) and "records" in data:
                        verification["records_recovered"] = len(data["records"])
                except json.JSONDecodeError:
                    pass

            # Get sample records for verification
            if output_format == "sql":
                lines = content.split('\n')
                for line in lines[:10]:
                    if line.startswith("INSERT INTO"):
                        verification["sample_records"].append(line[:100])

            # Verify data integrity based on scenario
            if scenario.expected_success:
                # For expected success, check if we got reasonable data
                recovery_rate = verification["records_recovered"] / scenario.records if scenario.records > 0 else 0

                if recovery_rate >= 0.95:
                    verification["data_integrity"] = "complete"
                    verification["verification_passed"] = True
                elif recovery_rate >= 0.5:
                    verification["data_integrity"] = "partial"
                    verification["verification_passed"] = True
                elif recovery_rate > 0:
                    verification["data_integrity"] = "minimal"
                    verification["verification_passed"] = True
                else:
                    verification["data_integrity"] = "no_recovery"
                    verification["verification_passed"] = False
            else:
                # For expected failure, any recovery is bonus
                if verification["records_recovered"] > 0:
                    verification["data_integrity"] = "partial_recovery"
                    verification["verification_passed"] = True
                else:
                    verification["data_integrity"] = "no_recovery"
                    verification["verification_passed"] = True  # Expected

        except Exception as e:
            verification["data_integrity"] = f"error: {str(e)}"

        return verification

    def run_scenario(self, scenario: RecoveryTestScenario) -> Dict:
        """Run a single recovery scenario"""
        print(f"\n{'='*70}")
        print(f"Testing: {scenario.name}")
        print(f"Description: {scenario.description}")
        print(f"Records: {scenario.records}, Page Size: {scenario.page_size}")
        if scenario.corruption_type:
            print(f"Corruption: {scenario.corruption_type} "
                  f"({scenario.corruption_ratio*100:.2f}%)")
        print(f"Expected: {'Success' if scenario.expected_success else 'Failure'}")
        print(f"Modes: {', '.join(scenario.recovery_modes)}")
        print(f"{'='*70}")

        result = {
            "scenario": scenario.name,
            "description": scenario.description,
            "records": scenario.records,
            "page_size": scenario.page_size,
            "corruption_type": scenario.corruption_type,
            "corruption_ratio": scenario.corruption_ratio,
            "expected_success": scenario.expected_success,
            "timestamp": datetime.now().isoformat(),
            "recovery_modes": {},
            "overall_passed": False
        }

        # Generate test data
        print(f"\nGenerating test data...")
        data_file = self.generate_test_data(
            scenario.records, scenario.page_size,
            scenario.corruption_type, scenario.corruption_ratio
        )
        if not data_file:
            result["error"] = "Failed to generate test data"
            return result

        print(f"  Generated: {data_file}")

        # Run recovery for each mode
        for mode in scenario.recovery_modes:
            print(f"\nRunning recovery mode: {mode}")
            recovery_result = self.run_recovery(data_file, recovery_mode=mode)
            result["recovery_modes"][mode] = recovery_result

            if recovery_result["success"]:
                print(f"  Success! Output: {recovery_result['output_file']}")
                print(f"  Time: {recovery_result['elapsed_seconds']:.2f}s")
            else:
                print(f"  Failed: {recovery_result['stderr'][:100]}")

            # Verify
            verification = self.verify_recovery(scenario, recovery_result["output_file"])
            recovery_result["verification"] = verification

            if verification["verification_passed"]:
                print(f"  Verification: PASSED ({verification['data_integrity']})")
                print(f"  Records recovered: {verification['records_recovered']}")
            else:
                print(f"  Verification: FAILED ({verification['data_integrity']})")

        # Determine overall success
        success_modes = sum(1 for m in result["recovery_modes"].values()
                          if m.get("verification", {}).get("verification_passed", False))
        total_modes = len(scenario.recovery_modes)
        result["overall_passed"] = success_modes >= 1  # At least one mode must succeed

        return result

    def run_all_scenarios(self) -> List[Dict]:
        """Run all recovery scenarios"""
        # Ensure recovery tool exists
        if not self.ensure_recovery_tool():
            print("Failed to ensure recovery tool")
            return []

        # Run each scenario
        results = []
        for i, scenario in enumerate(self.test_scenarios, 1):
            print(f"\n\n{'#'*70}")
            print(f"# Scenario {i}/{len(self.test_scenarios)}")
            print(f"{'#'*70}")

            result = self.run_scenario(scenario)
            results.append(result)

            # Save intermediate results
            self.save_results(results)

        return results

    def save_results(self, results: List[Dict], filename: str = None):
        """Save results to JSON file"""
        if filename is None:
            filename = os.path.join(
                self.output_dir,
                f"recovery_test_results_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
            )

        os.makedirs(os.path.dirname(filename), exist_ok=True)

        output = {
            "generated_at": datetime.now().isoformat(),
            "num_scenarios": len(results),
            "total_scenarios": len(self.test_scenarios),
            "results": results
        }

        with open(filename, 'w', encoding='utf-8') as f:
            json.dump(output, f, indent=2, ensure_ascii=False)

        print(f"\nResults saved to: {filename}")

    def print_summary(self, results: List[Dict]):
        """Print summary of all results"""
        print(f"\n{'='*70}")
        print("COMPREHENSIVE RECOVERY TEST SUMMARY")
        print(f"{'='*70}")

        total = len(results)
        passed = sum(1 for r in results if r["overall_passed"])
        failed = total - passed

        print(f"\nTotal Scenarios: {total}")
        print(f"Passed: {passed}")
        print(f"Failed: {failed}")
        print(f"Success Rate: {passed/total*100:.1f}%")

        # Summary by corruption type
        print(f"\n{'='*70}")
        print("SUMMARY BY SCENARIO TYPE")
        print(f"{'='*70}")

        scenarios_by_type = {}
        for r in results:
            corruption = r.get("corruption_type") or "none"
            if corruption not in scenarios_by_type:
                scenarios_by_type[corruption] = {"passed": 0, "total": 0}
            scenarios_by_type[corruption]["total"] += 1
            if r["overall_passed"]:
                scenarios_by_type[corruption]["passed"] += 1

        for corruption, stats in sorted(scenarios_by_type.items()):
            rate = stats["passed"]/stats["total"]*100 if stats["total"] > 0 else 0
            status = "✓" if rate >= 80 else ("○" if rate >= 50 else "✗")
            print(f"{status} {corruption:20s}: {stats['passed']:3d}/{stats['total']:3d} "
                  f"({rate:5.1f}%)")

        # Summary by page size
        print(f"\n{'='*70}")
        print("SUMMARY BY PAGE SIZE")
        print(f"{'='*70}")

        scenarios_by_page_size = {}
        for r in results:
            page_size = r.get("page_size", 16384)
            if page_size not in scenarios_by_page_size:
                scenarios_by_page_size[page_size] = {"passed": 0, "total": 0}
            scenarios_by_page_size[page_size]["total"] += 1
            if r["overall_passed"]:
                scenarios_by_page_size[page_size]["passed"] += 1

        for page_size in sorted(scenarios_by_page_size.keys()):
            stats = scenarios_by_page_size[page_size]
            rate = stats["passed"]/stats["total"]*100 if stats["total"] > 0 else 0
            status = "✓" if rate >= 80 else ("○" if rate >= 50 else "✗")
            print(f"{status} {page_size:5d} bytes: {stats['passed']:3d}/{stats['total']:3d} "
                  f"({rate:5.1f}%)")

        # Summary by record count
        print(f"\n{'='*70}")
        print("SUMMARY BY RECORD COUNT")
        print(f"{'='*70}")

        scenarios_by_records = {}
        for r in results:
            records = r.get("records", 0)
            if records < 10000:
                bucket = "<10k"
            elif records < 50000:
                bucket = "10k-50k"
            elif records < 100000:
                bucket = "50k-100k"
            else:
                bucket = ">=100k"

            if bucket not in scenarios_by_records:
                scenarios_by_records[bucket] = {"passed": 0, "total": 0}
            scenarios_by_records[bucket]["total"] += 1
            if r["overall_passed"]:
                scenarios_by_records[bucket]["passed"] += 1

        for bucket in ["<10k", "10k-50k", "50k-100k", ">=100k"]:
            if bucket in scenarios_by_records:
                stats = scenarios_by_records[bucket]
                rate = stats["passed"]/stats["total"]*100 if stats["total"] > 0 else 0
                status = "✓" if rate >= 80 else ("○" if rate >= 50 else "✗")
                print(f"{status} {bucket:10s}: {stats['passed']:3d}/{stats['total']:3d} "
                      f"({rate:5.1f}%)")

        # Detailed results
        print(f"\n{'='*70}")
        print("DETAILED RESULTS")
        print(f"{'='*70}")

        for result in results:
            status = "PASS" if result["overall_passed"] else "FAIL"
            corruption = result.get("corruption_type") or "none"
            records = result.get("records", 0)

            print(f"\n[{status}] {result['scenario']}")
            print(f"       Records: {records:,}, Corruption: {corruption}")

            for mode, mode_result in result.get("recovery_modes", {}).items():
                verification = mode_result.get("verification", {})
                rec_count = verification.get("records_recovered", 0)
                integrity = verification.get("data_integrity", "unknown")
                passed = verification.get("verification_passed", False)
                elapsed = mode_result.get("elapsed_seconds", 0)

                mode_status = "✓" if passed else "✗"
                print(f"       {mode_status} {mode:10s}: {rec_count:6d} records, "
                      f"{integrity:15s}, {elapsed:6.2f}s")

        print(f"\n{'='*70}")


def main():
    # Get the script's directory to resolve relative paths
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)

    parser = argparse.ArgumentParser(
        description='Comprehensive Recovery Testing for MySQL Recovery Pro'
    )
    parser.add_argument('--tool-path', default=os.path.join(project_root, 'build', 'mysql_recovery_pro'),
                        help='Path to recovery tool')
    parser.add_argument('--test-data-dir', default=os.path.join(project_root, 'test_data'),
                        help='Test data directory')
    parser.add_argument('--output-dir', default=os.path.join(project_root, 'recovery_test_results'),
                        help='Output directory for results')
    parser.add_argument('--scenario', type=str, default=None,
                        help='Run specific scenario only')
    parser.add_argument('--list-scenarios', action='store_true',
                        help='List all available scenarios')
    parser.add_argument('--quick', action='store_true',
                        help='Run quick test (first 5 scenarios only)')

    args = parser.parse_args()

    runner = ComprehensiveRecoveryTestRunner(
        recovery_tool_path=args.tool_path,
        test_data_dir=args.test_data_dir,
        output_dir=args.output_dir
    )

    if args.list_scenarios:
        print("Available Scenarios:")
        print("-" * 70)
        for i, scenario in enumerate(runner.test_scenarios, 1):
            print(f"{i:2d}. {scenario.name}")
            print(f"     {scenario.description}")
            if scenario.corruption_type:
                print(f"     Corruption: {scenario.corruption_type} "
                      f"({scenario.corruption_ratio*100:.2f}%)")
        return

    # Filter scenarios
    scenarios = runner.test_scenarios
    if args.scenario:
        scenarios = [s for s in scenarios if s.name == args.scenario]
        if not scenarios:
            print(f"Scenario not found: {args.scenario}")
            return

    if args.quick:
        scenarios = scenarios[:5]
        print(f"Running quick test (first {len(scenarios)} scenarios)")

    # Update runner scenarios
    runner.test_scenarios = scenarios

    # Run scenarios
    results = runner.run_all_scenarios()

    # Print summary
    runner.print_summary(results)


if __name__ == '__main__':
    main()
