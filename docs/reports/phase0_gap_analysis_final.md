# MySQL Recovery Pro Phase 0 生产级差距分析报告

> 生成日期: 2026-08-27
> 范围: 源码、CMake、CLI、测试和隔离构建结果的交叉审计
> 方法: 对 Phase 0 修复后的实际实现进行静态审查，并以 `/app/mysql_recovery_test` 隔离验证为准

---

## 摘要

本报告对 MySQL Recovery Pro 代码库进行生产级差距分析，覆盖：
- **架构蓝图映射**：`docs/architecture_v2_blueprint.md` 中 P0-P4 模块到实际源码、CMake 目标、CLI 参数和测试的映射（未找到 `ARCHITECTURE_v2_BEYOND_ODU.md`）
- **安全边界复核**：版本、Undo、SDI、compressed/TDE、salvage 和统一结果契约
- **风险等级评估**：CLEAN / 已修复 / 部分缓解 / 仍存在

**核心结论：已完成的版本、Undo、SDI、compressed 状态、salvage 隔离和统一结果契约修复均保持 fail-closed。尚未经过真实 MySQL compressed/TDE/keyring fixture 验证的路径不宣称支持；heuristic 和候选探测结果仅为 forensic evidence。**

---

## 第一部分：架构蓝图 → 源码/CMake/CLI/测试映射

### 1.1 构建目标：单一静态库

| 蓝图模块 | 源文件 | CMake 目标 | CLI 参数 | 测试覆盖 |
|---------|--------|-----------|---------|---------|
| Version Detection | `src/common/mysql_version_detector.cpp` | `dbrecovery_innodb` | `--force-mysql-version` | `test_version_detection` |
| Roll Pointer | `include/dbrecovery/innodb/roll_ptr_decoder.hpp` | 内联模板 | 隐式 | `test_roll_ptr_decoder` |
| SDI Extraction | `src/innodb/sdi_manager.cpp`, `sdi_extractor.hpp` | `dbrecovery_innodb` | `--file-sdi`, `--extract-sdi` | `test_sdi_manager`, `test_sdi_extractor` |
| Compressed Table | `src/innodb/compressed_table_recovery.cpp` | `dbrecovery_innodb` | `--compressed-recovery` | `test_compressed_table_recovery` |
| Encrypted/TDE | `src/innodb/encrypted_table_recovery.cpp`, `offline_keyring_parser.cpp` | `dbrecovery_innodb` | `--keyring-file`, `--tde-password` | `test_tde_keyring` |
| Undo/Flashback | `include/dbrecovery/innodb/deep_undo_analyzer.hpp` | `dbrecovery_innodb` | `--flashback-delete`, `--flashback-update` | `test_deep_undo_analyzer`, `test_flashback_generator` |
| Salvage Engine | `src/salvage/salvage_engine.cpp` | `dbrecovery_innodb` | `--salvage-mode`, `--evidence-fragments` | `test_p1_salvage_fuzz` |
| Secondary Index | `include/dbrecovery/innodb/secondary_index_rescuer.hpp` | `dbrecovery_innodb` | `--secondary-rescue`, `--secondary-index` | `test_secondary_index_rescuer` |
| Redo Log Replay | `src/innodb/redo_log_replayer.cpp` | `dbrecovery_innodb` | `--redo-replay`, `--redo-log`, `--redo-space-id` | `test_redo_log_replay` |
| Partition Table | `include/dbrecovery/innodb/partition_table_recovery.hpp` | `dbrecovery_innodb` | `--partition-recovery` | `test_partition_table_recovery` |
| DDL Detection | `include/dbrecovery/innodb/ddl_change_detector.hpp` | `dbrecovery_innodb` | `--detect-ddl-changes` | 无专用测试 |
| Page Scanner | `include/dbrecovery/innodb/page_scanner.hpp` | 内联 | 隐式 | `test_page_scanner` |
| B+Tree Parser | `include/dbrecovery/innodb/index_page_parser.hpp` | 内联 | 隐式 | `test_btree_node_parser` |
| Schema Inference | `include/dbrecovery/inference/*` | `dbrecovery_innodb` | `--schema-file` | `test_heuristic_inference` |
| Disk Carving | `src/disk/xfs_disk_carver.cpp`, `ext4_disk_carver.cpp` | `dbrecovery_innodb` | `--disk-carving`, `--fs-type` | `test_raw_disk_carver` |

