# MySQL 8.0 数据恢复引擎

基于 C++ 开发的高性能离线 MySQL 8.0+ 数据恢复工具。

## 功能特性

- **InnoDB 页解析**: 完整支持 COMPACT/REDUNDANT 行格式
- **B+树遍历**: 支持页链验证的递归索引遍历
- **BLOB 恢复**: 多页 BLOB 重建与校验和验证
- **跨平台**: Linux/Windows 统一 API

## 架构设计

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│   FileMapper    │────▶│  PageReader      │────▶│  BPlusTreeParser│
└─────────────────┘     └──────────────────┘     └─────────────────┘
                                                      │
                                                      ▼
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  BlobPointer    │◀────│  SchemaDefs      │◀────│  RecordParser   │
└─────────────────┘     └──────────────────┘     └─────────────────┘
```

## 快速开始

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)

# 恢复表数据
./mysql8_recover --ibd=/path/to/table.ibd --output=/tmp/recovered
```

## API 参考

### PageReader

```cpp
class PageReader {
public:
    std::vector<uint8_t> read_page(uint32_t page_no);
    static bool validate_checksum(const std::vector<uint8_t>& page_data);
};
```

### BPlusTreeParser

```cpp
class BPlusTreeParser {
public:
    bool parse() noexcept;
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;
};
```

## 测试

```bash
# 运行单元测试
ctest --verbose

# 生成测试数据
python3 tools/ibd_generator.py test_table
```
