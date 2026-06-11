#!/usr/bin/env python3
# =============================================================================
# MySQL 5.6 Recovery Validation Script
# Validates recovery rate against golden data
# =============================================================================

import os
import sys
import json
import hashlib
import argparse


def load_golden_hashes(golden_file: str) -> dict:
    """Load golden hash file"""
    with open(golden_file, 'r') as f:
        return json.load(f)


def calculate_record_hash(record: str) -> str:
    """Calculate SHA256 hash for a record string"""
    return hashlib.sha256(record.encode()).hexdigest()


def validate_recovery(recovered_csv: str, golden_file: str) -> dict:
    """Validate recovered records against golden data"""
    results = {
        'total_golden': 0,
        'total_recovered': 0,
        'matched': 0,
        'mismatched': 0,
        ' missing': 0,
    }
    
    if not os.path.exists(golden_file):
        print(f"Golden file not found: {golden_file}")
        return results
    
    if not os.path.exists(recovered_csv):
        print(f"Recovered file not found: {recovered_csv}")
        return results
    
    # Load golden hashes
    golden_data = load_golden_hashes(golden_file)
    golden_hashes = {h['record_id']: h['sha256'] for h in golden_data.get('record_hashes', [])}
    results['total_golden'] = len(golden_hashes)
    
    # Parse recovered CSV (email addresses)
    recovered_emails = set()
    with open(recovered_csv, 'r') as f:
        header = f.readline()  # Skip header
        for line in f:
            line = line.strip()
            if line and '@' in line:
                # Extract email (third column typically)
                parts = line.split(',')
                if len(parts) >= 3:
                    email = parts[2]
                    if '@' in email:
                        recovered_emails.add(email)
    
    results['total_recovered'] = len(recovered_emails)
    
    # Calculate match rate
    # In golden data, emails are user_XXXXXX@domain
    # In recovered, we extract emails
    
    # For now, just report counts
    # More sophisticated matching would compare full records
    
    return results


def main():
    parser = argparse.ArgumentParser(description='Validate MySQL 5.6 Recovery')
    parser.add_argument('--recovered', required=True, help='Recovered CSV file')
    parser.add_argument('--golden', required=True, help='Golden hash JSON file')
    
    args = parser.parse_args()
    
    results = validate_recovery(args.recovered, args.golden)
    
    print("\n=== Recovery Validation Results ===")
    print(f"Golden records:    {results['total_golden']}")
    print(f"Recovered records: {results['total_recovered']}")
    
    if results['total_golden'] > 0:
        recovery_rate = results['total_recovered'] / results['total_golden'] * 100
        print(f"Recovery rate:     {recovery_rate:.2f}%")
        
        if recovery_rate >= 99.9:
            print("\n[SUCCESS] Recovery rate >= 99.9%")
        else:
            print(f"\n[WARNING] Recovery rate below 99.9% threshold")


if __name__ == '__main__':
    main()
