#!/usr/bin/env python3
"""
Generate test .ibd files for integration testing
"""

import os
import struct
import random
import sys

# InnoDB page constants
FIL_PAGE_TYPE_INDEX = 9969
FIL_PAGE_TYPE_SDI = 9971
FIL_PAGE_TYPE_UNDO_LOG = 9970
FIL_PAGE_TYPE_BLOB = 9972

UNIV_PAGE_SIZE = 16384

def create_fil_header(page_no, page_type, checksum=0):
    """Create FIL header (38 bytes)"""
    header = bytearray(38)
    # prev_page (4 bytes)
    struct.pack_into('<I', header, 0, 0xFFFFFFFF)
    # next_page (4 bytes)
    struct.pack_into('<I', header, 4, 0xFFFFFFFF)
    # LSN (8 bytes)
    struct.pack_into('<Q', header, 8, 1000)
    # space_id (4 bytes)
    struct.pack_into('<I', header, 16, 0)
    # page_no (4 bytes)
    struct.pack_into('<I', header, 20, page_no)
    # checksum (4 bytes)
    struct.pack_into('<I', header, 24, checksum)
    # checksum (4 bytes) - duplicate for some algorithms
    struct.pack_into('<I', header, 28, checksum)
    return header

def create_page_header(n_dir_slots=2, heap_top=54+13+8, n_heap=2):
    """Create page header (16 bytes)"""
    header = bytearray(16)
    # n_dir_slots (2 bytes)
    struct.pack_into('<H', header, 0, n_dir_slots)
    # heap_top (2 bytes)
    struct.pack_into('<H', header, 2, heap_top)
    # n_heap (2 bytes) - with NEW_CHUNK_FLAG
    struct.pack_into('<H', header, 4, n_heap | 0x8000)
    # free_offset (2 bytes)
    struct.pack_into('<H', header, 6, 54)
    # garbage_end (2 bytes)
    struct.pack_into('<H', header, 8, 0)
    # last_insert (2 bytes)
    struct.pack_into('<H', header, 10, 0)
    # direction (2 bytes)
    struct.pack_into('<H', header, 12, 0)
    # n_direct_pages (2 bytes)
    struct.pack_into('<H', header, 14, 0)
    return header

def create_infimum_record():
    """Create INFIMUM record (13 bytes)"""
    record = bytearray(13)
    # info_bits (1 byte) - INFIMUM flag
    record[0] = 0x02
    # next_offset (2 bytes)
    struct.pack_into('<H', record, 1, 54 + 13)
    # heap_no (2 bytes)
    struct.pack_into('<H', record, 3, 0)
    # n_fields (1 byte)
    record[5] = 0x01
    # extra bytes (5 bytes)
    record[6:11] = bytes([0x00] * 5)
    # record type (1 byte)
    record[11] = 0x02  # INFIMUM
    # nulls (1 byte)
    record[12] = 0x00
    return record

def create_supremum_record():
    """Create SUPREMUM record (8 bytes)"""
    record = bytearray(8)
    # info_bits (1 byte) - SUPREMUM flag
    record[0] = 0x03
    # next_offset (2 bytes) - 0xFFFF
    struct.pack_into('<H', record, 1, 0xFFFF)
    # heap_no (2 bytes)
    struct.pack_into('<H', record, 3, 1)
    # n_fields (1 byte)
    record[5] = 0x01
    # extra bytes (2 bytes)
    record[6:8] = bytes([0x00] * 2)
    return record

def create_index_page(page_no, records=None):
    """Create a complete index page"""
    page = bytearray(UNIV_PAGE_SIZE)
    
    # FIL header
    fil_header = create_fil_header(page_no, FIL_PAGE_TYPE_INDEX)
    page[0:38] = fil_header
    
    # Page header
    page_header = create_page_header()
    page[38:54] = page_header
    
    # INFIMUM record at offset 54
    infimum = create_infimum_record()
    page[54:67] = infimum
    
    # SUPREMUM record
    supremum = create_supremum_record()
    page[67:75] = supremum
    
    # Fill remaining with zeros
    return page

