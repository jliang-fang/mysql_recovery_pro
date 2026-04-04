# MySQL Recovery Pro - 页解析层设计文档 (v5.6 - v9.0)

## 一、概述

本文档详细描述 MySQL Recovery Pro 项目的页解析层设计方案，支持从 MySQL 5.6 到 MySQL 9.0 全系列版本的 InnoDB 页结构解析。

### 1.1 设计目标

- **多版本兼容**: 支持 MySQL 5.6/5.7/8.0/8.4/9.0 的页结构差异
- **自动探测**: 通过 Page 0/3 的特征码自动判定版本及行格式
- **深度解析**: 完整解析 B+树索引页、Undo Log 页、SDI 页等
- **高性能**: 支持 io_uring 裸盘级别的扫描效率

### 1.2 核心模块

```
page_types_v2.hpp/cpp     - 基础类型定义与页结构
auto_detector.hpp         - 自动探测模块
page_parser_v2.hpp/cpp    - 页解析器
btree.hpp/cpp            - B+树遍历器
```

---

## 二、Page Header 结构详解

### 2.1 FIL Header (0-19 bytes)

| 偏移量 | 字段 | 大小 | 说明 |
|--------|------|------|------|
| 0-3 | FIL_PAGE_OFFSET | 4 bytes | 文件页偏移 |
| 4-7 | FIL_PAGE_PREV | 4 bytes | 前一个页号 (索引页) |
| 8-11 | FIL_PAGE_NEXT | 4 bytes | 后一个页号 (索引页) |
| 12-19 | FIL_PAGE_LSN | 8 bytes | 日志序列号 |

### 2.2 Page Type & Space ID (16-21 bytes)

| 偏移量 | 字段 | 大小 | MySQL 5.6/5.7 | MySQL 8.0+ |
|--------|------|------|---------------|------------|
| 16-17 | FIL_PAGE_TYPE | 2 bytes | 页类型 | 页类型 |
| 18-21 | FIL_PAGE_SPACE_ID | 4 bytes | 空间ID | 空间ID |

### 2.3 PAGE Header (22-55 bytes)

| 偏移量 | 字段 | 大小 | 说明 |
|--------|------|------|------|
| 22-23 | PAGE_LEVEL | 2 bytes | B+树层级 (0=leaf) |
| 24-25 | PAGE_N_DIR_SLOTS | 2 bytes | 目录槽数量 |
| 26-27 | PAGE_HEAP_TOP | 2 bytes | 堆顶偏移 |
| 28-29 | PAGE_N_HEAP | 2 bytes | 堆总记录数 (14bit + 2bit version) |
| 30-31 | PAGE_FREE | 2 bytes | 空闲链表头 |
| 32-33 | PAGE_GARBAGE | 2 bytes | 垃圾字节数 |
| 34-35 | PAGE_LAST_INSERT | 2 bytes | 最后插入位置 |
| 36-37 | PAGE_DIRECTION | 2 bytes | 插入方向 |
| 38-39 | PAGE_N_DIRECTION | 2 bytes | 同方向插入数 |
| 40-41 | PAGE_N_RECS | 2 bytes | 有效记录数 |
| 42-47 | PAGE_MAX_TRX_ID | 6 bytes | 最大事务ID |
| 48-49 | PAGE_CHECKSUM_START | 2 bytes | Checksum 起始 |
| 50-53 | PAGE_CHECKSUM | 4 bytes | Page Checksum |
| 54-55 | INSTANT_METADATA_OFFSET | 2 bytes | Instant DDL 元数据偏移 (8.0+) |
| 56-57 | INSTANT_METADATA_VERSION | 2 bytes | Instant DDL 版本 (8.0+) |

### 2.4 Page Type 枚举

```cpp
enum class PageType : uint16_t {
    ALLOCATED = 0x0000,      // 新分配页
    INDEX = 0x45bf,          // B+树索引页
    UNDO_LOG = 0x45bb,       // Undo Log 页
    INODE = 0x45bd,          // 段 inode 页
    IBUF_FREE_LIST = 0x45be, // Insert Buffer 空闲列表
    TRX_SYS = 0x45b8,        // 事务系统页
    FSP_HDR = 0x45b9,        // 文件空间头
    XDES = 0x45ba,           // 扩展描述符
    BLOB = 0x45b7,           // BLOB页
    SDI = 0x5441,            // SDI 页 (8.0+)
    SDI2 = 0x5a5a,           // SDI 辅助页 (8.0+)
};
```

---

## 三、行格式解析

