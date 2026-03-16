#!/usr/bin/env python3
"""InnoDB .ibd 文件生成器，用于创建测试用的 InnoDB 页结构"""

import json
import zlib

# ==================== 常量定义 ====================
PAGE_SIZE = 16384  # 默认 InnoDB 页大小（16KB）

# MySQL 类型常量（参考 MySQL 8.0 源码 include/mysql_com.h）
MYSQL_TYPE_DECIMAL     = 0
MYSQL_TYPE_TINY        = 1
MYSQL_TYPE_SHORT       = 2
MYSQL_TYPE_LONG        = 3
MYSQL_TYPE_FLOAT       = 4
MYSQL_TYPE_DOUBLE      = 5
MYSQL_TYPE_NULL        = 6
MYSQL_TYPE_TIMESTAMP   = 7
MYSQL_TYPE_LONGLONG    = 8
MYSQL_TYPE_INT24       = 9
MYSQL_TYPE_DATE        = 10
MYSQL_TYPE_TIME        = 11
MYSQL_TYPE_DATETIME    = 12
MYSQL_TYPE_YEAR        = 13
MYSQL_TYPE_NEWDATE     = 14
MYSQL_TYPE_VARCHAR     = 15
MYSQL_TYPE_BIT         = 16
MYSQL_TYPE_TIMESTAMP2  = 17
MYSQL_TYPE_DATETIME2   = 18
MYSQL_TYPE_TIME2       = 19
MYSQL_TYPE_JSON        = 245

# InnoDB 页类型常量（参考 MySQL 8.0 源码 include/fil0fil.h）
FIL_PAGE_TYPE_ALLOCATED = 0
FIL_PAGE_INDEX        = 9969   # B+Tree 索引页
FIL_PAGE_UNDO_LOG     = 9970   # Undo 日志页
FIL_PAGE_SDI          = 9971   # SDI 元数据页
FIL_PAGE_TYPE_BLOB    = 9972   # BLOB页

# ==================== 辅助函数 ====================
def write_le_uint32(data, offset, value):
    """在指定偏移处写入小端序的 32 位无符号整数"""
    data[offset]     = (value >> 0) & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF
    data[offset + 2] = (value >> 16) & 0xFF
    data[offset + 3] = (value >> 24) & 0xFF

def write_le_uint64(data, offset, value):
    """在指定偏移处写入小端序的 64 位无符号整数"""
    for i in range(8):
        data[offset + i] = (value >> (i * 8)) & 0xFF

def write_le_uint16(data, offset, value):
    """在指定偏移处写入小端序的 16 位无符号整数"""
    data[offset]     = (value >> 0) & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF

# ==================== 页面生成函数 ====================
def create_page_header(page_type):
    """Create a minimal InnoDB page header"""
    page = bytearray(PAGE_SIZE)
    
    # FIL Header (38 bytes)
    write_le_uint32(page, 0x00, page_type)      # Page type
    write_le_uint32(page, 0x04, 0)              # Checksum1
    write_le_uint32(page, 0x08, 0)              # LSN (64-bit low)
    write_le_uint32(page, 0x0C, 0)              # LSN high
    write_le_uint16(page, 0x10, 0)              # Page offset
    write_le_uint16(page, 0x12, 0xFFFF)         # Previous page (none)
    write_le_uint16(page, 0x14, 0xFFFF)         # Next page (none)
    write_le_uint32(page, 0x18, 0)              # B-tree index (0 = leaf)
    
    return page

def create_sdi_page(table_name):
    """Create a SDI page with compressed JSON"""
    schema = {
        "schema": {"name": "test_recovery"},
        "table": {
            "name": table_name,
            "columns": [
                {"name": "id", "type": MYSQL_TYPE_LONGLONG, "is_nullable": False},
                {"name": "name", "type": MYSQL_TYPE_VARCHAR, "is_nullable": False, 
                 "length": 128, "charset_id": 255, "collation_id": 255,
                 "default_value": "unknown"},
                {"name": "email", "type": MYSQL_TYPE_VARCHAR, "is_nullable": True,
                 "length": 255, "charset_id": 255, "collation_id": 255},
                {"name": "created_at", "type": MYSQL_TYPE_DATETIME, "is_nullable": True,
                 "default_value": "CURRENT_TIMESTAMP"},
                {"name": "metadata", "type": MYSQL_TYPE_JSON, "is_nullable": True}
            ],
            "indexes": [
                {"name": "PRIMARY", "columns": ["id"], "is_primary": True},
                {"name": "idx_name", "columns": ["name"], "is_unique": False}
            ]
        }
    }
    
    json_str = json.dumps(schema, separators=(',', ':'))
    compressed = zlib.compress(json_str.encode(), 1)[2:]  # Skip zlib header
    
    sdi_header = bytearray(20)
    write_le_uint32(sdi_header, 0, 1)           # type
    write_le_uint64(sdi_header, 4, 1)           # id
    write_le_uint32(sdi_header, 12, len(json_str))  # uncompressed_len
    write_le_uint32(sdi_header, 16, len(compressed)) # compressed_len
    
    payload = sdi_header + compressed
    page = create_page_header(FIL_PAGE_SDI)
    
    # Write SDI payload at offset 38 (after FIL header)
    page[38:38+len(payload)] = payload
    
    return bytes(page)

