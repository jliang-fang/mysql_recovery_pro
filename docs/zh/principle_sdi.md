# InnoDB SDI 原理详解

## 1. 概述

SDI（Space Descriptor Index）是 MySQL 8.0 引入的元数据存储机制。它将表的元数据（如表结构、索引定义等）存储在表空间中，取代了传统的 .frm 文件。

## 2. SDI 基本概念

### 2.1 作用

- **元数据存储**：存储表的元数据（结构、索引、分区等）
- **跨版本兼容**：支持不同版本的 MySQL 之间迁移
- **单文件存储**：元数据与数据存储在同一文件中

### 2.2 特性

- **JSON 格式**：元数据以 JSON 格式存储
- **压缩存储**：使用 zlib 压缩
- **校验和保护**：使用 CRC32C 校验和

## 3. SDI 结构

### 3.1 SDI 页

SDI 元数据存储在特殊的页中（FIL_PAGE_TYPE = 9971）：

```
+----------------------------------+
| FIL Header (38 bytes)            |
+----------------------------------+
| SDI Header (变长)                |
+----------------------------------+
| SDI Payload (JSON)               |
+----------------------------------+
| SDI Trailer (8 bytes)            |
+----------------------------------+
```

### 3.2 SDI Payload

SDI Payload 包含表的元数据：

```json
{
  "dictionary": {
    "object": {
      "Table": {
        "name": "test_db/test_table",
        "id": 12345,
        "flags": 1,
        "cols": [
          {
            "name": "id",
            "type": 8,
            "is_nullable": false,
            "length": 8
          },
          {
            "name": "name",
            "type": 15,
            "is_nullable": true,
            "length": 255
          }
        ],
        "idxs": [
          {
            "name": "PRIMARY",
            "type": 1,
            "cols": ["id"]
          }
        ]
      }
    }
  }
}
```

### 3.3 SDI Header

SDI Header 包含：

| 字段 | 大小 | 描述 |
|------|------|------|
| type | 4 bytes | 元数据类型 |
| id | 8 bytes | 对象 ID |
| compressed_len | 4 bytes | 压缩长度 |
| uncompressed_len | 4 bytes | 解压长度 |
| checksum | 4 bytes | 校验和 |

## 4. SDI 解析

### 4.1 解析步骤

```cpp
1. 读取 SDI 页
2. 解析 SDI Header
3. 解压缩 Payload
4. 解析 JSON
5. 提取元数据
```

### 4.2 解析示例

```cpp
// 1. 读取 SDI 页
auto sdi_page = read_page(space_id, page_no);

// 2. 解析 SDI Header
uint32_t type = read_le<uint32_t>(sdi_page.data(), 38);
uint64_t id = read_le<uint64_t>(sdi_page.data(), 42);
uint32_t compressed_len = read_le<uint32_t>(sdi_page.data(), 50);
uint32_t uncompressed_len = read_le<uint32_t>(sdi_page.data(), 54);

// 3. 解压缩 Payload
std::string payload = zlib_decompress(
    sdi_page.data() + 58, 
    compressed_len, 
    uncompressed_len
);

// 4. 解析 JSON
auto json = nlohmann::json::parse(payload);

// 5. 提取元数据
std::string table_name = json["dictionary"]["object"]["Table"]["name"];
```

## 5. SDI 恢复要点

### 5.1 表结构恢复

通过 SDI 恢复表结构：

```cpp
// 1. 找到 SDI 页
for (page_no = 0; page_no < total_pages; ++page_no) {
    page = read_page(page_no);
    
    if (page.page_type == FIL_PAGE_SDI) {
        // 2. 解析 SDI
        auto sdi = parse_sdi(page);
        
        // 3. 提取表结构
        schema = extract_schema(sdi);
    }
}
```

### 5.2 表 ID 匹配

通过表 ID 匹配 SDI：

```cpp
// 1. 读取表的表 ID
table_id = read_table_id(ibd_file);

// 2. 找到匹配的 SDI
for (auto& sdi : sdi_list) {
    if (sdi.id == table_id) {
        // 使用该 SDI
        schema = extract_schema(sdi);
        break;
    }
}
```

### 5.3 多版本兼容

SDI 支持不同版本的 MySQL：

```cpp
// 1. 检查版本
int version = json["version"];

// 2. 根据版本解析
if (version >= 80000) {
    // MySQL 8.0 格式
    parse_v80(json);
} else if (version >= 50700) {
    // MySQL 5.7 格式
    parse_v57(json);
}
```

## 6. SDI 损坏场景

| 场景 | 影响 | 恢复策略 |
|------|------|---------|
| SDI 页损坏 | 无法解析元数据 | 从其他副本恢复 |
| JSON 解析失败 | 元数据损坏 | 手动修复 JSON |
| 校验和错误 | 数据可能损坏 | 使用非严格模式 |

## 7. SDI 与 .frm 的区别

| 特性 | .frm 文件 | SDI |
|------|-----------|-----|
| 存储位置 | 独立文件 | 表空间内 |
| 格式 | 二进制 | JSON |
| 版本兼容 | 差 | 好 |
| 跨平台 | 差 | 好 |

## 8. 实际案例

### 8.1 从 SDI 恢复表结构

```cpp
// 1. 打开 .ibd 文件
auto reader = InnoDBFileReader(ibd_path);

// 2. 遍历所有页
for (page_no = 0; page_no < reader.get_total_pages(); ++page_no) {
    auto page = reader.read_page(page_no);
    
    // 3. 检查是否为 SDI 页
    if (page.page_type == FIL_PAGE_SDI) {
        // 4. 解析 SDI
        auto sdi_json = SDIManager::extract_sdi_json(page.data);
        
        // 5. 解析 JSON
        auto json = nlohmann::json::parse(sdi_json);
        
        // 6. 提取表结构
        schema.from_json(json);
    }
}
```

### 8.2 从 SDI 恢复索引

```cpp
// 1. 解析 SDI
auto sdi = parse_sdi(sdi_page);

// 2. 提取索引定义
for (auto& idx : sdi["indexes"]) {
    IndexInfo index;
    index.name = idx["name"];
    index.is_primary = idx["type"] == 1;
    
    for (auto& col : idx["columns"]) {
        index.columns.push_back(col["name"]);
    }
    
    schema.indexes.push_back(index);
}
```

## 9. 参考资料

- MySQL 8.0 源码：`include/dd/types/table.h`
- MySQL 8.0 源码：`include/dd/types/index.h`
- InnoDB SDI 实现文档
