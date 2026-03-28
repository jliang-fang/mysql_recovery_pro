#!/usr/bin/env python3
"""InnoDB .ibd 文件生成器，用于创建测试用的 InnoDB 页结构"""

import json
import zlib

# ==================== 常量定义 ====================
PAGE_SIZE = 4096  # ✅ 修复：使用 4KB 页大小（与 C++ 代码兼容）

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
# ✅ 新增：与 C++ 头文件 page_defs.hpp 保持一致
FIL_PAGE_TYPE_ALLOCATED = 0
FIL_PAGE_INDEX        = 9969   # B+Tree 索引页 (0x2041)
FIL_PAGE_UNDO_LOG     = 9970   # Undo 日志页 (0x2042) — MySQL 8.0 中替代了旧版的 FIL_PAGE_RTREE
FIL_PAGE_SDI          = 9971   # SDI 元数据页 (0x2043)
FIL_PAGE_TYPE_BLOB    = 9972   # BLOB页 (0x2044)
FIL_PAGE_COMPRESSED   = 9973   # 压缩页 (0x2045)

# ✅ 新增：页类型字符串映射（用于调试）
PAGE_TYPE_NAMES = {
    FIL_PAGE_TYPE_ALLOCATED: "ALLOCATED",
    FIL_PAGE_INDEX:        "INDEX",
    FIL_PAGE_UNDO_LOG:     "UNDO_LOG",
    FIL_PAGE_SDI:          "SDI",
    FIL_PAGE_TYPE_BLOB:    "BLOB",
    FIL_PAGE_COMPRESSED:   "COMPRESSED"
}

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

    # ✅ 修复：FIL Header (38 bytes) - 使用正确的偏移量（相对于页起始位置）
    # 参考 MySQL 8.0 源码 include/fil0fil.h
    # 所有偏移都是相对于页起始位置的相对偏移（0-based）
    # offset 0:   space_id (4 bytes)
    # offset 4:   checksum1 (4 bytes)
    # offset 8:   LSN low (4 bytes)
    # offset 12:  LSN high (4 bytes)
    # offset 16:  page_type (2 bytes) - ✅ 修复：正确偏移是 16，不是 12！
    # offset 18:  page_no (2 bytes)
    # offset 20:  prev_page (2 bytes)
    # offset 22:  next_page (2 bytes)
    # offset 24:  btree_index (4 bytes)
    # offset 26:  reserved/unused (12 bytes)
    # offset 38:  page data starts here

    # ✅ 修复：写入正确的 FIL Header 偏移（相对于页起始位置）
    write_le_uint32(page, 0x00, 0)              # space_id (offset 0)
    write_le_uint32(page, 0x04, 0)              # checksum1 (offset 4)
    write_le_uint32(page, 0x08, 0)              # LSN low (offset 8)
    write_le_uint32(page, 0x0C, 0)              # LSN high (offset 12)
    write_le_uint16(page, 0x10, page_type)      # page_type at offset 16 (relative to page start)
    write_le_uint16(page, 0x12, 0)              # page_no at offset 18
    write_le_uint16(page, 0x14, 0xFFFF)         # prev_page at offset 20
    write_le_uint16(page, 0x16, 0xFFFF)         # next_page at offset 22
    write_le_uint32(page, 0x18, 0)              # btree_index at offset 24

    return page

def create_sdi_page(table_name):
    """Create a SDI page with compressed JSON (raw deflate, no zlib header)"""
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
    # ✅ 修复：使用 raw deflate（无 zlib 头），与 MySQL SDI 格式一致
    # zlib.compress() 返回带 zlib header 的数据，需要手动压缩为 raw deflate
    import zlib
    compressor = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION, zlib.DEFLATED, -zlib.MAX_WBITS)
    compressed = compressor.compress(json_str.encode())
    compressed += compressor.flush()

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

