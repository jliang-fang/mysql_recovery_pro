#!/usr/bin/env python3
"""
Regenerate test file with correct format for MySQL Recovery Pro
Creates a 64KB page InnoDB file with page type cleared to 0x0000 for testing
"""

import struct
import os
import random
from datetime import datetime, timedelta

# Constants
PAGE_SIZE = 65536  # 64KB pages
FIL_PAGE_TYPE_INDEX = 0x2601  # B+Tree index
FIL_PAGE_TYPE_ALLOCATED = 0x0000  # Cleared (corrupted) page type

# Record format constants
REC_STATUS_INFIMUM = 0x02
REC_STATUS_SUPREMUM = 0x03
REC_STATUS_ORDINARY = 0x00
REC_INFO_MIN_REC_FLAG = 0x10

def write_le_uint16(data, offset, value):
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF

def write_le_uint32(data, offset, value):
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF
    data[offset + 2] = (value >> 16) & 0xFF
    data[offset + 3] = (value >> 24) & 0xFF

def write_le_uint64(data, offset, value):
    for i in range(8):
        data[offset + i] = (value >> (i * 8)) & 0xFF

def create_fil_header(page_type, page_no=0, prev_page=0xFFFFFFFF, next_page=0xFFFFFFFF):
    """Create FIL header (38 bytes)"""
    header = bytearray(38)
    write_le_uint32(header, 0, 0)       # space_id
    write_le_uint32(header, 4, 0)       # checksum1
    write_le_uint64(header, 8, 0)       # LSN
    write_le_uint32(header, 16, page_type)  # page_type at offset 16
    write_le_uint32(header, 20, (page_no & 0xFFFF) | ((prev_page & 0xFFFF) << 16))
    write_le_uint32(header, 24, (prev_page >> 16) | ((next_page & 0xFFFF) << 16))
    write_le_uint32(header, 28, next_page >> 16)
    return header

def create_page_header(n_dir_slots, heap_top, n_heap):
    """Create page header starting at offset 38 (after FIL header)"""
    header = bytearray(16)
    
    # BTR header (10 bytes)
    write_le_uint16(header, 0, 0)       # BTR_N_LEVELS = 0 (leaf)
    write_le_uint16(header, 2, 0)       # BTR_ZERO
    write_le_uint16(header, 4, heap_top)
    write_le_uint16(header, 6, n_heap)
    write_le_uint16(header, 8, 0)       # BTR_FREE
    
    # Page header fields at offset 48
    dir_start = 48
    write_le_uint16(header, 0, n_dir_slots)
    write_le_uint16(header, 2, heap_top)
    write_le_uint16(header, 4, n_heap)
    write_le_uint16(header, 6, 0)
    write_le_uint16(header, 8, 0)
    write_le_uint16(header, 10, 0)
    write_le_uint16(header, 12, 0)
    write_le_uint16(header, 14, 0)
    
    return header

def create_infimum_record():
    """Create INFIMUM record at offset 54 (after FIL + BTR headers)"""
    rec = bytearray(13)
    rec[0] = REC_STATUS_INFIMUM
    write_le_uint16(rec, 1, 0x005E)  # next_record_offset -> SUPREMUM
    write_le_uint16(rec, 3, 0)       # heap_no = 0
    # Data: "infimum" as null-terminated string + padding
    rec[5:13] = b'\x08\x00\x00\x00\x00\x00\x00\x00'  # dummy data
    return rec

def create_supremum_record():
    """Create SUPREMUM record after INFIMUM"""
    rec = bytearray(13)
    rec[0] = REC_STATUS_SUPREMUM
    write_le_uint16(rec, 1, 0x3FFF)  # next_record_offset = end
    write_le_uint16(rec, 3, 1)       # heap_no = 1
    rec[5:13] = b'\x08\x00\x00\x00\x00\x00\x00\x00'
    return rec

