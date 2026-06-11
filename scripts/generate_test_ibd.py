#!/usr/bin/env python3
"""
生成 MySQL 5.6 格式的测试 IBD 文件用于恢复测试
模拟真实的 InnoDB 表结构
"""

import struct
import os
import sys
import hashlib
from pathlib import Path

# MySQL 5.6 page type constants
FIL_PAGE_INDEX = 0x45bc      # B+Tree index page (MySQL 5.6)
FIL_PAGE_UNDO_LOG = 0x26f2   # Undo log page
FIL_PAGE_TYPE_LAST = 0x26f3  # Last page type in space
FIL_PAGE_ALLOCATED = 0       # Freshly allocated page

# InnoDB page size constants
PAGE_SIZE = 16384  # 16KB default
PAGE_SIZE_8K = 8192  # 8KB for MySQL 5.6 sometimes

# Row format constants
ROW_FORMAT_COMPACT = 1
ROW_FORMAT_REDUNDANT = 0

# BTR header constants
BTR_N_LEVELS_OFFSET = 42
BTR_INDEX_ID_OFFSET = 44

# Record header constants
REC_N_HEAP_OFFSET = 0
REC_NEXT_OFFSET = 2
REC_DELETED_MASK_OFFSET = 6
REC_N_OWNED_OFFSET = 7
REC_MIN_MARKED_OFFSET = 8
REC_N_FIELDS_OFFSET = 9
REC_HEAP_NO_OFFSET = 10
REC_OFFSETS_HEADER_SIZE = 11

