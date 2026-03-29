#!/usr/bin/env python3
"""生成测试数据脚本 - 用于创建 tests/data 目录下的测试文件"""

import os
import sys
import struct
import zlib

# ==================== 常量定义 ====================
PAGE_SIZE = 16384  # 标准 InnoDB 页大小

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
FIL_PAGE_COMPRESSED   = 9973   # 压缩页

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

def compute_crc32c(data):
    """计算 CRC32C 校验和（Castagnoli 多项式）"""
    import binascii
    # CRC32C 使用 Castagnoli 多项式 0x82F63B78
    crc = binascii.crc32(data) & 0xFFFFFFFF
    # 反转以匹配 CRC32C
    return crc ^ 0xFFFFFFFF

def create_page_header(page_type, page_no=0, prev_page=0xFFFFFFFF, next_page=0xFFFFFFFF):
    """创建 InnoDB 页头（FIL Header）"""
    page = bytearray(PAGE_SIZE)

    # FIL Header (38 bytes)
    write_le_uint32(page, 0x00, 0)              # space_id
    write_le_uint32(page, 0x04, 0)              # checksum1 (will be updated)
    write_le_uint32(page, 0x08, 0)              # LSN low
    write_le_uint32(page, 0x0C, 0)              # LSN high
    write_le_uint16(page, 0x10, page_type)      # page_type at offset 16
    write_le_uint16(page, 0x12, page_no)        # page_no at offset 18
    write_le_uint16(page, 0x14, prev_page & 0xFFFF)  # prev_page low
    write_le_uint16(page, 0x16, (prev_page >> 16) & 0xFFFF)  # prev_page high
    write_le_uint16(page, 0x18, next_page & 0xFFFF)  # next_page low
    write_le_uint16(page, 0x1A, (next_page >> 16) & 0xFFFF)  # next_page high
    write_le_uint32(page, 0x1C, 0)              # btree_index

    # 计算并写入校验和
    crc = compute_crc32c(bytes(page[:24]) + bytes(page[28:]))
    write_le_uint32(page, 0x18, crc)  # 简化：写入到 reserved 字段

    return page

def create_sdi_page(table_name="users"):
    """创建 SDI 页"""
    import json
    schema = {
        "schema": {"name": "test_recovery"},
        "table": {
            "name": table_name,
            "columns": [
                {"name": "id", "type": MYSQL_TYPE_LONGLONG, "is_nullable": False},
                {"name": "name", "type": MYSQL_TYPE_VARCHAR, "is_nullable": False,
                 "length": 128, "charset_id": 255, "collation_id": 255},
                {"name": "email", "type": MYSQL_TYPE_VARCHAR, "is_nullable": True,
                 "length": 255, "charset_id": 255, "collation_id": 255},
                {"name": "created_at", "type": MYSQL_TYPE_DATETIME, "is_nullable": True}
            ],
            "indexes": [
                {"name": "PRIMARY", "columns": ["id"], "is_primary": True}
            ]
        }
    }

    json_str = json.dumps(schema, separators=(',', ':'))
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

    page[38:38+len(payload)] = payload

    # 重新计算校验和
    crc = compute_crc32c(bytes(page[:24]) + bytes(page[28:]))
    for i in range(4):
        page[24 + i] = (crc >> (i * 8)) & 0xFF

    return bytes(page)

