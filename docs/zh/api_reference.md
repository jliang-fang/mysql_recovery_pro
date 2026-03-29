# MySQL 8.x 数据恢复引擎 - API 参考文档

## 概述

本文档提供了 MySQL 8.x 数据恢复引擎的完整 API 参考。

## 命名空间

所有类和函数都位于 `dbrecovery` 命名空间下，主要子命名空间包括：

- `dbrecovery::innodb` - InnoDB 核心功能
- `dbrecovery::recovery` - 数据恢复功能
- `dbrecovery::exporter` - 数据导出功能
- `dbrecovery::catalog` - 元数据目录
- `dbrecovery::utils` - 工具函数
- `dbrecovery::repair` - 修复功能
- `dbrecovery::disk` - 磁盘扫描功能
- `dbrecovery::crypt` - 加密扫描功能
- `dbrecovery::engine` - 引擎功能
- `dbrecovery::gui` - GUI 功能

## 核心类参考

### dbrecovery::innodb::InnoDBFileReader

管理 .ibd 文件并提供页读取服务。

```cpp
class InnoDBFileReader {
public:
    explicit InnoDBFileReader(const std::string& file_path);
    ~InnoDBFileReader();

    std::vector<uint8_t> read_page(uint32_t page_no);
    std::vector<uint8_t> read_page_raw(uint32_t page_no);
    std::span<const uint8_t> get_page_view(uint32_t page_no);

    uint32_t get_total_pages() const;
    uint64_t get_file_size() const;
    uint32_t get_page_size() const;
    [[nodiscard]] bool is_valid() const;
};
```

### dbrecovery::innodb::BPlusTreeParser

B+树索引页解析器。

```cpp
class BPlusTreeParser {
public:
    explicit BPlusTreeParser(const std::vector<uint8_t>& page_data);

    bool parse() noexcept;
    [[nodiscard]] bool is_valid_page_type() const;
    [[nodiscard]] BTreeNodeType node_type() const;

    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;

    void apply_redo_record(const RedoLogRecord& record);
    void update_page_checksum();

    static std::optional<uint16_t> calculate_dynamic_field_offset(
        const uint8_t* record_data,
        uint16_t field_no,
        size_t record_size,
        bool is_compact_format);
};
```

### dbrecovery::recovery::RecoveryEngine

统一的恢复引擎入口点。

```cpp
class RecoveryEngine {
public:
    explicit RecoveryEngine();
    ~RecoveryEngine();

    bool open(const std::string& file_path);
    void close();

    std::vector<std::map<std::string, std::string>> recover_truncate(
        const catalog::TableSchema& schema);

    bool repair_header(const std::string& output_path);

    bool export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                       const std::string& csv_path);

    bool export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                       const std::string& table_name,
                       const std::string& sql_path);

    [[nodiscard]] bool is_valid() const;
};
```

### dbrecovery::catalog::TableSchema

表结构定义。

```cpp
struct TableSchema {
    std::string table_name;
    uint32_t row_format;
    uint32_t page_size;
    std::map<uint32_t, ColumnInfo> columns;
    std::vector<IndexInfo> indexes;

    bool validate() const;
    uint32_t get_column_size(const ColumnInfo& col) const;
    uint32_t get_nullable_column_count() const;
    std::optional<uint32_t> get_column_offset(const std::string& col_name) const;

    std::string to_json_string() const;
    bool from_json_string(const std::string& json_str);
    bool from_json(const nlohmann::json& j);
    nlohmann::json to_json() const;
};
```

### dbrecovery::exporter::Exporter

导出工具类。

```cpp
class Exporter {
public:
    static bool export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                              const std::vector<std::string>& headers,
                              const std::string& output_path);

    static bool export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                              const std::string& table_name,
                              const std::string& output_path);

    static bool export_to_ibd(const std::vector<std::map<std::string, std::string>>& records,
                              const std::string& output_path);
};
```

## 工具函数参考

### dbrecovery::innodb::read_le / write_le

小端序读写函数。

```cpp
template<typename T>
T read_le(const uint8_t* data, size_t offset = 0);

template<typename T>
void write_le(uint8_t* data, size_t offset, T value);
```

### dbrecovery::innodb::PageReader

页校验和计算与验证工具类。

