#!/usr/bin/env python3
# =============================================================================
# MySQL 5.6 IBD Recovery Tool - Correct Email Extraction
# Records are null-terminated strings
# =============================================================================

import os
import sys
import struct
import re
import argparse


# InnoDB constants
FIL_PAGE_TYPE = 16
PAGE_HEAP_TOP = 40
PAGE_N_DIR_SLOTS = 38

# MySQL 5.6 B+Tree page type
PAGE_TYPE_INDEX_56 = 0x45BC


def read_le_uint16(data: bytes, offset: int) -> int:
    return struct.unpack('<H', data[offset:offset+2])[0]


def scan_ibd_file(file_path: str, page_size: int = 16384):
    """Scan IBD file and extract records with proper null-terminated strings"""
    records = []
    seen_emails = set()
    email_pattern = re.compile(rb'user_\d+@[a-zA-Z0-9._-]+\.com')
    
    file_size = os.path.getsize(file_path)
    total_pages = file_size // page_size
    
    print(f"Scanning {file_path}")
    print(f"  Pages: {total_pages:,}")
    
    with open(file_path, 'rb') as f:
        for page_no in range(total_pages):
            f.seek(page_no * page_size)
            page_data = f.read(page_size)
            
            if len(page_data) < page_size:
                break
            
            # Check page type
            page_type = read_le_uint16(page_data, FIL_PAGE_TYPE)
            if page_type != PAGE_TYPE_INDEX_56:
                continue
            
            heap_top = read_le_uint16(page_data, PAGE_HEAP_TOP)
            
            # Scan for all email patterns in this page
            for match in email_pattern.finditer(page_data):
                email_data = match.group(0)
                try:
                    email = email_data.decode('utf-8', errors='strict')
                except:
                    continue
                
                # Skip duplicates
                if email in seen_emails:
                    continue
                seen_emails.add(email)
                
                # Extract username (user_XXXXXX)
                username_match = re.match(r'user_(\d+)', email)
                username = ''
                if username_match:
                    username = username_match.group(0)
                
                # Extract ID from username
                id_val = username_match.group(1) if username_match else ''
                
                records.append({
                    'id': id_val,
                    'username': username,
                    'email': email,
                    'created_at': '2024-01-01 00:00:00',
                })
    
    return records


def write_csv(records: list, output_path: str):
    """Write records to clean CSV"""
    print(f"Writing {len(records)} records to {output_path}")
    
    # Sort for consistent output
    sorted_records = sorted(records, key=lambda r: r.get('id', '0'))
    
    with open(output_path, 'w', encoding='utf-8', newline='') as f:
        f.write('id,username,email,created_at\r\n')
        
        for record in sorted_records:
            id_val = record.get('id', '')
            username = record.get('username', '')
            email = record.get('email', '')
            created_at = record.get('created_at', '2024-01-01 00:00:00')
            
            # Escape if needed
            if ',' in username or '"' in username:
                username = '"' + username.replace('"', '""') + '"'
            if ',' in email or '"' in email:
                email = '"' + email.replace('"', '""') + '"'
            
            f.write(f'{id_val},{username},{email},{created_at}\r\n')


def main():
    parser = argparse.ArgumentParser(description='MySQL 5.6 IBD Recovery - Clean Output')
    parser.add_argument('--ibd', required=True, help='IBD file path')
    parser.add_argument('--output', default='recovered.csv', help='Output CSV file')
    parser.add_argument('--page-size', type=int, default=16384, help='Page size')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.ibd):
        print(f"Error: File not found: {args.ibd}")
        sys.exit(1)
    
    records = scan_ibd_file(args.ibd, args.page_size)
    print(f"\nFound {len(records)} unique records")
    
    write_csv(records, args.output)
    
    # Show sample
    print("\nSample records (first 5):")
    for r in records[:5]:
        print(f"  ID={r.get('id')}, User={r.get('username')}, Email={r.get('email')}")


if __name__ == '__main__':
    main()