class IBDWriter:
    def __init__(self, filepath, page_size=PAGE_SIZE):
        self.filepath = filepath
        self.page_size = page_size
        self.file = None
        self.pages_written = 0
        self.space_id = 1234567890
        
    def open(self):
        self.file = open(self.filepath, 'wb')
        return self
    
    def close(self):
        if self.file:
            self.file.close()
    
    def __enter__(self):
        return self.open()
    
    def __exit__(self, *args):
        self.close()
    
    def write_page_header(self, page_no, page_type):
        """Write standard InnoDB page header"""
        header = bytearray(self.page_size)
        
        # FIL_PAGE_SPACE_ID (4 bytes)
        struct.pack_into('<I', header, 0, self.space_id)
        
        # FIL_PAGE_OFFSET (4 bytes) - page number
        struct.pack_into('<I', header, 4, page_no)
        
        # FIL_PAGE_LSN (8 bytes) - Log Sequence Number
        # Use page_no * 1000 as simple LSN
        lsn = (page_no + 1) * 1000
        struct.pack_into('<Q', header, 8, lsn)
        
        # FIL_PAGE_TYPE (2 bytes)
        struct.pack_into('<H', header, 16, page_type)
        
        # FIL_PAGE_N_REco (2 bytes) - should be 0 for index pages
        struct.pack_into('<H', header, 18, 0)
        
        # FIL_PAGE_N_DIR_SLOTS (2 bytes) - for B-tree root page
        struct.pack_into('<H', header, 20, 10)
        
        # FIL_PAGE_N_REC_DELETE D (2 bytes)
        struct.pack_into('<H', header, 22, 0)
        
        # FIL_PAGE_N_HEAP (2 bytes) - number of records
        struct.pack_into('<H', header, 24, 4)  # 4: infimum + supremum + 2 fixed
        
        # FIL_PAGE_FREE (2 bytes) - offset to free space
        struct.pack_into('<H', header, 26, 112)
        
        # FIL_PAGE_FREE_FRAG_LIST (4 bytes) - should point to itself initially
        struct.pack_into('<I', header, 28, 120)
        
        # FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID (4 bytes)
        struct.pack_into('<I', header, 32, self.space_id)
        
        # FIL_PAGE_DATA starts at byte 38
        return header
    
    def write_index_page_header(self, header):
        """Write index page specific header after FIL header"""
        # BTR_BTR_LEVEL (2 bytes)
        struct.pack_into('<H', header, 38, 0)  # Leaf level
        
        # BTR_N_NODES (2 bytes)
        struct.pack_into('<H', header, 40, 3)  # 3 index nodes
        
        # BTR_INDEX_ID (8 bytes) - unique index identifier
        struct.pack_into('<Q', header, 44, 9876543210)
        
        # BTR_SPACE_VERSION (4 bytes)
        struct.pack_into('<I', header, 52, 1)
        
        # BTR_MARKER (4 bytes)
        struct.pack_into('<I', header, 56, 0)
        
        return header
    
    def write_infimum_record(self, data, offset):
        """Write infimum record"""
        # min record: 0x00858000 (fixed minimum)
        rec = b'\x00\x85\x80\x00'
        struct.pack_into('<H', rec, 0, 0x8000)  # n_fields = 1
        struct.pack_into('<I', rec, 2, 5)  # next = 5 bytes ahead
        data[offset:offset+4] = rec
        return offset + 4
    
    def write_supremum_record(self, data, offset):
        """Write supremum record"""
        # max record: 0x0085ff00 (fixed maximum)
        rec = b'\x00\x85\xff\x00'
        struct.pack_into('<H', rec, 0, 0x8000)  # n_fields = 1
        struct.pack_into('<I', rec, 2, 5)  # next = 5 bytes ahead
        data[offset:offset+4] = rec
        return offset + 4
    
    def create_data_page(self, page_no, records):
        """Create a data page with records"""
        # Start with page header
        header = self.write_page_header(page_no, FIL_PAGE_INDEX)
        
        # Add index header
        header = self.write_index_page_header(header)
        
        # Calculate data start after headers (38 + 14 = 52)
        data_start = 52
        
        # Write infimum and supremum fixed records
        data_offset = data_start
        data_offset = self.write_infimum_record(header, data_offset)
        data_offset = self.write_supremum_record(header, data_offset)
        
        # Write user records
        for record in records:
            rec_size = len(record)
            # Add record header
            rec_header = bytearray(REC_OFFS_HEADER_SIZE)
            struct.pack_into('<H', rec_header, 0, 0x8000 | rec_size)  # n_fields + length
            struct.pack_into('<H', rec_header, 2, 0)  # next record offset (will be set later)
            struct.pack_into('<H', rec_header, 6, 0)  # deleted mask
            struct.pack_into('<B', rec_header, 8, 0)  # min marked
            struct.pack_into('<B', rec_header, 9, 11)  # n_fields
            struct.pack_into('<B', rec_header, 10, 1)  # heap no
            
            # Put record header before data
            header[data_offset:data_offset + REC_OFFS_HEADER_SIZE] = rec_header
            header[data_offset + REC_OFFS_HEADER_SIZE:data_offset + REC_OFFS_HEADER_SIZE + rec_size] = record
            data_offset += REC_OFFS_HEADER_SIZE + rec_size
        
        # Set next pointers for fixed records
        struct.pack_into('<H', header, 52 + 2, data_start + 4)  # infimum -> supremum
        struct.pack_into('<H', header, data_start + 4 + 2, 100)  # supremum -> end
        
        # Set page trailer (last 8 bytes of page)
        trailer_offset = self.page_size - 8
        struct.pack_into('<I', header, trailer_offset, 0)  # check sum confirm
        
        return header
    
    def write_record(self, page_data, record_bytes, prev_offset):
        """Write a user record and update pointers"""
        offset = len(page_data)
        
        # Simple record format for testing
        # Record format: [header][data]
        record = bytearray(len(record_bytes) + 11)
        
        # Record header (11 bytes for compact format)
        # 1. n_fields + info_bits (2 bytes)
        struct.pack_into('<H', record, 0, 0x8000 | 11)  # 11 fields, info bits
        
        # 2. next record offset (2 bytes) 
        struct.pack_into('<H', record, 2, 0)  # Will be set later
        
        # 3. heap number (1 byte)
        struct.pack_into('<B', record, 4, 2)  # heap number 2
        
        # 4. record type (1 byte)
        struct.pack_into('<B', record, 5, 0)  # normal record
        
        # 5. n_owned (1 byte)
        struct.pack_into('<B', record, 6, 1)
        
        # 6. null bitmap (1 byte) - no nulls
        struct.pack_into('<B', record, 7, 0)
        
        # 7. variable length header (2 bytes per variable field)
        # 4 variable fields: name, email, phone, address
        struct.pack_into('<H', record, 8, 0)
        
        # 8. fixed data length (1 byte)
        struct.pack_into('<B', record, 10, 0)
        
        # Copy actual record data
        record[11:11 + len(record_bytes)] = record_bytes
        
        # Update previous record's next pointer
        struct.pack_into('<H', page_data, prev_offset + 2, offset)
        
        return page_data + record, offset + len(record)
    
    def finalize(self):
        """Finalize the IBD file"""
        pass

