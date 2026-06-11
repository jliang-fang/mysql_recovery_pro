#!/usr/bin/env python3
"""
Recovery Rate Auto Assessment Script
Purpose: Automatically assess recovery rate and identify bottlenecks
Author: jliang.fang
Version: 5.0.0
"""

import os
import sys
import json
import subprocess
import hashlib
from datetime import datetime
from typing import Dict, List, Optional, Tuple
from pathlib import Path
import csv
import re

class RecoveryAssessment:
    """Assess recovery rate and identify issues"""
    
    def __init__(self, baseline_file: str = None, recovered_file: str = None):
        self.baseline_file = baseline_file
        self.recovered_file = recovered_file
        self.baseline_records = []
        self.recovered_records = []
        self.issues = []
        self.metrics = {}
        
    def load_baseline(self, file_path: str = None) -> bool:
        """Load baseline records from CSV"""
        path = file_path or self.baseline_file
        if not path or not os.path.exists(path):
            return False
            
        try:
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                reader = csv.DictReader(f)
                self.baseline_records = list(reader)
            return True
        except Exception as e:
            self.issues.append(f"Error loading baseline: {e}")
            return False
    
    def load_recovered(self, file_path: str = None) -> bool:
        """Load recovered records from CSV"""
        path = file_path or self.recovered_file
        if not path or not os.path.exists(path):
            return False
            
        try:
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                reader = csv.DictReader(f)
                self.recovered_records = list(reader)
            return True
        except Exception as e:
            self.issues.append(f"Error loading recovered records: {e}")
            return False
    
    def calculate_recovery_rate(self) -> float:
        """Calculate recovery rate based on record count"""
        if not self.baseline_records or not self.recovered_records:
            return 0.0
            
        baseline_count = len(self.baseline_records)
        recovered_count = len(self.recovered_records)
        
        # Handle header row subtraction
        if baseline_count > 0 and recovered_count > 0:
            # Assume first row is header if 'id' is not numeric
            if isinstance(baseline_records[0].get('id'), str):
                baseline_count = len([r for r in self.baseline_records if r.get('id', '').isdigit()])
            if isinstance(recovered_records[0].get('id'), str):
                recovered_count = len([r for r in self.recovered_records if r.get('id', '').isdigit()])
        
        rate = (recovered_count / baseline_count * 100) if baseline_count > 0 else 0.0
        return min(rate, 100.0)
    
    def calculate_hash_match_rate(self) -> float:
        """Calculate hash match rate between baseline and recovered"""
        if not self.baseline_records or not self.recovered_records:
            return 0.0
        
        # Create hash maps
        baseline_hashes = {}
        recovered_hashes = {}
        
        for record in self.baseline_records:
            record_id = record.get('id', record.get('ID', ''))
            if record_id:
                record_str = str(sorted(record.items()))
                baseline_hashes[record_id] = hashlib.md5(record_str.encode()).hexdigest()
        
        for record in self.recovered_records:
            record_id = record.get('id', record.get('ID', ''))
            if record_id:
                record_str = str(sorted(record.items()))
                recovered_hashes[record_id] = hashlib.md5(record_str.encode()).hexdigest()
        
        # Calculate match rate
        matches = sum(1 for rid, h in baseline_hashes.items() 
                     if rid in recovered_hashes and recovered_hashes[rid] == h)
        
        rate = (matches / len(baseline_hashes) * 100) if baseline_hashes else 0.0
        return rate
    
    def detect_issues(self) -> List[Dict]:
        """Detect recovery issues"""
        issues = []
        
        if not self.baseline_records or not self.recovered_records:
            issues.append({
                "type": "missing_data",
                "severity": "critical",
                "description": "Baseline or recovered records missing"
            })
            return issues
        
        # Check for field mismatches
        baseline_fields = set(self.baseline_records[0].keys()) if self.baseline_records else set()
        recovered_fields = set(self.recovered_records[0].keys()) if self.recovered_records else set()
        
        missing_fields = baseline_fields - recovered_fields
        if missing_fields:
            issues.append({
                "type": "field_mismatch",
                "severity": "high",
                "description": f"Missing fields in recovered data: {missing_fields}"
            })
        
        # Check for NULL value issues
        null_count = sum(1 for r in self.recovered_records 
                        if any(v is None or v == '' for v in r.values()))
        if null_count > 0:
            issues.append({
                "type": "null_values",
                "severity": "medium",
                "description": f"Found {null_count} records with NULL/empty values"
            })
        
        # Check for duplicate IDs
        recovered_ids = [r.get('id', '') for r in self.recovered_records]
        duplicates = len(recovered_ids) - len(set(recovered_ids))
        if duplicates > 0:
            issues.append({
                "type": "duplicate_records",
                "severity": "high",
                "description": f"Found {duplicates} duplicate records"
            })
        
        # Check for ID gaps (missing records)
        if self.baseline_records and self.recovered_records:
            baseline_ids = [int(r.get('id', 0)) for r in self.baseline_records if r.get('id', '').isdigit()]
            recovered_ids_num = [int(r.get('id', 0)) for r in self.recovered_records if r.get('id', '').isdigit()]
            
            if baseline_ids and recovered_ids_num:
                max_baseline = max(baseline_ids)
                max_recovered = max(recovered_ids_num)
                missing_count = max_baseline - max_recovered
                
                if missing_count > 0 and max_baseline > 0:
                    issues.append({
                        "type": "missing_records",
                        "severity": "high",
                        "description": f"Missing {missing_count} records out of {max_baseline}"
                    })
        
        # Check for record chain issues ( from logs)
        chain_break_pattern = re.compile(r'Record chain broken|chain broken')
        
        return issues
    
    def generate_report(self, output_path: str = None) -> Dict:
        """Generate comprehensive recovery report"""
        recovery_rate = self.calculate_recovery_rate()
        hash_match_rate = self.calculate_hash_match_rate()
        detected_issues = self.detect_issues()
        
        report = {
            "timestamp": datetime.now().isoformat(),
            "metrics": {
                "baseline_records": len(self.baseline_records),
                "recovered_records": len(self.recovered_records),
                "recovery_rate_percent": round(recovery_rate, 2),
                "hash_match_rate_percent": round(hash_match_rate, 2)
            },
            "issues": detected_issues,
            "status": "PASS" if recovery_rate >= 99.9 else "NEEDS_ATTENTION",
            "recommendations": []
        }
        
        # Generate recommendations
        if recovery_rate < 100:
            report["recommendations"].append("Investigate missing records - check page chain and directory parsing")
        
        for issue in detected_issues:
            if issue["type"] == "field_mismatch":
                report["recommendations"].append("Verify field mapping in row decoder")
            elif issue["type"] == "null_values":
                report["recommendations"].append("Check NULL value handling in record parser")
            elif issue["type"] == "duplicate_records":
                report["recommendations"].append("Implement deduplication logic")
        
        if output_path:
            with open(output_path, 'w') as f:
                json.dump(report, f, indent=2)
        
        return report