**关键发现：**
- 所有 P0-P4 模块映射到**单一静态库目标** `dbrecovery_innodb`（`CMakeLists.txt:160`），无独立模块构建
- CLI 入口为 `RecoveryCLI` 类（`include/dbrecovery/tools/recovery_cli.hpp`），约 300 行参数注册
- 测试目录 `tests/CMakeLists.txt` 包含 60+ 测试目标，含 P0 回归测试和 P1 模糊测试
- 报告 `phase0_gap_analysis_updated.md` 中 "P1.10 trace_version_chain 集成测试" 声明已验证：`deep_undo_analyzer_test.cpp:1194-1526` 包含真实 undo 链集成测试，覆盖 3 记录链遍历、循环检测、损坏页、UNRESOLVED 拒绝等场景

### 1.2 可复用 API 清单

| 组件 | API 位置 | 关键接口 | 复用性 |
|------|---------|---------|-------|
| RollPtrDecoder | `roll_ptr_decoder.hpp` | `decode_storage()`, `encode()`, `get_page_no()`, `get_offset()` | 头文件内联，零依赖 |
| TDEDecryptor | `tde_decryptor.hpp` | `is_encrypted()`, `decrypt_page()`, `add_table_key()`, `set_primary_key()` | 独立加密模块 |
| OfflineKeyringParser | `offline_keyring_parser.hpp` | `parse_keyring_file()`, `derive_table_key()`, `verify_derived_key()` | 多后端 keyring 解析 |
| SDIManager | `sdi_manager.hpp` | `extract_zip_record()`, `extract_standard_payload()`, `parse_sdi_record()` | 完整 SDI 管道 |
| RedoLogFilter | `redo_log_replayer.hpp` | `passes_filter()` | 独立的 LSN/space_id 过滤 |
| SalvageEngine | `salvage_engine.hpp` | `salvage_page()`, `assess_damage()`, `export_csv()` | 4 级 salvage 管道 |
| BPlusTreeParser | `b_plus_tree_parser.hpp` | `parse()`, `parse_records()`, `parse_cursor()` | 可独立用于页面解析 |

---

## 第二部分：7 项静态审计发现详细验证

### 发现 1：版本检测器偏移量 / 5.6 回退分类

**文件：** `src/common/mysql_version_detector.cpp`

#### 1a. 9.0 检测路径缺失？（旧报告 P0.5）

**状态：✅ 已修复**

`decide_version()` L339-340 有明确的 9.0 返回路径：
```cpp
if (page_version_score >= 15) {
    return MySQLVersion::MYSQL_90;  // 9.0 (page_version >= 3)
}
```
提交 `1d0567f`。

#### 1b. 5.x 无条件返回 5.6？（旧报告 P0.6）

**状态：✅ 已修复**

`decide_version()` L346-360 中 5.x 分支在仅有 checksum 证据时返回 `UNKNOWN`，非旧报告的盲目返回 5.6：
```cpp
if (checksum_score >= 8) {
    if (fsp_score > 0) return MySQLVersion::MYSQL_57;
    if (undo_score >= 8) return MySQLVersion::MYSQL_57;
    return MySQLVersion::UNKNOWN;  // 不再盲目返回 5.6
}
```

#### 1c. 多页投票非确定性（旧报告 P1.8）

**状态：✅ 已修复**

`detect_from_multiple_pages()` 使用有序 `std::map<MySQLVersion, int>`（L64），并列时返回 UNKNOWN 而非任意选择。

