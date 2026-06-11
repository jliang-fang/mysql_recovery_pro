#!/usr/bin/env python3
"""
Chaos Testing Framework for MySQL Recovery Pro
Runs comprehensive corruption scenarios and validates recovery
"""

import os
import sys
import json
import subprocess
import argparse
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Optional
import time


CHAOS_SCENARIOS = [
    "random_3_pages",
    "random_5_pages",
    "random_10_pages",
    "all_headers",
    "btree_chain_broken",
    "metadata_lost",
    "file_fragments",
    "undo_log_corrupt",
    "record_chain_broken",
    "sdn_only",
    "header_only",
]

CHAOSE_TEST_DATA_DIR = "chaos-test-volumes"
BUILD_DIR = "build"


def run_chaos_corruption(input_file: str, output_file: str, scenario: str,
                         seed: int = 42, page_size: int = 16384) -> Dict:
    """Run chaos corruption on a file"""
    cmd = [
        sys.executable, "scripts/chaos_monkey.py",
        "-i", input_file,
        "-o", output_file,
        "-s", scenario,
        "--seed", str(seed),
    ]
    
    if page_size:
        cmd.extend(["--page-size", str(page_size)])
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    # Parse output
    lines = result.stdout.strip().split('\n')
    output = {}
    for line in lines:
        if ':' in line:
            key, value = line.split(':', 1)
            output[key.strip()] = value.strip()
    
    return {
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "parsed": output,
        "success": result.returncode == 0
    }


def run_recovery(input_file: str, output_dir: str, format: str = "sql",
                 recovery_mode: str = "default") -> Dict:
    """Run mysql_recovery_pro on a file"""
    cmd = [
        f"{BUILD_DIR}/mysql_recovery_pro",
        "--ibd", input_file,
        "--output", output_dir,
        "--format", format,
    ]
    
    if recovery_mode == "schemaless":
        cmd.append("--schemaless")
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    return {
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "success": result.returncode == 0
    }


