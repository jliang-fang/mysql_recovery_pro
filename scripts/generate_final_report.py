#!/usr/bin/env python3
"""
Final Report Generator - Comprehensive Chaos Testing Summary
Purpose: Generate cross-version recovery evaluation reports
Author: jliang.fang (Global MySQL Kernel QA Expert)
Version: 1.0.0
"""

import os
import sys
import json
import argparse
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional
from collections import defaultdict


def load_json_file(path: str) -> Dict:
    """Load JSON file or return empty dict"""
    try:
        with open(path, 'r') as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def generate_version_summary(output_dir: str, versions: List[str]) -> Dict:
    """Generate recovery summary for each version"""
    version_results = {}
    
    for version in versions:
        version_dir = os.path.join(output_dir, version)
        if os.path.exists(version_dir):
            # Load version-specific results
            result_file = os.path.join(version_dir, "results.json")
            version_results[version] = load_json_file(result_file)
    
    return version_results


def calculate_recovery_score(records_recovered: int, 
                             records_expected: int,
                             hashes_match: int,
                             hashes_total: int) -> Dict:
    """
    Calculate recovery score based on multiple criteria
    
    Returns dict with score, level, and details
    """
    if records_expected == 0:
        return {
            "score": 0.0,
            "level": "TOTAL FAILURE",
            "details": "No records expected"
        }
    
    # Calculate individual metrics
    record_recovery_rate = records_recovered / records_expected if records_expected > 0 else 0
    hash_match_rate = hashes_match / hashes_total if hashes_total > 0 else 0
    
    # Weighted score
    score = (record_recovery_rate * 0.5) + (hash_match_rate * 0.5) * 100
    
    # Determine level
    if score >= 100:
        level = "100% EXACT RECOVERY"
    elif score >= 90:
        level = "90% HEURISTIC RECOVERY"
    elif score >= 50:
        level = "50% PARTIAL RECOVERY"
    elif score > 0:
        level = "CRITICAL FAILURE"
    else:
        level = "TOTAL FAILURE"
    
    return {
        "score": round(score, 2),
        "level": level,
        "metrics": {
            "records_recovered": records_recovered,
            "records_expected": records_expected,
            "record_recovery_rate": round(record_recovery_rate * 100, 2),
            "hashes_match": hashes_match,
            "hashes_total": hashes_total,
            "hash_match_rate": round(hash_match_rate * 100, 2)
        },
        "details": f"Score: {score:.2f}, Level: {level}"
    }


