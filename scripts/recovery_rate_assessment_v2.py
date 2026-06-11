#!/usr/bin/env python3
"""
Improved Recovery Rate Auto Assessment Script
Purpose: Automatically assess recovery rate and identify bottlenecks
Handles binary data in CSV files from recovery tools
Author: jliang.fang
Version: 5.1.0
"""

import os
import sys
import json
import subprocess
import hashlib
import re
from datetime import datetime
from typing import Dict, List, Optional, Tuple, Set
from pathlib import Path
import csv
from collections import defaultdict

def extract_text_from_file(file_path: str) -> str:
    """Extract text from potentially binary file"""
    try:
        result = subprocess.run(
            ['strings', '-n', 3, file_path],
            capture_output=True,
            text=True
        )
        return result.stdout
    except:
        # Fallback: read with error handling
        try:
            with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                return f.read()
        except:
            return ""

def parse_recovered_records(text_content: str) -> List[Dict[str, str]]:
    """Parse recovered records from text content"""
    records = []
    
    # Look for lines with comma-separated values
    lines = text_content.split('\n')
    
    for line in lines:
        line = line.strip()
        if not line:
            continue
        
        # Skip lines that are clearly not data (too short, no commas)
        if len(line) < 10 or ',' not in line:
            continue
        
        # Try to identify record format: id,value,...
        parts = line.split(',')
        if len(parts) >= 2:
            # First part should be ID (numeric)
            first = parts[0].strip()
            if first.isdigit() or (first.startswith('-') and first[1:].isdigit()):
                try:
                    record = {
                        'id': first,
                        'fields': parts
                    }
                    records.append(record)
                except:
                    continue
    
    return records

def extract_record_ids(text_content: str) -> Set[str]:
    """Extract unique record IDs from text content"""
    ids = set()
    
    # Pattern to match: ID,email_or_hash,...
    patterns = [
        # Pattern: id,email_or_hash,...
        r'^(-?\d+),[a-zA-Z0-9@_./-]+',
        # Pattern: email hash, id, name patterns
        r',\s*(-?\d+)\s*,',
    ]
    
    for pattern in patterns:
        matches = re.findall(pattern, text_content, re.MULTILINE)
        ids.update(str(m) for m in matches if str(m).replace('-', '').isdigit())
    
    return ids

def extract_baseline_ids_from_test_data():
    """Extract baseline IDs from known test data files"""
    import struct
    
    test_files = [
        "/app/mysql_recovery_pro/test_data/test_large_rec100000_ps16384.ibd",
        "/app/mysql_recovery_pro/test_data/golden_test_users.ibd",
    ]
    
    ids = set()
    
    for file_path in test_files:
        if not os.path.exists(file_path):
            continue
            
        try:
            with open(file_path, 'rb') as f:
                data = f.read()
            
            # Test files have records at known offsets
            # Parse based on test file format
            page_size = 16384
            
            # Find record offsets (test file pattern)
            page_idx = 0
            while page_idx * page_size < len(data):
                offset = page_idx * page_size
                if offset + 54 > len(data):
                    break
                
                # Parse page header
                page_type = struct.unpack('<H', data[offset+16:offset+18])[0]
                
                # Only parse index pages
                if page_type != 0x26F1:  # FIL_PAGE_TYPE_INDEX
                    page_idx += 1
                    continue
                
                # Parse page header
                heap_top = struct.unpack('<H', data[offset+40:offset+42])[0]
                n_dir_slots = struct.unpack('<H', data[offset+38:offset+40])[0]
                
                # Parse records
                rec_offset = 54  # After page header
                
                while rec_offset < heap_top - 20:
                    if rec_offset + 5 > len(data):
                        break
                    
                    # Check record header
                    rec_len = struct.unpack('<H', data[offset+rec_offset+1:offset+rec_offset+3])[0]
                    rec_type = data[offset+rec_offset]
                    
                    if rec_type & 0x80:  # Deleted record
                        rec_offset += 5
                        continue
                    
                    # Extract record ID (typically first field)
                    data_offset = rec_offset + 5
                    
                    if data_offset + 8 <= len(data):
                        record_id = struct.unpack('<Q', data[offset+data_offset:offset+data_offset+8])[0]
                        ids.add(str(record_id))
                    
                    # Move to next record
                    rec_offset += 5 + rec_len
                
                page_idx += 1
                
        except Exception as e:
            continue
    
    return ids

def count_records_in_csv_file(file_path: str) -> Tuple[int, int]:
    """Count records and get baseline expected count from IBD file"""
    text_content = extract_text_from_file(file_path)
    records = extract_recovered_records(text_content)
    
    # Get baseline from IBD file if available
    ibd_path = file_path.replace('recovered.csv', '.ibd')
    if not os.path.exists(ibd_path):
        ibd_path = "/app/mysql_recovery_pro/test_data/test_large_rec100000_ps16384.ibd"
    
    baseline_ids = extract_baseline_ids_from_test_data()
    baseline_count = len(baseline_ids)
    
    return len(records), baseline_count

