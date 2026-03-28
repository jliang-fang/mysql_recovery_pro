# InnoDB 页格式原理文档

## 1. 概述

InnoDB 是 MySQL 的默认存储引擎，采用页（Page）作为基本存储单位。每个页的大小通常为 16KB，但也支持 4KB、8KB、32KB 和 64KB。

## 2. 页结构

### 2.1 页头（FIL Header）- 38 字节

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0-3 | 4 bytes | FIL_PAGE_PREV | 前一个页号（B+树叶子节点链表） |
| 4-7 | 4 bytes | FIL_PAGE_NEXT | 下一个页号（B+树叶子节点链表） |
| 8-15 | 8 bytes | FIL_PAGE_LSN | 日志序列号（LSN） |
| 16-19 | 4 bytes | FIL_PAGE_SPACE_ID | 表空间ID |
| 20-23 | 4 bytes | FIL_PAGE_OFFSET | 页偏移 |
| 24-27 | 4 bytes | FIL_PAGE_SPACE_CHECKSUM | 校验和（CRC32C/INNODB/OLD） |
| 28-31 | 4 bytes | FIL_PAGE_ARCH_LOG_NO | 归档日志号（MySQL 5.6+） |
| 32-37 | 6 bytes | FIL_PAGE_DATA_START | 数据开始标记 |

### 2.2 页类型（FIL_PAGE_TYPE）

| 类型值 | 类型名称 | 说明 |
|--------|----------|------|
| 0 | FIL_PAGE_TYPE_ALLOCATED | 新分配的页 |
| 9969 (0x2041) | FIL_PAGE_INDEX | B+树索引页 |
| 9970 (0x2042) | FIL_PAGE_UNDO_LOG | Undo日志页 |
| 9971 (0x2043) | FIL_PAGE_SDI | SDI元数据页 |
| 9972 (0x2044) | FIL_PAGE_TYPE_BLOB | BLOB数据页 |
| 9973 (0x2045) | FIL_PAGE_COMPRESSED | 压缩页 |

### 2.3 页头（Page Header）- 14+ 字节

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 bytes | PAGE_N_DIR_SLOTS | 页目录槽数量 |
| 2-3 | 2 bytes | PAGE_HEAP_TOP | 堆顶位置 |
| 4-5 | 2 bytes | PAGE_N_HEAP | 堆中记录数（15位） |
| 6-7 | 2 bytes | PAGE_FREE | 空闲空间起始位置 |
| 8-9 | 2 bytes | PAGE_GARBAGE_END | 垃圾空间结束位置 |
| 10-11 | 2 bytes | PAGE_LAST_INSERT | 最后插入位置 |
| 12-13 | 2 bytes | PAGE_DIRECTION | 插入方向 |
| 14-15 | 2 bytes | PAGE_N_DIRECT_PAGES | 直接页数量 |

### 2.4 B+树头（BTR Header）- 8 字节

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 bytes | BTR_SEG_ID | 段ID（8字节，但只用前4字节） |
| 4-5 | 2 bytes | BTR_LEVEL | B+树层级（0=叶子节点） |
| 6-7 | 2 bytes | BTR_INDEX_ID | 索引ID（8字节） |

### 2.5 记录区域

#### 2.5.1 系统记录

- **INFIMUM**：最小记录，偏移固定为 54
- **SUPREMUM**：最大记录，偏移动态计算

#### 2.5.2 记录头（Record Header）- 5 字节（COMPACT格式）

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 byte | info_bits | 信息位（bit 0-2: 类型, bit 3: 删除标志, bit 4: 最小记录, bit 5: 外部存储） |
| 1-2 | 2 bytes | next_record_offset | 下一条记录偏移（15位） |
| 3-4 | 2 bytes | heap_no | 堆编号（15位） |

#### 2.5.3 记录数据

记录数据包含：
- 列长度信息（变长字段）
- 列数据
- NULL位图（如果存在可空列）

## 3. 校验和算法

### 3.1 CRC32C（MySQL 8.0 默认）

使用 Castagnoli 多项式 (0x82F63B78)

```cpp
constexpr std::array<uint32_t, 256> generate_crc32c_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78;
            } else {
                crc >>= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}
```

### 3.2 INNODB（MySQL 5.6/5.7）

