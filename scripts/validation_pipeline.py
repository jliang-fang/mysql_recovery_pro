#!/usr/bin/env python3
"""
Closed-Loop Validation Framework for MySQL Recovery Pro
Compares recovered data against ground truth hashes
"""

import os
import sys
import json
import hashlib
import subprocess
import argparse
from datetime import datetime
from typing import List, Dict, Tuple, Optional
from pathlib import Path
import csv
import re


class HashCalculator:
    """Calculate hashes for verification"""
    
    @staticmethod
    def calculate_file_hash(filepath: str, algorithm: str = "sha256") -> str:
        """Calculate hash of a file"""
        hash_func = hashlib.new(algorithm)
        
        with open(filepath, 'rb') as f:
            for chunk in iter(lambda: f.read(8192), b''):
                hash_func.update(chunk)
        
        return hash_func.hexdigest()
    
    @staticmethod
    def calculate_csv_hash(filepath: str) -> str:
        """Calculate hash of CSV file (excluding header)"""
        hash_func = hashlib.sha256()
        
        with open(filepath, 'r', encoding='utf-8') as f:
            reader = csv.reader(f)
            header = next(reader, None)  # Skip header
            
            for row in reader:
                row_str = ','.join(str(cell) for cell in row)
                hash_func.update(row_str.encode('utf-8'))
        
        return hash_func.hexdigest()
    
    @staticmethod
    def calculate_json_hash(filepath: str) -> str:
        """Calculate hash of JSON file"""
        with open(filepath, 'r', encoding='utf-8') as f:
            data = json.load(f)
        
        # Normalize and hash
        json_str = json.dumps(data, sort_keys=True, ensure_ascii=False)
        return hashlib.sha256(json_str.encode()).hexdigest()
    
    @staticmethod
    def calculate_sql_hash(filepath: str) -> str:
        """Calculate hash of SQL file (INSERT statements)"""
        hash_func = hashlib.sha256()
        
        with open(filepath, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if line.startswith('INSERT INTO'):
                    hash_func.update(line.encode('utf-8'))
        
        return hash_func.hexdigest()


class DataValidator:
    """Validate recovered data against ground truth"""
    
    def __init__(self, ground_truth_file: str = "ground_truth.json"):
        self.ground_truth = self.load_ground_truth(ground_truth_file)
    
    def load_ground_truth(self, filepath: str) -> Dict:
        """Load ground truth from file"""
        if not os.path.exists(filepath):
            return {}
        
        with open(filepath, 'r') as f:
            return json.load(f)
    
    def compare_hashes(self, recovered_hash: str, expected_hash: str, 
                      tolerance: float = 0.0) -> Tuple[bool, float]:
        """Compare two hashes with optional tolerance"""
        if recovered_hash == expected_hash:
            return True, 1.0
        
        # For partial match (if needed)
        if tolerance > 0:
            # Calculate similarity (simple character-by-character)
            matches = sum(1 for a, b in zip(recovered_hash, expected_hash) if a == b)
            similarity = matches / max(len(recovered_hash), len(expected_hash))
            return similarity >= tolerance, similarity
        
        return False, 0.0
    
    def validate_csv(self, csv_file: str, expected_count: int = None) -> Dict:
        """Validate recovered CSV file"""
        result = {
            "file": csv_file,
            "exists": os.path.exists(csv_file),
            "hash": None,
            "record_count": 0,
            "valid": False,
            "errors": []
        }
        
        if not result["exists"]:
            result["errors"].append("File does not exist")
            return result
        
        try:
            # Calculate hash
            result["hash"] = HashCalculator.calculate_csv_hash(csv_file)
            
            # Count records
            with open(csv_file, 'r', encoding='utf-8') as f:
                reader = csv.reader(f)
                header = next(reader, None)
                result["record_count"] = sum(1 for _ in reader)
            
            # Validate record count
            if expected_count and result["record_count"] < expected_count:
                result["errors"].append(
                    f"Record count mismatch: {result['record_count']} < {expected_count}"
                )
            
            result["valid"] = len(result["errors"]) == 0
            
        except Exception as e:
            result["errors"].append(str(e))
        
        return result
    
    def validate_json(self, json_file: str) -> Dict:
        """Validate recovered JSON file"""
        result = {
            "file": json_file,
            "exists": os.path.exists(json_file),
            "hash": None,
            "valid": False,
            "errors": []
        }
        
        if not result["exists"]:
            result["errors"].append("File does not exist")
            return result
        
        try:
            result["hash"] = HashCalculator.calculate_json_hash(json_file)
            result["valid"] = len(result["errors"]) == 0
        except Exception as e:
            result["errors"].append(str(e))
        
        return result
    
    def validate_sql(self, sql_file: str) -> Dict:
        """Validate recovered SQL file"""
        result = {
            "file": sql_file,
            "exists": os.path.exists(sql_file),
            "hash": None,
            "insert_count": 0,
            "valid": False,
            "errors": []
        }
        
        if not result["exists"]:
            result["errors"].append("File does not exist")
            return result
        
        try:
            result["hash"] = HashCalculator.calculate_sql_hash(sql_file)
            
            # Count INSERT statements
            with open(sql_file, 'r', encoding='utf-8') as f:
                content = f.read()
                result["insert_count"] = len(re.findall(r'INSERT INTO', content))
            
            result["valid"] = len(result["errors"]) == 0
            
        except Exception as e:
            result["errors"].append(str(e))
        
        return result


class RecoveryValidator:
    """Validate recovery results"""
    
    def __init__(self, build_dir: str = "build", ground_truth_dir: str = "."):
        self.build_dir = Path(build_dir)
        self.ground_truth_dir = Path(ground_truth_dir)
        self.results: List[Dict] = []
        
        # Recovery binaries
        self.binaries = {
            'mysql_recovery_pro': self.build_dir / 'mysql_recovery_pro',
            'mysql_recover_cli': self.build_dir / 'mysql_recover_cli',
        }
    
    def check_binary(self, name: str) -> bool:
        """Check if recovery binary exists"""
        binary = self.binaries.get(name)
        return binary and binary.exists() and os.access(binary, os.X_OK)
    
    def run_recovery(self, ibd_file: str, output_dir: str, 
                    format_type: str = "csv", **kwargs) -> Tuple[bool, str]:
        """Run recovery tool"""
        binary = self.binaries.get('mysql_recovery_pro')
        if not binary:
            return False, "Recovery binary not found"
        
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
        
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=300
            )
            
            if result.returncode == 0:
                return True, result.stdout
            else:
                return False, f"stderr: {result.stderr}"
                
        except subprocess.TimeoutExpired:
            return False, "Command timed out"
        except Exception as e:
            return False, str(e)
    
    def validate_recovery(self, ibd_file: str, output_dir: str,
                         expected_count: int = None,
                         format_type: str = "csv") -> Dict:
        """Validate a recovery operation"""
        result = {
            "ibd_file": ibd_file,
            "output_dir": output_dir,
            "format": format_type,
            "recovery_success": False,
            "validation_results": {},
            "overall_passed": False,
            "timestamp": datetime.now().isoformat()
        }
        
        # Run recovery
        success, message = self.run_recovery(
            ibd_file, output_dir, format_type
        )
        result["recovery_success"] = success
        result["recovery_message"] = message
        
        if not success:
            return result
        
        # Validate output
        validator = DataValidator()
        
        if format_type == "csv":
            csv_file = os.path.join(output_dir, "recovered.csv")
            result["validation_results"]["csv"] = validator.validate_csv(
                csv_file, expected_count
            )
        elif format_type == "json":
            json_file = os.path.join(output_dir, "recovered.json")
            result["validation_results"]["json"] = validator.validate_json(
                json_file
            )
        elif format_type == "sql":
            sql_file = os.path.join(output_dir, "recovered.sql")
            result["validation_results"]["sql"] = validator.validate_sql(
                sql_file
            )
        
        # Determine overall pass/fail
        result["overall_passed"] = all(
            v.get("valid", False) 
            for v in result["validation_results"].values()
        )
        
        return result
    
    def validate_corrupted_recovery(self, ibd_file: str, output_dir: str,
                                   scenario: str, ground_truth_hash: str = None) -> Dict:
        """Validate recovery of corrupted file"""
        result = {
            "ibd_file": ibd_file,
            "scenario": scenario,
            "output_dir": output_dir,
            "ground_truth_hash": ground_truth_hash,
            "recovered_hash": None,
            "hash_match": False,
            "recovery_success": False,
            "timestamp": datetime.now().isoformat()
        }
        
        # Run recovery
        success, message = self.run_recovery(ibd_file, output_dir, "csv")
        result["recovery_success"] = success
        
        if not success:
            result["error_message"] = message
            return result
        
        # Calculate recovered hash
        csv_file = os.path.join(output_dir, "recovered.csv")
        if os.path.exists(csv_file):
            result["recovered_hash"] = HashCalculator.calculate_csv_hash(csv_file)
            
            if ground_truth_hash:
                result["hash_match"] = result["recovered_hash"] == ground_truth_hash
        
        return result


