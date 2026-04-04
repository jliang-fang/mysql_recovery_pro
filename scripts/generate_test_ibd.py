#!/usr/bin/env python3
"""
Generate minimal valid InnoDB .ibd file for recovery engine testing (MySQL 8.0+)
Author: jliang.fang
"""

import struct
import json
import os

PAGE_SIZE = 16384  # Default 16KB page

def create_fil_header(page_type):
    """Create 38-byte FIL header"""
    return struct.pack('<IIIIHH',
        0,                      # FIL_FSP_ID (unused in .ibd)
        0xFFFFFFFF,             # FIL_PAGE_PREV (null)
        0xFFFFFFFF,             # FIL_PAGE_NEXT (null)
        0x12345678,             # FIL_PAGE_LSN (dummy)
        page_type,              # FIL_PAGE_TYPE
        0                       # FIL_PAGE_SPACE_CHECKSUM (we'll skip for now)
    )

def create_index_page():
    """Create a minimal B+ Tree leaf page with exact offsets per MySQL 8.0 spec"""
    page = bytearray(PAGE_SIZE)
    
    # 1. FIL header (38 bytes at offset 0)
    fil_header = create_fil_header(17855)  # FIL_PAGE_INDEX
    page[0:38] = fil_header
    
    # 2. PAGE_HEADER (40 bytes at offset 38): n_recs=2, max_offset=119, heap_top=133
    header_data = struct.pack('<HHH', 2, 119, 133) + b'\x00' * 28  # Pad to 40 bytes
    page[38:78] = header_data
    
    # 3. Infimum record (fixed: offset=96, size=23)
    inf = struct.pack('<HBB', 5, 0x80, 1) + b'\x69\x6e\x66'
    page[96:96+23] = inf.ljust(23, b'\x00')
    
    # 4. Supremum record (fixed: offset=119, size=14)
    sup = struct.pack('<HBB', 28, 0x80, 1) + b'\x73\x75\x70'
    page[119:119+14] = sup.ljust(14, b'\x00')
    
    # 5. Page directory (at end of page)
    dir_start = PAGE_SIZE - 6  # 16378
    struct.pack_into('<H', page, dir_start, 2)  # n_dir_slots=2
    struct.pack_into('<HH', page, dir_start+2, 96, dir_start-14)  # inf & sup offsets
    
    return bytes(page)

def generate_sdi():
    """Generate minimal SDI v2 JSON for test table"""
    sdi = {
        "dictionary": {
            "version": [8, 0],
            "sdi_version": 2,
            "dd_version": 80036,
            "tables": [{
                "name": "t1",
                "schema": "recovery_test",
                "mysql_version_id": 80036,
                "tablespace_id": 4294967295,
                "engine": "InnoDB",
                "columns": [
                    {"name": "id", "type": 8, "is_nullable": False},
                    {"name": "data", "type": 15, "is_nullable": True},
                    {"name": "blob_data", "type": 253, "is_nullable": True}
                ],
                "indexes": [{
                    "name": "PRIMARY",
                    "type": 1,
                    "columns": [{"name": "id"}]
                }]
            }]
        }
    }
    return json.dumps(sdi, separators=(',', ':'))

def write_ibd_with_sdi(filepath):
    """Write .ibd file with embedded SDI"""
    # Page 0: FSP_HDR (simplified)
    fsp_hdr = bytearray(PAGE_SIZE)
    fsp_hdr[0:38] = create_fil_header(1)  # FIL_PAGE_TYPE_FSP_HDR
    struct.pack_into('<II', fsp_hdr, 38, 4294967295, PAGE_SIZE)
    
    # Page 1: IBUF_BITMAP (simplified)
    ibuf_bitmap = bytearray(PAGE_SIZE)
    
    # Page 2: FIRST INDEX PAGE
    index_page = create_index_page()
    
    with open(filepath, 'wb') as f:
        f.write(fsp_hdr + ibuf_bitmap + index_page)
        
    # Write SDI to file (MySQL 8.0 stores in first page after FSP_HDR)
    sdi_json = generate_sdi().encode('utf-8')
    padding = b'\x00' * (PAGE_SIZE - len(sdi_json))
    
    with open(filepath.replace('.ibd', '.sdi'), 'wb') as f:
        f.write(sdi_json + padding)

if __name__ == '__main__':
    os.makedirs('/app/mysql_recovery_proy/test_data', exist_ok=True)
    ibd_path = '/app/mysql_recovery_proy/test_data/test_table.ibd'
    write_ibd_with_sdi(ibd_path)
    print(f"✅ Generated test files:")
    print(f"   - {ibd_path}")
    print(f"   - {ibd_path.replace('.ibd', '.sdi')}")
