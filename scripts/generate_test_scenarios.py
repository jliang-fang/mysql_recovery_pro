#!/usr/bin/env python3
"""
损坏场景测试文件生成器

生成用于测试的损坏场景 IBD 文件：
1. 页头损坏场景
2. rm -rf 模拟场景
3. truncate 模拟场景
"""

import struct
import os
import random

# InnoDB 常量
FIL_PAGE_TYPE_OFFSET = 16
FIL_PAGE_TYPE_INDEX = 9969  # 0x26f1
FIL_PAGE_TYPE_SDI = 9971    # 0x26f3
FIL_PAGE_TYPE_UNDO_LOG = 9970  # 0x26f2
FIL_PAGE_DATA_START = 38

PAGE_N_DIR_SLOTS_OFFSET = 38
PAGE_HEAP_TOP_OFFSET = 40
PAGE_N_HEAP_OFFSET = 42
PAGE_FREE_OFFSET = 44

# Record status bits (lowest 3 bits of info_bits)
REC_STATUS_ORDINARY = 0x00
REC_STATUS_NODE_PTR = 0x01
REC_STATUS_INFIMUM = 0x02
REC_STATUS_SUPREMUM = 0x03

REC_INFO_MIN_REC_FLAG = 0x10
REC_INFO_DELETED_FLAG = 0x08

PAGE_INFIMUM_REC_OFFSET = 54
PAGE_SUPREMUM_REC_OFFSET = 61


def create_innodb_page(page_type=FIL_PAGE_TYPE_INDEX, page_no=0, 
                       prev_page=0xFFFFFFFF, next_page=0xFFFFFFFF,
                       heap_top=100, n_heap=4, is_corrupt=False):
    """创建一个 InnoDB 页"""
    
    # 创建 16KB 页
    page = bytearray(16384)
    
    # 填充页头
    struct.pack_into('<I', page, 0, prev_page)      # FIL_PAGE_PREV
    struct.pack_into('<I', page, 4, next_page)      # FIL_PAGE_NEXT
    struct.pack_into('<Q', page, 8, 0)              # FIL_PAGE_LSN (8 bytes)
    struct.pack_into('<H', page, 16, page_type)     # FIL_PAGE_TYPE
    struct.pack_into('<I', page, 20, page_no)       # FIL_PAGE_OFFSET
    struct.pack_into('<I', page, 24, 0)             # FIL_PAGE_SPACE_ID_EXT
    struct.pack_into('<H', page, 24, 0)             # FIL_PAGE_SPACE_ID (overlap)
    struct.pack_into('<I', page, 28, 0)             # FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID
    
    # 填充页头信息
    struct.pack_into('<H', page, 38, 2)             # PAGE_N_DIR_SLOTS
    struct.pack_into('<H', page, 40, heap_top)      # PAGE_HEAP_TOP
    struct.pack_into('<H', page, 42, n_heap)        # PAGE_N_HEAP (15-bit)
    struct.pack_into('<H', page, 44, 50)            # PAGE_FREE
    
    # 填充 BTR header
    struct.pack_into('<H', page, 42, 0)             # BTR_N_LEVELS (leaf)
    
    # 填充 INFIMUM 记录 (REC_STATUS_INFIMUM = 0x02)
    page[PAGE_INFIMUM_REC_OFFSET] = REC_STATUS_INFIMUM
    struct.pack_into('<H', page, PAGE_INFIMUM_REC_OFFSET + 1, PAGE_SUPREMUM_REC_OFFSET)
    struct.pack_into('<H', page, PAGE_INFIMUM_REC_OFFSET + 3, 0)

    # 填充 SUPREMUM 记录 (REC_STATUS_SUPREMUM = 0x03)
    page[PAGE_SUPREMUM_REC_OFFSET] = REC_STATUS_SUPREMUM
    struct.pack_into('<H', page, PAGE_SUPREMUM_REC_OFFSET + 1, 0x3FFF)  # 0xFFFF & 0x3FFF
    
    # 填充页目录
    struct.pack_into('<H', page, 74, PAGE_INFIMUM_REC_OFFSET)
    struct.pack_into('<H', page, 76, PAGE_SUPREMUM_REC_OFFSET)
    
    # 如果需要损坏，进行破坏
    if is_corrupt:
        corrupt_page(page)
    
    return page


def corrupt_page(page):
    """对页进行损坏"""
    
    # 随机选择损坏类型
    corrupt_type = random.choice(['header', 'records', 'chain', 'checksum'])
    
    if corrupt_type == 'header':
        # 损坏页头
        page[16] = 0xFF  # 无效页类型
        page[20] = 0xFF  # 无效页号
        
    elif corrupt_type == 'records':
        # 损坏记录
        page[PAGE_INFIMUM_REC_OFFSET] = 0xFF  # 无效记录类型
        page[PAGE_SUPREMUM_REC_OFFSET] = 0xFF
        
    elif corrupt_type == 'chain':
        # 损坏页链
        struct.pack_into('<I', page, 24, 0xFFFFFFF0)  # 无效 prev
        struct.pack_into('<I', page, 28, 0xFFFFFFF0)  # 无效 next
        
    elif corrupt_type == 'checksum':
        # 损坏校验和
        struct.pack_into('<I', page, 24, 0xDEADBEEF)
        struct.pack_into('<I', page, 28, 0xCAFEBABE)


