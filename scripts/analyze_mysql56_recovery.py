#!/usr/bin/env python3
"""
MySQL 5.6 Recovery Rate Analysis Script
Compares recovered records with baseline MySQL data to calculate recovery rate.
"""
import struct
import subprocess
import os
import sys

BASELINE_IDS = set()
BASELINE_RECORDS = {}

def get_mysql_baseline():
    """Get baseline data from MySQL."""
    result = subprocess.run(
        ['mysql', '-e', 'SELECT id, username, email, full_name, created_at FROM recovery_test.test_users;'],
        capture_output=True, text=True
    )
    
    lines = result.stdout.strip().split('\n')
    for line in lines[1:]:  # Skip header
        parts = line.split('\t')
        if len(parts) >= 5:
            record_id = int(parts[0])
            BASELINE_IDS.add(record_id)
            BASELINE_RECORDS[record_id] = {
                'username': parts[1],
                'email': parts[2],
                'full_name': parts[3],
                'created_at': parts[4]
            }
    
    print(f"Baseline: {len(BASELINE_IDS)} records from MySQL")

def parse_ibd_file(ibd_path):
    """Parse IBD file and extract records using user_ pattern scanning."""
    records = {}
    page_size = 16384
    record_start_positions = set()
    
    with open(ibd_path, 'rb') as f:
        file_size = os.path.getsize(ibd_path)
        total_pages = file_size // page_size
        
        print(f"Scanning {total_pages} pages...")
        
        for page_no in range(total_pages):
            f.seek(page_no * page_size)
            data = f.read(page_size)
            
            if len(data) < 16384:
                break
            
            # Scan for all 'user_' patterns in this page
            pos = 0
            while True:
                pos = data.find(b'user_', pos)
                if pos == -1:
                    break
                
                # Parse record backwards
                if pos < 10:
                    pos += 1
                    continue
                
                # Try different record start positions
                for offset in range(-120, 20):
                    record_start = pos + offset
                    if record_start < 54 or record_start + 20 > len(data):
                        continue
                    
                    # Check if this is a record start
                    rec_id = struct.unpack('<Q', data[record_start+7:record_start+15])[0]
                    uname_len = data[record_start+15]
                    
                    if uname_len > 50:
                        continue
                    
                    # Validate record
                    uname_end = record_start + 15 + uname_len
                    if uname_end + 2 > len(data):
                        continue
                    
                    email_len = data[uname_end]
                    if email_len > 150:
                        continue
                    
                    email_end = uname_end + 1 + email_len
                    if email_end + 2 > len(data):
                        continue
                    
                    full_name_len = data[email_end]
                    if full_name_len > 100:
                        continue
                    
                    # Successfully parsed record
                    record_start_positions.add(record_start)
                    
                    uname = data[record_start+15:record_start+15+uname_len].decode('utf-8', errors='replace')
                    email = data[pos:pos+email_len].decode('utf-8', errors='replace')
                    full_name_end = email_end + 1 + full_name_len
                    full_name = data[full_name_end-1-full_name_len:full_name_end-1].decode('utf-8', errors='replace')
                    
                    if rec_id > 0 and rec_id <= 5500000:  # Valid ID range
                        if rec_id not in records:
                            records[rec_id] = {
                                'username': uname,
                                'email': email,
                                'full_name': full_name
                            }
                
                pos += 1
            
            if page_no % 5000 == 0:
                print(f"  Page {page_no}, records found: {len(records)}")
    
    print(f"Total unique records recovered: {len(records)}")
    print(f"Unique record start positions: {len(record_start_positions)}")
    
    return records

def calculate_recovery_rate(ibd_path):
    """Calculate recovery rate from IBD file."""
    # Get baseline
    if not BASELINE_IDS:
        get_mysql_baseline()
    
    # Parse IBD file
    recovered = parse_ibd_file(ibd_path)
    
    # Calculate recovery rate
    recovered_ids = set(recovered.keys())
    matching_ids = recovered_ids & BASELINE_IDS
    
    print(f"\n=== Recovery Analysis ===")
    print(f"Baseline records: {len(BASELINE_IDS)}")
    print(f"Recovered records: {len(recovered_ids)}")
    print(f"Matching IDs: {len(matching_ids)}")
    
    recovery_rate = len(matching_ids) / len(BASELINE_IDS) * 100
    print(f"Recovery rate: {recovery_rate:.4f}%")
    
    # Calculate detailed statistics
    missing_ids = BASELINE_IDS - recovered_ids
    extra_ids = recovered_ids - BASELINE_IDS
    
    print(f"\nMissing records: {len(missing_ids)}")
    print(f"Extra records: {len(extra_ids)}")
    
    # Check first 100 records
    first_100 = set(range(1, 101))
    first_100_matching = first_100 & recovered_ids
    first_100_rate = len(first_100_matching) / 100 * 100
    print(f"\nFirst 100 records recovery rate: {first_100_rate:.2f}%")
    
    # Check by ranges
    ranges = [
        (1, 1000),
        (1001, 5000),
        (5001, 10000),
        (10001, 50000),
        (50001, 100000),
    ]
    
    for start, end in ranges:
        range_ids = set(range(start, end + 1))
        range_matching = range_ids & recovered_ids
        range_rate = len(range_matching) / (end - start + 1) * 100
        print(f"Records {start}-{end}: {len(range_matching)}/{end-start+1} ({range_rate:.2f}%)")

def main():
    if len(sys.argv) < 2:
        print("Usage: analyze_mysql56_recovery.py <ibd_file_path>")
        sys.exit(1)
    
    ibd_path = sys.argv[1]
    
    if not os.path.exists(ibd_path):
        print(f"File not found: {ibd_path}")
        sys.exit(1)
    
    calculate_recovery_rate(ibd_path)

if __name__ == '__main__':
    main()
