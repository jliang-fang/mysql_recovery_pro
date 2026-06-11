# MySQL 5.6-8.4 数据恢复引擎

[English](../en/README.md) | [中文](./README.md)

基于 C++ 开发的高性能离线 MySQL 5.6-8.4 数据恢复工具。

## 🚀 功能特性

- **InnoDB 页解析**：完整支持 COMPACT/REDUNDANT/DYNAMIC/COMPRESSED 行格式
- **B+树遍历**：支持页链验证的递归索引遍历
- **BLOB 恢复**：多页 BLOB 重建与校验和验证
- **Redo Log 回放**：支持所有 InnoDB 行格式的 Redo 日志解析与回放
- **Undo Log 解析**：事务回滚支持
- **SDI 元数据解析**：Space Descriptor Index 解析与表结构重建
- **跨平台**：Linux/Windows 统一 API
- **CLI/GUI**：命令行工具和图形界面

## 📊 架构设计

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│   FileMapper    │────▶│  PageReader      │────▶│  BPlusTreeParser│
└─────────────────┘     └──────────────────┘     └─────────────────┘
                                                      │
                                                      ▼
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  BlobPointer    │◀────│  SchemaDefs      │◀────│  RecordParser   │
└─────────────────┘     └──────────────────┘     └─────────────────┘
                                                      │
                                                      ▼
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  RedoLog        │     │  UndoLog         │     │  Recovery       │
│  Parser         │     │  Parser          │     │  Engine         │
└─────────────────┘     └──────────────────┘     └─────────────────┘
                                                      │
                                                      ▼
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  ExportModule   │     │  CLI/GUI         │     │  TestSuite      │
│  (CSV/SQL/ibd)  │     │  Frontend        │     │  (GTest)        │
└─────────────────┘     └──────────────────┘     └─────────────────┘
```

## 📦 快速开始

### 编译构建

```bash
# 克隆项目
cd /app/mysql_recovery_proy

# 创建构建目录
mkdir -p build && cd build

# 配置 CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)

# 运行测试
ctest --verbose
```

### 命令行工具

```bash
# 恢复表数据
./mysql_recovery_pro --ibd=/path/to/table.ibd --output=/tmp/recovered

# 指定表 ID
./mysql_recovery_pro --ibd=/path/to/table.ibd --output=/tmp/recovered --table-id=12345

# 查看帮助
./mysql_recovery_pro --help
```

### GUI 工具

```bash
# 启动图形界面
cd build/gui
./mysql_recovery_proy_gui
```

## 📚 API 参考

### PageReader

```cpp
class PageReader {
public:
    static bool validate_checksum(const std::vector<uint8_t>& page_data,
                                  bool strict_mode = true,
                                  uint32_t* out_stored = nullptr,
                                  uint32_t* out_computed = nullptr);
    
    static uint32_t compute_crc32c(const std::vector<uint8_t>& page_data);
    static uint32_t compute_innodb_checksum(const std::vector<uint8_t>& page_data);
};
```

### BPlusTreeParser

```cpp
class BPlusTreeParser {
public:
    bool parse() noexcept;
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;
    
    void apply_redo_record(const RedoLogRecord& record);
    void update_page_checksum();
};
```

### RecoveryEngine

```cpp
class RecoveryEngine {
public:
    bool open(const std::string& file_path);
    void close();
    
    std::vector<std::map<std::string, std::string>> recover_truncate(
        const catalog::TableSchema& schema);
    
    bool export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                       const std::string& csv_path);
    bool export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                       const std::string& table_name,
                       const std::string& sql_path);
};
```

## 🧪 测试

```bash
# 运行所有测试
ctest --verbose

# 运行特定测试
./build/tests/test_page_header
./build/tests/test_redo_log
./build/tests/test_blob_recovery

# 生成测试数据
python3 tools/ibd_generator.py test_table
```

## 📖 文档

- [架构设计](./architecture.md) - 详细的架构设计文档
- [用户指南](./user_guide.md) - 完整的用户指南
- [原理文档](./zh/) - 原理说明

## 🔧 技术栈

- **语言**：C++20
- **构建系统**：CMake 3.20+
- **测试框架**：Google Test
- **JSON 处理**：nlohmann/json
- **压缩库**：zlib

## 🌍 支持的 MySQL 版本

| 版本 | 行格式 | 校验和 | SDI | 状态 |
|------|--------|--------|-----|------|
| 5.6  | REDUNDANT, COMPACT | INNODB, OLD | 无 | ✅ 基本支持 |
| 5.7  | REDUNDANT, COMPACT | INNODB, CRC32C | 无 | ✅ 基本支持 |
| 8.0  | REDUNDANT, COMPACT, DYNAMIC, COMPRESSED | CRC32C | 支持 | ✅ 完整支持 |
| 8.1+ | 所有 8.0 格式 | CRC32C | 支持 | ✅ 完整支持 |
| 9.0+ | 所有 8.0 格式 | CRC32C | 支持 | 🔄 预留支持 |

## 📊 支持的灾难场景

| 场景 | 恢复能力 | 备注 |
|------|----------|------|
| Truncate | ✅ 完整恢复 | 基于页链和 Redo Log |
| rm -rf | ✅ 部分恢复 | 需要未覆盖的数据页 |
| 文件头损坏 | ✅ 修复 | 重建文件头 |
| 表空间损坏 | ✅ 部分恢复 | 基于可用页 |
| Redo Log 应用 | ✅ 完整恢复 | 前滚恢复 |
| BLOB 损坏 | ✅ 部分恢复 | 基于页链 |

## 🎯 项目目标

1. **世界领先**：在底层数据解析能力上达到顶尖水平
2. **离线可用**：无需连接数据库即可恢复数据
3. **工业级**：符合严苛的工业级交付标准
4. **跨平台**：Linux/Windows 统一 API
5. **友好界面**：CLI 和 GUI 双支持

## 🤝 贡献指南

欢迎提交 Issue 和 Pull Request！

### 代码风格

- 遵循 Google C++ Style Guide
- 使用 clang-format 进行格式化
- 添加单元测试

### 提交规范

```
type(scope): description

body

footer
```

类型：feat, fix, docs, style, refactor, test, chore

## 📄 许可证

MIT License

## 👨‍💻 作者

**jliang.fang** - 首席数据库内核架构师

---

<div align="center">
  <p>打造世界领先的 MySQL 数据恢复工具</p>
</div>