def generate_record_data(id_val, name, email, phone, address, age, salary, status):
    """Generate record data for a user row"""
    # Format: id(8) | name(50) | email(100) | phone(20) | address(200) | age(4) | salary(10) | status(1)
    # Using compact format: variable fields first
    
    record = bytearray()
    
    # id (BIGINT UNSIGNED) - 8 bytes
    record.extend(struct.pack('<Q', id_val))
    
    # name (VARCHAR 50) - variable length
    name_bytes = name.encode('utf-8')
    record.extend(struct.pack('<B', len(name_bytes)))  # length prefix
    record.extend(name_bytes)
    
    # email (VARCHAR 100)
    email_bytes = email.encode('utf-8')
    record.extend(struct.pack('<B', len(email_bytes)))
    record.extend(email_bytes)
    
    # phone (VARCHAR 20)
    if phone:
        phone_bytes = phone.encode('utf-8')
        record.extend(struct.pack('<B', len(phone_bytes)))
        record.extend(phone_bytes)
    else:
        record.extend(b'\xff')  # NULL
    
    # address (VARCHAR 200)
    if address:
        addr_bytes = address.encode('utf-8')
        record.extend(struct.pack('<B', len(addr_bytes)))
        record.extend(addr_bytes)
    else:
        record.extend(b'\xff')
    
    # age (INT) - 4 bytes
    record.extend(struct.pack('<i', age if age else 0))
    
    # salary (DECIMAL(10,2)) - encoded as 3 bytes
    if salary:
        int_val = int(salary * 100)
        salary_bytes = int_val.to_bytes(3, 'little', signed=True)
        record.extend(salary_bytes)
    else:
        record.extend(b'\x00\x00\x00')
    
    # status (TINYINT) - 1 byte
    record.extend(struct.pack('<b', status if status else 1))
    
    return bytes(record)