```cpp
class PageReader {
public:
    static bool validate_checksum(const std::vector<uint8_t>& page_data,
                                  bool strict_mode = true,
                                  uint32_t* out_stored = nullptr,
                                  uint32_t* out_computed = nullptr);

    static uint32_t compute_crc32c(const std::vector<uint8_t>& page_data);
    static uint32_t compute_innodb_checksum(const std::vector<uint8_t>& page_data);
    static uint32_t compute_crc32(const std::vector<uint8_t>& page_data);
    static uint32_t compute_old_checksum(const std::vector<uint8_t>& page_data);
};
```

### dbrecovery::utils::CliParser

命令行参数解析器。

```cpp
class CliParser {
public:
    explicit CliParser(int argc, char** argv);

    bool has_option(const std::string& name) const;
    std::string get_option(const std::string& name, const std::string& default_val = "") const;
    std::optional<int> get_int_option(const std::string& name, int default_val = 0) const;
    bool get_bool_option(const std::string& name, bool default_val = false) const;
    std::optional<double> get_double_option(const std::string& name, double default_val = 0.0) const;

    void require_options(const std::vector<std::string>& required) const;
    bool validate() const;

    static std::string get_help_message(const std::string& program_name,
                                        const std::vector<std::string>& usage);
    static void print_help(const std::string& program_name,
                          const std::vector<std::string>& usage);
};
```

## 常量参考

### InnoDB 页类型

```cpp
constexpr uint16_t FIL_PAGE_INDEX       = 9969;  // B+Tree index page
constexpr uint16_t FIL_PAGE_UNDO_LOG    = 9970;  // Undo log page
constexpr uint16_t FIL_PAGE_SDI         = 9971;  // SDI metadata page
constexpr uint16_t FIL_PAGE_TYPE_BLOB   = 9972;  // BLOB page
constexpr uint16_t FIL_PAGE_COMPRESSED  = 9973;  // Compressed page
```

### MySQL 类型

```cpp
constexpr uint8_t TINY        = 1;
constexpr uint8_t SHORT       = 2;
constexpr uint8_t LONG        = 3;
constexpr uint8_t FLOAT       = 4;
constexpr uint8_t DOUBLE      = 5;
constexpr uint8_t LONGLONG    = 8;
constexpr uint8_t INT24       = 9;
constexpr uint8_t DATE        = 10;
constexpr uint8_t DATETIME    = 12;
constexpr uint8_t VARCHAR     = 15;
constexpr uint8_t DATETIME2   = 18;
constexpr uint8_t BLOB        = 252;
```

## 使用示例

### 基本恢复流程

```cpp
#include "dbrecovery/recovery/engine.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

using namespace dbrecovery;

// 打开文件
recovery::RecoveryEngine engine;
if (!engine.open("/path/to/table.ibd")) {
    std::cerr << "Failed to open file" << std::endl;
    return 1;
}

// 定义表结构
catalog::TableSchema schema;
schema.table_name = "users";
schema.row_format = 2;  // DYNAMIC
schema.page_size = 16384;

// 添加列
catalog::ColumnInfo id_col;
id_col.name = "id";
id_col.mysql_type_id = innodb::mysql_types::LONGLONG;
id_col.is_nullable = false;
schema.columns[1] = id_col;

// 执行 Truncate 恢复
auto records = engine.recover_truncate(schema);

// 导出为 CSV
engine.export_to_csv(records, "/tmp/recovered.csv");

engine.close();
```

### 命令行工具

```bash
# 恢复表数据
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered

# 指定表 ID
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered --table-id=12345

# Truncate 恢复
./mysql8_recover --ibd=/path/to/table.ibd --truncate-recovery --format=csv

# rm -rf 恢复
./mysql8_recover --ibd=/path/to/table.ibd --rmrf-recovery --format=sql

# BLOB 恢复
./mysql8_recover --ibd=/path/to/table.ibd --blob-recovery
```

## 错误处理

所有主要函数都返回布尔值表示成功/失败。错误信息通过 std::cerr 输出。

```cpp
if (!engine.open(file_path)) {
    // 错误信息已输出到 std::cerr
    return 1;
}
```

## 线程安全

- `RecoveryEngine` 类不是线程安全的，每个实例只能在一个线程中使用
- `PageReader` 工具类是线程安全的（纯静态方法）
- `InnoDBFileReader` 需要外部同步（如果在多线程中使用同一个实例）

## 版本兼容性

- MySQL 5.6/5.7: 支持 REDUNDANT/COMPACT 行格式
- MySQL 8.0/8.1/8.2/8.3/8.4: 支持 DYNAMIC/COMPRESSED 行格式
- MySQL 9.0: 预留扩展点

## 许可证

MIT License