#### 1d. page_version 偏移冲突（旧报告 MEDIUM）

**状态：⚠️ 部分缓解**

`page_defs.hpp:214` 定义 `FIL_PAGE_PAGE_VERSION_OFFSET = FIL_HEADER_SIZE` (38)。`check_page_version_field()` L234-236 添加了 **page-type guard**：仅在 `FIL_PAGE_INDEX` 页面读取 offset 38。非 INDEX 页面返回 0。

**残余风险：** 极低。在 INDEX 页面上 offset 38 在 5.6/5.7 中确实是 `PAGE_N_DIR_SLOTS`，但 `page_version > 0` 检测几乎总是 false（PAGE_N_DIR_SLOTS 通常为 0 或很小值）。

#### 1e. FLUSH_LSN 信号不可靠（旧报告 MEDIUM）

**状态：⚠️ 仍存在**

`check_lsn_extension()` 仍使用 offset 26 的 FIL_PAGE_FILE_FLUSH_LSN。该字段自 5.6.4 已存在，数据页上通常为 0。

**实际影响：** 低。8.0+ 检测主要由 `page_version` 字段驱动，LSN 仅为辅助信号。

#### 1f. 校验和启发式仅检查大小（旧报告 LOW）

**状态：⚠️ 仍存在**

`check_checksum_type()` L250-254 返回 0（无实际验证）。注释明确说明"checksum value alone does not identify an algorithm"。

---

### 发现 2：Roll Pointer 编码

**文件：** `include/dbrecovery/innodb/roll_ptr_decoder.hpp`

**状态：✅ 无问题**

- 位布局正确：掩码 `0x3FFFF`（18-bit offset）和 `0x3F`（6-bit flags）
- 6-byte BE（5.6/5.7）vs 7-byte BE（8.0+）存储格式正确处理
- 测试确认最大值的无损往返

---

### 发现 3：Deep Undo 6/7-byte 格式化

**文件：** `include/dbrecovery/innodb/deep_undo_analyzer.hpp`

#### 3a. UndoFormatProfile 默认 UNRESOLVED（严格安全）

**状态：✅ 正确**

`deep_undo_analyzer.hpp:333` 默认 `undo_format_profile_ = UndoFormatProfile::UNRESOLVED`，fail-closed。MySQL 版本通过 `engine.cpp:966-967` 从 CLI 传递。

#### 3b. read_prev_undo_ptr() 死代码（旧报告 MEDIUM）

**状态：❌ 声经过时 — 代码已删除**

**重要更正：** 旧报告声称 `deep_undo_analyzer.hpp:530-534` 存在版本感知 `read_prev_undo_ptr()` 函数。**实际代码中该函数已不存在**。全局搜索 `read_prev_undo_ptr` 在源码中零匹配，仅在旧报告文档中出现。

当前实现使用 `RollPtrDecoder::decode_storage()`（`roll_ptr_decoder.hpp`）和 `extract_prev_ptr_from_field()`（`deep_undo_analyzer.hpp:318`），后者是 `static` 函数，包含 6/7-byte 版本感知逻辑。

#### 3c. trace_version_chain() 集成测试

**状态：✅ 已实现（旧报告 P1.10 已修复）**

`deep_undo_analyzer_test.cpp:1194-1526` 包含完整的集成测试套件：
- `TraceVersionChainThreeVersionWalk`（L1196）：3 记录 UNDO 链遍历验证
- `TraceVersionChainRollPtrPointsToCorruptPage`（L1253）：损坏页中断链
- `TraceVersionChainCycleDetected`（L1278）：循环检测
- `TraceVersionChainConflictingHeaderAndFieldBreak`（L1389）：冲突头/字段中断
- `TraceVersionChainNullHeaderEndsDespiteNonzeroField`（L1416）：空头终结链
- `TraceVersionChainNullFieldContinuesViaHeader`（L1443）：空字段通过头继续
- `TraceVersionChainUnresolvedProfileRejects`（L1507）：UNRESOLVED fail-closed
- `TraceVersionChainInvalidContinuationBreaks`（L1470）：无效延续中断