def create_test_ibd(num_records=5000000, output_path='/app/mysql_recovery_pro/test_data/generated.ibd'):
    """Create a test IBD file with the specified number of records"""
    print(f"Creating test IBD file with {num_records} records...")
    print(f"Output: {output_path}")
    
    # Structure:
    # Page 0: Root page (BTR page)
    # Page 1: Leaf page 1
    # Page 2: Leaf page 2
    # ...
    
    id_val = 1
    pages = []
    
    # Generate user data
    names = ['张三', '李四', '王五', '赵六', '孙七', '周八', '吴九', '郑十', '王十二', '刘十三']
    cities = ['北京市', '上海市', '广州市', '深圳市', '杭州市', '成都市', '武汉市', '西安市', '重庆市', '天津市']
    
    records_per_page = 100  # Estimate records per page
    
    print("Generating records...")
    for page_no in range(num_records // records_per_page + 1):
        records = []
        for i in range(records_per_page):
            if id_val > num_records:
                break
            
            name = names[id_val % len(names)] + str(id_val)
            email = f"{name}@example.com"
            phone = f"13{100000000 + id_val}"
            address = f"{cities[id_val % len(cities)]}{id_val}号"
            age = 20 + (id_val % 40)
            salary = 5000.0 + (id_val % 10000)
            status = 1 + (id_val % 3)
            
            record = generate_record_data(id_val, name, email, phone, address, age, salary, status)
            records.append(record)
            id_val += 1
        
        if records:
            pages.append(records)
    
    print(f"Generated {len(pages)} pages of records")
    
    # Create IBD file with proper InnoDB format
    # Note: This is a simplified representation
    page_size = 16384
    file_size = (num_records // records_per_page + 2) * page_size
    
    print(f"Creating file of size {file_size} bytes...")
    
    with open(output_path, 'wb') as f:
        # Write pages
        for page_no, records in enumerate(pages):
            # Create page data
            page_data = bytearray(page_size)
            
            # Write FIL header
            struct.pack_into('<I', page_data, 0, 1234567890)  # space_id
            struct.pack_into('<I', page_data, 4, page_no)      # offset
            struct.pack_into('<Q', page_data, 8, (page_no + 1) * 1000)  # lsn
            struct.pack_into('<H', page_data, 16, FIL_PAGE_INDEX)  # page type
            struct.pack_into('<H', page_data, 18, 0)  # n_reco
            struct.pack_into('<H', page_data, 20, 10)  # n_dir_slots
            struct.pack_into('<H', page_data, 22, 0)  # n_deleted
            struct.pack_into('<H', page_data, 24, 4 + len(records))  # n_heap
            struct.pack_into('<H', page_data, 26, 52)  # free offset
            
            # Write BTR header
            struct.pack_into('<H', page_data, 38, 0)  # level
            struct.pack_into('<H', page_data, 40, 1)  # n_nodes
            struct.pack_into('<Q', page_data, 44, 9876543210)  # index_id
            
            # Write record data after headers
            data_offset = 52
            
            # Write infimum record (fixed minimum)
            infimum = b'\x00\x85\x80\x00\x00\x01\x00\x00\x00\x00\x00'
            page_data[data_offset:data_offset+len(infimum)] = infimum
            data_offset += len(infimum)
            
            # Write supremum record (fixed maximum)  
            supremum = b'\x00\x85\xff\x00\x00\x01\x00\x00\x00\x00\x00'
            page_data[data_offset:data_offset+len(supremum)] = supremum
            data_offset += len(supremum)
            
            # Set next pointers
            struct.pack_into('<H', page_data, data_offset - len(supremum) + 2, data_offset)
            
            # Write user records
            for record in records:
                # Simple variable-length format
                var_header = struct.pack('<H', 0)  # no variable fields for simplicity
                page_data[data_offset:data_offset+len(var_header)] = var_header
                data_offset += len(var_header)
                
                page_data[data_offset:data_offset+len(record)] = record
                data_offset += len(record)
            
            # Write page trailer
            page_data[-8:] = b'\x00' * 8
            
            # Write page to file
            f.write(page_data)
    
    print(f"IBD file created: {output_path}")
    print(f"File size: {os.path.getsize(output_path)} bytes")
    
    return output_path

def verify_ibd_file(ibd_path):
    """Verify the IBD file structure"""
    print(f"\nVerifying: {ibd_path}")
    
    file_size = os.path.getsize(ibd_path)
    page_size = 16384
    num_pages = file_size // page_size
    
    print(f"Size: {file_size} bytes, Pages: {num_pages}")
    
    with open(ibd_path, 'rb') as f:
        for page_no in [0, 1, 10, 100, num_pages - 1]:
            if page_no >= num_pages:
                break
            
            f.seek(page_no * page_size)
            header = f.read(64)
            
            space_id = struct.unpack('<I', header[0:4])[0]
            offset = struct.unpack('<I', header[4:8])[0]
            lsn = struct.unpack('<Q', header[8:16])[0]
            page_type = struct.unpack('<H', header[16:18])[0]
            
            type_name = "UNKNOWN"
            if page_type == FIL_PAGE_INDEX:
                type_name = "FIL_PAGE_INDEX (0x45bc)"
            elif page_type == 0:
                type_name = "FIL_PAGE_ALLOCATED (0x0000)"
            
            print(f"  Page {page_no}: type={type_name}, offset={offset}, lsn={lsn}")
            
            if page_no == 0:
                # Check BTR header
                level = struct.unpack('<H', header[38:40])[0]
                n_nodes = struct.unpack('<H', header[40:42])[0]
                index_id = struct.unpack('<Q', header[44:52])[0]
                print(f"    BTR: level={level}, n_nodes={n_nodes}, index_id=0x{index_id:016x}")

if __name__ == '__main__':
    ibd_path = '/app/mysql_recovery_pro/test_data/generated.ibd'
    
    # Create IBD file
    create_test_ibd(num_records=5000000, output_path=ibd_path)
    
    # Verify
    verify_ibd_file(ibd_path)