class ValidationPipeline:
    """Run complete validation pipeline"""
    
    def __init__(self, build_dir: str = "build", 
                 test_data_dir: str = "test_data",
                 results_dir: str = "test_results"):
        self.build_dir = Path(build_dir)
        self.test_data_dir = Path(test_data_dir)
        self.results_dir = Path(results_dir)
        self.results_dir.mkdir(parents=True, exist_ok=True)
        
        self.validator = RecoveryValidator(build_dir)
        self.all_results: List[Dict] = []
    
    def run_basic_recovery_test(self) -> Dict:
        """Run basic recovery test"""
        print("Running basic recovery test...")
        
        # Find test file
        test_files = list(self.test_data_dir.glob("test_table*.ibd"))
        if not test_files:
            return {"error": "No test files found"}
        
        test_file = str(test_files[0])
        output_dir = self.results_dir / "basic_recovery"
        
        result = self.validator.validate_recovery(
            test_file, str(output_dir), expected_count=100
        )
        
        self.all_results.append(result)
        return result
    
    def run_corruption_test(self, scenario: str, ground_truth_hash: str = None) -> Dict:
        """Run corruption recovery test"""
        print(f"Running corruption test: {scenario}...")
        
        # Find test file
        test_files = list(self.test_data_dir.glob("test_corrupted*.ibd"))
        if not test_files:
            return {"error": "No corrupted test files found"}
        
        test_file = str(test_files[0])
        output_dir = self.results_dir / f"corruption_{scenario}"
        
        result = self.validator.validate_corrupted_recovery(
            test_file, str(output_dir), scenario, ground_truth_hash
        )
        
        self.all_results.append(result)
        return result
    
    def run_version_compatibility_test(self, version: str) -> Dict:
        """Run version compatibility test"""
        print(f"Running version compatibility test: MySQL {version}...")
        
        # Find version-specific test file
        pattern = f"test_table_v{version.replace('.', '')}_*.ibd"
        test_files = list(self.test_data_dir.glob(pattern))
        
        if not test_files:
            return {"error": f"No test files for MySQL {version}"}
        
        test_file = str(test_files[0])
        output_dir = self.results_dir / f"version_{version}"
        
        result = self.validator.validate_recovery(
            test_file, str(output_dir), expected_count=100
        )
        
        self.all_results.append(result)
        return result
    
    def run_all_tests(self) -> Dict:
        """Run all validation tests"""
        print("=" * 60)
        print("MySQL Recovery Pro - Validation Pipeline")
        print("=" * 60)
        print()
        
        results = {
            "timestamp": datetime.now().isoformat(),
            "tests": [],
            "summary": {
                "total": 0,
                "passed": 0,
                "failed": 0
            }
        }
        
        # Run basic recovery test
        result = self.run_basic_recovery_test()
        results["tests"].append(result)
        
        # Run version compatibility tests
        versions = ["56", "57", "80", "84", "90"]
        for version in versions:
            result = self.run_version_compatibility_test(version)
            results["tests"].append(result)
        
        # Calculate summary
        results["summary"]["total"] = len(results["tests"])
        results["summary"]["passed"] = sum(
            1 for t in results["tests"] 
            if t.get("overall_passed", False) or t.get("hash_match", False)
        )
        results["summary"]["failed"] = results["summary"]["total"] - results["summary"]["passed"]
        
        # Save results
        results_file = self.results_dir / "validation_results.json"
        with open(results_file, 'w') as f:
            json.dump(results, f, indent=2)
        
        print()
        print("=" * 60)
        print("Validation Summary")
        print("=" * 60)
        print(f"Total tests: {results['summary']['total']}")
        print(f"Passed: {results['summary']['passed']}")
        print(f"Failed: {results['summary']['failed']}")
        print()
        print(f"Results saved to: {results_file}")
        
        return results


