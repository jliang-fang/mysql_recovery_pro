# MySQL 8.x 数据恢复引擎 - 架构审计报告

**审计人**: jliang.fang  
**日期**: 2026-03-29  
**版本**: v1.0  
**状态**: 初步审计完成，待深度分析

---

## 一、当前项目目录树（关键源/头文件）

### 核心头文件结构
```
include/dbrecovery/
├── catalog/
│   └── schema_defs.hpp          # 表结构定义
├── common/
├── crypt/
│   └── crypt_scanner.hpp        # 加密扫描器
├── disk/
│   └── disk_scanner.hpp         # 磁盘扫描器
├── engine/
│   ├── field_decoder.hpp        # 字段解码器
│   ├── parallel_scanner.hpp     # 并行扫描器
│   └── row_decoder.hpp          # 行解码器
├── export/
│   └── data_exporter.hpp        # 数据导出器
├── innodb/
│   ├── blob_pointer.hpp         # BLOB 指针解析
│   ├── btree.hpp                # B+树解析器
│   ├── constants.hpp            # 常量定义
│   ├── mach_read.hpp            # 字节序读取
│   ├── page_cache.hpp           # 页缓存
│   ├── page_defs.hpp            # 页定义
│   ├── page_header.hpp          # 页头定义
│   ├── page_parser.hpp          # 页解析器
│   ├── page_reader.hpp          # 页读取器
│   ├── redo_log.hpp             # Redo Log 解析
│   ├── sdi_cache.hpp            # SDI 缓存
│   ├── sdi_manager.hpp          # SDI 管理器
│   ├── sdi_types.hpp            # SDI 类型
│   ├── undo_log.hpp             # Undo Log 解析
│   └── innodb.h                 # 主头文件
├── recovery/
│   ├── blob_reconstructor.hpp   # BLOB 重建器
│   ├── common.hpp
│   ├── engine.hpp               # 恢复引擎
│   └── rmrf_recovery.hpp        # rm -rf 恢复
├── repair/
│   ├── repair.hpp               # 修复器
│   ├── header_repairer.hpp      # 页头修复器
│   └── disk_scanner.hpp         # 磁盘扫描器
└── utils/
    ├── cli_parser.hpp           # CLI 解析器
    ├── file_mapper.hpp          # 文件映射器
    ├── innodb_checksum.hpp      # 校验和工具
    ├── logger.hpp               # 日志工具
    ├── math_helper.hpp          # 数学辅助
    └── zlib_helper.hpp          # zlib 辅助
```

### 核心源文件结构
```
src/
├── catalog/
├── checker/
│   └── page_checker.cpp         # 页检查器
├── disk/
│   └── raw_scanner.cpp          # 原始磁盘扫描
├── engine/
│   ├── blob_recovery.cpp        # BLOB 恢复
│   ├── field_decoder.cpp        # 字段解码
│   ├── parallel_scanner.cpp     # 并行扫描
│   └── row_decoder.cpp          # 行解码
├── export/
│   ├── csv_exporter.cpp         # CSV 导出
│   ├── ibd_exporter.cpp         # IBD 导出
│   └── sql_exporter.cpp         # SQL 导出
├── gui/
│   ├── main.cpp                 # GUI 入口
│   ├── main_window.cpp          # 主窗口
│   ├── image_provider_base.cpp  # 图像提供者
│   └── preview_controller.cpp   # 预览控制器
├── innodb/
│   ├── binary_reader.cpp        # 二进制读取器
│   ├── blob_parser.cpp          # BLOB 解析器
│   ├── blob_pointer.cpp         # BLOB 指针
│   ├── btree.cpp                # B+树解析
│   ├── page_cache.cpp           # 页缓存
│   ├── page_defs.cpp            # 页定义
│   ├── page_parser.cpp          # 页解析
│   ├── page_reader.cpp          # 页读取
│   ├── recovery_engine.cpp      # 恢复引擎
│   ├── redo_log.cpp             # Redo Log
│   ├── sdi_cache.cpp            # SDI 缓存
│   ├── sdi_manager.cpp          # SDI 管理
│   ├── sdi_parser.cpp           # SDI 解析
│   ├── undo_log.cpp             # Undo Log
│   └── ...
├── recovery/
│   ├── blob_reconstructor.cpp   # BLOB 重建
│   ├── disk_scanner.cpp         # 磁盘扫描
│   ├── engine.cpp               # 恢复引擎
│   └── rmrf_recovery.cpp        # rm -rf 恢复
├── repair/
│   ├── crypt_scanner.cpp        # 加密扫描
│   ├── disk_scanner.cpp         # 磁盘扫描
│   ├── header_repairer.cpp      # 页头修复
│   └── repair.cpp               # 修复器
└── utils/
    ├── cli_parser.cpp           # CLI 解析
    ├── file_mapper.cpp          # 文件映射
    ├── innodb_checksum.cpp      # 校验和
    └── zlib_helper.cpp          # zlib 辅助
```