def generate_ibd(table_name="users", include_blob=False):
    """Generate a minimal .ibd file with SDI + B+ tree with optional test data"""
    pages = []
    
    # Page 0: SDI
    pages.append(create_sdi_page(table_name))
    
    # Page 1: Root page of clustered index (with test records)
    root_page = create_page_header(FIL_PAGE_INDEX)
    write_le_uint32(root_page, 38, 0)   # PAGE_LEVEL = 0 (leaf)
    
    # 构建测试记录
    record_data = bytearray()
    
    # INFIMUM 记录（固定偏移 54）
    infimum = bytearray(13)
    infimum[0] = 0x02  # info_bits: REC_INFO_MIN_REC_FLAG
    write_le_uint16(infimum, 1, 56)  # next_record_offset
    record_data.extend(infimum)
    
    # 用户记录示例：id=1, name='test'
    user_rec = bytearray()
    user_rec.append(0x00)  # info_bits (普通记录)
    user_rec.append(0x00)  # n_fields_high + next_record_offset[15:8]
    write_le_uint16(user_rec, 1, 56 + len(record_data) + 2)  # next_record_offset
    
    # NULL 位图（假设所有列都非空）
    user_rec.extend(b'\x00')  # 1 byte null bitmap
    
    # id 字段 (LONGLONG, 8 bytes)
    write_le_uint64(user_rec, len(user_rec), 1)
    
    # name 字段 (VARCHAR, 前缀长度 1 字节)
    name_str = "test"
    user_rec.append(len(name_str))
    user_rec.extend(name_str.encode())
    
    # email 字段 (VARCHAR, 可为空，这里设为非空)
    email_str = "test@example.com"
    user_rec.append(len(email_str))
    user_rec.extend(email_str.encode())
    
    # created_at 字段 (DATETIME2(6), 8 bytes for precision=6)
    import struct
    # ✅ 修复：正确的 DATETIME2 编码（参考 MySQL 8.0 源码 sql/field.cc）
    # 前5字节 packed format: [year_low, year_high, month_day, hour_minute, second_packed]
    # 后3字节小数部分 (precision=6)
    
    year = 2024
    month = 1
    day = 1
    hour = 0
    minute = 0
    second = 0
    frac = 0
    
    # Pack date/time into 5 bytes
    year_low = year & 0xFF
    year_high = (year >> 8) & 0xFF
    month_day = ((day << 4) | month) & 0xFF
    hour_minute = ((minute & 0x3F) << 2) | (hour & 0x1F)
    second_packed = ((second & 0x3F) << 2) | ((frac >> 16) & 0x03)
    
    user_rec.append(year_low)
    user_rec.append(year_high)
    user_rec.append(month_day)
    user_rec.append(hour_minute)
    user_rec.append(second_packed)
    
    # Pack fractional part (3 bytes for precision=6)
    # ✅ 确认：MySQL DATETIME2 小数部分使用小端序存储（低位在前）
    frac_bytes = [
        (frac >> 0) & 0xFF,
        (frac >> 8) & 0xFF,
        (frac >> 16) & 0xFF
    ]
    user_rec.extend(frac_bytes)
    
    record_data.extend(user_rec)
    
    # SUPREMUM 记录
    supremum = bytearray(5)
    supremum[0] = 0x01  # info_bits: REC_INFO_STATUS_MASK for supremum
    write_le_uint16(supremum, 1, 0xFFFF)  # next_record_offset = 0xFFFF marks end
    
    record_data.extend(supremum)
    
    # 写入记录数据到页
    rec_start = 38 + 16  # PAGE_HEADER 后开始
    root_page[rec_start:rec_start+len(record_data)] = record_data
    
    write_le_uint16(root_page, 42, 4)   # PAGE_N_HEAP = 4 (infimum + user + supremum + dummy)
    
    pages.append(bytes(root_page))
    
    return b''.join(pages)

# ==================== 主程序入口 ====================
if __name__ == "__main__":
    import sys
    
    if len(sys.argv) > 1:
        table_name = sys.argv[1]
    else:
        table_name = "users"
        
    ibd_data = generate_ibd(table_name)
    
    output_file = f"{table_name}.ibd"
    with open(output_file, 'wb') as f:
        f.write(ibd_data)
        
    print(f"Generated {output_file} ({len(ibd_data)} bytes)")