def main():
    parser = argparse.ArgumentParser(
        description='Validation Pipeline for MySQL Recovery Pro'
    )
    parser.add_argument('--build-dir', default='build',
                       help='Build directory')
    parser.add_argument('--test-data-dir', default='test_data',
                       help='Test data directory')
    parser.add_argument('--results-dir', default='test_results',
                       help='Results directory')
    parser.add_argument('--scenario', '-s',
                       choices=['basic', 'corruption', 'version', 'all'],
                       default='all',
                       help='Test scenario')
    parser.add_argument('--version', '-v',
                       help='MySQL version for version test')
    parser.add_argument('--ground-truth', '-g',
                       help='Ground truth hash file')
    parser.add_argument('--ibd-file', '-i',
                       help='IBD file to test')
    parser.add_argument('--output-dir', '-o',
                       help='Output directory')
    
    args = parser.parse_args()
    
    pipeline = ValidationPipeline(
        build_dir=args.build_dir,
        test_data_dir=args.test_data_dir,
        results_dir=args.results_dir
    )
    
    if args.scenario == 'basic':
        result = pipeline.run_basic_recovery_test()
    elif args.scenario == 'corruption':
        ground_truth = None
        if args.ground_truth:
            with open(args.ground_truth) as f:
                ground_truth = json.load(f).get('hashes', {}).get('test_users')
        result = pipeline.run_corruption_test(args.scenario, ground_truth)
    elif args.scenario == 'version':
        result = pipeline.run_version_compatibility_test(args.version)
    else:
        result = pipeline.run_all_tests()
    
    # Print detailed results
    print()
    print("Detailed Results:")
    print("-" * 60)
    
    if isinstance(result, dict) and 'tests' in result:
        for test in result['tests']:
            status = "PASS" if test.get('overall_passed', False) else "FAIL"
            print(f"{status}: {test.get('ibd_file', 'N/A')}")
            if 'error' in test:
                print(f"  Error: {test['error']}")
    else:
        status = "PASS" if result.get('overall_passed', False) else "FAIL"
        print(f"{status}: {result.get('ibd_file', 'N/A')}")


if __name__ == '__main__':
    main()