---

### 发现 4：SDI 解压安全限制

**文件：** `include/dbrecovery/innodb/sdi_manager.hpp`, `src/innodb/sdi_manager.cpp`

#### 4a. inflateInit → inflateInit2(-15)（旧报告 P0.1 CRITICAL）

**状态：✅ 已修复**

`sdi_manager.hpp:90` 正确使用 `inflateInit2(&strm, -15)`（raw deflate，无 zlib 头）。所有压缩 SDI 路径委托给 `SDIManager::extract_zip_record()` 或 `SDIManager::extract_standard_payload()`。

#### 4b. extract_sdi_payload() 无安全限制（旧报告 P0.2 CRITICAL）

**状态：✅ 已修复**

完整安全限制（`sdi_manager.hpp:66-78`）：
- `SDI_MAX_UNCOMPRESSED_SIZE = 16MB`（L43）
- `SDI_MAX_COMPRESSION_RATIO = 20`（L44）
- 无效 header 拒绝（L66-70）
- 压缩长度超 payload 拒绝（L79-82）
- 解压后长度/完整性验证（L100-107）

#### 4c. SDI 列排序与解析

**状态：✅ 已验证**

`sdi_manager.cpp:parse_sdi_record()` 处理 "dictionary"（MySQL 8.0+）、"schema"（legacy）、"table" 三种 JSON 格式。列按 `physical_pos` 排序。支持 charset_id、collation_id、datetime_precision、ENUM/SET base64 解码。

---

### 发现 5：压缩表 zip_size 位移

**文件：** `include/dbrecovery/innodb/compressed_table_recovery.hpp`, `src/innodb/compressed_table_recovery.cpp`

#### 5a. zip_ssize 验证（旧报告 P1.1 HIGH）

**状态：✅ 已修复**

`page_defs.hpp:99-100` 的 `parse_fsp_compression_metadata()` 明确验证 `zip_ssize >= 5` → 返回 nullopt。物理页大小 < 1024 或 > 逻辑页大小也拒绝。

#### 5b. 压缩比检查（旧报告 P1.2 HIGH）

**状态：✅ 已修复**

`compressed_table_recovery.cpp:173` 有 `MAX_COMPRESSION_RATIO = 256` 检查：
```cpp
if (payload_size > 0 && expected_size / payload_size > MAX_COMPRESSION_RATIO) {
    return {};
}
```

#### 5c. space_id 偏移量（旧报告 P1.3 HIGH）

**状态：✅ 已修复**

`compressed_table_recovery.cpp:85` 使用 `FIL_PAGE_SPACE_ID` (34)：
```cpp
page.space_id = read_be<uint32_t>(p, FIL_PAGE_SPACE_ID);
```

#### 5d. ZSD 头部验证

**状态：✅ 完整实现**

`is_page_compressed()` L110-147 验证：
- zip_size 必须为 8/16/32（对应 4K/8K/16K）
- version 必须为 1
- zlen 在合理范围内
- orig_size 必须是标准页大小（4096/8192/16384/32768/65536）

---

### 发现 6：加密表 space_id=0 + XOR 占位符

**文件：** `include/dbrecovery/innodb/tde_decryptor.hpp`, `include/dbrecovery/innodb/offline_keyring_parser.hpp`, `src/innodb/encrypted_table_recovery.cpp`

#### 6a. is_page_encrypted() 偏移量（旧报告 P0.3 CRITICAL）

**状态：✅ 已修复**

`encrypted_table_recovery.cpp:142-144` 委托给 `TDEDecryptor::is_encrypted()`，后者在 `tde_decryptor.hpp:264` 读取 `data[38]`（正确偏移量）。此外有 page_type 验证（仅 INDEX/SDI 类型）和全零检测。

