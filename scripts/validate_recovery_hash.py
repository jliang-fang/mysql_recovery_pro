#!/usr/bin/env python3
# =============================================================================
# MySQL Recovery Validation - SHA-256 Hash Comparison
# Purpose: Validate 99.9%+ recovery rate by comparing recovered data against golden hash
# =============================================================================

import os
import sys
import json
import hashlib
import csv
import argparse
from datetime import datetime
from typing import Dict, List, Tuple


def calculate_record_hashes(record_data: dict) -> str:
    """Calculate SHA-256 hash for a single record"""
    # Create canonical representation
    record_str = f"{record_data['id']},{record_data['user_id']},{record_data['email']},{record_data['name']}"
    return hashlib.sha256(record_str.encode()).hexdigest()


def generate_golden_hashes(csv_path: str, output_path: str) -> dict:
    """Generate golden hash database from CSV export"""
    print(f"[*] Reading golden data from: {csv_path}")
    
    golden_data = {
        'metadata': {
            'generated_at': datetime.now().isoformat(),
            'source_file': csv_path,
            'total_records': 0,
            'hash_type': 'SHA-256'
        },
        'record_hashes': []
    }
    
    file_size = os.path.getsize(csv_path)
    expected_records = file_size // 100  # Approximate records per line
    
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f, fieldnames=['id', 'user_id', 'email', 'name', 'created_at'])
        
        for i, row in enumerate(reader):
            if i % 100000 == 0:
                print(f"    Processed {i:,} records...")
            
            record_hash = calculate_record_hashes(row)
            golden_data['record_hashes'].append({
                'record_id': int(row['id']),
                'sha256': record_hash,
                'user_id': row['user_id'],
                'email': row['email']
            })
    
    golden_data['metadata']['total_records'] = len(golden_data['record_hashes'])
    
    # Save to JSON
    with open(output_path, 'w') as f:
        json.dump(golden_data, f, indent=2)
    
    print(f"[+] Generated {len(golden_data['record_hashes']):,} hashes to: {output_path}")
    return golden_data


def load_golden_hashes(hash_file: str) -> dict:
    """Load golden hash database from JSON file"""
    with open(hash_file, 'r') as f:
        return json.load(f)


def validate_recovered_csv(recovered_csv: str, golden_hashes: dict) -> dict:
    """Validate recovered CSV against golden hashes"""
    print(f"[*] Validating recovered data: {recovered_csv}")
    
    results = {
        'validation_time': datetime.now().isoformat(),
        'recovered_file': recovered_csv,
        'golden_file': golden_hashes.get('metadata', {}).get('source_file', 'unknown'),
        'metadata': {
            'total_golden': len(golden_hashes['record_hashes']),
            'unique_golden_hashes': len(set(h['sha256'] for h in golden_hashes['record_hashes'])),
            'total_recovered': 0,
            'matched': 0,
            'missing': 0,
            'mismatched': 0
        },
        'recovered_hashes': set(),
        'golden_hash_set': set(h['sha256'] for h in golden_hashes['record_hashes']),
        'recovered_by_id': {},
        'golden_by_id': {h['record_id']: h['sha256'] for h in golden_hashes['record_hashes']},
        'mismatches': [],
        'missing_ids': []
    }
    
    # Load recovered data
    with open(recovered_csv, 'r') as f:
        reader = csv.DictReader(f, fieldnames=['id', 'user_id', 'email', 'name', 'created_at'])
        
        for row in reader:
            record_id = int(row['id'])
            record_hash = calculate_record_hashes(row)
            
            results['recovered_hashes'].add(record_hash)
            results['recovered_by_id'][record_id] = row
            results['metadata']['total_recovered'] += 1
    
    # Compare
    print(f"[*] Comparing {len(results['recovered_by_id']):,} recovered vs {len(golden_hashes['record_hashes']):,} golden records...")
    
    # Check each golden record
    for golden_hash in golden_hashes['record_hashes']:
        record_id = golden_hash['record_id']
        golden_hash_value = golden_hash['sha256']
        
        if record_id in results['recovered_by_id']:
            recovered_hash = calculate_record_hashes(results['recovered_by_id'][record_id])
            if recovered_hash == golden_hash_value:
                results['metadata']['matched'] += 1
            else:
                results['metadata']['mismatched'] += 1
                results['mismatches'].append({
                    'record_id': record_id,
                    'golden_hash': golden_hash_value,
                    'recovered_hash': recovered_hash
                })
        else:
            results['metadata']['missing'] += 1
            results['missing_ids'].append(record_id)
    
    return results


def calculate_recovery_rate(results: dict) -> float:
    """Calculate recovery rate as percentage"""
    total_golden = results['metadata']['total_golden']
    matched = results['metadata']['matched']
    
    if total_golden == 0:
        return 0.0
    
    return (matched / total_golden) * 100