def generate_final_report(
    output_dir: str = "./recovery-output",
    golden_hashes_dir: str = "./golden-hashes",
    diagnosis_dir: str = "./diagnosis",
    output_file: str = "chaos_testing_report.json",
    versions: List[str] = None
):
    """Generate comprehensive testing report"""
    
    if versions is None:
        versions = ["5.0", "5.5", "5.6", "5.7", "8.0", "8.4", "9.0"]
    
    report = {
        "report_header": {
            "report_type": "Cross-Version Chaos Testing Report",
            "version": "2.1.0",
            "generated_at": datetime.now().isoformat(),
            "author": "jliang.fang (Global MySQL Kernel QA Expert)",
            "project": "mysql_recovery_pro"
        },
        
        "executive_summary": {
            "recovery_confidence_score": 0.0,
            "versions_tested": [],
            "data_integrity_verified": False,
            "code_evolution_patterns": [],
            "recommendations": []
        },
        
        "version_results": {},
        
        "corruption_scenario_results": {},
        
        "recovery_scoring": {
            "by_version": {},
            "overall": {}
        },
        
        "performance_metrics": {},
        
        "code_evolution": {
            "patches_generated": 0,
            "patches_applied": 0,
            "patches_verified": 0,
            "evolution_patterns": []
        },
        
        "auto_healing_loop": {
            "max_retries": 5,
            "actual_retries": 0,
            "total_failures": 0,
            "crash_types": defaultdict(int),
            "gdb_analysis": {},
            "asan_analysis": {}
        },
        
        "recommendations": {
            "immediate": [],
            "short_term": [],
            "long_term": []
        },
        
        "appendix": {
            "file_references": {
                "docker_compose": "docker/docker-compose.multiversions.yml",
                "data_injector": "scripts/ultra_data_injector.py",
                "chaos_monkey": "scripts/advanced_chaos_monkey.py",
                "self_healing": "scripts/self_healing_loop.py",
                "golden_hash": "scripts/golden_hash_base.py"
            },
            "metric_definitions": {
                "recovery_score": "Weighted average of record recovery and hash match rates",
                "exact_recovery": "100% records recovered with matching SHA-256 hashes",
                "heuristic_recovery": ">=90% records recovered with some field reconstruction",
                "partial_recovery": ">=50% records recovered, significant data loss",
                "critical_failure": "<50% records recovered, crash during recovery",
                "total_failure": "0% records recovered, engine crashed"
            }
        }
    }
    
    # Process each version
    total_score_sum = 0
    tested_versions = 0
    
    for version in versions:
        version_report = {
            "version": version,
            "status": "not_tested",
            "records_recovered": 0,
            "records_expected": 0,
            "hashes_match": 0,
            "hashes_total": 0,
            "recovery_score": {
                "score": 0.0,
                "level": "N/A"
            },
            "corruption_scenarios": {},
            "performance": {}
        }
        
        # Look for version output
        version_dir = os.path.join(output_dir, f"mysql-{version}")
        if os.path.exists(version_dir):
            version_report["status"] = "tested"
            tested_versions += 1
            
            # Load recovery results
            results_file = os.path.join(version_dir, "results.json")
            results = load_json_file(results_file)
            
            if results:
                version_report["records_recovered"] = results.get("records_recovered", 0)
                version_report["records_expected"] = results.get("records_expected", 100000)
                version_report["hashes_match"] = results.get("hashes_match", 0)
                version_report["hashes_total"] = results.get("hashes_total", 100000)
                
                # Calculate score
                score_result = calculate_recovery_score(
                    version_report["records_recovered"],
                    version_report["records_expected"],
                    version_report["hashes_match"],
                    version_report["hashes_total"]
                )
                version_report["recovery_score"] = score_result
                total_score_sum += score_result["score"]
        
        report["version_results"][version] = version_report
    
    # Calculate overall metrics
    if tested_versions > 0:
        avg_score = total_score_sum / tested_versions
        report["executive_summary"]["recovery_confidence_score"] = round(avg_score, 2)
        report["executive_summary"]["versions_tested"] = [
            v for v in versions 
            if report["version_results"][v]["status"] == "tested"
        ]
    
    # Aggregate corruption scenarios
    scenario_results = defaultdict(lambda: {"passed": 0, "failed": 0, "total": 0})
    
    for version, vreport in report["version_results"].items():
        for scenario, sdata in vreport.get("corruption_scenarios", {}).items():
            scenario_results[scenario]["total"] += 1
            if sdata.get("recovered", False):
                scenario_results[scenario]["passed"] += 1
            else:
                scenario_results[scenario]["failed"] += 1
    
    report["corruption_scenario_results"] = {
        scenario: {
            "total_tests": data["total"],
            "passed": data["passed"],
            "failed": data["failed"],
            "success_rate": round(data["passed"] / data["total"] * 100, 2) if data["total"] > 0 else 0
        }
        for scenario, data in scenario_results.items()
    }
    
    # Generate recommendations based on patterns
    recommendations = generate_recommendations(report)
    report["recommendations"] = recommendations
    
    # Save report
    output_path = os.path.join(output_dir, output_file)
    with open(output_path, 'w') as f:
        json.dump(report, f, indent=2, default=str)
    
    print(f"Report generated: {output_path}")
    return report