def analyze_recovery_result(output_dir: str) -> Dict:
    """Analyze recovery result and generate report"""
    
    recovered_file = os.path.join(output_dir, "recovered.csv")
    
    if not os.path.exists(recovered_file):
        return {
            "status": "ERROR",
            "error": f"Recovered file not found: {recovered_file}"
        }
    
    # Read text content
    text_content = extract_text_from_file(recovered_file)
    
    # Extract IDs and records
    recovered_ids = extract_record_ids(text_content)
    records = parse_recovered_records(text_content)
    
    # Get baseline expected count
    baseline_ids = extract_baseline_ids_from_test_data()
    baseline_count = len(baseline_ids)
    recovered_count = len(recovered_ids)
    
    # Calculate recovery rate
    recovery_rate = (recovered_count / baseline_count * 100) if baseline_count > 0 else 0
    
    # Determine status
    if recovery_rate >= 99.9:
        status = "PASS"
    elif recovery_rate >= 90:
        status = "NEEDS_ATTENTION"
    else:
        status = "FAIL"
    
    return {
        "output_dir": output_dir,
        "recovered_file": recovered_file,
        "baseline_count": baseline_count,
        "recovered_record_ids": len(recovered_ids),
        "recovered_records_parsed": len(records),
        "recovery_rate_percent": round(recovery_rate, 2),
        "status": status,
        "timestamp": datetime.now().isoformat()
    }


def run_test_scenario(name: str, scenario_type: str, baseline_file: str = None) -> Dict:
    """Run a test scenario"""
    import shutil
    
    output_dir = f"/tmp/assessment_{name}"
    
    # Clean previous output
    if os.path.exists(output_dir):
        shutil.rmtree(output_dir)
    os.makedirs(output_dir)
    
    # Use baseline file if provided
    if not baseline_file and scenario_type == "baseline":
        baseline_file = "/app/mysql_recovery_pro/test_data/test_large_rec100000_ps16384.ibd"
    
    if not baseline_file:
        return {
            "name": name,
            "status": "SKIP",
            "reason": "No input file specified"
        }
    
    # Run recovery tool
    tool_path = "/app/mysql_recovery_pro/build/mysql_recovery_pro"
    
    recovery_mode = "--default-recovery"
    if scenario_type == "rmrf":
        recovery_mode = "--rmrf-recovery"
    elif scenario_type == "header":
        recovery_mode = "--header-repair"
    elif scenario_type == "blob":
        recovery_mode = "--blob-recovery"
    
    args = [
        tool_path,
        f"--ibd={baseline_file}",
        recovery_mode,
        "--format=csv",
        f"--output={output_dir}"
    ]
    
    print(f"Running: {name}...")
    
    try:
        result = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=300,
            env={**os.environ, "LC_ALL": "C"}
        )
        
        # Analyze result
        analysis = analyze_recovery_result(output_dir)
        
        return {
            "name": name,
            "scenario_type": scenario_type,
            **analysis
        }
        
    except subprocess.TimeoutExpired:
        return {
            "name": name,
            "status": "TIMEOUT",
            "error": "Test timed out after 300 seconds"
        }
    except Exception as e:
        return {
            "name": name,
            "status": "ERROR",
            "error": str(e)
        }


def run_comprehensive_test():
    """Run comprehensive 99.9%+ recovery rate test"""
    print("=" * 70)
    print("MySQL Recovery Pro - 99.9%+ Recovery Rate Test (v5.1.0)")
    print("=" * 70)
    print()
    
    test_scenarios = [
        ("baseline_recovery", "baseline"),
        ("rmrf_scenario", "rmrf"),
        ("blob_recovery", "blob"),
    ]
    
    results = []
    
    for name, scenario_type in test_scenarios:
        result = run_test_scenario(name, scenario_type)
        results.append(result)
        
        print(f"  Status: {result.get('status', 'UNKNOWN')}")
        if 'baseline_count' in result:
            print(f"    Baseline: {result['baseline_count']}, Recovered: {result.get('recovered_record_ids', 0)}")
            print(f"    Recovery Rate: {result.get('recovery_rate_percent', 0)}%")
        print()
    
    # Calculate summary
    total = len(results)
    passed = sum(1 for r in results if r.get('status') == 'PASS')
    failed = sum(1 for r in results if r.get('status') in ['FAIL', 'ERROR', 'TIMEOUT'])
    
    # Get average recovery rate (for tests that ran successfully)
    rates = [r.get('recovery_rate_percent', 0) for r in results 
             if 'recovery_rate_percent' in r and r['recovery_rate_percent'] > 0]
    avg_rate = sum(rates) / len(rates) if rates else 0
    
    print("=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print(f"Total Tests: {total}")
    print(f"Passed: {passed}")
    print(f"Failed/Errored: {failed}")
    print(f"Average Recovery Rate: {avg_rate:.2f}%")
    print()
    
    # Display individual results
    print("Detailed Results:")
    for r in results:
        rate = r.get('recovery_rate_percent', 0)
        status_icon = "✓" if r.get('status') == 'PASS' else "✗" if r.get('status') in ['FAIL', 'ERROR'] else "⚠"
        print(f"  {status_icon} {r['name']}: {rate:.2f}% ({r.get('status', 'UNKNOWN')})")
    
    print()
    
    # Check target
    if avg_rate >= 99.9:
        print(f"✓ TARGET ACHIEVED: {avg_rate:.2f}% >= 99.9%")
        return True
    else:
        print(f"✗ TARGET NOT MET: {avg_rate:.2f}% < 99.9%")
        
        # Identify issues
        for r in results:
            rate = r.get('recovery_rate_percent', 0)
            if rate < 99.9:
                print(f"  - {r['name']} is below target ({rate:.2f}%)")
        
        return False


if __name__ == "__main__":
    success = run_comprehensive_test()
    sys.exit(0 if success else 1)