def analyze_recovered_file(recovered_file: str) -> Dict:
    """Analyze recovered file for records"""
    result = {
        "records_recovered": 0,
        "has_data": False,
        "file_size": 0,
        "first_100_lines": []
    }
    
    if not os.path.exists(recovered_file):
        return result
    
    result["file_size"] = os.path.getsize(recovered_file)
    
    try:
        with open(recovered_file, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
            lines = content.split('\n')[:100]
            
            result["first_100_lines"] = lines
            
            # Count records
            result["records_recovered"] = content.count("INSERT INTO")
            result["records_recovered"] += content.count("VALUES (")
            result["records_recovered"] = max(0, result["records_recovered"] // 2)
            
            result["has_data"] = len(content.strip()) > 100
    except Exception as e:
        result["error"] = str(e)
    
    return result


def run_chaos_test(input_file: str, scenario: str, seed: int = 42,
                   output_dir: str = CHAOSE_TEST_DATA_DIR) -> Dict:
    """Run a complete chaos test scenario"""
    scenario_output = f"{output_dir}/chaos_{scenario}"
    os.makedirs(scenario_output, exist_ok=True)
    
    corrupted_file = f"{scenario_output}/corrupted.ibd"
    recovered_dir = f"{scenario_output}/recovered"
    recovered_sql = f"{recovered_dir}/output.sql"
    
    result = {
        "scenario": scenario,
        "seed": seed,
        "timestamp": datetime.now().isoformat(),
        "input_file": input_file,
    }
    
    # Run chaos corruption
    print(f"Running corruption: {scenario}")
    chaos_result = run_chaos_corruption(input_file, corrupted_file, scenario, seed)
    result["chaos"] = chaos_result
    
    if not chaos_result["success"]:
        result["error"] = "Chaos corruption failed"
        return result
    
    # Run recovery
    print(f"Running recovery on corrupted file")
    os.makedirs(recovered_dir, exist_ok=True)
    recovery_result = run_recovery(corrupted_file, recovered_dir)
    result["recovery"] = recovery_result
    
    # Analyze recovered file
    analysis = analyze_recovered_file(recovered_sql)
    result["recovered_file_analysis"] = analysis
    
    return result


def run_all_scenarios(input_files: List[str], scenarios: List[str] = None,
                      seeds: List[int] = None,
                      output_dir: str = CHAOSE_TEST_DATA_DIR) -> List[Dict]:
    """Run all scenarios on all input files"""
    if scenarios is None:
        scenarios = CHAOS_SCENARIOS
    if seeds is None:
        seeds = [42]
    
    results = []
    
    for input_file in input_files:
        print(f"\nProcessing input file: {input_file}")
        
        for seed in seeds:
            for scenario in scenarios:
                result = run_chaos_test(input_file, scenario, seed, output_dir)
                results.append(result)
    
    return results


def save_results(results: List[Dict], filename: str = None) -> str:
    """Save results to JSON file"""
    if filename is None:
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        filename = f"chaos_test_results_{timestamp}.json"
    
    output = {
        "generated_at": datetime.now().isoformat(),
        "num_scenarios": len(results),
        "results": results
    }
    
    with open(filename, 'w', encoding='utf-8') as f:
        json.dump(output, f, indent=2, ensure_ascii=False)
    
    return filename


def print_summary(results: List[Dict]):
    """Print summary of chaos testing results"""
    print(f"\n{'='*70}")
    print("CHAOS TESTING SUMMARY")
    print(f"{'='*70}")
    
    # Count by scenario
    scenario_stats = {}
    for r in results:
        scenario = r.get("scenario", "unknown")
        if scenario not in scenario_stats:
            scenario_stats[scenario] = {"total": 0, "success": 0, "records_avg": 0, "records_sum": 0}
        scenario_stats[scenario]["total"] += 1
        if r.get("chaos", {}).get("success", False):
            scenario_stats[scenario]["success"] += 1
        analysis = r.get("recovered_file_analysis", {})
        scenario_stats[scenario]["records_sum"] += analysis.get("records_recovered", 0)
    
    print(f"\n{'='*70}")
    print("RESULTS BY SCENARIO")
    print(f"{'='*70}")
    
    for scenario, stats in sorted(scenario_stats.items()):
        success_rate = stats["success"] / stats["total"] * 100 if stats["total"] > 0 else 0
        avg_records = stats["records_sum"] / stats["total"] if stats["total"] > 0 else 0
        status = "✓" if success_rate >= 80 else ("○" if success_rate >= 50 else "✗")
        print(f"{status} {scenario:25s}: {stats['success']:3d}/{stats['total']:3d} "
              f"chaos_success ({success_rate:5.1f}%), avg_records: {avg_records:5.1f}")
    
    # Count chaos success vs failure
    chaos_success = sum(1 for r in results if r.get("chaos", {}).get("success", False))
    total = len(results)
    chaos_fail = total - chaos_success
    
    print(f"\n{'='*70}")
    print("CHAOS CORRUPTION STATISTICS")
    print(f"{'='*70}")
    print(f"Total scenarios run: {total}")
    print(f"Chaos corruption successful: {chaos_success} ({chaos_success/total*100:.1f}%)")
    print(f"Chaos corruption failed: {chaos_fail} ({chaos_fail/total*100:.1f}%)")


def main():
    parser = argparse.ArgumentParser(
        description='Chaos Testing for MySQL Recovery Pro'
    )
    parser.add_argument('--input', '-i', nargs='+', default=[],
                       help='Input .ibd files to test')
    parser.add_argument('--output-dir', '-o', default=CHAOSE_TEST_DATA_DIR,
                       help='Output directory for chaos test results')
    parser.add_argument('--scenarios', '-s', nargs='+',
                       help='Specific scenarios to run (default: all)')
    parser.add_argument('--seed', type=int, default=42,
                       help='Random seed for reproducibility')
    parser.add_argument('--no-save', action='store_true',
                       help='Do not save results to JSON')
    parser.add_argument('--quiet', '-q', action='store_true',
                       help='Reduced output')
    
    args = parser.parse_args()
    
    # Find input files if not specified
    if not args.input:
        # Look for test files
        input_files = []
        for pattern in ["test_data/*.ibd", "chaos-test-volumes/*.ibd"]:
            for f in Path(".").glob(pattern):
                if f.is_file():
                    input_files.append(str(f))
        args.input = input_files
    
    if not args.input:
        print("No input files found. Run with --input to specify files.")
        sys.exit(1)
    
    print(f"Running chaos testing on {len(args.input)} input file(s)")
    
    # Run scenarios
    results = run_all_scenarios(args.input, args.scenarios, [args.seed], args.output_dir)
    
    # Print summary
    print_summary(results)
    
    # Save results
    if not args.no_save:
        filename = save_results(results)
        print(f"\nResults saved to: {filename}")


if __name__ == '__main__':
    main()