def print_validation_report(results: dict, threshold: float = 99.9) -> bool:
    """Print validation report and return pass/fail status"""
    rate = calculate_recovery_rate(results)
    
    print()
    print("=" * 70)
    print("RECOVERY VALIDATION REPORT")
    print("=" * 70)
    print()
    print("RECOVERY STATISTICS:")
    print("-" * 70)
    print(f"  Golden Records:       {results['metadata']['total_golden']:,}")
    print(f"  Unique Golden Hashes: {results['metadata']['unique_golden_hashes']:,}")
    print(f"  Recovered Records:    {results['metadata']['total_recovered']:,}")
    print()
    print("MATCH ANALYSIS:")
    print("-" * 70)
    print(f"  Matched:              {results['metadata']['matched']:,}")
    print(f"  Missing:              {results['metadata']['missing']:,}")
    print(f"  Mismatched:           {results['metadata']['mismatched']:,}")
    print()
    print("RECOVERY RATE:")
    print("-" * 70)
    print(f"  Recovery Rate:        {rate:.4f}%")
    print(f"  Threshold:            {threshold}%")
    print()
    
    # Show mismatches (if any)
    if results['metadata']['mismatched'] > 0:
        print("MISMATCHED RECORDS (first 10):")
        print("-" * 70)
        for mismatch in results['mismatches'][:10]:
            print(f"    ID {mismatch['record_id']}:")
            print(f"      Golden:  {mismatch['golden_hash']}")
            print(f"      Recovered: {mismatch['recovered_hash']}")
        if len(results['mismatches']) > 10:
            print(f"    ... and {len(results['mismatches']) - 10} more")
        print()
    
    # Show missing IDs (if any)
    if results['metadata']['missing'] > 0:
        print("MISSING RECORD IDs (first 20):")
        print("-" * 70)
        missing_samples = results['missing_ids'][:20]
        print(f"    {missing_samples}")
        if len(results['missing_ids']) > 20:
            print(f"    ... and {len(results['missing_ids']) - 20} more")
        print()
    
    # Final verdict
    print("=" * 70)
    if rate >= threshold:
        print(f"RESULT: [PASS] Recovery rate {rate:.4f}% >= {threshold}%")
        print("=" * 70)
        return True
    else:
        print(f"RESULT: [FAIL] Recovery rate {rate:.4f}% < {threshold}%")
        print("=" * 70)
        return False


def generate_hashes_summary(hash_file: str) -> dict:
    """Generate summary statistics for hash file"""
    print(f"[*] Analyzing hash file: {hash_file}")
    
    with open(hash_file, 'r') as f:
        data = json.load(f)
    
    print()
    print("HASH FILE SUMMARY:")
    print("-" * 70)
    print(f"  Source File:     {data['metadata'].get('source_file', 'N/A')}")
    print(f"  Total Records:   {data['metadata'].get('total_records', 0):,}")
    print(f"  Hash Type:       {data['metadata'].get('hash_type', 'SHA-256')}")
    print(f"  Generated:       {data['metadata'].get('generated_at', 'N/A')}")
    print()


def main():
    parser = argparse.ArgumentParser(
        description='MySQL Recovery Validation - SHA-256 Hash Comparison'
    )
    parser.add_argument(
        '--gen-hashes',
        help='Generate golden hashes from CSV'
    )
    parser.add_argument(
        '--golden',
        help='Golden hash JSON file'
    )
    parser.add_argument(
        '--recovered',
        help='Recovered CSV file'
    )
    parser.add_argument(
        '--output',
        default='recovery_validation_report.md',
        help='Output report file (Markdown)'
    )
    parser.add_argument(
        '--threshold',
        type=float,
        default=99.9,
        help='Recovery rate threshold (default: 99.9%)'
    )
    
    args = parser.parse_args()
    
    if args.gen_hashes:
        # Generate hashes
        if not args.recovered:
            print("Error: --recovered is required with --gen-hashes")
            sys.exit(1)
        generate_golden_hashes(args.gen_hashes, args.recovered)
    
    elif args.golden and args.recovered:
        # Validate
        golden_data = load_golden_hashes(args.golden)
        results = validate_recovered_csv(args.recovered, golden_data)
        passed = print_validation_report(results, args.threshold)
        
        # Generate report
        report_path = args.output
        generate_markdown_report(results, report_path, args.threshold)
        print(f"[+] Report saved to: {report_path}")
        
        sys.exit(0 if passed else 1)
    
    else:
        parser.print_help()
        print()
        print("Examples:")
        print("  # Generate golden hashes from CSV")
        print("  python3 validate_recovery_hash.py --gen-hashes recovered.csv --output golden_hashes.json")
        print()
        print("  # Validate recovery against golden hashes")
        print("  python3 validate_recovery_hash.py --golden golden_hashes.json --recovered recovered.csv")
        print()
        sys.exit(1)


def generate_markdown_report(results: dict, output_path: str, threshold: float):
    """Generate Markdown report"""
    rate = calculate_recovery_rate(results)
    
    with open(output_path, 'w') as f:
        f.write("# MySQL Recovery Validation Report\n\n")
        
        f.write("## Summary\n\n")
        f.write(f"| Metric | Value |\n")
        f.write(f"|--------|-------|\n")
        f.write(f"| Recovery Rate | {rate:.4f}% |\n")
        f.write(f"| Threshold | {threshold}% |\n")
        f.write(f"| Passed | {'Yes' if rate >= threshold else 'No'} |\n\n")
        
        f.write("## Statistics\n\n")
        f.write(f"| Metric | Count |\n")
        f.write(f"|--------|-------|\n")
        f.write(f"| Golden Records | {results['metadata']['total_golden']:,} |\n")
        f.write(f"| Unique Golden Hashes | {results['metadata']['unique_golden_hashes']:,} |\n")
        f.write(f"| Recovered Records | {results['metadata']['total_recovered']:,} |\n")
        f.write(f"| Matched | {results['metadata']['matched']:,} |\n")
        f.write(f"| Missing | {results['metadata']['missing']:,} |\n")
        f.write(f"| Mismatched | {results['metadata']['mismatched']:,} |\n\n")
        
        if results['metadata']['mismatches']:
            f.write("## Mismatched Records\n\n")
            for m in results['mismatches'][:20]:
                f.write(f"- ID {m['record_id']}: Golden={m['golden_hash']}, Recovered={m['recovered_hash']}\n")
        
        if results['metadata']['missing_ids']:
            f.write("## Missing Record IDs\n\n")
            f.write(f"First 20: {results['missing_ids'][:20]}\n")
            f.write(f"Total missing: {len(results['missing_ids'])}\n")


if __name__ == '__main__':
    main()
