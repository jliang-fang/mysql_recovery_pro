# MySQL 8.0 数据恢复引擎 - 架构设计文档

## 1. 概述

本项目是一个基于 C++ 开发的高性能离线 MySQL 8.0+ 数据恢复引擎，目标是构建一个世界领先的、离线可用的 MySQL 8.0+ 深度数据恢复工具。

### 1.1 核心能力

- **InnoDB 页解析**：完整支持 COMPACT/REDUNDANT/DYNAMIC/COMPRESSED 行格式
- **B+树遍历**：支持页链验证的递归索引遍历
- **BLOB 恢复**：多页 BLOB 重建与校验和验证
- **Redo Log 回放**：支持所有 InnoDB 行格式的 Redo 日志解析与回放
- **Undo Log 解析**：事务回滚支持
- **SDI 元数据解析**：Space Descriptor Index 解析与表结构重建
- **跨平台支持**：Linux/Windows 统一 API

### 1.2 技术栈

- **语言**：C++20
- **构建系统**：CMake 3.20+
- **测试框架**：Google Test
- **JSON 处理**：nlohmann/json
- **压缩库**：zlib (SDI 解压缩)

## 2. 模块架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                         MySQL 8.0 Recovery Engine                   │
├─────────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────┐ │
│  │   FileMapper │  │  PageReader  │  │ BPlusTreeParser││SchemaDefs│ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────┘ │
│         │                │                  │               │        │
│         ▼                ▼                  ▼               ▼        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────┐ │
│  │   FileMapper │  │ CRC32C/INNODB│  │  RecordParser  │  │Catalog  │ │
│  │  (mmap/Win32)│  │  Checksum    │  │  BLOB Parser   │  │Table    │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────┘ │
│         │                │                  │               │        │
│         ▼                ▼                  ▼               ▼        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────┐ │
│  │  RedoLog     │  │  UndoLog     │  │  Recovery      │  │Export   │ │
│  │  Parser      │  │  Parser      │  │  Engine        │  │Module   │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────┘ │
│         │                │                  │               │        │
│         ▼                ▼                  ▼               ▼        │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                    CLI/GUI Frontend                          │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 3. 核心模块详解

### 3.1 FileMapper

**职责**：跨平台文件映射

**功能**：
- Linux: 使用 `mmap()` 进行内存映射
- Windows: 使用 `CreateFileMappingA()` 和 `MapViewOfFile()`
- RAII 资源管理
- 移动语义支持

**关键接口**：
```cpp
class FileMapper {
public:
    explicit FileMapper(const std::string& path);
    ~FileMapper();
    
    bool is_valid() const;
    const uint8_t* data() const;
    size_t size() const;
};
```

### 3.2 PageReader

**职责**：InnoDB 页校验和计算与验证

**功能**：
- CRC32C 校验和验证 (MySQL 8.0 默认)
- INNODB 校验兼容性 (MySQL 5.6/5.7)
- OLD 校验兼容性 (MySQL 5.5)
- 性能优化的查表法实现

**关键接口**：
```cpp
class PageReader {
public:
    static bool validate_checksum(const std::vector<uint8_t>& page_data,
                                  bool strict_mode = true,
                                  uint32_t* out_stored = nullptr,
                                  uint32_t* out_computed = nullptr);
    
    static uint32_t compute_crc32c(const std::vector<uint8_t>& page_data);
    static uint32_t compute_innodb_checksum(const std::vector<uint8_t>& page_data);
    static uint32_t compute_old_checksum(const std::vector<uint8_t>& page_data);
};
```

### 3.3 BPlusTreeParser

**职责**：B+树索引页解析

**功能**：
- 页头信息解析
- 记录偏移提取
- 健康状态检查
- INFIMUM/SUPREMUM 记录验证
- 目录槽解析

**关键接口**：
```cpp
class BPlusTreeParser {
public:
    explicit BPlusTreeParser(const std::vector<uint8_t>& page_data);
    
    bool parse() noexcept;
    bool is_valid_page_type() const;
    
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;
    
    // Redo 回放支持
    void apply_redo_record(const RedoLogRecord& record);
    void update_page_checksum();
    
    // 字段偏移计算
    static std::optional<uint16_t> calculate_dynamic_field_offset(
        const uint8_t* record_data,
        size_t record_size,
        uint16_t field_no,
        bool is_compact_format);
};
```

### 3.4 SchemaDefs

**职责**：表结构定义与元数据管理

**功能**：
- ColumnInfo：列定义
- IndexInfo：索引定义
- TableSchema：表级元数据
- JSON 序列化/反序列化

**关键接口**：
```cpp
struct ColumnInfo {
    std::string name;
    uint32_t mysql_type_id;
    bool is_nullable;
    uint32_t column_length;
    uint32_t charset_id;
    // ... 更多字段
};

struct TableSchema {
    std::string table_name;
    uint32_t row_format;
    uint32_t page_size;
    std::map<uint32_t, ColumnInfo> columns;
    std::vector<IndexInfo> indexes;
    // ... 更多字段
};
```

