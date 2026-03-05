#!/usr/bin/env python3
"""Extract and decompress SDI from .ibd file (MySQL 8.0+)"""
import struct
import zlib
import sys
import json

SDI_PAGE_TYPE = 0x4573  # FIL_PAGE_SDI in MySQL 8.0
MAX_SDI_SIZE = 16 * 1024 * 1024

def read_le_uint32(data, offset):
    return struct.unpack('<I', data[offset:offset+4])[0]

def read_le_uint64(data, offset):
    return struct.unpack('<Q', data[offset:offset+8])[0]

def extract_sdi_from_page(page_data):
    """Extract SDI payload from a single page"""
    if len(page_data) < 38:
        return None
    
    # Check page type
    page_type = read_le_uint32(page_data, 0x00)
    if page_type != SDI_PAGE_TYPE:
        return None
    
    # SDI payload starts at offset 38 (after FIL header + FSP header)
    sdi_start = 38
    if len(page_data) < sdi_start + 20:  # min header size
        return None
    
    # Read SDI header
    type_ = read_le_uint32(page_data, sdi_start)
    id_ = read_le_uint64(page_data, sdi_start + 4)
    uncompressed_len = read_le_uint32(page_data, sdi_start + 12)
    compressed_len = read_le_uint32(page_data, sdi_start + 16)
    
    if uncompressed_len == 0 or uncompressed_len > MAX_SDI_SIZE:
        return None
    
    raw_data = page_data[sdi_start+20 : sdi_start+20+compressed_len]
    
    # Decompress (raw deflate, -15 window bits)
    try:
        decompressed = zlib.decompress(raw_data, -zlib.MAX_WBITS)
        if len(decompressed) != uncompressed_len:
            return None
        return decompressed.decode('utf-8', errors='replace')
    except Exception as e:
        print(f"[!] Decompression failed: {e}", file=sys.stderr)
        return None

def extract_all_sdi(ibd_path):
    """Scan .ibd for all SDI pages and extract JSON"""
    PAGE_SIZE = 16384  # Default; adjust if needed (4K/8K/32K/64K)
    
    with open(ibd_path, 'rb') as f:
        data = f.read()
    
    results = []
    for offset in range(0, len(data), PAGE_SIZE):
        page = data[offset : offset + PAGE_SIZE]
        if len(page) < 38:
            continue
        
        sdi_json = extract_sdi_from_page(page)
        if sdi_json:
            try:
                parsed = json.loads(sdi_json)
                results.append(parsed)
            except json.JSONDecodeError as e:
                print(f"[!] Invalid JSON in SDI page at offset {offset}: {e}", file=sys.stderr)
    
    return results

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python extract_sdi.py <file.ibd>", file=sys.stderr)
        sys.exit(1)
    
    ibd_path = sys.argv[1]
    sdis = extract_all_sdi(ibd_path)
    
    if not sdis:
        print(f"[!] No SDI found in {ibd_path}", file=sys.stderr)
    else:
        for i, sdi in enumerate(sdis):
            print(f"=== SDI #{i+1} ===")
            print(json.dumps(sdi, indent=2))