def generate_recommendations(report: Dict) -> Dict[str, List[str]]:
    """Generate recommendations based on test results"""
    immediate = []
    short_term = []
    long_term = []
    
    # Check recovery scores
    for version, vreport in report.get("version_results", {}).items():
        score = vreport.get("recovery_score", {}).get("score", 0)
        if score < 50:
            immediate.append(f"CRITICAL: Version {version} recovery rate below 50%")
    
    # Check crash rates
    total_failures = report.get("auto_healing_loop", {}).get("total_failures", 0)
    if total_failures > 5:
        immediate.append(f"High crash rate detected: {total_failures} failures")
    
    # General recommendations
    if report.get("executive_summary", {}).get("recovery_confidence_score", 0) < 80:
        long_term.append("Consider adding more sophisticated recovery heuristics")
        long_term.append("Implement machine learning-based corruption pattern recognition")
    
    # Page-specific issues
    scenario_results = report.get("corruption_scenario_results", {})
    if "torn_page" in scenario_results:
        if scenario_results["torn_page"]["success_rate"] < 70:
            short_term.append("Improve torn page recovery - partial page reconstruction")
    
    if "metadata_annihilation" in scenario_results:
        if scenario_results["metadata_annihilation"]["success_rate"] < 50:
            short_term.append("Enhance metadata reconstruction from residual data")
    
    if "swiss_cheese" in scenario_results:
        if scenario_results["swiss_cheese"]["success_rate"] < 60:
            short_term.append("Improve random corruption recovery with checksum verification")
    
    # ASAN-specific recommendations
    if report.get("auto_healing_loop", {}).get("asan_analysis", {}).get("errors_detected", 0) > 0:
        long_term.append("Implement address sanitizer integration into CI pipeline")
        long_term.append("Add memory leak detection to automated testing")
    
    return {
        "immediate": immediate if immediate else ["No critical issues found"],
        "short_term": short_term if short_term else ["Recovery rates acceptable"],
        "long_term": long_term if long_term else ["Continue monitoring stability"]
    }


def print_summary(report: Dict):
    """Print human-readable summary"""
    print("\n" + "="*70)
    print("CHaos TESTING SUMMARY")
    print("="*70)
    
    summary = report.get("executive_summary", {})
    print(f"Recovery Confidence Score: {summary.get('recovery_confidence_score', 0)}%")
    print(f"Versions Tested: {len(summary.get('versions_tested', []))}")
    print(f"Data Integrity Verified: {summary.get('data_integrity_verified', False)}")
    
    print("\nVersion Results:")
    print("-"*70)
    for version, vreport in report.get("version_results", {}).items():
        score = vreport.get("recovery_score", {}).get("score", 0)
        level = vreport.get("recovery_score", {}).get("level", "N/A")
        status = vreport.get("status", "unknown")
        print(f"  {version}: {score:.1f} ({level}) [{status}]")
    
    print("\nCorruption Scenarios:")
    print("-"*70)
    for scenario, sdata in report.get("corruption_scenario_results", {}).items():
        rate = sdata.get("success_rate", 0)
        status = "✓" if rate >= 70 else "✗"
        print(f"  {status} {scenario}: {rate:.1f}% success rate")
    
    print("\n" + "="*70)


def main():
    parser = argparse.ArgumentParser(
        description='Generate Final Chaos Testing Report'
    )
    parser.add_argument('--output-dir', default='./recovery-output',
                       help='Output directory containing test results')
    parser.add_argument('--golden-hashes', default='./golden-hashes',
                       help='Directory containing golden hash files')
    parser.add_argument('--diagnosis-dir', default='./diagnosis',
                       help='Directory containing diagnosis reports')
    parser.add_argument('--output-file', default='chaos_testing_report.json',
                       help='Output report file name')
    parser.add_argument('--versions', nargs='+',
                       default=['5.0', '5.5', '5.6', '5.7', '8.0', '8.4', '9.0'],
                       help='MySQL versions to include in report')
    parser.add_argument('--json', action='store_true',
                       help='Output JSON only')
    
    args = parser.parse_args()
    
    report = generate_final_report(
        output_dir=args.output_dir,
        golden_hashes_dir=args.golden_hashes,
        diagnosis_dir=args.diagnosis_dir,
        output_file=args.output_file,
        versions=args.versions
    )
    
    if not args.json:
        print_summary(report)
    
    return report


if __name__ == '__main__':
    main()