### 3.1 Redundant 格式 (MySQL 4.0-5.0)

**记录头结构 (6字节)**:
```
[1 byte info_bits] + [5 bytes extra]
```

**变长字段**: 每个字段2字节长度

**NULL位图**: 每个字段1bit

### 3.2 Compact 格式 (MySQL 5.0+)

**记录头结构 (5字节)**:
```
[3 bytes variable] + [2 bytes fixed]
```

**变长字段**: 每个字段1字节长度

**NULL位图**: ceil(n_null_fields / 8) 字节

**额外字段**:
- 第5字节: n_fields (字段数)

### 3.3 Dynamic 格式 (MySQL 8.0+)

**与 Compact 相同的记录头**，但支持:

- **虚拟列 (Virtual Column)**: is_virtual 标志
- **Instant DDL**: instant_n_cols, default_n_cols
- **变长默认值**: varint 编码

---

## 四、自动探测模块

### 4.1 版本探测逻辑

```cpp
MySQLVersion detect_version_from_page0(const uint8_t* page0) {
    // 1. 检查 Page Type
    if (page_type == SDI || page_type == SDI2) {
        return VERSION_8_0;
    }
    
    // 2. 检查 Space ID
    if (space_id > 0) {
        return VERSION_8_0;
    }
    
    // 3. 检查压缩页特征
    if (zip_size > 0 && zip_size < 16384) {
        return VERSION_8_0;
    }
    
    // 默认返回 5.7
    return VERSION_5_7;
}
```

### 4.2 行格式探测

```cpp
RowFormat detect_row_format_from_header(const uint8_t* record_header) {
    // Redundant: extra_bytes = 6
    if (record_header[4] == 6) {
        return REDUNDANT;
    }
    
    // COMPACT/DYNAMIC: extra_bytes = 5
    return COMPACT;
}
```

---

## 五、B+树索引页解析

### 5.1 目录槽结构

```cpp
struct DirSlot {
    uint16_t offset : 15;  // 记录偏移
    uint16_t n_owned : 1;  // 拥有记录数
};
```

### 5.2 记录类型

```cpp
enum RecordType {
    INFIMUM = 0x02,   // 最小值记录
    SUPREMUM = 0x03,  // 最大值记录
    ORDINARY = 0x00,  // 普通记录
    NODE_PTR = 0x01,  // 节点指针 (非叶子页)
};
```

---

## 六、API 参考

### 6.1 PageHeaderV2

```cpp
struct PageHeaderV2 {
    bool parse(const uint8_t* data, size_t size);
    bool is_index_page() const;
    bool is_undo_page() const;
    bool is_sdi_page() const;
    uint32_t get_page_size() const;
    std::string to_string() const;
};
```

### 6.2 AutoDetector

```cpp
class AutoDetector {
public:
    static MySQLVersion detect_version_from_page0(const uint8_t* page0);
    static MySQLVersion detect_version_from_sdi(const uint8_t* sdi_page);
    static RowFormat detect_row_format(const uint8_t* record, size_t len);
    static DetectionResult detect(const std::filesystem::path& ibd_path);
    static DetectionResult detect_from_file_header(const uint8_t* file_data, size_t size);
};
```

---

## 七、测试覆盖

### 7.1 单元测试

| 测试文件 | 测试项 |
|----------|--------|
| test_page_types_v2.cpp | PageHeaderV2, RecordHeaderV2, Row 格式 |
| test_auto_detector.cpp | 版本探测, 行格式探测 |

### 7.2 集成测试

| 测试场景 | 说明 |
|----------|------|
| MySQL 5.6/5.7 Compact | 基础行格式解析 |
| MySQL 8.0+ Dynamic | Instant DDL 支持 |
| 压缩页 | zip_size = 4096/8192/16384 |
| SDI 页 | 元数据解析 |

---

## 八、未来扩展

### 8.1 TDE 解密

```cpp
class TDEDecryptor {
public:
    bool decrypt_page(uint8_t* page, size_t page_size, const Key& key);
};
```

### 8.2 io_uring 高性能 I/O

```cpp
class IOUringReader {
public:
    void read_pages_async(std::vector<PageRequest>& requests);
    void wait_for_completion();
};
```

---

## 九、参考文献

1. MySQL 8.0.36 源码 - `include/page0page.h`
2. MySQL 5.7.31 源码 - `include/page0page.h`
3. InnoDB Page Format 白皮书

---

**文档版本**: v1.0  
**最后更新**: 2026-03-29  
**作者**: jliang.fang
