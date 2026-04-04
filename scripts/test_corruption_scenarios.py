#!/usr/bin/env python3
"""
Comprehensive Corruption Scenario Testing for MySQL Recovery Pro
Tests various data corruption scenarios and validates recovery
"""

import os
import sys
import json
import subprocess
import argparse
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Tuple, Optional


class CorruptionScenario:
    """Represents a corruption scenario to test"""

    def __init__(self, name: str, description: str, corruption_type: str,
                 corruption_ratio: float = 0.01, expected_recovery: bool = True):
        self.name = name
        self.description = description
        self.corruption_type = corruption_type
        self.corruption_ratio = corruption_ratio
        self.expected_recovery = expected_recovery


class CorruptionTestRunner:
    """Run corruption scenarios and test recovery"""

    SCENARIOS = [
        # Basic corruption scenarios
        CorruptionScenario(
            name="header_type_corruption",
            description="Corrupt page type header - page marked as wrong type",
            corruption_type="header_type",
            corruption_ratio=0.01,
            expected_recovery=True
        ),
        CorruptionScenario(
            name="header_checksum_corruption",
            description="Corrupt page checksum - page integrity check failure",
            corruption_type="header_checksum",
            corruption_ratio=0.01,
            expected_recovery=True
        ),
        CorruptionScenario(
            name="infimum_record_corruption",
            description="Corrupt INFIMUM record - page chain broken at start",
            corruption_type="infimum_corrupt",
            corruption_ratio=0.01,
            expected_recovery=True
        ),
        CorruptionScenario(
            name="supremum_record_corruption",
            description="Corrupt SUPREMUM record - page chain broken at end",
            corruption_type="supremum_corrupt",
            corruption_ratio=0.01,
            expected_recovery=True
        ),
        CorruptionScenario(
            name="page_chain_broken",
            description="Corrupt next_page pointer - page chain broken",
            corruption_type="chain_broken",
            corruption_ratio=0.01,
            expected_recovery=True
        ),
        CorruptionScenario(
            name="record_chain_broken",
            description="Corrupt record next pointer - record chain broken",
            corruption_type="record_chain_broken",
            corruption_ratio=0.01,
            expected_recovery=True
        ),
        CorruptionScenario(
            name="random_byte_corruption",
            description="Random byte corruption - simulate bit flips",
            corruption_type="random",
            corruption_ratio=0.01,
            expected_recovery=True
        ),
        CorruptionScenario(
            name="partial_page_truncation",
            description="Truncate page - simulate incomplete write",
            corruption_type="truncate",
            corruption_ratio=0.01,
            expected_recovery=True
        ),
        CorruptionScenario(
            name="all_zero_page",
            description="Zero out page - simulate complete data loss",
            corruption_type="all_zero",
            corruption_ratio=0.01,
            expected_recovery=False
        ),
        # Multi-page corruption scenarios
        CorruptionScenario(
            name="multi_page_random_corruption",
            description="Multiple pages with random corruption",
            corruption_type="random",
            corruption_ratio=0.05,
            expected_recovery=True
        ),
        CorruptionScenario(
            name="multi_page_chain_corruption",
            description="Multiple pages with chain corruption",
            corruption_type="chain_broken",
            corruption_ratio=0.03,
            expected_recovery=True
        ),
        # Extreme corruption scenarios
        CorruptionScenario(
            name="extreme_random_corruption",
            description="High corruption ratio - extreme data loss",
            corruption_type="random",
            corruption_ratio=0.10,
            expected_recovery=False
        ),
        CorruptionScenario(
            name="extreme_truncation",
            description="Severe truncation - most data lost",
            corruption_type="truncate",
            corruption_ratio=0.30,
            expected_recovery=False
        ),
    ]

    def __init__(self, recovery_tool_path: str = "./mysql_recovery_pro",
                 test_data_dir: str = "test_data",
                 output_dir: str = "corruption_test_results"):
        self.recovery_tool_path = recovery_tool_path
        self.test_data_dir = test_data_dir
        self.output_dir = output_dir
        self.results = []

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

    def generate_test_data(self, num_records: int = 100000,
                          page_size: int = 16384) -> str:
        """Generate test data with specified parameters"""
        script = "scripts/generate_large_scale_test_data.py"
        output_path = os.path.join(
            self.test_data_dir,
            f"test_large_rec{num_records}_ps{page_size}.ibd"
        )

        if os.path.exists(output_path):
            return output_path

        cmd = [
            sys.executable, script,
            "--output-dir", self.test_data_dir,
            "--num-records", str(num_records),
            "--page-size", str(page_size)
        ]

        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Failed to generate test data: {result.stderr}")
            return None

        return output_path

    def generate_corrupted_data(self, scenario: CorruptionScenario,
                                num_records: int = 100000,
                                page_size: int = 16384) -> str:
        """Generate corrupted test data for a scenario"""
        script = "scripts/generate_large_scale_test_data.py"
        output_path = os.path.join(
            self.test_data_dir,
            f"test_large_corrupted_{scenario.corruption_type}_"
            f"rec{num_records}_ps{page_size}.ibd"
        )

        cmd = [
            sys.executable, script,
            "--output-dir", self.test_data_dir,
            "--num-records", str(num_records),
            "--page-size", str(page_size),
            "--corruption", scenario.corruption_type,
            "--corruption-ratio", str(scenario.corruption_ratio),
            "--seed", "42"  # Fixed seed for reproducibility
        ]

        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Failed to generate corrupted data: {result.stderr}")
            return None

        return output_path

    def run_recovery(self, input_file: str, output_format: str = "sql",
                     output_dir: str = None) -> Dict:
        """Run recovery on a file"""
        if output_dir is None:
            output_dir = self.output_dir

        os.makedirs(output_dir, exist_ok=True)

        output_file = os.path.join(
            output_dir,
            f"recovered_{os.path.basename(input_file).replace('.ibd', '')}.{output_format}"
        )

        cmd = [
            self.recovery_tool_path,
            "--mode", "default",
            "--input", input_file,
            "--output", output_file,
            "--format", output_format,
            "--verbose"
        ]

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=300  # 5 minute timeout
            )

            return {
                "returncode": result.returncode,
                "stdout": result.stdout,
                "stderr": result.stderr,
                "output_file": output_file if result.returncode == 0 else None,
                "success": result.returncode == 0
            }
        except subprocess.TimeoutExpired:
            return {
                "returncode": -1,
                "stdout": "",
                "stderr": "Recovery timed out",
                "output_file": None,
                "success": False
            }
        except Exception as e:
            return {
                "returncode": -1,
                "stdout": "",
                "stderr": str(e),
                "output_file": None,
                "success": False
            }

    def verify_recovery(self, scenario: CorruptionScenario,
                        original_records: int,
                        recovered_file: str,
                        output_format: str = "sql") -> Dict:
        """Verify recovery results"""
        verification = {
            "records_recovered": 0,
            "data_integrity": "unknown",
            "verification_passed": False
        }

        if not recovered_file or not os.path.exists(recovered_file):
            verification["data_integrity"] = "no_output"
            return verification

        # Count records in output
        try:
            with open(recovered_file, 'r', encoding='utf-8') as f:
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

            # Verify data integrity based on scenario
            if scenario.expected_recovery:
                # For expected recovery, check if we got reasonable data
                if verification["records_recovered"] > original_records * 0.5:
                    verification["data_integrity"] = "partial"
                    verification["verification_passed"] = True
                elif verification["records_recovered"] > original_records * 0.9:
                    verification["data_integrity"] = "complete"
                    verification["verification_passed"] = True
                else:
                    verification["data_integrity"] = "minimal"
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

    def run_scenario(self, scenario: CorruptionScenario,
                    num_records: int = 100000,
                    page_size: int = 16384) -> Dict:
        """Run a single corruption scenario"""
        print(f"\n{'='*60}")
        print(f"Testing: {scenario.name}")
        print(f"Description: {scenario.description}")
        print(f"Corruption: {scenario.corruption_type} "
              f"({scenario.corruption_ratio*100:.1f}%)")
        print(f"{'='*60}")

        result = {
            "scenario": scenario.name,
            "description": scenario.description,
            "corruption_type": scenario.corruption_type,
            "corruption_ratio": scenario.corruption_ratio,
            "expected_recovery": scenario.expected_recovery,
            "timestamp": datetime.now().isoformat(),
            "records_original": num_records,
            "page_size": page_size,
            "generated_data": None,
            "recovery_result": None,
            "verification": None
        }

        # Generate corrupted data
        print(f"Generating corrupted data...")
        data_file = self.generate_corrupted_data(scenario, num_records, page_size)
        if not data_file:
            result["error"] = "Failed to generate corrupted data"
            return result

        result["generated_data"] = data_file
        print(f"  Generated: {data_file}")

        # Run recovery
        print(f"Running recovery...")
        recovery_result = self.run_recovery(data_file)
        result["recovery_result"] = recovery_result

        if recovery_result["success"]:
            print(f"  Recovery succeeded")
            print(f"  Output: {recovery_result['output_file']}")
        else:
            print(f"  Recovery failed: {recovery_result['stderr']}")

        # Verify
        print(f"Verifying recovery...")
        verification = self.verify_recovery(
            scenario, num_records,
            recovery_result["output_file"]
        )
        result["verification"] = verification

        if verification["verification_passed"]:
            print(f"  Verification: PASSED ({verification['data_integrity']})")
        else:
            print(f"  Verification: FAILED ({verification['data_integrity']})")

        return result

    def run_all_scenarios(self, num_records: int = 100000,
                         page_size: int = 16384,
                         scenarios: List[CorruptionScenario] = None) -> List[Dict]:
        """Run all corruption scenarios"""
        if scenarios is None:
            scenarios = self.SCENARIOS

        # Ensure recovery tool exists
        if not self.ensure_recovery_tool():
            print("Failed to ensure recovery tool")
            return []

        # Run each scenario
        results = []
        for scenario in scenarios:
            result = self.run_scenario(scenario, num_records, page_size)
            results.append(result)

            # Save intermediate results
            self.save_results(results)

        return results

    def save_results(self, results: List[Dict], filename: str = None):
        """Save results to JSON file"""
        if filename is None:
            filename = os.path.join(
                self.output_dir,
                f"corruption_test_results_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
            )

        os.makedirs(os.path.dirname(filename), exist_ok=True)

        output = {
            "generated_at": datetime.now().isoformat(),
            "num_scenarios": len(results),
            "results": results
        }

        with open(filename, 'w', encoding='utf-8') as f:
            json.dump(output, f, indent=2, ensure_ascii=False)

        print(f"\nResults saved to: {filename}")

    def print_summary(self, results: List[Dict]):
        """Print summary of all results"""
        print(f"\n{'='*60}")
        print("SUMMARY")
        print(f"{'='*60}")

        passed = sum(1 for r in results if r["verification"]["verification_passed"])
        failed = len(results) - passed
        expected_passed = sum(1 for r in results if r["expected_recovery"])

        print(f"\nTotal scenarios: {len(results)}")
        print(f"Passed: {passed}")
        print(f"Failed: {failed}")
        print(f"Expected recovery: {expected_passed}")

        print(f"\nScenario Results:")
        print("-" * 60)

        for result in results:
            status = "PASS" if result["verification"]["verification_passed"] else "FAIL"
            expected = "EXPECTED" if result["expected_recovery"] else "UNEXPECTED"
            records = result["verification"]["records_recovered"]
            integrity = result["verification"]["data_integrity"]

            print(f"[{status}] {result['scenario']}")
            print(f"       Expected: {expected}, Records: {records}, "
                  f"Integrity: {integrity}")

        print(f"\n{'='*60}")