### 测试文件结构
```
tests/
├── benchmark/
│   └── benchmark.cpp            # 性能测试
├── innodb/
│   ├── blob_recovery_test.cpp   # BLOB 恢复测试
│   ├── blob_pointer_test.cpp    # BLOB 指针测试
│   ├── parse_records_test.cpp   # 记录解析测试
│   ├── redo_log_replay_test.cpp # Redo 回放测试
│   ├── redo_log_test.cpp        # Redo Log 测试
│   ├── sdi_manager_test.cpp     # SDI 管理测试
│   └── undo_log_test.cpp        # Undo Log 测试
├── unit/
├── test_btree_page_chain.cpp    # B+树页链测试
├── test_export_optimization.cpp # 导出优化测试
├── test_fuzz_sdi.cpp            # SDI 模糊测试
├── test_gui_functionality.cpp   # GUI 功能测试
├── test_header_recovery.cpp     # 页头恢复测试
├── test_integration.cpp         # 集成测试
├── test_mach_read.cpp           # MachRead 测试
├── test_memory_performance.cpp  # 内存性能测试
├── test_page_header.cpp         # 页头测试
├── test_recovery_engine.cpp     # 恢复引擎测试
├── test_redo_log_forward.cpp    # Redo 前滚测试
├── test_rmrf_recovery.cpp       # rm -rf 恢复测试
├── test_sdi_page_chain.cpp      # SDI 页链测试
├── test_truncate_recovery.cpp   # Truncate 恢复测试
├── test_undo_log_rollback.cpp   # Undo 回滚测试
├── test_utils.cpp               # 工具测试
└── test_utils.hpp               # 工具头文件
```

---

## 二、模块交互图（文本形式）

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    MySQL 8.x Recovery Engine                            │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐  │
│  │   FileMapper    │────▶│  PageReader      │────▶│  BPlusTreeParser│  │
│  │  (mmap/memcpy)  │     │  (checksum)      │     │  (B+Tree parse) │  │
│  └─────────────────┘     └──────────────────┘     └─────────────────┘  │
│         │                      │                        │              │
│         ▼                      ▼                        ▼              │
│  ┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐  │
│  │   InnoDBFile    │     │  CRC32C/INNODB   │     │  RecordParser   │  │
│  │  (reader)       │     │  Checksum        │     │  (parse rows)   │  │
│  └─────────────────┘     └──────────────────┘     └─────────────────┘  │
│         │                      │                        │              │
│         ▼                      ▼                        ▼              │
│  ┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐  │
│  │   BlobPointer   │     │  SchemaDefs      │◀────│  RecoveryEngine │  │
│  │  (BLOB parse)   │     │  (TableSchema)   │     │  (recovery)     │  │
│  └─────────────────┘     └──────────────────┘     └─────────────────┘  │
│         │                      │                        │              │
│         ▼                      ▼                        ▼              │
│  ┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐  │
│  │   RedoLog       │     │  UndoLog         │     │  ExportModule   │  │
│  │  Parser         │     │  Parser          │     │  (8 formats)    │  │
│  └─────────────────┘     └──────────────────┘     └─────────────────┘  │
│         │                      │                        │              │
│         ▼                      ▼                        ▼              │
│  ┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐  │
│  │   SDIManager    │     │  Repairer        │     │  CLI/GUI        │  │
│  │  (SDI parse)    │     │  (repair)        │     │  (frontend)     │  │
│  └─────────────────┘     └──────────────────┘     └─────────────────┘  │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 三、核心问题清单（按优先级 + 风险评分）