#### 6b. KeyringFile::load() XOR 占位符（旧报告 MEDIUM）

**状态：⚠️ 仍存在，已缓解**

`tde_decryptor.hpp:358-360` 仍使用 XOR 占位符：
```cpp
decrypted_key[i] = encrypted_key[i] ^ xor_key[i % xor_key_len];
```

**缓解措施：** `EncryptedTableRecovery::load_keyring()` 使用 `OfflineKeyringParser`（真实 AES-256-CBC 路径，`offline_keyring_parser.cpp`），而非 `KeyringFile`。XOR 路径仅当调用者直接使用 `KeyringFile::load()` 时触发。`OfflineKeyringParser` 支持 keyring_file（JSON）和 keyring_encrypted_file（AES-256-CBC）两种后端。

**残余风险：** 低。主恢复路径使用 OfflineKeyringParser。但 `KeyringFile` 类仍导出 XOR 路径，建议在后续阶段移除或标记为 `@deprecated`。

#### 6c. TDE 算法检测范围

**状态：✅ 完整**

`TDEDecryptor::is_encrypted()` 支持算法值 1-9（AES-128/192/256 × ECB/CBC/CTR）。

---

### 发现 7：Salvage L3/L4 阈值双重过滤

**文件：** `src/salvage/salvage_engine.cpp`, `include/dbrecovery/tools/recovery_cli.hpp`

#### 7a. 置信度阈值双重过滤（旧报告 LOW）

**状态：⚠️ 仍存在**

双阈值确实存在：`salvage_engine.hpp` 默认 `min_confidence=0.3`，CLI 覆盖为 0.15。L3（0.4 阈值）和 L4（0.3 阈值）都通过 CLI 的 0.15 阈值。

**安全前提：** `export_csv()`（`salvage_engine.cpp:580-635`）**fail-closed** — 任何非 STRUCTURED 行被拒绝写入 CSV/SQL。L3/L4 仅通过 `evidence_fragments` 报告序列化。

`--evidence-fragments` 控制单独 forensic evidence 报告的输出；L3/L4 永远不会写入 CSV/SQL。`--auto-salvage` 当前实际执行 SalvageEngine、PageIdentityEngine 和 DataCorrelationEngine；其结果仍需按证据等级理解，不能视为已验证的可导入恢复。

#### 7b. SecondaryIndexRescuer REDUNDANT 支持（旧报告 MEDIUM）

**状态：❌ 声经过时 — 已实现 REDUNDANT 路径**

**重要更正：** 旧报告声称 `SecondaryIndexRescuer::parse_secondary_leaf_records()` 仅支持 COMPACT 格式。**实际代码已包含完整 REDUNDANT 实现**：

- `secondary_index_rescuer.hpp:279-282`：检测非 COMPACT 格式并委托：
  ```cpp
  if (!is_compact_format(page_data)) {
      return parse_secondary_leaf_records_redundant(page_data, page_size);
  }
  ```
- `parse_secondary_leaf_records_redundant()`（L429-552）：完整 REDUNDANT 实现，包含：
  - 6 字节记录头（`RedundantHeader`，定义于 `page_defs.hpp:482`）
  - 1 或 2 字节偏移数组
  - 相邻偏移量相等判断 NULL
  - INFIMUM 在偏移量 100（vs COMPACT 的 54）
  - 无变长列表、无 NULL 位图

---

## 第三部分：未验证声明与文件级差异

### 3.1 旧报告声明过时

| 旧报告声明 | 位置 | 实际状态 | 影响 |
|-----------|------|---------|------|
| `read_prev_undo_ptr()` 死代码在 `deep_undo_analyzer.hpp:530-534` | 2.4 节 | 函数已删除，改用 `RollPtrDecoder` | 报告需更新，移除该条目 |
| SecondaryIndexRescuer 仅 COMPACT | 6.3 节 | REDUNDANT 已实现 | 报告需更新，移除该条目 |
| DDL 列数 ROW_ID 问题"需检查" | 7.1 节 | ROW_ID 检测已实现（L189-204） | 报告需更新，标记为已修复 |

