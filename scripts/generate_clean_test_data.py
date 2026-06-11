#!/usr/bin/env python3
"""
从 MySQL 5.6 生成干净的测试数据 IBD 文件
确保数据完整性和格式正确性
"""

import struct
import os
import subprocess
import sys
from pathlib import Path

def mysql_query(query):
    """Execute MySQL query via podman"""
    cmd = [
        'podman', 'exec', 'mysql_recovery_56',
        'mysql', '-u', 'root', '-proot123',
        '-e', query
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.stdout
    except Exception as e:
        print(f"Query error: {e}")
        return None

def dump_innodb_page(ibd_path, page_no, page_size=8192):
    """Dump a single page from IBD file"""
    with open(ibd_path, 'rb') as f:
        offset = page_no * page_size
        f.seek(offset)
        data = f.read(page_size)
        return data[:1024]  # First 1KB for debugging

def analyze_ibd_file(ibd_path):
    """Analyze the IBD file structure"""
    file_size = os.path.getsize(ibd_path)
    
    # Test different page sizes
    for page_size in [4096, 8192, 16384, 32768, 65536]:
        if file_size % page_size == 0:
            num_pages = file_size // page_size
            print(f"\n{'='*60}")
            print(f"Page Size: {page_size}, Total Pages: {num_pages}")
            print(f"{'='*60}")
            
            valid_pages = 0
            with open(ibd_path, 'rb') as f:
                for page_no in range(min(20, num_pages)):
                    offset = page_no * page_size
                    f.seek(offset)
                    header = f.read(38)
                    
                    if len(header) < 38:
                        break
                    
                    space_id = struct.unpack('<I', header[0:4])[0]
                    page_offset = struct.unpack('<I', header[4:8])[0]
                    lsn_parts = struct.unpack('<II', header[8:16])
                    lsn = (lsn_parts[1] << 32) | lsn_parts[0]
                    page_type = struct.unpack('<H', header[16:18])[0]
                    
                    # Check for valid InnoDB page types
                    is_valid = False
                    type_str = "UNKNOWN"
                    
                    if page_type == 0x45bc:  # MySQL 5.6 index
                        type_str = "FIL_PAGE_INDEX (5.6)"
                        is_valid = True
                    elif page_type == 0x26f1:  # MySQL 8.0 index
                        type_str = "FIL_PAGE_INDEX (8.0)"
                        is_valid = True
                    elif page_type == 0x26f2:  # Undo log
                        type_str = "FIL_PAGE_UNDO_LOG"
                        is_valid = True
                    elif page_type == 0x26f3:  # SDI
                        type_str = "FIL_PAGE_SDI"
                        is_valid = True
                    elif page_type == 0:  # Allocated
                        type_str = "FIL_PAGE_ALLOCATED"
                        is_valid = True
                    
                    if is_valid:
                        valid_pages += 1
                        if page_no < 5:  # Print first 5 valid pages
                            print(f"  Page {page_no}: {type_str}, offset={page_offset}, lsn={lsn}")
                    
                    # Check BTR header for index pages
                    if page_type in [0x45bc, 0x26f1]:
                        if len(header) >= 50:
                            btr_n_levels = struct.unpack('<H', header[42:44])[0]
                            btr_index_id = struct.unpack('<Q', header[44:52])[0]
                            print(f"    -> BTR: levels={btr_n_levels}, index_id=0x{btr_index_id:016x}")
            
            print(f"  Valid pages in first 20: {valid_pages}")

def main():
    ibd_path = "/app/mysql_recovery_pro/test_data/test_users_large.ibd"
    
    if not os.path.exists(ibd_path):
        print(f"File not found: {ibd_path}")
        return 1
    
    print(f"Analyzing: {ibd_path}")
    print(f"File size: {os.path.getsize(ibd_path)} bytes")
    
    analyze_ibd_file(ibd_path)
    
    # Also check the first 100 bytes of the file
    print(f"\n{'='*60}")
    print("Raw file header (first 100 bytes):")
    print(f"{'='*60}")
    with open(ibd_path, 'rb') as f:
        data = f.read(100)
        hex_str = ' '.join(f'{b:02x}' for b in data)
        print(hex_str)
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