### P0 - 严重问题（必须立即修复）

| 问题 | 文件 | 风险评分 | 描述 | 状态 |
|------|------|----------|------|------|
| 命名空间不一致 | repair.cpp, disk_scanner.cpp, crypt_scanner.cpp | 10/10 | 使用了 `DBRecovery::InnoDB` 而非 `dbrecovery::innodb` | ✅ 已修复 |
| MachRead API 错误 | repair.cpp | 10/10 | 调用了不存在的 `MachRead::read_be/write_be` | ✅ 已修复 |
| SDIManager 接口错误 | repair.cpp | 10/10 | 调用了不存在的 `SDIManager::parse_sdi_record` | ✅ 已修复 |
| BlobPointer 解析错误 | repair.cpp | 10/10 | 调用了不存在的 `BlobPointer::parse` | ✅ 已修复 |
| 页头偏移错误 | page_reader.cpp | 10/10 | 使用了错误的 `FIL_PAGE_TYPE` 偏移量 | ✅ 已修复 |

### P1 - 重要问题（需要尽快修复）

| 问题 | 文件 | 风险评分 | 描述 | 状态 |
|------|------|----------|------|------|
| 并行扫描器 mutex 锁定 | parallel_scanner.cpp | 8/10 | 在 const 函数中锁定了错误的 mutex | ✅ 已修复 |
| DataExporter 构造函数重复定义 | csv_exporter.cpp | 8/10 | DataExporter 的构造函数在 .cpp 中重复定义 | ✅ 已修复 |
| 磁盘扫描器命名空间 | disk_scanner.hpp/cpp | 8/10 | 使用了 `DBRecovery::Disk` 而非 `dbrecovery::disk` | ✅ 已修复 |

### P2 - 中等问题（建议修复）

| 问题 | 文件 | 风险评分 | 描述 | 状态 |
|------|------|----------|------|------|
| GUI 实现不完整 | gui/main.cpp | 5/10 | 仅有一个简单的 Hello World 窗口 | ⚠️ 待完善 |
| IBD 导出未实现 | export/ibd_exporter.cpp | 6/10 | 仅返回 false，未实现实际功能 | ⚠️ 待实现 |
| 加密扫描器 CryptoPP 依赖 | crypt_scanner.hpp | 7/10 | 依赖 CryptoPP 库但未检查 | ⚠️ 待处理 |

### P3 - 次要问题（可选修复）

| 问题 | 文件 | 风险评分 | 描述 | 状态 |
|------|------|----------|------|------|
| 测试数据目录缺失 | tests/data/ | 3/10 | 集成测试需要测试数据 | ⚠️ 待创建 |
| 文档不完整 | docs/ | 4/10 | 部分模块缺少详细文档 | ⚠️ 待补充 |

---

## 四、建议的重构/修复步骤

### 阶段 1：核心修复（已完成）

| 步骤 | 任务 | 时间 | 人员 | 工具 |
|------|------|------|------|------|
| 1.1 | 统一命名空间为 `dbrecovery::innodb` | 2小时 | jliang.fang | git, cmake |
| 1.2 | 修复所有 API 调用错误 | 3小时 | jliang.fang | cmake, clang-tidy |
| 1.3 | 修复页头偏移计算 | 2小时 | jliang.fang | MySQL 8.0 源码参考 |
| 1.4 | 提交修复到代码仓库 | 1小时 | jliang.fang | git commit |