def generate_ibd(table_name="users", include_blob=False, multi_page_blob=False, row_format="DYNAMIC"):
    """Generate a minimal .ibd file with SDI + B+ tree with optional test data"""
    pages = []
    
    # Page 0: SDI
    pages.append(create_sdi_page(table_name))
    
    # Page 1: Root page of clustered index (with test records)
    root_page = create_page_header(FIL_PAGE_INDEX)
    write_le_uint32(root_page, 38, 0)   # PAGE_LEVEL = 0 (leaf)
    
    # ✅ 新增：根据行格式设置 flags（BTR 标志位）
    row_format_flags = {"REDUNDANT": 0, "COMPACT": 1, "DYNAMIC": 2, "COMPRESSED": 3}
    flags = row_format_flags.get(row_format.upper(), 2)  # 默认 DYNAMIC
    write_le_uint16(root_page, 40, flags << 8 | 0x07)   # 设置行格式标志
    
    # 构建测试记录
    record_data = bytearray()
    
    # INFIMUM 记录（固定偏移 54）
    infimum = bytearray(13)
    infimum[0] = 0x02  # info_bits: REC_INFO_MIN_REC_FLAG
    write_le_uint16(infimum, 1, 56)  # next_record_offset
    record_data.extend(infimum)
    
    if include_blob:
        # 用户记录示例：包含 BLOB 字段
        user_rec = bytearray()
        user_rec.append(0x00)  # info_bits (普通记录)
        user_rec.append(0x00)  # n_fields_high + next_record_offset[15:8]
        
        # NULL 位图（假设所有列都非空）
        user_rec.extend(b'\x00')  # 1 byte null bitmap
        
        # id 字段 (LONGLONG, 8 bytes)
        write_le_uint64(user_rec, len(user_rec), 1)
        
        # name 字段 (VARCHAR, 前缀长度 1 字节)
        name_str = "test"
        user_rec.append(len(name_str))
        user_rec.extend(name_str.encode())
        
        if multi_page_blob:
            # 多页 BLOB 指针（20字节）：space_id=0, page_no=100, offset=0, len=50, total_len=200
            write_le_uint32(user_rec, len(user_rec), 0)   # space_id
            write_le_uint32(user_rec, len(user_rec), 100) # page_no (first BLOB页)
            write_le_uint32(user_rec, len(user_rec), 0)   # offset
            write_le_uint32(user_rec, len(user_rec), 50)  # len (当前页)
            write_le_uint32(user_rec, len(user_rec), 200) # total_len (跨两页)
        else:
            # 单页 BLOB 指针（20字节）：space_id=0, page_no=100, offset=0, len=50, total_len=50
            write_le_uint32(user_rec, len(user_rec), 0)   # space_id
            write_le_uint32(user_rec, len(user_rec), 100) # page_no
            write_le_uint32(user_rec, len(user_rec), 0)   # offset
            write_le_uint32(user_rec, len(user_rec), 50)  # len
            write_le_uint32(user_rec, len(user_rec), 50)  # total_len
        
        record_data.extend(user_rec)
    else:
        # 用户记录示例：id=1, name='test'
        user_rec = bytearray()
        user_rec.append(0x00)  # info_bits (普通记录)
        user_rec.append(0x00)  # n_fields_high + next_record_offset[15:8]
        # ✅ 修复：确保 user_rec 有足够的空间（预留 2 字节用于 next_record_offset）
        while len(user_rec) < 3:
            user_rec.append(0)
        write_le_uint16(user_rec, 1, 56 + len(record_data) + 2)  # next_record_offset

        # NULL 位图（假设所有列都非空）
        user_rec.extend(b'\x00')  # 1 byte null bitmap

        # id 字段 (LONGLONG, 8 bytes)
        # ✅ 修复：确保有足够空间
        while len(user_rec) < 12:
            user_rec.append(0)
        write_le_uint64(user_rec, len(user_rec) - 8, 1)
        
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
        # ✅ 修复：MySQL DATETIME2 小数部分使用小端序存储（低位在前）
        frac_bytes = [
            (frac >> 0) & 0xFF,
            (frac >> 8) & 0xFF,
            (frac >> 16) & 0xFF
        ]
        user_rec.extend(frac_bytes)
        
        record_data.extend(user_rec)
    
    # SUPREMUM 记录
    supremum = bytearray(5)
    supremum[0] = 0x03  # info_bits: REC_STATUS_SUPREMUM (0x03) - matches C++ parser expectation
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
    import argparse
    
    parser = argparse.ArgumentParser(description="Generate InnoDB test data for recovery testing")
    parser.add_argument("table_name", nargs="?", default="users", help="Table name")
    parser.add_argument("--scenario", choices=["default", "rm_rf", "header_corrupt", "truncate_partial"], 
                       default="default", help="Scenario type")
    parser.add_argument("--output", "-o", default=None, help="Output file path (default: <table_name>.ibd)")
    parser.add_argument("--rows_kept", type=int, default=0, help="For truncate_partial: number of rows to keep")
    
    args = parser.parse_args()
    
    if args.scenario == "rm_rf":
        # 模拟 rm -rf 场景：生成一个空文件或损坏的文件
        output_file = args.output or f"{args.table_name}_rm_rf.ibd"
        with open(output_file, 'wb') as f:
            # 写入一个损坏的页头（FIL_PAGE_TYPE 错误）
            page = bytearray(16384)
            page[0] = 0xFF  # 故意设置错误的页类型
            f.write(page)
        print(f"Generated {output_file} (simulated rm -rf scenario)")
    elif args.scenario == "header_corrupt":
        # 模拟文件头损坏场景
        output_file = args.output or f"{args.table_name}_header_corrupt.ibd"
        ibd_data = generate_ibd(args.table_name)
        if len(ibd_data) >= 512:
            # 破坏页头的最后 256 字节（包含关键元数据）
            for i in range(256):
                ibd_data[256 + i] = 0xFF
        with open(output_file, 'wb') as f:
            f.write(ibd_data)
        print(f"Generated {output_file} (header corrupted scenario)")
    elif args.scenario == "truncate_partial":
        # 模拟部分 Truncate 场景：只保留前 N 行数据
        output_file = args.output or f"{args.table_name}_truncated.ibd"
        ibd_data = generate_ibd(args.table_name, include_blob=True)
        
        # 简单模拟：截断文件到一半大小（实际应更精细地处理页结构）
        if len(ibd_data) > 16384:
            truncated_size = len(ibd_data) // 2
            ibd_data = ibd_data[:truncated_size]
        
        with open(output_file, 'wb') as f:
            f.write(ibd_data)
        print(f"Generated {output_file} (partial truncate scenario)")
    else:  # default
        output_file = args.output or f"{args.table_name}.ibd"
        ibd_data = generate_ibd(args.table_name)
        
        with open(output_file, 'wb') as f:
            f.write(ibd_data)
            
        print(f"Generated {output_file} ({len(ibd_data)} bytes)")
