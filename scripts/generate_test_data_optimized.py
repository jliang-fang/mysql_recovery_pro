#!/usr/bin/env python3
"""
Optimized Test Data Generator for MySQL Recovery Pro
Generates 100k+ records with optimized data types for better page utilization
"""

import os
import sys
import struct
import random
import argparse
import json
from datetime import datetime, timedelta
from typing import List, Dict, Tuple
from pathlib import Path


# MySQL InnoDB constants
FIL_PAGE_TYPE_INDEX = 0x26F1  # 9969
FIL_PAGE_TYPE_SDI = 0x26F3    # 9971
# Rec status bits (lowest 3 bits of info_bits)
REC_STATUS_ORDINARY = 0x00   # ordinary record
REC_STATUS_NODE_PTR = 0x01   # node pointer record
REC_STATUS_INFIMUM = 0x02    # infimum system record
REC_STATUS_SUPREMUM = 0x03   # supremum system record
REC_INFO_MIN_REC_FLAG = 0x10  # min mark flag (bit 4)


def write_le_uint16(data: bytearray, offset: int, value: int) -> None:
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF


def write_le_uint32(data: bytearray, offset: int, value: int) -> None:
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF
    data[offset + 2] = (value >> 16) & 0xFF
    data[offset + 3] = (value >> 24) & 0xFF


def write_le_uint64(data: bytearray, offset: int, value: int) -> None:
    for i in range(8):
        data[offset + i] = (value >> (i * 8)) & 0xFF


def write_le_float(data: bytearray, offset: int, value: float) -> None:
    packed = struct.pack('<f', value)
    for i, b in enumerate(packed):
        data[offset + i] = b


def write_le_double(data: bytearray, offset: int, value: float) -> None:
    packed = struct.pack('<d', value)
    for i, b in enumerate(packed):
        data[offset + i] = b


class OptimizedDataGenerator:
    """Generate optimized test data with smaller record size"""

    CHINESE_NAMES = [
        "张伟", "李娜", "王芳", "刘强", "陈静", "杨洋", "赵敏", "黄勇", "周杰", "吴刚",
        "徐丽", "孙浩", "朱伟", "胡敏", "郭强", "何燕", "高杰", "林娜", "郑勇", "谢芳",
    ]

    def __init__(self, seed: int = None):
        if seed:
            random.seed(seed)
        else:
            random.seed(datetime.now().microsecond)

    def generate_name(self) -> str:
        return random.choice(self.CHINESE_NAMES)

    def generate_email(self) -> str:
        return f"user{random.randint(10000, 99999)}@test.com"

    def generate_status(self) -> int:
        return random.randint(0, 3)

    def generate_score(self) -> int:
        return random.randint(0, 1000)

    def generate_float_score(self) -> float:
        return round(random.uniform(0, 100), 2)

    def generate_double_score(self) -> float:
        return round(random.uniform(0, 1000), 4)

    def generate_datetime(self, start_year: int = 2020, end_year: int = 2024) -> datetime:
        start = datetime(start_year, 1, 1)
        end = datetime(end_year, 12, 31)
        delta = end - start
        random_days = random.randint(0, delta.days)
        random_seconds = random.randint(0, 86400)
        return start + timedelta(days=random_days, seconds=random_seconds)

    def generate_record(self, record_id: int) -> Dict:
        """Generate a single record with optimized size (~100 bytes)"""
        name = self.generate_name()
        email = self.generate_email()
        status = self.generate_status()
        score = self.generate_score()
        float_score = self.generate_float_score()
        double_score = self.generate_double_score()
        created_at = self.generate_datetime()

        return {
            "id": record_id,
            "name": name,
            "email": email,
            "status": status,
            "score": score,
            "float_score": float_score,
            "double_score": double_score,
            "created_at": created_at.isoformat(),
        }