def create_user_record(record_id, pos):
    """Create a user record with 'user' in email field"""
    rec = bytearray(128)
    
    # Record header (COMPACT format - 5 bytes)
    rec[0] = REC_STATUS_ORDINARY  # info_bits
    n_fields_high = (4 << 3) & 0xF8  # 4 fields
    rec[1] = n_fields_high          # n_fields in high nibble
    write_le_uint16(rec, 3, record_id + 2)  # heap_no
    
    # Set previous record's next_offset
    write_le_uint16(rec, 1, pos + 128)  # next_record_offset
    
    # Record data starts at offset 5
    data_pos = 5
    
    # id (BIGINT - 8 bytes)
    write_le_uint64(rec, data_pos, record_id)
    data_pos += 8
    
    # name_len (1 byte) + name (VARCHAR)
    name = f"user_{record_id:08d}"
    name_bytes = name.encode('utf-8')
    rec[data_pos] = len(name_bytes)
    rec[data_pos + 1:data_pos + 1 + len(name_bytes)] = name_bytes
    data_pos += 1 + 50  # Skip reserved space
    
    # email_len (1 byte) + email (VARCHAR with 'user')
    email = f"user_{record_id:08d}@test.com"
    email_bytes = email.encode('utf-8')
    rec[data_pos] = len(email_bytes)
    rec[data_pos + 1:data_pos + 1 + len(email_bytes)] = email_bytes
    data_pos += 1 + 30
    
    # status (TINYINT - 1 byte)
    rec[data_pos] = record_id % 4
    data_pos += 1
    
    # score (INT - 4 bytes)
    write_le_uint32(rec, data_pos, record_id * 100)
    data_pos += 4
    
    # float_score (FLOAT - 4 bytes)
    float_val = struct.pack('<f', float(record_id))
    rec[data_pos:data_pos+4] = float_val
    data_pos += 4
    
    # double_score (DOUBLE - 8 bytes)
    double_val = struct.pack('<d', float(record_id * 1.5))
    rec[data_pos:data_pos+8] = double_val
    data_pos += 8
    
    # created_at (DATETIME2 - 8 bytes)
    dt_val = (2024 << 48) | (1 << 40) | (1 << 32) | (record_id % 24 << 24) | (record_id % 60 << 16) | (record_id % 60 << 8) | record_id
    write_le_uint64(rec, data_pos, dt_val)
    
    return rec

def generate_test_file(output_path, num_records=5500000, records_per_page=100):
    """Generate a test file with proper InnoDB structure"""
    
    print(f"Generating {num_records} records...")
    print(f"Records per page: {records_per_page}")
    
    pages = []
    total_pages = max(3, (num_records + records_per_page - 1) // records_per_page + 2)
    
    records = list(range(1, num_records + 1))
    
    for page_no in range(total_pages):
        page = bytearray(PAGE_SIZE)
        
        # Calculate records for this page
        start_idx = page_no * records_per_page
        end_idx = min(start_idx + records_per_page, num_records)
        page_records = records[start_idx:end_idx]
        n_user_records = len(page_records)
        
        # Calculate directory slots
        n_dir_slots = 2 + n_user_records  # INFIMUM + SUPREMUM + user records
        
        # Calculate heap top
        record_start = 54 + n_dir_slots * 2  # After header + directory
        heap_top = record_start + n_user_records * 128 + 100
        
        # FIL header
        page[0:38] = create_fil_header(FIL_PAGE_TYPE_ALLOCATED, page_no)
        
        # Page header (BTR + PAGE)
        ph_data = create_page_header(n_dir_slots, heap_top, n_dir_slots)
        page[38:38+len(ph_data)] = ph_data
        
        # Directory slots
        dir_start = 54
        record_offsets = []
        
        # INFIMUM at record_start
        infimum = create_infimum_record()
        infimum_offset = record_start
        page[infimum_offset:infimum_offset+len(infimum)] = infimum
        record_offsets.append(infimum_offset)
        
        # SUPREMUM after INFIMUM
        supremum = create_supremum_record()
        supremum_offset = infimum_offset + len(infimum) + 2  # +2 for INFIMUM data
        page[supremum_offset:supremum_offset+len(supremum)] = supremum
        record_offsets.append(supremum_offset)
        
        # User records
        current_offset = supremum_offset + len(supremum)
        prev_offset = infimum_offset
        
        for i, record_id in enumerate(page_records):
            user_rec = create_user_record(record_id, current_offset)
            page[current_offset:current_offset+len(user_rec)] = user_rec
            record_offsets.append(current_offset)
            prev_offset = current_offset
            current_offset += len(user_rec)
        
        # Update directory slots
        for i, offset in enumerate(record_offsets):
            write_le_uint16(page, dir_start + i * 2, offset)
        
        # Update heap top
        write_le_uint16(page, 38 + 4, current_offset)
        
        # Store page
        pages.append(bytes(page))
        
        if (page_no + 1) % 1000 == 0:
            print(f"Generated {page_no + 1}/{total_pages} pages")
    
    # Write all pages
    with open(output_path, 'wb') as f:
        for page in pages:
            f.write(page)
    
    print(f"\nGenerated {len(pages)} pages, {len(pages) * PAGE_SIZE} bytes")
    print(f"Records per page: ~{records_per_page}")
    print(f"Total records: {num_records}")

if __name__ == "__main__":
    output_file = "/app/mysql_recovery_pro/test_data/test_mysql56_5m_full.ibd"
    generate_test_file(output_file, num_records=5500000, records_per_page=100)
    print(f"\nTest file generated: {output_file}")