### 3.2 需进一步验证的声明

| 声明 | 文件 | 优先级 | 验证方式 |
|------|------|-------|---------|
| `p0_regression_results` 构建路径 | 汇总表 P0.4 | 已验证 | 路径正确 |
| `allowlist` 路径检测 | 汇总表 P1.7 | 已验证 | 路径正确 |
| FlashbackGenerator/DeepUndoAnalyzer 代码重复 | 汇总表 P1.9 | 已验证 | 死代码已删除 |
| RedoLogReplayer CLI 集成 | 汇总表 P1.6 | 已验证 | CLI 代码存在（L1451-1488） |

---

## 第四部分：风险等级汇总

### P0 项目（边界修复已完成，仍需真实 fixture 认证）

| 编号 | 描述 | 文件 | 提交 | 验证 |
|------|------|------|------|------|
| P0.1 | SDI inflateInit → inflateInit2(-15) | `sdi_manager.hpp:90` | `833d1fc` | `inflateInit2(&strm, -15)` 确认 |
| P0.2 | extract_sdi_payload 无安全限制 | `sdi_manager.hpp:66-78` | `833d1fc` | 16MB/20:1 限制确认 |
| P0.3 | is_page_encrypted 偏移 0→38 | `tde_decryptor.hpp:264` | `1d0567f` | `data[38]` 读取确认 |
| P0.4 | p0_regression_results 构建路径 | `CMakeLists.txt:275` | `833d1fc` | 路径正确 |
| P0.5 | 添加 9.0 检测路径 | `mysql_version_detector.cpp:339` | `1d0567f` | `page_version >= 3` 确认 |
| P0.6 | 修复 5.7 非 FSP 检测 | `mysql_version_detector.cpp:346-360` | `1d0567f` | 返回 UNKNOWN 非 5.6 |
| P0.7 | RecoveryEngine → DeepUndoAnalyzer | `engine.cpp:946-986` | `8b47d2a` | 使用 FlashbackGenerator |

### 剩余问题

| 严重性 | 数量 | 描述 | 文件引用 |
|--------|------|------|---------|
| MEDIUM | 1 | PartitionTableRecovery 未集成 SDI 自动检测 | `partition_table_recovery.hpp:283` |
| MEDIUM | 1 | RedoLogReplayer schema 参数死代码 | `redo_log_replayer.cpp:39` |
| LOW | 1 | 共享表空间缓存缺失 | `shared_tablespace_recovery.hpp` |
| LOW | 1 | KeyringFile XOR 占位符（主路径已用 OfflineKeyringParser） | `tde_decryptor.hpp:358-360` |
| LOW | 1 | 校验和启发式仅检查大小 | `mysql_version_detector.cpp:250-254` |
| NOT VERIFIED | 1 | compressed/TDE/native keyring 缺少真实 MySQL fixture 验证，生产路径保持 unsupported 或 candidate-only | `unified_recovery.cpp`, `encrypted_table_recovery.cpp`, `offline_keyring_parser.cpp` |
| NOT VERIFIED | 1 | 分布式恢复未实现 | 能力矩阵外 |

### 已过时需移除的条目

| 旧报告条目 | 原严重性 | 移除原因 |
|-----------|---------|---------|
| read_prev_undo_ptr 死代码 | MEDIUM | 函数已删除，改用 RollPtrDecoder |
| SecondaryIndexRescuer 仅 COMPACT | MEDIUM | REDUNDANT 路径已实现 |
| DDL ROW_ID 检测"需检查" | MEDIUM | ROW_ID 检测已实现 |

---

## 第五部分：建议

### 生产前必修复（P0 — 已完成并经隔离回归验证）

已完成的 P0 边界修复均保持 fail-closed；这不等同于 compressed/TDE/keyring 获得真实 MySQL fixture 认证。