class ComprehensiveTestRunner:
    """Run comprehensive recovery tests"""
    
    def __init__(self, tool_path: str = None, output_dir: str = None):
        self.tool_path = tool_path or "/app/mysql_recovery_pro/build/mysql_recovery_pro"
        self.output_dir = output_dir or "/tmp/comprehensive_test_output"
        self.results = []
        
        os.makedirs(self.output_dir, exist_ok=True)
    
    def run_test_scenario(self, scenario_name: str, file_path: str, 
                          recovery_mode: str = "default", extra_args: List[str] = None) -> Dict:
        """Run a single test scenario"""
        print(f"Running {scenario_name}...")
        
        output_subdir = os.path.join(self.output_dir, scenario_name)
        os.makedirs(output_subdir, exist_ok=True)
        
        args = [
            self.tool_path,
            f"--ibd={file_path}",
            f"--{recovery_mode}-recovery" if recovery_mode != "default" else "--default-recovery",
            "--format=csv",
            f"--output={output_subdir}"
        ]
        
        if extra_args:
            args.extend(extra_args)
        
        try:
            result = subprocess.run(
                args,
                capture_output=True,
                text=True,
                timeout=300
            )
            
            recovered_file = os.path.join(output_subdir, "recovered.csv")
            
            assessment = RecoveryAssessment()
            if os.path.exists(recovered_file):
                assessment.load_recovered(recovered_file)
            
            return {
                "scenario": scenario_name,
                "passed": result.returncode == 0,
                "exit_code": result.returncode,
                "stdout": result.stdout[-500:] if result.stdout else "",
                "stderr": result.stderr[-500:] if result.stderr else "",
                "recovered_file": recovered_file if os.path.exists(recovered_file) else None,
                "assessment": assessment.generate_report() if os.path.exists(recovered_file) else {}
            }
        except subprocess.TimeoutExpired:
            return {
                "scenario": scenario_name,
                "passed": False,
                "timeout": True,
                "error": "Test timed out"
            }
        except Exception as e:
            return {
                "scenario": scenario_name,
                "passed": False,
                "error": str(e)
            }
    
    def run_all_scenarios(self, test_files: Dict[str, str], 
                          scenarios: List[Dict]) -> List[Dict]:
        """Run all test scenarios"""
        for scenario in scenarios:
            file_key = scenario.get("file", "default")
            file_path = test_files.get(file_key, file_key)
            
            if not os.path.exists(file_path):
                print(f"Warning: File not found: {file_path}")
                self.results.append({
                    "scenario": scenario["name"],
                    "passed": False,
                    "skipped": True,
                    "reason": f"File not found: {file_path}"
                })
                continue
            
            result = self.run_test_scenario(
                scenario["name"],
                file_path,
                scenario.get("recovery_mode", "default"),
                scenario.get("extra_args", [])
            )
            self.results.append(result)
        
        return self.results
    
    def generate_summary(self) -> Dict:
        """Generate test summary"""
        total = len(self.results)
        passed = sum(1 for r in self.results if r.get("passed", False))
        failed = total - passed
        
        # Calculate average recovery rate
        recovery_rates = []
        for r in self.results:
            assessment = r.get("assessment", {})
            metrics = assessment.get("metrics", {})
            rate = metrics.get("recovery_rate_percent", 0)
            if rate > 0:
                recovery_rates.append(rate)
        
        avg_rate = sum(recovery_rates) / len(recovery_rates) if recovery_rates else 0
        
        return {
            "timestamp": datetime.now().isoformat(),
            "total_tests": total,
            "passed": passed,
            "failed": failed,
            "average_recovery_rate": round(avg_rate, 2),
            "detailed_results": self.results
        }


