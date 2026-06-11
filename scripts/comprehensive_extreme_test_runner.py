#!/usr/bin/env python3
"""
Comprehensive Extreme Test Runner
综合极端测试运行器 - 运行所有极端场景测试
"""

import os
import sys
import json
import subprocess
import argparse
from datetime import datetime
from typing import List, Dict, Tuple
from pathlib import Path


class ComprehensiveExtremeTestRunner:
    """综合极端测试运行器"""
    
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
        
        # Test categories
        self.test_categories = {
            "cli_mode": {
                "script": "cli_mode_extreme_test.py",
                "scenarios": ['scan', 'search', 'bulk', 'check-headers', 'check-corruption',
                             'repair', 'extract-sdi', 'rebuild-chain', 'parse-page']
            },
            "recovery_mode": {
                "script": "recovery_mode_extreme_test.py",
                "scenarios": ['truncate-recovery', 'rmrf-recovery', 'default-recovery',
                             'blob-recovery', 'header-repair']
            },
            "output_format": {
                "script": "output_format_extreme_test.py",
                "scenarios": ['csv', 'sql', 'json', 'ibd']
            },
            "boundary_condition": {
                "script": "boundary_condition_extreme_test.py",
                "scenarios": ['empty', 'minimal', 'single_page', 'large', 'odd_page_size',
                             'truncated', 'fragmented']
            },
            "stress_test": {
                "script": "stress_test_suite.py",
                "scenarios": list(range(1, 9))
            },
            "exception_input": {
                "script": "exception_input_extreme_test.py",
                "scenarios": list(range(1, 9))
            },
            "performance": {
                "script": "performance_extreme_test.py",
                "scenarios": list(range(1, 9))
            },
            "extreme_chaos": {
                "script": "extreme_chaos_monkey.py",
                "scenarios": ['file_hole', 'truncate_middle', 'scramble_order',
                             'random_bytes', 'bit_flip', 'all_checksums',
                             'sequential_headers', 'sparse_file', 'duplicate_pages',
                             'pattern_overwrite', 'corrupt_btree', 'corrupt_sdi',
                             'corrupt_undo', 'corrupt_blob', 'all_page_types',
                             'fragmentation', 'checksum_and_type', 'chain_and_checksum',
                             'all_headers', 'record_chain']
            },
            "multi_badblock": {
                "script": "multi_badblock_generator.py",
                "scenarios": list(range(1, 11))
            },
            "metadata_loss": {
                "script": "metadata_loss_generator.py",
                "scenarios": list(range(1, 11))
            }
        }
    
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
    
    def run_test_category(self, category: str, input_file: str, 
                         output_dir: str) -> List[Dict]:
        """Run a test category"""
        results = []
        category_config = self.test_categories.get(category, {})
        script_name = category_config.get("script", "")
        scenarios = category_config.get("scenarios", [])
        
        script_path = self.project_dir / "scripts" / script_name
        
        if not script_path.exists():
            print(f"Warning: Script not found: {script_path}")
            return results
        
        for scenario in scenarios:
            scenario_str = str(scenario) if isinstance(scenario, int) else scenario
            scenario_output = Path(output_dir) / category / scenario_str
            
            if category in ["multi_badblock", "metadata_loss"]:
                # Use different script format
                success, stdout, stderr = self.run_command([
                    "python3", str(script_path),
                    "-i", input_file,
                    "-o", str(scenario_output),
                    "-s", scenario_str
                ])
            elif category == "extreme_chaos":
                success, stdout, stderr = self.run_command([
                    "python3", str(script_path),
                    "-i", input_file,
                    "-o", str(scenario_output / f"extreme_{scenario_str}.ibd"),
                    "-s", scenario_str
                ])
            else:
                success, stdout, stderr = self.run_command([
                    "python3", str(script_path),
                    "-i", input_file,
                    "-o", str(scenario_output),
                    "-s", scenario_str
                ])
            
            result = {
                "category": category,
                "scenario": scenario_str,
                "success": success,
                "output_dir": str(scenario_output),
                "stdout": stdout[:200],
                "stderr": stderr[:200]
            }
            results.append(result)
        
        return results
    
    def run_all_tests(self, input_file: str = None) -> Dict:
        """Run all extreme tests"""
        self.start_time = datetime.now()
        
        print("="*60)
        print("MySQL Recovery Pro - Comprehensive Extreme Test Suite")
        print("="*60)
        print(f"Started: {self.start_time.isoformat()}")
        
        # Build project
        print("\n" + "="*60)
        print("BUILD PHASE")
        print("="*60)
        build_success, build_output = self.build_project()
        
        if not build_success:
            return {"error": "Build failed", "details": build_output}
        
        # Find input file
        if not input_file:
            test_files = list(self.test_data_dir.glob("test_table*.ibd"))
            if test_files:
                input_file = str(test_files[0])
            else:
                return {"error": "No test files found"}
        
        print(f"\nUsing input file: {input_file}")
        
        # Run all test categories
        for category, config in self.test_categories.items():
            print(f"\n{'='*60}")
            print(f"Running: {category.upper()} ({len(config['scenarios'])} scenarios)")
            print('='*60)
            
            category_output = self.results_dir / category
            category_output.mkdir(parents=True, exist_ok=True)
            
            category_results = self.run_test_category(category, input_file, str(category_output))
            self.results.extend(category_results)
            
            passed = sum(1 for r in category_results if r.get("success", False))
            print(f"  Passed: {passed}/{len(category_results)}")
        
        self.end_time = datetime.now()
        
        # Generate report
        report = self.generate_report()
        
        return report
    
    def generate_report(self) -> Dict:
        """Generate comprehensive extreme test report"""
        report = {
            "timestamp": datetime.now().isoformat(),
            "start_time": self.start_time.isoformat() if self.start_time else None,
            "end_time": self.end_time.isoformat() if self.end_time else None,
            "duration_seconds": None,
            "summary": {
                "total_tests": len(self.results),
                "passed": 0,
                "failed": 0,
                "pass_rate": 0.0,
                "by_category": {}
            },
            "results": self.results
        }
        
        if self.start_time and self.end_time:
            report["duration_seconds"] = (
                self.end_time - self.start_time
            ).total_seconds()
        
        # Calculate summary
        for result in self.results:
            category = result.get("category", "unknown")
            
            if category not in report["summary"]["by_category"]:
                report["summary"]["by_category"][category] = {"total": 0, "passed": 0}
            
            report["summary"]["by_category"][category]["total"] += 1
            
            if result.get("success", False):
                report["summary"]["passed"] += 1
                report["summary"]["by_category"][category]["passed"] += 1
            else:
                report["summary"]["failed"] += 1
        
        total = report["summary"]["total_tests"]
        report["summary"]["pass_rate"] = (
            report["summary"]["passed"] / max(1, total) * 100
        )
        
        # Save JSON report
        json_report = self.results_dir / "comprehensive_extreme_test_report.json"
        with open(json_report, 'w') as f:
            json.dump(report, f, indent=2)
        
        # Save Markdown report
        md_report = self.results_dir / "comprehensive_extreme_test_report.md"
        with open(md_report, 'w') as f:
            f.write(self.generate_markdown_report(report))
        
        print(f"\nReports saved:")
        print(f"  JSON: {json_report}")
        print(f"  Markdown: {md_report}")
        
        return report
    
    def generate_markdown_report(self, report: Dict) -> str:
        """Generate markdown report"""
        lines = [
            "# MySQL Recovery Pro - Comprehensive Extreme Test Report",
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
            "## Test Results by Category",
            ""
        ]
        
        for category, stats in report['summary']['by_category'].items():
            pass_rate = stats['passed'] / max(1, stats['total']) * 100
            lines.append(f"- **{category}**: {stats['passed']}/{stats['total']} ({pass_rate:.1f}%)")
        
        lines.extend([
            "",
            "## Detailed Results",
            ""
        ])
        
        for i, result in enumerate(report['results'], 1):
            status = "✅ PASS" if result.get("success", False) else "❌ FAIL"
            category = result.get("category", "N/A")
            scenario = result.get("scenario", "N/A")
            
            lines.append(f"### Test {i}: {category} - {scenario}")
            lines.append("")
            lines.append(f"- **Status:** {status}")
            lines.append("")
        
        return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Comprehensive Extreme Test Runner'
    )
    parser.add_argument('--project-dir', default='.',
                       help='Project directory')
    parser.add_argument('--build-dir', default='build',
                       help='Build directory')
    parser.add_argument('--test-data-dir', default='test_data',
                       help='Test data directory')
    parser.add_argument('--results-dir', default='test_results',
                       help='Results directory')
    parser.add_argument('--category', '-c',
                       choices=list(ComprehensiveExtremeTestRunner(
                           ).test_categories.keys()),
                       help='Specific category to run')
    parser.add_argument('--input-file', '-i',
                       help='Input IBD file')
    
    args = parser.parse_args()
    
    runner = ComprehensiveExtremeTestRunner(
        project_dir=args.project_dir,
        build_dir=args.build_dir,
        test_data_dir=args.test_data_dir,
        results_dir=args.results_dir
    )
    
    if args.category:
        # Run specific category
        test_files = list(Path(args.test_data_dir).glob("test_table*.ibd"))
        if not test_files:
            print("No test files found")
            sys.exit(1)
        
        input_file = args.input_file or str(test_files[0])
        output_dir = Path(args.results_dir) / args.category
        
        results = runner.run_test_category(args.category, input_file, str(output_dir))
        
        passed = sum(1 for r in results if r.get("success", False))
        print(f"\nCategory: {args.category}")
        print(f"Passed: {passed}/{len(results)}")
        
        if passed < len(results):
            sys.exit(1)
    else:
        # Run all tests
        report = runner.run_all_tests()
        
        print("\n" + "="*60)
        print("Comprehensive Extreme Test Summary")
        print("="*60)
        print(f"Total tests: {report['summary']['total_tests']}")
        print(f"Passed: {report['summary']['passed']}")
        print(f"Failed: {report['summary']['failed']}")
        print(f"Pass rate: {report['summary']['pass_rate']:.1f}%")
        print(f"Duration: {report.get('duration_seconds', 'N/A')} seconds")
        print()
        print("Results by Category:")
        for category, stats in report['summary']['by_category'].items():
            pass_rate = stats['passed'] / max(1, stats['total']) * 100
            print(f"  {category}: {stats['passed']}/{stats['total']} ({pass_rate:.1f}%)")
        
        if report['summary']['failed'] > 0:
            sys.exit(1)


if __name__ == '__main__':
    main()