def create_sdi_page(page_no, sdi_json=b'{"name":"test_table"}'):
    """Create an SDI page"""
    page = bytearray(UNIV_PAGE_SIZE)
    
    # FIL header
    fil_header = create_fil_header(page_no, FIL_PAGE_TYPE_SDI)
    page[0:38] = fil_header
    
    # Page header
    page_header = create_page_header()
    page[38:54] = page_header
    
    # SDI payload (JSON)
    payload_start = 54
    page[payload_start:payload_start+len(sdi_json)] = sdi_json
    
    return page

def create_blob_page(page_no, prev_page=0xFFFFFFFF, next_page=0xFFFFFFFF, data_len=0):
    """Create a BLOB page"""
    page = bytearray(UNIV_PAGE_SIZE)
    
    # FIL header
    fil_header = create_fil_header(page_no, FIL_PAGE_TYPE_BLOB)
    page[0:38] = fil_header
    
    # BLOB header (at offset 38)
    # next_bpage (4 bytes)
    struct.pack_into('<I', page, 38, next_page)
    # prev_bpage (4 bytes)
    struct.pack_into('<I', page, 42, prev_page)
    # data_len (4 bytes)
    struct.pack_into('<I', page, 46, data_len)
    
    return page

def write_page_to_file(file_path, pages):
    """Write pages to file"""
    with open(file_path, 'wb') as f:
        for page in pages:
            f.write(page)

def generate_test_ibd(output_path, num_pages=10, include_sdi=True, include_blob=True):
    """Generate a test .ibd file"""
    pages = []
    
    # Generate index pages
    for i in range(num_pages):
        page = create_index_page(i)
        pages.append(page)
    
    # Add SDI page if requested
    if include_sdi:
        sdi_page = create_sdi_page(num_pages)
        pages.append(sdi_page)
    
    # Add BLOB pages if requested
    if include_blob:
        blob_page = create_blob_page(num_pages + 1)
        pages.append(blob_page)
    
    write_page_to_file(output_path, pages)
    print(f"Generated {output_path} with {len(pages)} pages")

def generate_truncated_ibd(output_path, partial_pages=5):
    """Generate a truncated .ibd file (for truncate recovery testing)"""
    pages = []
    
    for i in range(partial_pages):
        page = create_index_page(i)
        pages.append(page)
    
    write_page_to_file(output_path, pages)
    print(f"Generated truncated {output_path} with {len(pages)} pages")

def generate_header_corrupted_ibd(output_path):
    """Generate a .ibd file with corrupted header (for header repair testing)"""
    page = bytearray(UNIV_PAGE_SIZE)
    
    # Corrupt FIL header
    page[0:38] = bytes([0xFF] * 38)
    
    # Set invalid page type
    struct.pack_into('<H', page, 50, 0xFFFF)
    
    with open(output_path, 'wb') as f:
        f.write(page)
    
    print(f"Generated corrupted {output_path}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 generate_test_ibd.py <test_name>")
        print("Test names: basic, truncated, corrupted, full")
        sys.exit(1)
    
    test_name = sys.argv[1]
    output_dir = os.path.join(os.path.dirname(__file__), "test", "output")
    os.makedirs(output_dir, exist_ok=True)
    
    if test_name == "basic":
        generate_test_ibd(os.path.join(output_dir, "test_basic.ibd"))
    elif test_name == "truncated":
        generate_truncated_ibd(os.path.join(output_dir, "test_truncated.ibd"))
    elif test_name == "corrupted":
        generate_header_corrupted_ibd(os.path.join(output_dir, "test_corrupted.ibd"))
    elif test_name == "full":
        generate_test_ibd(os.path.join(output_dir, "test_full.ibd"), 
                         num_pages=100, include_sdi=True, include_blob=True)
    else:
        print(f"Unknown test name: {test_name}")
        sys.exit(1)

if __name__ == "__main__":
    main()