def create_btree_leaf_page(table_name="users"):
    """创建 B+树叶子页"""
    page = create_page_header(FIL_PAGE_INDEX, page_no=1)

    # B+Tree Header (10 bytes after FIL header)
    write_le_uint16(page, 38 + 4, 0)   # PAGE_LEVEL = 0 (leaf)
    write_le_uint16(page, 38 + 6, 0)   # PAGE_N_DIR_SLOTS = 0
    write_le_uint16(page, 38 + 8, 100) # PAGE_HEAP_TOP = 100
    write_le_uint16(page, 38 + 10, 4)  # PAGE_N_HEAP = 4 (infimum + 1 user + supremum + dummy)

    # INFIMUM 记录（固定偏移 54）
    infimum = bytearray(13)
    infimum[0] = 0x02  # info_bits: REC_INFO_MIN_REC_FLAG
    write_le_uint16(infimum, 1, 99)  # next_record_offset
    page[54:54+13] = infimum

    # 用户记录：id=1, name='test', email='test@example.com'
    user_rec = bytearray()
    user_rec.append(0x00)  # info_bits
    user_rec.append(0x00)  # n_fields_high
    user_rec.extend(b'\x00\x00')  # next_record_offset (placeholder)
    user_rec.extend(b'\x00\x00')  # heap_no
    user_rec.extend(b'\x00')  # NULL bitmap

    # id 字段 (LONGLONG, 8 bytes)
    id_pos = len(user_rec)
    user_rec.extend(b'\x00' * 8)
    write_le_uint64(user_rec, id_pos, 1)

    # name 字段 (VARCHAR)
    name_str = "test"
    user_rec.append(len(name_str))
    user_rec.extend(name_str.encode())

    # email 字段 (VARCHAR)
    email_str = "test@example.com"
    user_rec.append(len(email_str))
    user_rec.extend(email_str.encode())

    # created_at 字段 (DATETIME2)
    dt_pos = len(user_rec)
    user_rec.extend(b'\x00' * 8)
    year_low = 2024 & 0xFF
    year_high = (2024 >> 8) & 0xFF
    month_day = ((1 << 4) | 1) & 0xFF
    hour_minute = ((0 & 0x3F) << 2) | (0 & 0x1F)
    second_packed = ((0 & 0x3F) << 2) | ((0 >> 16) & 0x03)
    user_rec[dt_pos] = year_low
    user_rec[dt_pos + 1] = year_high
    user_rec[dt_pos + 2] = month_day
    user_rec[dt_pos + 3] = hour_minute
    user_rec[dt_pos + 4] = second_packed
    user_rec[dt_pos + 5] = 0
    user_rec[dt_pos + 6] = 0
    user_rec[dt_pos + 7] = 0

    # 更新 next_record_offset
    write_le_uint16(user_rec, 2, 99)

    page[67:67+len(user_rec)] = user_rec

    # SUPREMUM 记录
    supremum = bytearray(5)
    supremum[0] = 0x03  # REC_STATUS_SUPREMUM
    write_le_uint16(supremum, 1, 0xFFFF)
    page[99:99+5] = supremum

    # 重新计算校验和
    crc = compute_crc32c(bytes(page[:24]) + bytes(page[28:]))
    for i in range(4):
        page[24 + i] = (crc >> (i * 8)) & 0xFF

    return bytes(page)

def create_btree_leaf_page_with_multiple_rows(num_rows=10):
    """创建包含多行数据的 B+树叶子页"""
    page = create_page_header(FIL_PAGE_INDEX, page_no=1)

    # B+Tree Header
    write_le_uint16(page, 38 + 4, 0)   # PAGE_LEVEL = 0 (leaf)
    write_le_uint16(page, 38 + 8, 300) # PAGE_HEAP_TOP
    write_le_uint16(page, 38 + 10, 2 + num_rows)  # PAGE_N_HEAP

    rec_offset = 54  # INFIMUM starts at 54

    # INFIMUM 记录
    infimum = bytearray(13)
    infimum[0] = 0x02
    write_le_uint16(infimum, 1, rec_offset + 13)
    page[rec_offset:rec_offset+13] = infimum
    rec_offset += 13

    # 用户记录
    for i in range(1, num_rows + 1):
        user_rec = bytearray()
        user_rec.append(0x00)  # info_bits
        user_rec.append(0x00)  # n_fields_high
        user_rec.extend(b'\x00\x00')  # next_record_offset
        user_rec.extend(b'\x00\x00')  # heap_no
        user_rec.extend(b'\x00')  # NULL bitmap

        # id 字段 (LONGLONG, 8 bytes)
        id_pos = len(user_rec)
        user_rec.extend(b'\x00' * 8)
        write_le_uint64(user_rec, id_pos, i)

        # name 字段 (VARCHAR)
        name_str = f"user_{i}"
        name_pos = len(user_rec)
        user_rec.append(len(name_str))
        user_rec.extend(name_str.encode())

        # email 字段 (VARCHAR)
        email_str = f"user{i}@example.com"
        email_pos = len(user_rec)
        user_rec.append(len(email_str))
        user_rec.extend(email_str.encode())

        # created_at 字段 (DATETIME2, 8 bytes)
        dt_pos = len(user_rec)
        user_rec.extend(b'\x00' * 8)
        year_low = 2024 & 0xFF
        year_high = (2024 >> 8) & 0xFF
        month_day = ((1 << 4) | 1) & 0xFF
        hour_minute = ((0 & 0x3F) << 2) | (0 & 0x1F)
        second_packed = ((0 & 0x3F) << 2) | ((0 >> 16) & 0x03)
        user_rec[dt_pos] = year_low
        user_rec[dt_pos + 1] = year_high
        user_rec[dt_pos + 2] = month_day
        user_rec[dt_pos + 3] = hour_minute
        user_rec[dt_pos + 4] = second_packed
        # fractional part (3 bytes)
        user_rec[dt_pos + 5] = 0
        user_rec[dt_pos + 6] = 0
        user_rec[dt_pos + 7] = 0

        # 更新 next_record_offset
        next_off = rec_offset + len(user_rec)
        write_le_uint16(user_rec, 2, next_off)

        page[rec_offset:rec_offset+len(user_rec)] = user_rec
        rec_offset += len(user_rec)

    # SUPREMUM 记录
    supremum = bytearray(5)
    supremum[0] = 0x03
    write_le_uint16(supremum, 1, 0xFFFF)
    page[rec_offset:rec_offset+5] = supremum

    # 重新计算校验和
    crc = compute_crc32c(bytes(page[:24]) + bytes(page[28:]))
    for i in range(4):
        page[24 + i] = (crc >> (i * 8)) & 0xFF

    return bytes(page)