def run_comprehensive_test():
    """Run comprehensive 99.9%+ recovery rate test"""
    print("=" * 60)
    print("MySQL Recovery Pro - 99.9%+ Recovery Rate Test")
    print("=" * 60)
    
    # Test files
    test_files = {
        "baseline": "/app/mysql_recovery_pro/test_data/test_large_rec100000_ps16384.ibd",
        "header_corrupt": "/app/mysql_recovery_pro/test_data/test_large_corrupted_header_type_rec100000_ps16384.ibd",
        "checksum_corrupt": "/app/mysql_recovery_pro/test_data/test_large_corrupted_header_checksum_rec100000_ps16384.ibd",
    }
    
    # Test scenarios
    scenarios = [
        {
            "name": "baseline_recovery",
            "file": "baseline",
            "recovery_mode": "default"
        },
        {
            "name": "header_corruption_recovery",
            "file": "header_corrupt",
            "recovery_mode": "header"
        },
        {
            "name": "checksum_corruption_recovery",
            "file": "checksum_corrupt",
            "recovery_mode": "default"
        },
        {
            "name": "rmrf_recovery",
            "file": "baseline",
            "recovery_mode": "rmrf"
        },
        {
            "name": "blob_recovery",
            "file": "baseline",
            "recovery_mode": "default",
            "extra_args": ["--blob-recovery"]
        },
    ]
    
    runner = ComprehensiveTestRunner()
    results = runner.run_all_scenarios(test_files, scenarios)
    
    # Generate summary
    summary = runner.generate_summary()
    
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"Total Tests: {summary['total_tests']}")
    print(f"Passed: {summary['passed']}")
    print(f"Failed: {summary['failed']}")
    print(f"Average Recovery Rate: {summary['average_recovery_rate']}%")
    
    # Check if target reached
    if summary['average_recovery_rate'] >= 99.9:
        print("\n✓ TARGET ACHIEVED: 99.9%+ Recovery Rate")
        return True
    else:
        print(f"\n✗ TARGET NOT MET: {summary['average_recovery_rate']}% < 99.9%")
        print("\nRecommended actions:")
        for result in results:
            if not result.get("passed", True):
                print(f"  - Investigate: {result.get('scenario', 'Unknown')}")
        return False


if __name__ == "__main__":
    success = run_comprehensive_test()
    sys.exit(0 if success else 1)