class InnoDBPageBuilder:
    """Build InnoDB pages with optimized record size"""

    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.generator = OptimizedDataGenerator()

    def create_btree_page(self, page_no: int, records: List[Dict],
                          prev_page: int = 0xFFFFFFFF,
                          next_page: int = 0xFFFFFFFF,
                          level: int = 0) -> bytearray:
        """Create a B+Tree index page with user records"""
        page = bytearray(self.page_size)

        # FIL Header (38 bytes)
        write_le_uint32(page, 0, prev_page)
        write_le_uint32(page, 4, next_page)
        write_le_uint64(page, 8, 0)
        write_le_uint32(page, 16, 0)
        write_le_uint32(page, 20, page_no)
        write_le_uint32(page, 24, 0)
        write_le_uint32(page, 28, 0)
        write_le_uint16(page, 36, 0)

        # Page Type
        write_le_uint16(page, 16, FIL_PAGE_TYPE_INDEX)

        # Page Header
        write_le_uint16(page, 38, 2)
        heap_top = 54 + len(records) * 128 + 100
        write_le_uint16(page, 40, heap_top)
        write_le_uint16(page, 42, (len(records) + 2) | (level << 14))
        write_le_uint16(page, 44, 54)
        write_le_uint16(page, 46, 2)
        write_le_uint16(page, 48, heap_top & 0xFFFF)
        write_le_uint16(page, 50, level)
        write_le_uint16(page, 52, 2)

        # INFIMUM record (fixed offset 54)
        # info_bits = REC_STATUS_INFIMUM (0x02) - status bits only, NOT OR with REC_INFO_MIN_REC_FLAG
        page[54] = REC_STATUS_INFIMUM
        write_le_uint16(page, 55, 54 + 13)
        write_le_uint16(page, 57, 0)
        write_le_uint16(page, 59, 13)

        # SUPREMUM record
        page[54 + 13] = REC_STATUS_SUPREMUM
        write_le_uint16(page, 54 + 14, 0x3FFF)  # next_record_offset = 0x3FFF (15-bit)
        write_le_uint16(page, 54 + 16, 1)       # heap_no (at 64-65, not 63-64!)
        write_le_uint16(page, 54 + 18, 7)       # record_length (at 66-67, not 65-66!)

        # User records
        current_offset = 54 + 13 + 7  # SUPREMUM record is 7 bytes
        prev_offset = 54

        for i, record in enumerate(records):
            # Optimized record size: ~100 bytes
            record_space = 128

            if current_offset + record_space >= self.page_size - 100:
                break

            # Record header (7 bytes)
            page[current_offset] = REC_STATUS_ORDINARY
            write_le_uint16(page, current_offset + 1, 0x3FFF)
            write_le_uint16(page, current_offset + 3, i + 2)
            write_le_uint16(page, current_offset + 5, 0)

            # Set previous record's next_offset
            write_le_uint16(page, prev_offset + 1, current_offset)
            prev_offset = current_offset

            # Record data - compact format (~100 bytes)
            data_offset = current_offset + 7

            # id (BIGINT - 8 bytes)
            write_le_uint64(page, data_offset, record["id"])

            # name (VARCHAR - 1 byte length + data, max 50 chars)
            name_bytes = record["name"].encode('utf-8')
            name_len = min(len(name_bytes), 50)
            page[data_offset + 8] = name_len
            page[data_offset + 9:data_offset + 9 + name_len] = name_bytes[:name_len]

            # email (VARCHAR - 1 byte length + data, max 30 chars)
            email_bytes = record["email"].encode('utf-8')
            email_len = min(len(email_bytes), 30)
            email_start = data_offset + 9 + 50
            page[email_start] = email_len
            page[email_start + 1:email_start + 1 + email_len] = email_bytes[:email_len]

            # status (TINYINT - 1 byte)
            status_start = email_start + 1 + 30
            page[status_start] = record["status"]

            # score (INT - 4 bytes)
            score_start = status_start + 1
            write_le_uint32(page, score_start, record["score"])

            # float_score (FLOAT - 4 bytes)
            float_start = score_start + 4
            write_le_float(page, float_start, record["float_score"])

            # double_score (DOUBLE - 8 bytes)
            double_start = float_start + 4
            write_le_double(page, double_start, record["double_score"])

            # created_at (DATETIME - 8 bytes)
            dt = datetime.fromisoformat(record["created_at"])
            year = dt.year
            month = dt.month
            day = dt.day
            hour = dt.hour
            minute = dt.minute
            second = dt.second
            dt_encoded = (year * 10000 + month * 100 + day) * 1000000 + hour * 10000 + minute * 100 + second
            created_at_start = double_start + 8
            write_le_uint64(page, created_at_start, dt_encoded)

            # Calculate record length
            record_end = created_at_start + 8
            record_length = record_end - current_offset - 7
            write_le_uint16(page, current_offset + 5, record_length)

            # Update next_record_offset
            next_offset = record_end
            write_le_uint16(page, current_offset + 1, next_offset)

            current_offset = next_offset

        # Update next_record_offset for last record to point to SUPREMUM
        write_le_uint16(page, prev_offset + 1, 54 + 13)

        # Set directory slots after all records are created
        # Directory starts at offset 38 (FIL_HEADER) + 16 (PAGE_HEADER) = 54
        # Slot 0 (infimum): offset 54
        write_le_uint16(page, 54, 54)  # INFIMUM at offset 54
        # Slot 1 (supremum): offset 54 + 13 = 67
        write_le_uint16(page, 56, 54 + 13 | 0x8000)  # SUPREMUM at offset 67 with bit 15 set

        # Update heap_top
        write_le_uint16(page, 40, current_offset)

        return page

    def create_sdi_page(self, page_no: int, table_id: int = 12345,
                        table_name: str = "test_users",
                        database_name: str = "testdb") -> bytearray:
        """Create an SDI page (MySQL 8.0+)"""
        page = bytearray(self.page_size)

        # FIL Header
        write_le_uint32(page, 0, 0xFFFFFFFF)
        write_le_uint32(page, 4, 0xFFFFFFFF)
        write_le_uint64(page, 8, 0)
        write_le_uint32(page, 16, 0)
        write_le_uint32(page, 20, page_no)
        write_le_uint32(page, 24, 0)
        write_le_uint32(page, 28, 0)
        write_le_uint16(page, 36, 0)

        # Page Type
        write_le_uint16(page, 16, FIL_PAGE_TYPE_SDI)

        # SDI payload
        sdi_data = {
            "dictionary": {
                "table": {
                    "name": table_name,
                    "database": database_name,
                    "id": table_id,
                    "columns": [
                        {"name": "id", "type": "LONGLONG", "is_nullable": False},
                        {"name": "name", "type": "VARCHAR", "length": 50, "is_nullable": True},
                        {"name": "email", "type": "VARCHAR", "length": 30, "is_nullable": True},
                        {"name": "status", "type": "TINYINT", "is_nullable": True},
                        {"name": "score", "type": "INT", "is_nullable": True},
                        {"name": "float_score", "type": "FLOAT", "is_nullable": True},
                        {"name": "double_score", "type": "DOUBLE", "is_nullable": True},
                        {"name": "created_at", "type": "DATETIME", "is_nullable": True},
                    ],
                    "row_format": 2,
                    "page_size": self.page_size,
                }
            }
        }

        sdi_json = json.dumps(sdi_data, ensure_ascii=False).encode('utf-8')
        
        # Compress using zlib with raw deflate
        import zlib
        compressed = zlib.compress(sdi_json, level=6)
        compressed = compressed[2:-4]  # Remove zlib header and checksum
        
        # SDI header at offset 38
        sdi_header_offset = 38
        write_le_uint32(page, sdi_header_offset, 0)
        write_le_uint64(page, sdi_header_offset + 4, table_id)
        write_le_uint32(page, sdi_header_offset + 12, len(sdi_json))
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
                           records_per_page: int = 100) -> Tuple[bytearray, List[Dict]]:
        """Generate a complete test table with many records"""
        print(f"Generating {num_records} records...")

        # Generate all records
        records = []
        for i in range(1, num_records + 1):
            record = self.builder.generator.generate_record(i)
            records.append(record)

        # Calculate pages needed
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


def main():
    parser = argparse.ArgumentParser(description="Generate optimized test data for MySQL Recovery Pro")
    parser.add_argument("--records", type=int, default=100000, help="Number of records to generate")
    parser.add_argument("--records-per-page", type=int, default=100, help="Records per page")
    parser.add_argument("--output", type=str, default="/tmp/test_optimized.ibd", help="Output file path")
    parser.add_argument("--page-size", type=int, default=16384, help="Page size in bytes")
    args = parser.parse_args()

    generator = LargeScaleDataGenerator(page_size=args.page_size)
    table_data, records = generator.generate_test_table(
        num_records=args.records,
        records_per_page=args.records_per_page
    )

    # Create output directory if needed
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    # Write to file
    with open(args.output, "wb") as f:
        f.write(table_data)

    print(f"\nGenerated test table: {args.output}")
    print(f"  - Records: {len(records)}")
    print(f"  - Page size: {args.page_size}")
    print(f"  - File size: {len(table_data)} bytes")
    print(f"  - Pages: {len(table_data) // args.page_size}")


if __name__ == "__main__":
    main()