def main():
    parser = argparse.ArgumentParser(
        description='Test corruption scenarios for MySQL Recovery Pro'
    )
    parser.add_argument('--tool-path', default='./mysql_recovery_pro',
                        help='Path to recovery tool')
    parser.add_argument('--test-data-dir', default='test_data',
                        help='Test data directory')
    parser.add_argument('--output-dir', default='corruption_test_results',
                        help='Output directory for results')
    parser.add_argument('--num-records', type=int, default=100000,
                        help='Number of records (default: 100000)')
    parser.add_argument('--page-size', type=int, default=16384,
                        choices=[4096, 8192, 16384, 32768, 65536],
                        help='Page size (default: 16384)')
    parser.add_argument('--scenario', type=str, default=None,
                        help='Run specific scenario only')
    parser.add_argument('--list-scenarios', action='store_true',
                        help='List all available scenarios')

    args = parser.parse_args()

    if args.list_scenarios:
        runner = CorruptionTestRunner()
        print("Available Scenarios:")
        print("-" * 60)
        for scenario in runner.SCENARIOS:
            print(f"  {scenario.name}: {scenario.description}")
        return

    runner = CorruptionTestRunner(
        recovery_tool_path=args.tool_path,
        test_data_dir=args.test_data_dir,
        output_dir=args.output_dir
    )

    # Filter scenarios if specified
    scenarios = runner.SCENARIOS
    if args.scenario:
        scenarios = [s for s in scenarios if s.name == args.scenario]
        if not scenarios:
            print(f"Scenario not found: {args.scenario}")
            return

    # Run scenarios
    results = runner.run_all_scenarios(
        num_records=args.num_records,
        page_size=args.page_size,
        scenarios=scenarios
    )

    # Print summary
    runner.print_summary(results)


if __name__ == '__main__':
    main()
