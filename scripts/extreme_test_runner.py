#!/usr/bin/env python3
"""
Extreme Test Runner - Run all extreme scenarios
运行所有极端场景测试
"""

import os
import sys
import json
import subprocess
import argparse
from datetime import datetime
from typing import List, Dict, Tuple
from pathlib import Path


class ExtremeTestRunner:
    """极端测试运行器"""
    
    def __init__(self, project_dir: str = ".", build_dir: str = "build",
                 test_data_dir: str = "test_data", results_dir: str = "test_results"):
        self.project_dir = Path(project_dir)
        self.build_dir = Path(build_dir)
        self.test_data_dir = Path(test_data_dir)
        self.results_dir = Path(results_dir)
        
        # Ensure directories exist
        self.results_dir.mkdir(parents=True, exist_ok=True)
        self.test_data_dir.mkdir(parents=True, exist_ok=True)
        
        self.results: List[Dict] = []
        self.start_time = None
        self.end_time = None
    
    def run_command(self, cmd: List[str], timeout: int = 300) -> Tuple[bool, str, str]:
        """Run a command"""
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout,
                cwd=str(self.project_dir)
            )
            return result.returncode == 0, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return False, "", "Command timed out"
        except Exception as e:
            return False, "", str(e)
    
    def build_project(self) -> Tuple[bool, str]:
        """Build the project"""
        build_script = self.project_dir / "scripts" / "build_linux.sh"
        
        if not build_script.exists():
            return False, "Build script not found"
        
        success, stdout, stderr = self.run_command(["bash", str(build_script)])
        
        if success:
            return True, "Build successful"
        else:
            return False, stderr
    
    def run_extreme_chaos_monkey(self, input_file: str, scenario: str,
                                 output_dir: str) -> Tuple[bool, Dict]:
        """Run extreme chaos monkey"""
        extreme_monkey = self.project_dir / "scripts" / "extreme_chaos_monkey.py"
        
        if not extreme_monkey.exists():
            return False, {"error": "Extreme chaos monkey not found"}
        
        output_file = Path(output_dir) / f"extreme_{scenario}.ibd"
        
        success, stdout, stderr = self.run_command([
            "python3", str(extreme_monkey),
            "-i", input_file,
            "-o", str(output_file),
            "-s", scenario
        ])
        
        if success and output_file.exists():
            return True, {
                "scenario": scenario,
                "output_file": str(output_file),
                "output": stdout
            }
        else:
            return False, {
                "scenario": scenario,
                "error": stderr
            }
    
    def run_multi_badblock_generator(self, input_file: str, scenario: int,
                                     output_dir: str) -> Tuple[bool, Dict]:
        """Run multi badblock generator"""
        badblock_gen = self.project_dir / "scripts" / "multi_badblock_generator.py"
        
        if not badblock_gen.exists():
            return False, {"error": "Multi badblock generator not found"}
        
        output_file = Path(output_dir) / f"test_multi_badblock_scenario{scenario}.ibd"
        
        success, stdout, stderr = self.run_command([
            "python3", str(badblock_gen),
            "-i", input_file,
            "-o", str(output_dir),
            "-s", str(scenario)
        ])
        
        if success and output_file.exists():
            return True, {
                "scenario": scenario,
                "output_file": str(output_file),
                "output": stdout
            }
        else:
            return False, {
                "scenario": scenario,
                "error": stderr
            }
    
    def run_metadata_loss_generator(self, input_file: str, scenario: int,
                                    output_dir: str) -> Tuple[bool, Dict]:
        """Run metadata loss generator"""
        metadata_gen = self.project_dir / "scripts" / "metadata_loss_generator.py"
        
        if not metadata_gen.exists():
            return False, {"error": "Metadata loss generator not found"}
        
        output_file = Path(output_dir) / f"test_metadata_loss_scenario{scenario}.ibd"
        
        success, stdout, stderr = self.run_command([
            "python3", str(metadata_gen),
            "-i", input_file,
            "-o", str(output_dir),
            "-s", str(scenario)
        ])
        
        if success and output_file.exists():
            return True, {
                "scenario": scenario,
                "output_file": str(output_file),
                "output": stdout
            }
        else:
            return False, {
                "scenario": scenario,
                "error": stderr
            }
    
    def run_recovery(self, ibd_file: str, output_dir: str,
                    format_type: str = "csv") -> Dict:
        """Run recovery on a test file"""
        binary = self.build_dir / "mysql_recovery_pro"
        
        if not binary.exists():
            return {"error": "Recovery binary not found"}
        
        output_path = Path(output_dir)
        output_path.mkdir(parents=True, exist_ok=True)
        
        cmd = [
            str(binary),
            f"--ibd={ibd_file}",
            f"--output={output_dir}",
            f"--format={format_type}"
        ]
        
        success, stdout, stderr = self.run_command(cmd)
        
        return {
            "success": success,
            "stdout": stdout[:500],
            "stderr": stderr[:500],
            "output_dir": str(output_dir)
        }
    
    def run_extreme_scenario(self, scenario_name: str, input_file: str,
                            output_dir: str) -> Dict:
        """Run a complete extreme scenario"""
        print(f"\n{'='*60}")
        print(f"Extreme Scenario: {scenario_name}")
        print('='*60)
        
        result = {
            "scenario": scenario_name,
            "input_file": input_file,
            "timestamp": datetime.now().isoformat(),
            "steps": {}
        }
        
        # Step 1: Generate corruption
        print(f"\n[1/2] Generating corruption...")
        
        if scenario_name.startswith("extreme_"):
            # Extreme chaos monkey scenario
            success, info = self.run_extreme_chaos_monkey(
                input_file, scenario_name.replace("extreme_", ""), output_dir
            )
        elif scenario_name.startswith("badblock_"):
            # Multi badblock scenario
            scenario_num = int(scenario_name.replace("badblock_", ""))
            success, info = self.run_multi_badblock_generator(
                input_file, scenario_num, output_dir
            )
        elif scenario_name.startswith("metadata_"):
            # Metadata loss scenario
            scenario_num = int(scenario_name.replace("metadata_", ""))
            success, info = self.run_metadata_loss_generator(
                input_file, scenario_num, output_dir
            )
        else:
            return {
                **result,
                "error": f"Unknown scenario: {scenario_name}"
            }
        
        result["steps"]["corruption"] = info
        
        if not success:
            result["overall_passed"] = False
            return result
        
        # Step 2: Run recovery
        print(f"\n[2/2] Running recovery...")
        
        output_subdir = Path(output_dir) / "recovery"
        recovery_result = self.run_recovery(
            info.get("output_file", ""),
            str(output_subdir),
            format_type="csv"
        )
        
        result["steps"]["recovery"] = recovery_result
        result["overall_passed"] = recovery_result.get("success", False)
        
        return result
    
    def run_all_extreme_scenarios(self) -> Dict:
        """Run all extreme scenarios"""
        self.start_time = datetime.now()
        
        print("="*60)
        print("MySQL Recovery Pro - Extreme Test Suite")
        print("="*60)
        print(f"Started: {self.start_time.isoformat()}")
        
        # Build project
        print("\n" + "="*60)
        print("BUILD PHASE")
        print("="*60)
        build_success, build_output = self.build_project()
        
        if not build_success:
            return {"error": "Build failed", "details": build_output}
        
        # Find test input file
        test_files = list(self.test_data_dir.glob("test_table*.ibd"))
        if not test_files:
            return {"error": "No test files found"}
        
        input_file = str(test_files[0])
        print(f"\nUsing input file: {input_file}")
        
        # Define extreme scenarios
        scenarios = [
            # Extreme chaos monkey scenarios
            "extreme_file_hole",
            "extreme_bit_flip",
            "extreme_all_checksums",
            "extreme_all_headers",
            "extreme_sparse_file",
            "extreme_scramble",
            "extreme_chain",
            "extreme_record",
            
            # Multi badblock scenarios
            "badblock_1",
            "badblock_2",
            "badblock_3",
            "badblock_4",
            "badblock_5",
            
            # Metadata loss scenarios
            "metadata_1",
            "metadata_2",
            "metadata_3",
            "metadata_4",
            "metadata_5",
            "metadata_6",
            "metadata_7",
            "metadata_8",
            "metadata_9",
            "metadata_10",
        ]
        
        # Run scenarios
        for scenario in scenarios:
            output_dir = self.results_dir / scenario
            result = self.run_extreme_scenario(scenario, input_file, str(output_dir))
            self.results.append(result)
        
        self.end_time = datetime.now()
        
        # Generate report
        report = self.generate_report()
        
        return report
    
    def generate_report(self) -> Dict:
        """Generate extreme test report"""
        report = {
            "timestamp": datetime.now().isoformat(),
            "start_time": self.start_time.isoformat() if self.start_time else None,
            "end_time": self.end_time.isoformat() if self.end_time else None,
            "duration_seconds": None,
            "summary": {
                "total_scenarios": len(self.results),
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
            if result.get("overall_passed", False):
                report["summary"]["passed"] += 1
            else:
                report["summary"]["failed"] += 1
        
        total = report["summary"]["total_scenarios"]
        report["summary"]["pass_rate"] = (
            report["summary"]["passed"] / max(1, total) * 100
        )
        
        # Save JSON report
        json_report = self.results_dir / "extreme_test_report.json"
        with open(json_report, 'w') as f:
            json.dump(report, f, indent=2)
        
        # Save Markdown report
        md_report = self.results_dir / "extreme_test_report.md"
        with open(md_report, 'w') as f:
            f.write(self.generate_markdown_report(report))
        
        print(f"\nReports saved:")
        print(f"  JSON: {json_report}")
        print(f"  Markdown: {md_report}")
        
        return report
    
    def generate_markdown_report(self, report: Dict) -> str:
        """Generate markdown report"""
        lines = [
            "# MySQL Recovery Pro - Extreme Test Report",
            "",
            f"**Generated:** {report['timestamp']}",
            f"**Duration:** {report.get('duration_seconds', 'N/A')} seconds",
            "",
            "## Summary",
            "",
            f"| Metric | Value |",
            f"|--------|-------|",
            f"| Total Scenarios | {report['summary']['total_scenarios']} |",
            f"| Passed | {report['summary']['passed']} |",
            f"| Failed | {report['summary']['failed']} |",
            f"| Pass Rate | {report['summary']['pass_rate']:.1f}% |",
            "",
            "## Test Results",
            ""
        ]
        
        for i, result in enumerate(report['results'], 1):
            status = "✅ PASS" if result.get("overall_passed", False) else "❌ FAIL"
            scenario = result.get("scenario", "N/A")
            
            lines.append(f"### Test {i}: {scenario}")
            lines.append("")
            lines.append(f"- **Status:** {status}")
            lines.append(f"- **Timestamp:** {result.get('timestamp', 'N/A')}")
            
            if "steps" in result:
                for step_name, step_result in result["steps"].items():
                    step_status = "✅" if step_result.get("success", False) else "❌"
                    lines.append(f"- **{step_name}:** {step_status}")
            
            if "error" in result:
                lines.append(f"- **Error:** {result['error']}")
            
            lines.append("")
        
        return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Extreme Test Runner - Run all extreme scenarios'
    )
    parser.add_argument('--project-dir', default='.',
                       help='Project directory')
    parser.add_argument('--build-dir', default='build',
                       help='Build directory')
    parser.add_argument('--test-data-dir', default='test_data',
                       help='Test data directory')
    parser.add_argument('--results-dir', default='test_results',
                       help='Results directory')
    parser.add_argument('--scenario', '-s',
                       help='Specific scenario to run')
    parser.add_argument('--input-file', '-i',
                       help='Input IBD file')
    
    args = parser.parse_args()
    
    runner = ExtremeTestRunner(
        project_dir=args.project_dir,
        build_dir=args.build_dir,
        test_data_dir=args.test_data_dir,
        results_dir=args.results_dir
    )
    
    if args.scenario:
        # Run specific scenario
        test_files = list(Path(args.test_data_dir).glob("test_table*.ibd"))
        if not test_files:
            print("No test files found")
            sys.exit(1)
        
        input_file = args.input_file or str(test_files[0])
        output_dir = Path(args.results_dir) / args.scenario
        
        result = runner.run_extreme_scenario(args.scenario, input_file, str(output_dir))
        
        print(f"\nScenario: {args.scenario}")
        print(f"Status: {'PASS' if result.get('overall_passed', False) else 'FAIL'}")
        
        if not result.get('overall_passed', False):
            sys.exit(1)
    else:
        # Run all extreme scenarios
        report = runner.run_all_extreme_scenarios()
        
        print("\n" + "="*60)
        print("Extreme Test Summary")
        print("="*60)
        print(f"Total scenarios: {report['summary']['total_scenarios']}")
        print(f"Passed: {report['summary']['passed']}")
        print(f"Failed: {report['summary']['failed']}")
        print(f"Pass rate: {report['summary']['pass_rate']:.1f}%")
        print(f"Duration: {report.get('duration_seconds', 'N/A')} seconds")
        
        if report['summary']['failed'] > 0:
            sys.exit(1)


if __name__ == '__main__':
    main()