### 3.5 RedoLogParser

**职责**：Redo Log 解析

**功能**：
- 解析 Redo Log 记录头
- 支持所有 InnoDB Redo Log 类型
- Redo 回放支持

**关键接口**：
```cpp
class RedoLogParser {
public:
    std::optional<RedoLogRecord> parse_record(const uint8_t* data, size_t len);
    std::vector<RedoLogRecord> parse_file(const std::string& path);
};
```

### 3.6 UndoLogParser

**职责**：Undo Log 解析

**功能**：
- 解析 Undo Log 记录
- 事务回滚支持

**关键接口**：
```cpp
class UndoLogParser {
public:
    std::optional<UndoLogRecord> parse_record(const uint8_t* data, size_t len);
    std::vector<UndoLogRecord> parse_file(const std::string& path);
};
```

### 3.7 SDIManager

**职责**：SDI (Space Descriptor Index) 解析

**功能**：
- 解析 SDI 头部
- ZLIB 解压缩 (raw deflate)
- 表结构提取

**关键接口**：
```cpp
class SDIManager {
public:
    static std::optional<std::string> extract_sdi_json(
        const uint8_t* payload, size_t payload_len);
    
    static catalog::TableSchema parse_sdi_record(
        const uint8_t* payload, size_t payload_len);
};
```

### 3.8 BlobPointer

**职责**：BLOB 指针解析与页链管理

**功能**：
- BLOB 指针解析 (20字节)
- 多页 BLOB 页链构建
- 循环引用检测
- 页链完整性验证

**关键接口**：
```cpp
struct BlobPointer {
    uint32_t space_id;
    uint32_t page_no;
    uint32_t offset;
    uint32_t len;
    uint32_t total_len;
    std::vector<std::pair<uint32_t, uint32_t>> chain_pages;
    
    bool is_complete() const;
    bool is_valid(const std::function<...>& page_reader, bool strict_mode) const;
};

struct BlobPageChain {
    std::vector<std::pair<uint32_t, uint32_t>> pages;
    
    void add_page(uint32_t space_id, uint32_t page_no);
    bool is_valid(const std::function<...>& page_reader, bool strict_mode) const;
    bool has_circular_reference() const;
};
```

## 4. 数据流

```
.ibd 文件 → FileMapper → PageReader → BPlusTreeParser → 记录
                                              ↓
                                         BlobPointer → BLOB 数据
                                              ↓
                                         SchemaDefs → 表结构
                                              ↓
                                         RecoveryEngine → 恢复结果
                                              ↓
                                         ExportModule → CSV/SQL/ibd
```

## 5. 扩展点设计

### 5.1 添加新的 MySQL 版本

```cpp
// 在 page_defs.hpp 中添加版本特定常量
constexpr uint16_t MYSQL_8_1_FEATURE_FLAG = 0x8000;
```

### 5.2 添加新的行格式

```cpp
enum class RowFormat {
    COMPACT,
    DYNAMIC,
    REDUNDANT,
    COMPRESSED,  // 未来支持
    CUSTOM,      // 自定义格式
};
```

### 5.3 添加新的导出格式

```cpp
class ExportModule {
public:
    virtual bool export_to_format(const std::vector<...>& records,
                                  const std::string& path) = 0;
};

class CSVExporter : public ExportModule { ... };
class SQLE exporter : public ExportModule { ... };
class IBDExporter : public ExportModule { ... };
```

## 6. 性能优化

### 6.1 CRC32C 查表法

```cpp
static constexpr auto crc32c_table = generate_crc32c_table();
// 编译时生成，零运行时开销
```

### 6.2 内存映射

使用 mmap 避免额外的内存拷贝

### 6.3 零拷贝视图

```cpp
std::span<const uint8_t> get_page_view(uint32_t page_no);
```

## 7. 测试策略

### 7.1 单元测试

- 每个核心模块都有独立的单元测试
- 覆盖正常场景、边界情况、异常处理

### 7.2 集成测试

- 完整的恢复流程测试
- 多页 BLOB 恢复测试
- Redo Log 回放测试

### 7.3 模糊测试

- 使用 libFuzzer 或 AFL 进行模糊测试
- 测试文件解析器的健壮性

## 8. 未来扩展

### 8.1 支持的 MySQL 版本

- MySQL 5.6/5.7 (REDUNDANT/COMPACT)
- MySQL 8.0/8.1/8.2/8.3/8.4/8.5 (DYNAMIC/COMPRESSED)
- MySQL 9.0 (未来版本)

### 8.2 支持的灾难场景

- Truncate 恢复
- rm -rf 恢复
- 文件头损坏恢复
- 表空间损坏恢复
- Redo Log 应用

### 8.3 GUI 功能

- 文件选择与预览
- 页级可视化
- 记录级预览
- 导出配置