```cpp
uint32_t compute_innodb_checksum(const std::vector<uint8_t>& page_data) {
    const uint32_t* data = reinterpret_cast<const uint32_t*>(page_data.data());
    size_t len = page_data.size() / 4;
    
    uint32_t checksum = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        if (i == 6 || i == 7) continue; // 跳过校验和字段
        checksum ^= read_le<uint32_t>(page_data.data(), i * 4);
        checksum += (checksum >> 16);
        checksum &= 0xFFFFFFFF;
    }
    return (checksum >> 16) ^ (checksum & 0xFFFF);
}
```

### 3.3 OLD（MySQL 5.5）

```cpp
uint32_t compute_old_checksum(const std::vector<uint8_t>& page_data) {
    const uint16_t* data = reinterpret_cast<const uint16_t*>(page_data.data());
    size_t len = page_data.size() / 2;
    
    uint64_t checksum = 0;
    for (size_t i = 0; i < len; ++i) {
        if (i == 12 || i == 13) continue; // 跳过校验和字段
        checksum += read_le<uint16_t>(page_data.data(), i * 2);
        checksum = (checksum & 0xFFFFFFFF) + (checksum >> 32);
    }
    return static_cast<uint32_t>((checksum & 0xFFFFFFFF) + (checksum >> 32));
}
```

## 4. 页类型详解

### 4.1 B+树索引页

B+树是InnoDB索引的基础数据结构，具有以下特性：
- 所有数据存储在叶子节点
- 内部节点只存储索引键
- 叶子节点通过双向链表连接

### 4.2 Undo Log页

用于事务回滚，包含：
- 事务ID
- 回滚指针
- 旧版本数据

### 4.3 SDI页

Space Descriptor Index，存储表的元数据：
- 表名
- 列定义
- 索引定义
- 分区信息

### 4.4 BLOB页

用于存储大对象数据：
- 链表结构（next_bpage, prev_bpage）
- 数据长度字段
- 实际数据

## 5. 页解析流程

```cpp
bool PageParser::parse_header() {
    // 1. 解析 FIL Header (offset 0-37)
    fil_prev_ = read_le<uint32_t>(buffer_, 0);
    fil_next_ = read_le<uint32_t>(buffer_, 4);
    fil_lsn_ = read_le<uint64_t>(buffer_, 8);
    page_type_ = read_le<uint16_t>(buffer_, 12);
    space_id_ = read_le<uint32_t>(buffer_, 16);
    page_offset_ = read_le<uint32_t>(buffer_, 20);
    
    // 2. 校验页类型
    if (page_type_ != FIL_PAGE_INDEX &&
        page_type_ != FIL_PAGE_UNDO_LOG &&
        page_type_ != FIL_PAGE_SDI) {
        return false;
    }
    
    // 3. 解析 Page Header (offset 38)
    header_.n_dir_slots = read_le<uint16_t>(buffer_, 38);
    header_.heap_top = read_le<uint16_t>(buffer_, 40);
    header_.n_heap = read_le<uint16_t>(buffer_, 42) & 0x7FFF;
    
    // 4. 解析 BTR Header (offset 38+16)
    page_level_ = read_le<uint16_t>(buffer_, 54) >> 12;
    
    return true;
}
```

## 6. 实际应用

### 6.1 数据恢复

通过解析页结构，可以从损坏的表空间中恢复数据：
1. 读取所有页
2. 验证页类型和校验和
3. 解析记录数据
4. 重建表结构

### 6.2 页健康检查

```cpp
struct PageHealth {
    bool is_valid = true;
    std::string status_message;
    uint32_t checksum_expected = 0;
    uint32_t checksum_actual = 0;
    bool checksum_match = false;
    
    // B+Tree specific
    bool has_infimum = false;
    bool has_supremum = false;
    uint16_t n_records = 0;
    uint16_t n_dir_slots = 0;
    uint16_t page_level = 0;
    uint32_t next_page = 0;
    uint32_t prev_page = 0;
    uint16_t free_space = 0;
};
```

## 7. 参考资料

- MySQL 8.0 源码：`include/page0page.h`, `include/fil0fil.h`
- InnoDB 内部结构：https://dev.mysql.com/doc/refman/8.0/en/innodb-file-space.html
- B+树算法：https://dev.mysql.com/doc/refman/8.0/en/innodb-index-types.html