### 建议优先修复（P1）

| 优先级 | 问题 | 原因 |
|-------|------|------|
| P1.1 | PartitionTableRecovery SDI 集成 | 功能缺口，影响分区表恢复 |
| P1.2 | RedoLogReplayer schema 死参数 | 维护负担，API 误导 |

### 建议后续修复（P2）

| 优先级 | 问题 | 原因 |
|-------|------|------|
| P2.1 | 移除 KeyringFile XOR 路径 | 安全隐患，防止误用 |
| P2.2 | 共享表空间缓存 | 大型表空间性能优化 |

### 已过时建议移除

| 条目 | 原因 |
|------|------|
| 删除 `read_prev_undo_ptr()` 死代码 | 函数已不存在 |
| 为 SecondaryIndexRescuer 添加 REDUNDANT 支持 | 已实现 |
| 确认 DDL 列数 ROW_ID 处理 | 已实现 |

---

## 附：代码库文件引用索引

| 文件 | 行数 | 角色 |
|------|------|------|
| `include/dbrecovery/innodb/roll_ptr_decoder.hpp` | ~120 | Roll Pointer 编码/解码（内联） |
| `include/dbrecovery/innodb/page_defs.hpp` | ~950 | FIL 页偏移量、RedundantHeader、压缩元数据 |
| `include/dbrecovery/innodb/tde_decryptor.hpp` | 406 | TDE 加密检测、解密、KeyringFile |
| `include/dbrecovery/innodb/offline_keyring_parser.hpp` | 109 | 离线 keyring 解析器接口 |
| `include/dbrecovery/innodb/sdi_manager.hpp` | 120 | SDI 安全解压管理器 |
| `include/dbrecovery/innodb/compressed_table_recovery.hpp` | 117 | 压缩表恢复接口 |
| `include/dbrecovery/innodb/encrypted_table_recovery.hpp` | 132 | 加密表恢复接口 |
| `include/dbrecovery/innodb/deep_undo_analyzer.hpp` | ~600 | Undo 日志分析、版本链追踪 |
| `include/dbrecovery/innodb/secondary_index_rescuer.hpp` | 604 | 二级索引恢复（COMPACT + REDUNDANT） |
| `include/dbrecovery/innodb/ddl_change_detector.hpp` | 244 | DDL 变更检测、ROW_ID 感知列计数 |
| `include/dbrecovery/innodb/partition_table_recovery.hpp` | ~300 | 分区表恢复 |
| `include/dbrecovery/tools/recovery_cli.hpp` | ~1500 | CLI 参数注册与调度 |
| `src/innodb/compressed_table_recovery.cpp` | 353 | 压缩表恢复实现 |
| `src/innodb/encrypted_table_recovery.cpp` | 357 | 加密表恢复实现 |
| `src/innodb/offline_keyring_parser.cpp` | ~80 | 真实 AES-256-CBC keyring 解析 |
| `src/innodb/sdi_manager.cpp` | 360 | SDI 记录解析、JSON 列映射 |
| `src/innodb/redo_log_replayer.cpp` | ~350 | Redo 日志重放实现 |
| `src/salvage/salvage_engine.cpp` | 638 | 4 级 salvage 引擎 |
| `src/engine/engine.cpp` | ~1000 | 恢复引擎主逻辑 |
| `src/common/mysql_version_detector.cpp` | ~400 | MySQL 版本检测器 |
| `CMakeLists.txt` | 348 | 主构建配置（dbrecovery_innodb 静态库） |
| `tests/CMakeLists.txt` | 310 | 60+ 测试目标配置 |
| `tests/innodb/deep_undo_analyzer_test.cpp` | ~1536 | Undo 链集成测试 |
| `tests/p0_regression_test.cpp` | ~1200 | P0 回归测试 |

---

*报告结束。本阶段包含源码、测试和文档变更；构建与验证均在隔离目录执行。*