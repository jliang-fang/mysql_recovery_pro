#!/usr/bin/env python3
"""
Large-Scale Test Data Generator for MySQL Recovery Pro
Generates 100k+ records with complex data types for comprehensive testing
"""

import os
import sys
import struct
import random
import argparse
import hashlib
import json
from datetime import datetime, timedelta
from typing import List, Dict, Tuple, Optional
from pathlib import Path


# MySQL InnoDB constants
FIL_PAGE_TYPE_INDEX = 0x26F1  # 9969
FIL_PAGE_TYPE_SDI = 0x26F3    # 9971
FIL_PAGE_TYPE_UNDO_LOG = 0x26F2  # 9970
FIL_PAGE_TYPE_BLOB = 0x26F4   # 9972
FIL_PAGE_TYPE_ALLOCATED = 0

# Record status bits (lowest 3 bits of info_bits)
REC_STATUS_ORDINARY = 0x00   # ordinary record
REC_STATUS_NODE_PTR = 0x01   # node pointer record
REC_STATUS_INFIMUM = 0x02    # infimum system record
REC_STATUS_SUPREMUM = 0x03   # supremum system record

REC_INFO_MIN_REC_FLAG = 0x10
REC_INFO_DELETED_FLAG = 0x20


def write_le_uint16(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint16"""
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF


def write_le_uint32(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint32"""
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF
    data[offset + 2] = (value >> 16) & 0xFF
    data[offset + 3] = (value >> 24) & 0xFF


def write_le_uint64(data: bytearray, offset: int, value: int) -> None:
    """Write little-endian uint64"""
    for i in range(8):
        data[offset + i] = (value >> (i * 8)) & 0xFF


def write_le_float(data: bytearray, offset: int, value: float) -> None:
    """Write little-endian float"""
    packed = struct.pack('<f', value)
    for i, b in enumerate(packed):
        data[offset + i] = b


def write_le_double(data: bytearray, offset: int, value: float) -> None:
    """Write little-endian double"""
    packed = struct.pack('<d', value)
    for i, b in enumerate(packed):
        data[offset + i] = b


class DataGenerator:
    """Generate realistic test data"""

    CHINESE_NAMES = [
        "张伟", "李娜", "王芳", "刘强", "陈静", "杨洋", "赵敏", "黄勇", "周杰", "吴刚",
        "徐丽", "孙浩", "朱伟", "胡敏", "郭强", "何燕", "高杰", "林娜", "郑勇", "谢芳",
        "王磊", "张敏", "李杰", "刘洋", "陈勇", "杨静", "赵强", "黄丽", "周杰", "吴刚",
        "徐涛", "孙鹏", "朱磊", "胡静", "郭勇", "何敏", "高杰", "林娜", "郑强", "谢芳",
        "陈东", "王军", "李红", "张华", "刘伟", "杨柳", "赵峰", "黄萍", "周军", "吴敏",
    ]

    EMAIL_DOMAINS = ["gmail.com", "yahoo.com", "hotmail.com", "outlook.com", "qq.com",
                    "163.com", "126.com", "sina.com", "sohu.com", "weibo.com"]

    USERNAMES = [
        "user_001", "user_002", "user_003", "user_004", "user_005",
        "admin", "root", "test", "guest", "demo", "developer", "operator"
    ]

    STATUSES = [0, 1, 2, 3]

    def __init__(self, seed: int = None):
        if seed:
            random.seed(seed)
        else:
            random.seed(datetime.now().microsecond)

    def generate_name(self) -> str:
        return random.choice(self.CHINESE_NAMES)

    def generate_email(self) -> str:
        username = f"user_{random.randint(10000, 99999)}"
        domain = random.choice(self.EMAIL_DOMAINS)
        return f"{username}@{domain}"

    def generate_username(self) -> str:
        return random.choice(self.USERNAMES)

    def generate_password_hash(self) -> str:
        password = f"password_{random.randint(100000, 999999)}"
        return hashlib.sha256(password.encode()).hexdigest()

    def generate_status(self) -> int:
        return random.choice(self.STATUSES)

    def generate_score(self) -> int:
        return random.randint(0, 1000)

    def generate_json_data(self) -> str:
        patterns = [
            {"type": "customer", "level": 1, "tags": ["vip", "premium"]},
            {"type": "customer", "level": 2, "tags": ["vip", "gold"]},
            {"type": "customer", "level": 3, "tags": ["vip", "platinum"]},
            {"type": "vendor", "level": 1, "tags": ["verified"]},
            {"type": "vendor", "level": 2, "tags": ["verified", "premium"]},
            {"type": "system", "level": 0, "tags": ["admin"]},
            {"type": "guest", "level": 0, "tags": ["anonymous"]},
        ]
        pattern = random.choice(patterns)
        pattern["timestamp"] = datetime.now().isoformat()
        pattern["random_value"] = round(random.random(), 6)
        return json.dumps(pattern, ensure_ascii=False)

    def generate_blob_data(self, size: int = 1024) -> bytes:
        return bytes([random.randint(0, 255) for _ in range(size)])

    def generate_text_description(self, min_words: int = 10, max_words: int = 50) -> str:
        words = [
            "测试", "数据", "记录", "用户", "系统", "应用", "开发", "运维", "数据库",
            "MySQL", "InnoDB", "恢复", "备份", "查询", "更新", "插入", "删除", "表",
            "索引", "页", "块", "事务", "提交", "回滚", "锁", "并发", "性能", "优化"
        ]
        num_words = random.randint(min_words, max_words)
        return " ".join(random.choice(words) for _ in range(num_words))

    def generate_datetime(self, start_year: int = 2020, end_year: int = 2024) -> datetime:
        start = datetime(start_year, 1, 1)
        end = datetime(end_year, 12, 31)
        delta = end - start
        random_days = random.randint(0, delta.days)
        random_seconds = random.randint(0, 86400)
        return start + timedelta(days=random_days, seconds=random_seconds)

    def generate_date(self) -> str:
        dt = self.generate_datetime()
        return dt.strftime("%Y-%m-%d")

    def generate_time(self) -> str:
        dt = self.generate_datetime()
        return dt.strftime("%H:%M:%S")

    def generate_float(self) -> float:
        return round(random.uniform(0, 1000), 2)

    def generate_double(self) -> float:
        return round(random.uniform(0, 1000000), 6)

    def generate_decimal(self, precision: int = 10, scale: int = 2) -> str:
        integer_part = random.randint(0, 10**(precision-scale)-1)
        decimal_part = random.randint(0, 10**scale-1)
        return f"{integer_part}.{decimal_part:0{scale}d}"

    def generate_record(self, record_id: int, include_vector: bool = False) -> Dict:
        record = {
            "id": record_id,
            "name": self.generate_name(),
            "email": self.generate_email(),
            "username": self.generate_username(),
            "password_hash": self.generate_password_hash(),
            "status": self.generate_status(),
            "score": self.generate_score(),
            "description": self.generate_text_description(),
            "json_data": self.generate_json_data(),
            "blob_data": self.generate_blob_data(512).hex(),
            "created_at": self.generate_datetime().isoformat(),
            "updated_at": self.generate_datetime().isoformat(),
            "created_date": self.generate_date(),
            "created_time": self.generate_time(),
            "score_float": self.generate_float(),
            "score_double": self.generate_double(),
            "balance": self.generate_decimal(10, 2),
        }

        if include_vector:
            vector = [round(random.gauss(0, 1), 6) for _ in range(128)]
            record["vector_data"] = json.dumps(vector, ensure_ascii=False)

        return record


class InnoDBPageBuilder:
    """Build InnoDB pages with realistic data"""

    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.generator = DataGenerator()

    def create_btree_page(self, page_no: int, records: List[Dict],
                          prev_page: int = 0xFFFFFFFF,
                          next_page: int = 0xFFFFFFFF,
                          level: int = 0) -> bytearray:
        """Create a B+Tree index page with user records"""
        page = bytearray(self.page_size)

        # FIL Header (38 bytes)
        write_le_uint32(page, 0, prev_page)      # prev_page
        write_le_uint32(page, 4, next_page)      # next_page
        write_le_uint64(page, 8, 0)              # LSN
        # Note: space_id is 4 bytes starting at offset 16
        # But we need to write page_no and page_type carefully
        write_le_uint32(page, 16, page_no)       # page_no at offset 20, but page_type goes in space_id low bytes
        write_le_uint32(page, 20, page_no)       # page_no
        write_le_uint32(page, 24, 0)             # checksum
        write_le_uint32(page, 28, 0)             # checksum
        write_le_uint16(page, 36, 0)             # arch_log_no

        # Page Type - MUST be at offset 16-17 (overwrites low 2 bytes of space_id)
        write_le_uint16(page, 16, FIL_PAGE_TYPE_INDEX)

        # Page Header (offset 38)
        # n_dir_slots - will be set later after all records are created
        write_le_uint16(page, 38, 2)  # Start with INFIMUM + SUPREMUM
        # heap_top - starts after INFIMUM + SUPREMUM + some space
        heap_top = 54 + len(records) * 64 + 200
        write_le_uint16(page, 40, heap_top)
        # n_heap (includes level in high bits)
        write_le_uint16(page, 42, (len(records) + 2) | (level << 14))
        # free_offset
        write_le_uint16(page, 44, 54)
        # n_owned
        write_le_uint16(page, 46, 2)
        # heap_top (again for MySQL 8.0)
        write_le_uint16(page, 48, heap_top & 0xFFFF)
        # page_level
        write_le_uint16(page, 50, level)
        # n_owned (again)
        write_le_uint16(page, 52, 2)

        # INFIMUM record (fixed offset 54, 7 bytes header)
        # INFIMUM: info_bits(1) + next_offset(2) + heap_no(2) + record_length(2) = 7 bytes
        # info_bits should be REC_STATUS_INFIMUM (0x02) only for status bits
        # REC_INFO_MIN_REC_FLAG (0x10) is a separate flag bit, not part of status
        page[54] = REC_STATUS_INFIMUM  # Only status bits, not OR with flag
        write_le_uint16(page, 55, 68)   # next_record_offset = 68 (first user record)
        write_le_uint16(page, 57, 0)    # heap_no
        write_le_uint16(page, 59, 7)    # record_length = 7

        # SUPREMUM record (fixed at 61, 7 bytes header)
        # SUPREMUM is the last system record, user records come before it
        page[61] = REC_STATUS_SUPREMUM  # offset 61
        write_le_uint16(page, 62, 0x3FFF)   # next_record_offset = 0x3FFF (end of chain)
        write_le_uint16(page, 64, 1)        # heap_no
        write_le_uint16(page, 66, 7)        # record_length = 7

        # User records - start after INFIMUM but before SUPREMUM (offset 68)
        # Each user record has a 7-byte header, + data (variable, ~800 bytes max)
        current_offset = 68
        prev_offset = 54  # INFIMUM
        user_record_offsets = []  # Track user record offsets for directory slots
        record_end = 61  # SUPREMUM start offset

        for i, record in enumerate(records):
            # Calculate required space for this record
            # Record header: 7 bytes
            # id: 8 bytes
            # name: 1 + len(name_bytes) bytes (max 256)
            # email: 1 + len(email_bytes) bytes (max 256)
            # username: 1 + len(username_bytes) bytes (max 256)
            # password_hash: 64 bytes
            # status: 1 byte
            # score: 4 bytes
            # description: 1 + min(len, 255) bytes
            # json_data: 1 + min(len, 255) bytes
            # blob_data: 1 byte (NULL marker)
            # created_at: 8 bytes
            # updated_at: 4 bytes
            # created_date: 3 bytes
            # created_time: 3 bytes
            # score_float: 4 bytes
            # score_double: 8 bytes
            # balance: 1 + len(bytes) bytes
            # Total: ~800 bytes per record maximum
            record_space = 800

            if current_offset + record_space >= self.page_size - 100:
                break

            # Record header (7 bytes)
            page[current_offset] = REC_STATUS_ORDINARY
            write_le_uint16(page, current_offset + 1, 0x3FFF)  # next_record_offset (will be set later)
            write_le_uint16(page, current_offset + 3, i + 2)   # heap_no
            write_le_uint16(page, current_offset + 5, 0)       # record_length (will be set later)

            # Set previous record's next_offset to current
            write_le_uint16(page, prev_offset + 1, current_offset)
            
            # Update prev_offset for next iteration
            prev_offset = current_offset

            # Record data - compact format
            data_offset = current_offset + 7

            # id (BIGINT - 8 bytes)
            write_le_uint64(page, data_offset, record["id"])

            # name (VARCHAR 255 - 1 byte length + data)
            name_bytes = record["name"].encode('utf-8')
            name_len = min(len(name_bytes), 255)
            page[data_offset + 8] = name_len
            page[data_offset + 9:data_offset + 9 + name_len] = name_bytes[:name_len]

            # email (VARCHAR 255 - 1 byte length + data)
            email_bytes = record["email"].encode('utf-8')
            email_len = min(len(email_bytes), 255)
            email_start = data_offset + 9 + name_len
            page[email_start] = email_len
            page[email_start + 1:email_start + 1 + email_len] = email_bytes[:email_len]

            # username (VARCHAR 255)
            username_bytes = record["username"].encode('utf-8')
            username_len = min(len(username_bytes), 255)
            username_start = email_start + 1 + email_len
            page[username_start] = username_len
            page[username_start + 1:username_start + 1 + username_len] = username_bytes[:username_len]

            # password_hash (CHAR 64 - fixed)
            password_bytes = record["password_hash"].encode('utf-8')
            password_start = username_start + 1 + username_len
            page[password_start:password_start + 64] = password_bytes[:64]

            # status (TINYINT - 1 byte)
            status_start = password_start + 64
            page[status_start] = record["status"]

            # score (INT - 4 bytes)
            score_start = status_start + 1
            write_le_uint32(page, score_start, record["score"])

            # description (TEXT - stored inline with length prefix)
            desc_bytes = record["description"].encode('utf-8')
            desc_len = min(len(desc_bytes), 255)
            desc_start = score_start + 4
            page[desc_start] = desc_len
            page[desc_start + 1:desc_start + 1 + desc_len] = desc_bytes[:desc_len]

            # json_data (JSON - stored inline with length prefix)
            json_bytes = record["json_data"].encode('utf-8')
            json_len = min(len(json_bytes), 255)
            json_start = desc_start + 1 + desc_len
            page[json_start] = json_len
            page[json_start + 1:json_start + 1 + json_len] = json_bytes[:json_len]

            # blob_data (LONGBLOB - NULL marker for simplicity)
            blob_start = json_start + 1 + json_len
            page[blob_start] = 0xFF  # NULL marker

            # created_at (DATETIME - 8 bytes)
            dt = datetime.fromisoformat(record["created_at"])
            year = dt.year
            month = dt.month
            day = dt.day
            hour = dt.hour
            minute = dt.minute
            second = dt.second
            # MySQL datetime encoding: YYYY*10000 + MM*100 + DD for date, HH*10000 + MM*100 + SS for time
            dt_encoded = (year * 10000 + month * 100 + day) * 1000000 + hour * 10000 + minute * 100 + second
            created_at_start = blob_start + 1
            write_le_uint64(page, created_at_start, dt_encoded)

            # updated_at (TIMESTAMP - 4 bytes)
            ts = int(dt.timestamp())
            updated_at_start = created_at_start + 8
            write_le_uint32(page, updated_at_start, ts)

            # created_date (DATE - 3 bytes)
            date_encoded = year * 10000 + month * 100 + day
            created_date_start = updated_at_start + 4
            page[created_date_start] = date_encoded & 0xFF
            page[created_date_start + 1] = (date_encoded >> 8) & 0xFF
            page[created_date_start + 2] = (date_encoded >> 16) & 0xFF

            # created_time (TIME - 3 bytes)
            time_encoded = hour * 10000 + minute * 100 + second
            created_time_start = created_date_start + 3
            page[created_time_start] = time_encoded & 0xFF
            page[created_time_start + 1] = (time_encoded >> 8) & 0xFF
            page[created_time_start + 2] = (time_encoded >> 16) & 0xFF

            # score_float (FLOAT - 4 bytes)
            score_float_start = created_time_start + 3
            write_le_float(page, score_float_start, record["score_float"])

            # score_double (DOUBLE - 8 bytes)
            score_double_start = score_float_start + 4
            write_le_double(page, score_double_start, record["score_double"])

            # balance (DECIMAL - variable, simplified to 10 bytes)
            balance_str = record["balance"]
            balance_bytes = balance_str.replace('.', '').encode('utf-8')
            balance_len = min(len(balance_bytes), 255)
            balance_start = score_double_start + 8
            page[balance_start] = balance_len
            page[balance_start + 1:balance_start + 1 + balance_len] = balance_bytes[:balance_len]

            # Calculate record length
            record_end = balance_start + 1 + balance_len
            record_length = record_end - current_offset - 7
            write_le_uint16(page, current_offset + 5, record_length)

            # Update next_record_offset
            # For the last user record, point to SUPREMUM (offset 61)
            # For other records, point to the start of the next record
            if i == len(records) - 1:
                # Last user record - point to SUPREMUM
                next_offset = 61
            else:
                # Point to the start of the next record
                next_offset = record_end
            write_le_uint16(page, current_offset + 1, next_offset)

            # Track user record offset for directory slot
            user_record_offsets.append(current_offset)

            current_offset = next_offset

        # After all records are linked, calculate final heap_top
        # heap_top should be after all records and before directory slots
        # For simplicity, set it to the end of the last user record data
        final_heap_top = record_end if len(records) > 0 else 61 + 7  # SUPREMUM end
        write_le_uint16(page, 40, final_heap_top)

        # Build directory slots
        # Directory structure: [INFIMUM, SUPREMUM, user_record_0, user_record_1, ...]
        # Directory slots are stored AFTER all records, pointing to record offsets
        # Directory slots are written in descending order of record offsets
        dir_slots = [54]  # INFIMUM record offset (offset 54)
        dir_slots.extend(user_record_offsets)  # User records
        dir_slots.append(61)  # SUPREMUM record offset (offset 61)
        
        # Debug: verify dir_slots before writing
        print(f"DEBUG: user_record_offsets = {user_record_offsets}", file=sys.stderr)
        print(f"DEBUG: dir_slots before reverse = {dir_slots}", file=sys.stderr)
        print(f"DEBUG: n_dir_slots = {len(dir_slots)}", file=sys.stderr)

        # Update n_dir_slots to reflect total slot count
        n_dir_slots = len(dir_slots)

        # Calculate directory slot position properly:
        # The page directory should be placed after all user records
        # and before the heap_top
        # We use a fixed position near the end of the page
        # but far enough from INFIMUM/SUPREMUM (offsets 54 and 61)
        # For a page with many records, put directory after last record
        # For pages with few records, put directory at a safe position
        
        # Find the maximum record offset in the chain
        max_record_offset = max(user_record_offsets) if user_record_offsets else 61
        
        # Directory should be after the last record's data
        # Assume each record takes about 100 bytes for calculation
        estimated_dir_pos = max_record_offset + 100
        
        # Make sure directory is not too close to INFIMUM/SUPREMUM
        dir_slot_position = max(estimated_dir_pos, 15000)
        
        # But ensure it's within page bounds
        page_size = self.page_size  # Use instance variable
        if dir_slot_position + n_dir_slots * 2 > page_size - 100:
            dir_slot_position = page_size - 100 - n_dir_slots * 2

        # Write n_dir_slots to page header
        write_le_uint16(page, 38, n_dir_slots)

        # Write directory slots at the calculated position
        # Directory slots are written in descending order of record offsets
        # dir_slots is [INFIMUM, user_records..., SUPREMUM]
        # but slots in memory should be [SUPREMUM, user_records..., INFIMUM] (descending)
        for i, rec_offset in enumerate(reversed(dir_slots)):
            # Directory slot format: 15-bit offset + 1-bit flag (0 for most records)
            slot_value = rec_offset & 0x7FFF
            write_le_uint16(page, dir_slot_position + i * 2, slot_value)

        # Verify written slots
        import struct
        for i in range(n_dir_slots):
            slot_offset = dir_slot_position + i * 2
            val = struct.unpack('<H', page[slot_offset:slot_offset+2])[0]
            expected = list(reversed(dir_slots))[i]
            if val != expected:
                print(f"DEBUG MISMATCH: Slot {i} = {val}, expected {expected}", file=sys.stderr)

        # Update heap_top to be after all records and directory slots
        # heap_top should be dir_slot_position + n_dir_slots * 2
        # so that dir_start = heap_top - n_dir_slots * 2 = dir_slot_position
        final_heap_top = dir_slot_position + n_dir_slots * 2
        write_le_uint16(page, 40, final_heap_top)

        return page

    def create_sdi_page(self, page_no: int, table_id: int = 12345,
                        table_name: str = "test_users",
                        database_name: str = "testdb") -> bytearray:
        """Create an SDI page (MySQL 8.0+)"""
        page = bytearray(self.page_size)

        # FIL Header (38 bytes)
        # prev_page (FIL_PAGE_PREV) - offset 0
        write_le_uint32(page, 0, 0xFFFFFFFF)
        # next_page (FIL_PAGE_NEXT) - offset 4
        write_le_uint32(page, 4, 0xFFFFFFFF)
        # LSN (FIL_PAGE_LSN) - offset 8 (8 bytes)
        write_le_uint64(page, 8, 0)
        # space_id (FIL_PAGE_SPACE_ID) - offset 16 (4 bytes)
        write_le_uint32(page, 16, 0)
        # page_no (FIL_PAGE_OFFSET) - offset 20 (4 bytes)
        write_le_uint32(page, 20, page_no)
        # checksum (FIL_PAGE_CHECKSUM) - offset 24 (4 bytes)
        write_le_uint32(page, 24, 0)
        # arch_log_no or page_type_high - offset 28 (4 bytes)
        write_le_uint32(page, 28, 0)
        # arch_log_no (FIL_PAGE_ARCH_LOG_NO) - offset 36 (2 bytes)
        write_le_uint16(page, 36, 0)

        # Page Type - MUST be at offset 16-17 (overwrites space_id low 2 bytes)
        write_le_uint16(page, 16, FIL_PAGE_TYPE_SDI)

        # SDI payload starts at offset 38 (after FIL header)
        # SDI format: type(4) + id(8) + uncompressed_len(4) + compressed_len(4) + data
        sdi_data = {
            "dictionary": {
                "table": {
                    "name": table_name,
                    "database": database_name,
                    "id": table_id,
                    "columns": [
                        {"name": "id", "type": "LONGLONG", "is_nullable": False},
                        {"name": "name", "type": "VARCHAR", "length": 255, "is_nullable": True},
                        {"name": "email", "type": "VARCHAR", "length": 255, "is_nullable": True},
                        {"name": "username", "type": "VARCHAR", "length": 255, "is_nullable": True},
                        {"name": "password_hash", "type": "CHAR", "length": 64, "is_nullable": True},
                        {"name": "status", "type": "TINYINT", "is_nullable": True},
                        {"name": "score", "type": "INT", "is_nullable": True},
                        {"name": "description", "type": "TEXT", "is_nullable": True},
                        {"name": "json_data", "type": "JSON", "is_nullable": True},
                        {"name": "blob_data", "type": "LONGBLOB", "is_nullable": True},
                        {"name": "created_at", "type": "DATETIME", "is_nullable": True},
                        {"name": "updated_at", "type": "TIMESTAMP", "is_nullable": True},
                        {"name": "created_date", "type": "DATE", "is_nullable": True},
                        {"name": "created_time", "type": "TIME", "is_nullable": True},
                        {"name": "score_float", "type": "FLOAT", "is_nullable": True},
                        {"name": "score_double", "type": "DOUBLE", "is_nullable": True},
                        {"name": "balance", "type": "DECIMAL", "precision": 10, "scale": 2, "is_nullable": True},
                    ],
                    "row_format": 2,  # DYNAMIC
                    "page_size": self.page_size,
                }
            }
        }

        sdi_json = json.dumps(sdi_data, ensure_ascii=False).encode('utf-8')
        
        # Compress using zlib with raw deflate (no header)
        import zlib
        compressed = zlib.compress(sdi_json, level=6)
        # Remove zlib header (first 2 bytes) and checksum (last 4 bytes) for raw deflate
        compressed = compressed[2:-4]
        
        # SDI header at offset 38
        sdi_header_offset = 38
        # type (4 bytes) - type 0 for table metadata
        write_le_uint32(page, sdi_header_offset, 0)
        # id (8 bytes) - table ID
        write_le_uint64(page, sdi_header_offset + 4, table_id)
        # uncompressed_len (4 bytes)
        write_le_uint32(page, sdi_header_offset + 12, len(sdi_json))
        # compressed_len (4 bytes)
        write_le_uint32(page, sdi_header_offset + 16, len(compressed))
        
        # SDI payload after header
        sdi_payload_offset = sdi_header_offset + 20
        page[sdi_payload_offset:sdi_payload_offset + len(compressed)] = compressed

        return page


class LargeScaleDataGenerator:
    """Generate large-scale test data with corruption scenarios"""

    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.builder = InnoDBPageBuilder(page_size)

    def generate_test_table(self, num_records: int = 100000,
                           records_per_page: int = 18) -> Tuple[bytearray, List[Dict]]:
        """Generate a complete test table with many records"""
        print(f"Generating {num_records} records...")

        # Generate all records
        records = []
        for i in range(1, num_records + 1):
            record = self.builder.generator.generate_record(i)
            records.append(record)

        # Calculate pages needed - use smaller records_per_page to fit more records per page
        # Each record is ~800 bytes, so we can fit about 15-20 records per 16KB page
        records_per_page = 15
        total_pages = max(3, (num_records + records_per_page - 1) // records_per_page + 2)

        # Create pages
        pages = []

        # Page 0: Root B+Tree page
        root_records = records[:records_per_page]
        root_page = self.builder.create_btree_page(
            page_no=0,
            records=root_records,
            prev_page=0xFFFFFFFF,
            next_page=1,
            level=0
        )
        pages.append(root_page)

        # Create leaf pages
        for i in range(1, total_pages - 1):
            start_idx = i * records_per_page
            end_idx = min(start_idx + records_per_page, num_records)
            page_records = records[start_idx:end_idx]

            page = self.builder.create_btree_page(
                page_no=i,
                records=page_records,
                prev_page=i - 1,
                next_page=i + 1,
                level=0
            )
            pages.append(page)

        # Last page
        if len(pages) > 1:
            start_idx = (total_pages - 2) * records_per_page
            end_idx = num_records
            page_records = records[start_idx:end_idx]

            last_page = self.builder.create_btree_page(
                page_no=len(pages) - 1,
                records=page_records,
                prev_page=len(pages) - 2,
                next_page=0xFFFFFFFF,
                level=0
            )
            pages.append(last_page)

        # SDI page
        sdi_page = self.builder.create_sdi_page(
            page_no=len(pages),
            table_id=12345,
            table_name="test_users",
            database_name="testdb"
        )
        pages.append(sdi_page)

        # Combine pages
        result = bytearray()
        for page in pages:
            result.extend(page)

        print(f"Generated {len(pages)} pages, {len(result)} bytes")
        return result, records

    def corrupt_page(self, page: bytearray, corruption_type: str) -> bytearray:
        """Apply corruption to a page"""
        corrupted = bytearray(page)

        if corruption_type == "header_type":
            # Corrupt page type
            write_le_uint16(corrupted, 16, 0xFFFF)
        elif corruption_type == "header_checksum":
            # Corrupt checksum
            write_le_uint32(corrupted, 24, 0xDEADBEEF)
            write_le_uint32(corrupted, 28, 0xDEADBEEF)
        elif corruption_type == "infimum_corrupt":
            # Corrupt INFIMUM record
            corrupted[54] = 0xFF
        elif corruption_type == "supremum_corrupt":
            # Corrupt SUPREMUM record
            corrupted[54 + 13] = 0xFF
        elif corruption_type == "chain_broken":
            # Break page chain
            write_le_uint32(corrupted, 4, 0xFFFFFFF0)
        elif corruption_type == "record_chain_broken":
            # Break record chain
            write_le_uint16(corrupted, 55, 0xFFFF)
        elif corruption_type == "random":
            # Random corruption - corrupt 1% of bytes
            num_corruptions = max(1, len(corrupted) // 100)
            for _ in range(num_corruptions):
                pos = random.randint(0, len(corrupted) - 1)
                corrupted[pos] = random.randint(0, 255)
        elif corruption_type == "truncate":
            # Truncate page
            corrupted = corrupted[:len(corrupted) // 2]
        elif corruption_type == "all_zero":
            # Zero out the page
            corrupted = bytearray(len(corrupted))

        return corrupted

    def generate_corrupted_table(self, num_records: int = 100000,
                                 corruption_type: str = "random",
                                 corruption_ratio: float = 0.01) -> Tuple[bytearray, List[Dict]]:
        """Generate a table with corrupted pages"""
        table_data, records = self.generate_test_table(num_records)

        # Calculate which pages to corrupt
        page_size = self.page_size
        total_pages = len(table_data) // page_size
        num_corruptions = max(1, int(total_pages * corruption_ratio))

        print(f"Corrupting {num_corruptions} pages ({corruption_ratio*100}%)...")

        # Corrupt random pages
        corrupted_positions = random.sample(range(1, total_pages - 1), num_corruptions)

        for page_no in corrupted_positions:
            start = page_no * page_size
            end = start + page_size
            original_page = table_data[start:end]
            corrupted_page = self.corrupt_page(original_page, corruption_type)
            table_data[start:end] = corrupted_page

        print(f"Corrupted pages: {corrupted_positions}")
        return table_data, records


def main():
    parser = argparse.ArgumentParser(
        description='Generate large-scale test data for MySQL Recovery Pro'
    )
    parser.add_argument('--output-dir', default='test_data',
                        help='Output directory')
    parser.add_argument('--num-records', type=int, default=100000,
                        help='Number of records (default: 100000)')
    parser.add_argument('--records-per-page', type=int, default=100,
                        help='Records per page (default: 100)')
    parser.add_argument('--page-size', type=int, default=16384,
                        choices=[4096, 8192, 16384, 32768, 65536],
                        help='Page size (default: 16384)')
    parser.add_argument('--corruption', type=str, default=None,
                        choices=['header_type', 'header_checksum', 'infimum_corrupt',
                                 'supremum_corrupt', 'chain_broken', 'record_chain_broken',
                                 'random', 'truncate', 'all_zero'],
                        help='Corruption type')
    parser.add_argument('--corruption-ratio', type=float, default=0.01,
                        help='Corruption ratio (default: 0.01 = 1%)')
    parser.add_argument('--seed', type=int, default=None,
                        help='Random seed for reproducibility')

    args = parser.parse_args()

    if args.seed:
        random.seed(args.seed)

    os.makedirs(args.output_dir, exist_ok=True)

    generator = LargeScaleDataGenerator(page_size=args.page_size)

    if args.corruption:
        # Generate corrupted table
        table_data, records = generator.generate_corrupted_table(
            num_records=args.num_records,
            corruption_type=args.corruption,
            corruption_ratio=args.corruption_ratio
        )

        output_path = os.path.join(
            args.output_dir,
            f'test_large_corrupted_{args.corruption}_rec{args.num_records}_ps{args.page_size}.ibd'
        )

        with open(output_path, 'wb') as f:
            f.write(table_data)

        print(f"\nGenerated corrupted table: {output_path}")
        print(f"  - Records: {args.num_records}")
        print(f"  - Page size: {args.page_size}")
        print(f"  - File size: {len(table_data)} bytes")
        print(f"  - Corruption: {args.corruption} ({args.corruption_ratio*100}%)")
    else:
        # Generate normal table
        table_data, records = generator.generate_test_table(
            num_records=args.num_records,
            records_per_page=args.records_per_page
        )

        output_path = os.path.join(
            args.output_dir,
            f'test_large_rec{args.num_records}_ps{args.page_size}.ibd'
        )

        with open(output_path, 'wb') as f:
            f.write(table_data)

        print(f"\nGenerated test table: {output_path}")
        print(f"  - Records: {args.num_records}")
        print(f"  - Page size: {args.page_size}")
        print(f"  - File size: {len(table_data)} bytes")
        print(f"  - Pages: {len(table_data) // args.page_size}")


if __name__ == '__main__':
    main()