def create_truncated_scenario():
    """创建 Truncate 场景：文件被截断"""
    full_page = create_btree_leaf_page_with_multiple_rows(10)
    # 截断到一半
    return full_page[:len(full_page)//2]

def create_rmrf_scenario():
    """创建 rm -rf 场景：文件头损坏"""
    page = bytearray(16384)
    # 破坏页头
    page[0] = 0xFF
    page[1] = 0xFF
    page[2] = 0xFF
    page[3] = 0xFF
    return bytes(page)

def create_header_corrupt_scenario():
    """创建页头损坏场景"""
    page = bytearray(create_btree_leaf_page())
    # 破坏页头的关键字段
    page[16] = 0xFF  # page_type
    page[17] = 0xFF
    return bytes(page)

def main():
    """主函数"""
    data_dir = "/app/mysql8_recovery/tests/data"
    os.makedirs(data_dir, exist_ok=True)

    scenarios = [
        ("default", create_btree_leaf_page_with_multiple_rows, 10),
        ("truncate", create_truncated_scenario, 1),
        ("rm_rf", create_rmrf_scenario, 1),
        ("header_corrupt", create_header_corrupt_scenario, 1),
    ]

    for scenario_name, generator, num_pages in scenarios:
        output_file = os.path.join(data_dir, f"test_{scenario_name}.ibd")
        data = generator()
        with open(output_file, 'wb') as f:
            f.write(data)
        print(f"Generated {output_file} ({len(data)} bytes)")

    # 生成 SDI 页
    sdi_file = os.path.join(data_dir, "test_sdi.ibd")
    sdi_data = create_sdi_page("users")
    with open(sdi_file, 'wb') as f:
        f.write(sdi_data)
    print(f"Generated {sdi_file} ({len(sdi_data)} bytes)")

    # 生成多页 BLOB 场景
    blob_file = os.path.join(data_dir, "test_blob.ibd")
    # 创建 BLOB 页
    blob_page = create_page_header(FIL_PAGE_TYPE_BLOB, page_no=100)
    # 写入 BLOB 数据
    blob_page[54:54+100] = b'BLOB_DATA_' * 10
    write_le_uint32(blob_page, 38, 0xFFFFFFFF)  # next_bpage = NULL
    write_le_uint32(blob_page, 42, 0)  # prev_bpage = NULL
    write_le_uint32(blob_page, 46, 100)  # data_len = 100

    crc = compute_crc32c(bytes(blob_page[:24]) + bytes(blob_page[28:]))
    for i in range(4):
        blob_page[24 + i] = (crc >> (i * 8)) & 0xFF

    with open(blob_file, 'wb') as f:
        f.write(blob_page)
    print(f"Generated {blob_file} ({len(blob_page)} bytes)")

    print(f"\n✅ All test data generated in {data_dir}")

if __name__ == "__main__":
    main()