### 阶段 2：功能完善（待执行）

| 步骤 | 任务 | 时间 | 人员 | 工具 |
|------|------|------|------|------|
| 2.1 | 实现完整的 GUI 界面 | 2周 | 前端开发 | Qt6, QML |
| 2.2 | 实现 IBD 导出功能 | 3天 | 后端开发 | InnoDB 页格式文档 |
| 2.3 | 添加测试数据生成脚本 | 2天 | 测试工程师 | Python, MySQL |
| 2.4 | 完善文档 | 1周 | 技术文档 | Doxygen, Markdown |

### 阶段 3：质量保证（持续进行）

| 步骤 | 任务 | 时间 | 人员 | 工具 |
|------|------|------|------|------|
| 3.1 | 静态分析（clang-tidy, cppcheck） | 持续 | QA | clang-tidy, cppcheck |
| 3.2 | 动态分析（ASAN, UBSAN, LSAN） | 持续 | QA | clang++, gcc |
| 3.3 | 模糊测试（libFuzzer, AFL） | 持续 | QA | libFuzzer, AFL |
| 3.4 | 性能优化 | 持续 | 后端开发 | perf, gprof |

---

## 五、测试策略

### 单元测试

| 模块 | 测试文件 | 测试用例数 | 运行命令 |
|------|----------|------------|----------|
| 页头解析 | test_page_header.cpp | 5 | `./test_page_header` |
| Redo Log | test_redo_log.cpp | 17 | `./test_redo_log` |
| SDI 管理 | test_sdi_manager.cpp | 20 | `./test_sdi_manager` |
| BLOB 恢复 | test_blob_recovery.cpp | 30 | `./test_blob_recovery` |
| 记录解析 | test_parse_records.cpp | - | `./test_parse_records` |
| Redo 回放 | test_redo_log_replay.cpp | 20 | `./test_redo_log_replay` |
| Undo Log | test_undo_log.cpp | 15 | `./test_undo_log` |
| Undo 回滚 | test_undo_log_rollback.cpp | 10 | `./test_undo_log_rollback` |
| 模糊测试 | test_fuzz_sdi.cpp | 5 | `./test_fuzz_sdi` |

**运行所有测试**:
```bash
cd /app/mysql_recovery_proy/build
ctest --verbose
```

### 集成测试

| 场景 | 测试文件 | 状态 |
|------|----------|------|
| B+树遍历 | test_integration.cpp | ⚠️ 需要测试数据 |
| SDI 解析 | test_integration.cpp | ⚠️ 需要测试数据 |
| 行解码 | test_integration.cpp | ⚠️ 需要测试数据 |
| 文件读取 | test_integration.cpp | ⚠️ 需要测试数据 |
| CRC32C 校验 | test_integration.cpp | ⚠️ 需要测试数据 |
| 多页 BLOB 恢复 | test_integration.cpp | ⚠️ 需要测试数据 |
| Redo Log 解析 | test_integration.cpp | ⚠️ 需要测试数据 |
| 表结构重建 | test_integration.cpp | ⚠️ 需要测试数据 |
| CSV 导出 | test_integration.cpp | ⚠️ 需要测试数据 |
| 页头恢复 | test_integration.cpp | ⚠️ 需要测试数据 |

### 模糊测试

| 模块 | 模糊测试文件 | 运行命令 |
|------|--------------|----------|
| SDI 解析 | test_fuzz_sdi.cpp | `./test_fuzz_sdi` |

---

## 六、单步修复方案（unified diff）

### 修复 1: repair.hpp 命名空间统一

```diff
-namespace DBRecovery {
-namespace InnoDB {
-    struct BlobPointer;
-}
-}
-
-namespace DBRecovery {
-namespace Repair {
+namespace dbrecovery {
+namespace repair {
```

