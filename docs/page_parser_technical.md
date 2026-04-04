# MySQL Recovery Pro - 页解析层技术文档

## 目录
1. [架构设计](#架构设计)
2. [MySQL 版本差异](#mysql-版本差异)
3. [Page Header 结构](#page-header-结构)
4. [行格式解析](#行格式解析)
5. [SDI 页解析](#sdi-页解析)
6. [自动探测机制](#自动探测机制)
7. [测试验证](#测试验证)

---

## 架构设计

### 整体架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Page Parser Layer (抽象层)                         │
├─────────────────────────────────────────────────────────────────────┤
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐   │
│  │  PageHeaderV56   │  │  PageHeaderV80   │  │  PageHeaderV90   │   │
│  └──────────────────┘  └──────────────────┘  └──────────────────┘   │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐   │
│  │  RowParserV56    │  │  RowParserV80    │  │  RowParserV90    │   │
│  └──────────────────┘  └──────────────────┘  └──────────────────┘   │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │              PageFactory (自动探测工厂)                         │   │
│  │  - Magic Byte 检测 (0xff 0xd7)                               │   │
│  │  - FIL_PAGE_TYPE 分析 (INDEX=9977, SYSTEM=9984)              │   │
│  │  - Page Level 判断 (0=叶子节点, >0=内部节点)                  │   │
│  │  - SDI Signature 检测 (0x08SDI0000)                          │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### 类设计

| 类名 | 说明 |
|------|------|
| `PageHeaderBase` | Page Header 抽象基类 |
| `PageHeaderV56` | MySQL 5.6/5.7 Page Header 实现 |
| `PageHeaderV80` | MySQL 8.0+ Page Header 实现 |
| `PageFactory` | Page 自动探测工厂 |
| `RowParser` | 行格式解析器基类 |
| `CompactRowParser` | Compact 行格式解析器 |
| `RedundantRowParser` | Redundant 行格式解析器 |
| `SDIParser` | SDI 页解析器 |

---

## MySQL 版本差异

### 5.6/5.7 vs 8.0+ 关键差异

| 特性 | MySQL 5.6/5.7 | MySQL 8.0+ |
|------|---------------|------------|
| **Page Header 大小** | 16 bytes | 24 bytes |
| **PAGE_MAX_TRX_ID** | 存在于每个索引页 | 移至 TRX_SYS 页 |
| **PAGE_HEADER_VERSION** | 不存在 | 存在 (offset 54-55) |
| **FIL_PAGE_FILE_FLUSH_LSN** | 4 bytes | 8 bytes |
| **默认行格式** | Compact | Dynamic |
| **SDI 页** | 不存在 | 存在 (9971) |

---

## Page Header 结构

### 1. FIL_HEADER (38 bytes)

| 偏移 | 字段 | 大小 | 说明 |
|------|------|------|------|
| 0-3 | FIL_PAGE_PREV | 4 bytes | 前一个页 |
| 4-7 | FIL_PAGE_NEXT | 4 bytes | 后一个页 |
| 8-15 | FIL_PAGE_LSN | 8 bytes | LSN |
| 16-17 | FIL_PAGE_TYPE | 2 bytes | 页类型 |
| 18-21 | FIL_PAGE_SPACE_ID | 4 bytes | 空间 ID |
| 22-23 | FIL_PAGE_SPACE_OFFSET | 2 bytes | 页偏移 |
| 24-31 | FIL_PAGE_FILE_FLUSH_LSN | 8 bytes | 文件刷新 LSN (8.0+ 扩展) |

### 2. PAGE_HEADER (16/24 bytes)

#### MySQL 5.6/5.7 (16 bytes)

| 偏移 | 字段 | 大小 | 说明 |
|------|------|------|------|
| 38-39 | PAGE_N_DIR_SLOTS | 2 bytes | 页目录槽数量 |
| 40-41 | PAGE_HEAP_TOP | 2 bytes | 堆顶偏移 |
| 42-43 | PAGE_N_HEAP | 2 bytes | 记录数量 |
| 44-45 | PAGE_FREE | 2 bytes | 空闲列表起始 |
| 46-47 | PAGE_GARBAGE_END | 2 bytes | 垃圾结束 |
| 48-49 | PAGE_LAST_INSERT | 2 bytes | 最后插入偏移 |
| 50-51 | PAGE_DIRECTION | 2 bytes | 插入方向 |
| 52-53 | PAGE_N_DIRECT_PAGES | 2 bytes | 直接页数量 |

#### MySQL 8.0+ (24 bytes)

| 偏移 | 字段 | 大小 | 说明 |
|------|------|------|------|
| 38-39 | PAGE_N_DIR_SLOTS | 2 bytes | 页目录槽数量 |
| 40-41 | PAGE_HEAP_TOP | 2 bytes | 堆顶偏移 |
| 42-43 | PAGE_N_HEAP | 2 bytes | 记录数量 |
| 44-45 | PAGE_FREE | 2 bytes | 空闲列表起始 |
| 46-47 | PAGE_GARBAGE_END | 2 bytes | 垃圾结束 |
| 48-49 | PAGE_LAST_INSERT | 2 bytes | 最后插入偏移 |
| 50-51 | PAGE_DIRECTION | 2 bytes | 插入方向 |
| 52-53 | PAGE_N_DIRECT_PAGES | 2 bytes | 直接页数量 |
| 54-55 | PAGE_HEADER_VERSION | 2 bytes | 页头版本 (8.0+) |

### 3. BTR_HEADER (12 bytes)

| 偏移 | 字段 | 大小 | 说明 |
|------|------|------|------|
| 38-41 | BTR_N_LEVELS | 4 bytes | B+树层数 |
| 42-49 | BTR_IND_PAGE_OFFSET | 8 bytes | 索引 ID |

---

## 行格式解析

### Redundant 行格式 (5.6/5.7)

```
Record Header (6 bytes):
+----------+------------------+------------------+----------+-----------+
| info_bits| next_record_off  |    heap_no       | n_fields | extra_bytes |
|  (1B)    |     (2B)         |      (2B)        |   (1B)   |    (1B)   |
+----------+------------------+------------------+----------+-----------+

extra_bytes = 6
```

### Compact 行格式 (5.7/8.0+)

```
Record Header (5 bytes):
+----------+------------------+------------------+
| info_bits| next_record_off  |    heap_no       |
|  (1B)    |     (2B)         |      (2B)        |
+----------+------------------+------------------+
| n_fields | extra_bytes |
|   (1B)   |    (1B)     |
+----------+-----------+

extra_bytes = 5
```

### Dynamic 行格式 (8.0+)

- 支持变长字段
- 外部存储 BLOB
- extra_bytes = 5

---

## SDI 页解析

### SDI 页结构

```
Offset 0-37:    FIL_HEADER
Offset 38-61:   PAGE_HEADER (24 bytes)
Offset 64-71:   SDI Signature ("08SDI0000")
Offset 72+:     SDI JSON Data
```

### SDI Signature

```
0x08, 'S', 'D', 'I', '0', '0', '0', '0'
```

### SDI 页类型

- **FIL_PAGE_TYPE**: 9971 (0x26F3)
- **SDI Signature**: 固定 8 字节

---

## 自动探测机制

### 版本探测流程

```
1. 检查 FIL_PAGE_TYPE
   ├─ SDI (9971) → MySQL 8.0+
   └─ 其他 → 继续

2. 检查 SDI Signature (offset 64)
   ├─ 匹配 "08SDI0000" → MySQL 8.0+
   └─ 不匹配 → 继续

3. 检查 PAGE_HEADER_VERSION (offset 40-41)
   ├─ 非零 → MySQL 8.0+
   └─ 零 → 继续

4. 检查 FIL_PAGE_FILE_FLUSH_LSN (offset 24-31)
   ├─ 高 32 位非零 → MySQL 8.0+
   └─ 零 → 继续

5. 检查 PAGE_MAX_TRX_ID (offset 38-41)
   ├─ 非零且非全 1 → MySQL 5.6/5.7
   └─ 继续

6. 默认返回 MySQL 5.7
```

### 行格式探测

```
1. 检查 extra_bytes
   ├─ 6 → Redundant
   ├─ 5 → Compact/Dynamic
   └─ 其他 → COMPACT (默认)

2. 对于 extra_bytes = 5
   ├─ 检查 external pointer (info_bits & 0x20)
   │  ├─ 存在 → Dynamic
   │  └─ 不存在 → Compact
   └─ 8.0+ 默认 Dynamic
```

---

## 测试验证

### 测试文件

| 文件 | 说明 |
|------|------|
| `test_compact.ibd` | Compact 行格式测试 |
| `test_redundant.ibd` | Redundant 行格式测试 |
| `test_header_corrupt.ibd` | 坏块场景测试 |
| `test_sdi.ibd` | SDI 页测试 |
| `test_truncated.ibd` | 截断场景测试 |
| `test_mysql56.ibd` | MySQL 5.6 兼容性测试 |
| `test_mysql80.ibd` | MySQL 8.0 兼容性测试 |

### 测试结果

```
========================================
MySQL Recovery Pro - Page Parser Tests
========================================

[TEST] P0-01: PageHeader offset detection...
    [PASS] PageHeader offset detection

[TEST] P0-02: Row format detection...
    [PASS] Row format detection

[TEST] P0-03: SDI page detection...
    [PASS] SDI page detection

[TEST] P0-04: PageHeaderV56 parsing...
    [PASS] PageHeaderV56 parsing

[TEST] P0-04: PageHeaderV80 parsing...
    [PASS] PageHeaderV80 parsing

[TEST] P1-01: Row parser...
    [PASS] Row parser

[TEST] P1-02: Corrupted page handling...
    [PASS] Corrupted page handling

[TEST] P2-01: Heuristic metadata inference...
    [PASS] Heuristic metadata inference

========================================
Test Results: 8 passed, 0 failed
========================================
```

---

## 使用示例

### C++ 示例

```cpp
#include "page/page_parser.h"

// 读取页数据
std::vector<uint8_t> page_data = read_file("test.ibd");

// 自动创建 PageHeader
auto header = mysql_recovery::PageFactory::create_page_header(
    page_data.data(), 
    page_data.size()
);

// 获取版本
MySQLVersion version = header->get_version();

// 获取行格式
RowFormat format = header->get_row_format();

// 解析记录
auto records = parse_records(page_data.data(), format);
```

### Python 示例

```python
from tools.ibd_generator import create_page

# 生成测试页
page = create_page(
    page_type=FIL_PAGE_TYPE_INDEX,
    mysql_version=57,
    row_format='compact'
)

# 保存到文件
with open('test.ibd', 'wb') as f:
    f.write(page)
```

---

## 参考资料

- MySQL 5.6 源码: `include/page0page.h`
- MySQL 5.7 源码: `include/page0page.h`
- MySQL 8.0 源码: `include/page0page.h`
- MySQL 8.0 SDI: `include/sdi.h`

---

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.0.0 | 2026-03-30 | 初始版本，支持 MySQL 5.6-9.0 |