def create_header_corrupt_scenario():
    """创建页头损坏场景"""
    
    print("Creating header corrupt scenario...")
    
    # 创建一个页头损坏的页
    page = create_innodb_page(
        page_type=FIL_PAGE_TYPE_INDEX,
        page_no=0,
        is_corrupt=True
    )
    
    # 特别标记为损坏
    page[16] = 0xFF    # 无效页类型 (低8位)
    page[17] = 0xFF    # 无效页类型 (高8位)
    page[38] = 0xFF    # 无效 n_dir_slots
    
    return page


def create_rmrf_scenario():
    """创建 rm -rf 模拟场景（只有部分数据）"""
    
    print("Creating rm -rf scenario...")
    
    # 只保留前 1KB（模拟 rm -rf 后的残留）
    page = create_innodb_page(page_no=0)
    
    # 截断到 1KB
    return page[:1024]


def create_truncate_scenario():
    """创建 truncate 模拟场景（页链断裂）"""
    
    print("Creating truncate scenario...")
    
    # 创建多个页，但链不连续
    pages = []
    
    for i in range(5):
        page = create_innodb_page(
            page_no=i,
            prev_page=i-1 if i > 0 else 0xFFFFFFFF,
            next_page=i+1 if i < 4 else 0xFFFFFFFF
        )
        pages.append(page)
    
    # 断裂第 2 页的 next 指针
    struct.pack_into('<I', pages[2], 28, 0xFFFFFFF0)
    
    return pages


def create_full_recovery_scenario():
    """创建完整的恢复场景（包含所有页类型）"""
    
    print("Creating full recovery scenario...")
    
    scenario = {
        'index_pages': [],
        'sdi_pages': [],
        'undo_pages': []
    }
    
    # 创建索引页
    for i in range(10):
        page = create_innodb_page(
            page_type=FIL_PAGE_TYPE_INDEX,
            page_no=i,
            prev_page=i-1 if i > 0 else 0xFFFFFFFF,
            next_page=i+1 if i < 9 else 0xFFFFFFFF
        )
        scenario['index_pages'].append(page)
    
    # 创建 SDI 页
    for i in range(2):
        page = create_innodb_page(
            page_type=FIL_PAGE_TYPE_SDI,
            page_no=100 + i,
            prev_page=100 + i - 1 if i > 0 else 0xFFFFFFFF,
            next_page=100 + i + 1 if i < 1 else 0xFFFFFFFF
        )
        scenario['sdi_pages'].append(page)
    
    # 创建 Undo Log 页
    for i in range(3):
        page = create_innodb_page(
            page_type=FIL_PAGE_TYPE_UNDO_LOG,
            page_no=200 + i,
            prev_page=200 + i - 1 if i > 0 else 0xFFFFFFFF,
            next_page=200 + i + 1 if i < 2 else 0xFFFFFFFF
        )
        scenario['undo_pages'].append(page)
    
    return scenario


def write_pages_to_file(pages, filename):
    """将页写入文件"""
    
    with open(filename, 'wb') as f:
        for page in pages:
            f.write(page)
    
    print(f"  Written {len(pages)} pages to {filename}")


def main():
    """主函数"""
    
    output_dir = os.path.join(os.path.dirname(__file__), 'test_data')
    os.makedirs(output_dir, exist_ok=True)
    
    # 1. 创建页头损坏场景
    print("\n=== Creating Header Corrupt Scenario ===")
    header_corrupt = create_header_corrupt_scenario()
    write_pages_to_file([header_corrupt], os.path.join(output_dir, 'test_header_corrupt.ibd'))
    
    # 2. 创建 rm -rf 场景
    print("\n=== Creating rm -rf Scenario ===")
    rmrf_data = create_rmrf_scenario()
    write_pages_to_file([rmrf_data], os.path.join(output_dir, 'test_rmrf_scenario.ibd'))
    
    # 3. 创建 truncate 场景
    print("\n=== Creating Truncate Scenario ===")
    truncate_pages = create_truncate_scenario()
    write_pages_to_file(truncate_pages, os.path.join(output_dir, 'test_truncate_scenario.ibd'))
    
    # 4. 创建完整恢复场景
    print("\n=== Creating Full Recovery Scenario ===")
    full_recovery = create_full_recovery_scenario()
    
    # 写入索引页
    write_pages_to_file(
        full_recovery['index_pages'],
        os.path.join(output_dir, 'test_full_recovery.ibd')
    )
    
    # 分别写入 SDI 页
    write_pages_to_file(
        full_recovery['sdi_pages'],
        os.path.join(output_dir, 'test_sdi_pages.ibd')
    )
    
    # 分别写入 Undo Log 页
    write_pages_to_file(
        full_recovery['undo_pages'],
        os.path.join(output_dir, 'test_undo_pages.ibd')
    )
    
    print("\n=== All scenarios created ===")
    print(f"Output directory: {output_dir}")
    
    # 列出生成的文件
    print("\nGenerated files:")
    for f in sorted(os.listdir(output_dir)):
        filepath = os.path.join(output_dir, f)
        size = os.path.getsize(filepath)
        print(f"  {f}: {size} bytes")


if __name__ == '__main__':
    main()