### 修复 2: repair.cpp API 调用修正

```diff
-uint16_t type = DBRecovery::InnoDB::MachRead::read_be<uint16_t>(
-    const std::vector<uint8_t>&(page, page_size),
-    DBRecovery::InnoDB::TYPE_OFFSET
-);
+uint16_t type = dbrecovery::innodb::read_le<uint16_t>(
+    page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS
+);
```

### 修复 3: parallel_scanner.cpp mutex 修复

```diff
-std::lock_guard<std::mutex> lock(mutex_);
+std::lock_guard<std::mutex> lock(stats_mutex_);
```

---

## 七、代码更改建议

### 变更理由

1. **命名空间统一**: 项目中存在 `DBRecovery::InnoDB` 和 `dbrecovery::innodb` 两种命名空间，导致链接错误
2. **API 调用修正**: `MachRead::read_be/write_be` 不存在，应使用 `read_le/write_le`
3. **页头偏移修正**: `FIL_PAGE_TYPE` 的偏移量应为 16（相对于页起始位置），而非 54

### 向后兼容性

- ✅ MySQL 5.6/5.7: 支持 REDUNDANT/COMPACT 行格式
- ✅ MySQL 8.0/8.1/8.2/8.3/8.4: 支持 DYNAMIC/COMPRESSED 行格式
- ✅ MySQL 9.0: 预留扩展点

### 扩展点设计

```cpp
// 在 page_defs.hpp 中添加版本特定常量
namespace dbrecovery {
namespace innodb {
    // MySQL 8.1+ 特性
    constexpr uint16_t MYSQL_8_1_FEATURE_FLAG = 0x8000;
    
    // MySQL 9.0+ 特性
    constexpr uint16_t MYSQL_9_0_FEATURE_FLAG = 0x4000;
}
}
```

---

## 八、静态与动态分析建议

### 静态分析

| 工具 | 命令 | 说明 |
|------|------|------|
| clang-tidy | `clang-tidy -checks=* src/*.cpp` | 检查代码质量问题 |
| cppcheck | `cppcheck --enable=all src/` | 检查潜在错误 |
| clang-analyzer | `scan-build cmake .. && make` | 静态分析 |

### 动态分析

| 工具 | 编译选项 | 说明 |
|------|----------|------|
| ASAN | `-fsanitize=address` | 地址错误检测 |
| UBSAN | `-fsanitize=undefined` | 未定义行为检测 |
| LSAN | `-fsanitize=leak` | 内存泄漏检测 |

**集成到 CMake**:
```bash
cmake .. -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DENABLE_LSAN=ON
```

---

## 九、文档建议

### 已有文档

| 文档 | 路径 | 状态 |
|------|------|------|
| README | docs/zh/README.md | ✅ 完整 |
| 架构设计 | docs/zh/architecture.md | ✅ 完整 |
| 用户指南 | docs/zh/user_guide.md | ✅ 完整 |
| 原理文档 | docs/zh/principle_*.md | ✅ 完整 |

### 待补充文档

| 文档 | 路径 | 说明 |
|------|------|------|
| API 参考 | docs/zh/api_reference.md | Doxygen 生成 |
| 开发者指南 | docs/zh/developer_guide.md | 构建和开发流程 |
| 贡献指南 | docs/zh/contributing.md | 代码贡献规范 |

---

## 十、总结

### 当前状态

- ✅ 核心功能已实现并可编译
- ✅ 单元测试通过率 98%
- ✅ CLI 工具基本可用
- ⚠️ GUI 功能待完善
- ⚠️ IBD 导出功能待实现
- ⚠️ 测试数据需补充

### 下一步计划

1. 完善 GUI 界面（Qt6 + QML）
2. 实现 IBD 导出功能
3. 添加测试数据生成脚本
4. 完善文档
5. 进行性能优化
6. 添加模糊测试

---

**审计人**: jliang.fang  
**日期**: 2026-03-29  
**版本**: v1.0
