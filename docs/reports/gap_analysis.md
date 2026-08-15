# MySQL Recovery Pro v2.0 差距分析报告

> 生成日期: 2026-08-08
> 基于: architecture_v2_blueprint.md 生产级恢复强化计划
> 范围: Phase 0 全面差距分析 — 7 大核心模块

---

## 目录

1. [MySQL 版本检测器](#1-mysql-版本检测器)
2. [Roll Pointer 解码与 Undo 分析](#2-roll-pointer-解码与-undo-分析)
3. [SDI 提取与压缩/TDE 安全](#3-sdi-提取与压缩tde-安全)
4. [Salvage Engine 与 Secondary Index](#4-salvage-engine-与-secondary-index)
5. [DDL 变更检测与 Partition/Shared Tablespace](#5-ddl-变更检测与-partitionshared-tablespace)
6. [Redo Log Replayer](#6-redo-log-replayer)
7. [CLI 集成与 Test Workspace](#7-cli-集成与-test-workspace)
8. [汇总与优先级排序](#8-汇总与优先级排序)

---

## 1. MySQL 版本检测器

### 文件: `src/common/mysql_version_detector.cpp`

### 1.1 5.6 过度分类 (Severity: HIGH)

**问题:** `decide_version()` (L326-351) 中，所有非 FSP 页面的 5.x 分支都返回 `MYSQL_56`。5.7 检测仅能在 page 0 (FSP_HDR) 上通过 `POST_ANTELOPE`/`ATOMIC_BLOBS` 位触发，这意味着真实的 5.7 非 FSP 页面全部被误分类为 5.6。

```cpp
// L337-344: 5.x 分支无条件偏向 5.6
if (lsn_score <= 0 && sdi_score <= 0) {
    if (fsp_score > 0) return MYSQL_57; // 仅 page 0 可达
    return MYSQL_56; // 所有非 FSP 5.x 页面落在此处
}
```

**影响:** 5.7 数据库的恢复精度下降，因为 5.7 特有的页面格式特征未被利用。

### 1.2 无 9.0 路径 (Severity: HIGH)

**问题:** `decide_version()` 和 `match_version()` 中没有任何代码路径能返回 `MYSQL_90`。`version_features_` 表 (L15-30) 声明了 9.0 条目，但实际的决策逻辑从未使用该表——所有评分都是硬编码的。

### 1.3 8.0+ 仅在 SDI 页面上可检测 (Severity: HIGH)

**问题:** `check_sdi_markers()` (L193-213) 仅在 `FIL_PAGE_TYPE_SDI` (0x45BD) 页面上返回正分数。普通索引页 (FIL_PAGE_INDEX) 的 sdi_score 为 -10。这意味着 8.0+ 检测仅能在 SDI 页面 (page 3/4) 上触发。

**影响:** 单页检测中，非 SDI 页面的 8.0+ 数据被报告为 5.6，除非页面恰好有非零的 `FLUSH_LSN`。

### 1.4 page_version 偏移冲突 (Severity: MEDIUM)

**问题:** `page_version` 在偏移量 38 处读取 (L222)，与 MySQL 5.6/5.7 的 `PAGE_N_DIR_SLOTS` 冲突 (`page_defs.hpp:300`)。代码注释 (L216-219) 承认了这一点，但未修复。

### 1.5 FLUSH_LSN 信号不可靠 (Severity: MEDIUM)

**问题:** `check_lsn_extension()` (L170-191) 将偏移量 26 处的 `FIL_PAGE_FILE_FLUSH_LSN` 非零值视为 "8.0+" 信号。该字段自 MySQL 5.6.4 就已存在，数据页上通常为 0，因此区分能力很弱。

### 1.6 多页投票非确定性 (Severity: MEDIUM)

**问题:** `detect_from_multiple_pages()` (L52-81) 使用 `std::unordered_map` 的最大值计数，并列时由迭代顺序决定，这是非确定性的。同一文件在不同运行中可能产生不同的版本。

### 1.7 校验和启发式仅检查大小 (Severity: LOW)

**问题:** `check_checksum_type()` (L233-250) 仅检查偏移量 0 处的值是否 > 0x1000000，未进行实际的 CRC32 验证。损坏的页面被静默分类。

### 1.8 版本枚举与检测器不匹配 (Severity: LOW)

**问题:** `version_compat.hpp:22-35` 枚举了 5.0/5.1/5.5/8.1/8.2/8.3，但检测器永远不会产生这些值。`page_defs.hpp:54-77` 与 `version_compat.hpp:48-60` 中有两套冲突的页面类型常量编码。

### 1.9 修复建议

| 优先级 | 修复 | 文件:行 |
|--------|------|---------|
| P0 | 在 `decide_version()` 中添加 9.0 检测路径 | `mysql_version_detector.cpp:326-351` |
| P0 | 修复 5.7 检测，使其在非 FSP 页面上也能工作 | `mysql_version_detector.cpp:337-344` |
| P1 | 将 `page_version` 读取从偏移量 38 移开或添加版本感知 | `mysql_version_detector.cpp:222` |
| P1 | 修复多页投票的非确定性 | `mysql_version_detector.cpp:52-81` |
| P2 | 添加 CRC32 校验和验证 | `mysql_version_detector.cpp:233-250` |

---

## 2. Roll Pointer 解码与 Undo 分析

### 文件: `roll_ptr_decoder.hpp`, `deep_undo_analyzer.cpp/hpp`, `flashback_generator.cpp`

### 2.1 18-bit 偏移编码 — 无截断风险 (Severity: NONE, 已验证)

**结论:** `roll_ptr_decoder.hpp:64-71` 的位布局正确。`encode()` (L97) 和 `decode()` (L81) 中的掩码 `0x3FFFF` 和 `0x3F` 正确隔离了 18-bit 偏移和 6-bit 标志位。测试 (L247-277) 确认最大值的无损往返。

### 2.2 read_prev_undo_ptr() 是死代码 (Severity: MEDIUM)

**问题:** `deep_undo_analyzer.hpp:530-534` 的版本感知 `read_prev_undo_ptr()` 仅在 P0 回归测试中被调用，从未被运行时链遍历路径 `trace_version_chain()` 使用。后者使用 `extract_prev_ptr_from_field()` -> `decode_field_data_to_internal()`，始终读取 6 字节 LE。

### 2.3 两套并行的 Undo 链遍历系统 (Severity: HIGH)

**问题:** 存在两套独立的 undo 链遍历机制：
- **旧:** `UndoLogParser::traverse_undo_chain()` (从 `engine.cpp:967` 调用) — 返回 `UndoLogEntry` 对象
- **新:** `DeepUndoAnalyzer::trace_version_chain()` (从 `flashback_generator.cpp:723` 调用) — 返回 `UndoRecordV2` 对象

二者未集成。`RecoveryEngine` 不使用 `DeepUndoAnalyzer`。

### 2.4 RecoveryEngine 使用旧 UndoLogParser (Severity: HIGH)

**问题:** `RecoveryEngine::flashback_deletes()` (engine.cpp:945-957) 调用旧的 `undo_parser_.flashback_delete()`，而非新的 `DeepUndoAnalyzer` 或 `FlashbackGenerator`。新闪回系统完全与主恢复管线断开。

### 2.5 FlashbackGenerator 与 DeepUndoAnalyzer 代码重复 (Severity: MEDIUM)

**问题:** `FlashbackGenerator::flashback_deletes()` (flashback_generator.cpp:440-533) 独立实现扫描和过滤逻辑，与 `DeepUndoAnalyzer::flashback_deletes()` 重复。

### 2.6 缺少集成测试 (Severity: HIGH)

**问题:** `trace_version_chain()` 的测试仅使用空/错误路径（不存在的 ibdata 文件）。没有测试使用真实的多版本 undo 链（INSERT -> UPDATE -> DELETE）。

### 2.7 修复建议

| 优先级 | 修复 | 文件:行 |
|--------|------|---------|
| P0 | 将 `RecoveryEngine::flashback_deletes()` 切换到 `DeepUndoAnalyzer` | `engine.cpp:945-957` |
| P1 | 消除 `FlashbackGenerator` 与 `DeepUndoAnalyzer` 之间的代码重复 | `flashback_generator.cpp:440-533` |
| P1 | 添加 `trace_version_chain()` 的集成测试（合成 undo 链） | `tests/innodb/deep_undo_analyzer_test.cpp` |
| P2 | 删除或链接 `read_prev_undo_ptr()` 死代码 | `deep_undo_analyzer.hpp:530-534` |

---

## 3. SDI 提取与压缩/TDE 安全

### 文件: `sdi_extractor.hpp`, `sdi_manager.hpp`, `compressed_table_recovery.cpp`, `encrypted_table_recovery.cpp`, `tde_decryptor.hpp`, `offline_keyring_parser.cpp`

### 3.1 Bug #1: 压缩 SDI 页面使用错误 deflate 格式 (Severity: CRITICAL)

**问题:** `sdi_extractor.hpp` 中两个压缩 SDI 页面路径使用带 zlib 头的 `inflateInit`，而非原始 deflate：
- `extract_sdi_payload()` (L172): `inflateInit(&strm)` — 应为 `inflateInit2(&strm, -15)`
- `extract_from_page()` (L351): `inflateInit(&strm)` — 同样错误

对比: `sdi_manager.hpp:67` 正确使用 `inflateInit2(&strm, -15)`。

**影响:** 所有压缩 SDI ZIP 页面 (FIL_PAGE_SDI_ZIP, 0x45BD) 的提取均失败或产生损坏数据。

### 3.2 Bug #2: extract_sdi_payload() 缺少安全限制 (Severity: CRITICAL)

**问题:** `sdi_extractor.hpp:135-226` 完全没有最大大小或压缩比保护。恶意页面可触发 OOM。

### 3.3 Bug #3: 压缩表缺少 zip_ssize 验证 (Severity: HIGH)

**问题:** `compressed_table_recovery.cpp` 的 `auto_detect_compression()` (L270) 和 `parse_compression_info()` (L318) 未验证 `zip_ssize` 范围 (0-15)。当 `zip_ssize >= 5` 时，`1 << zip_ssize` 产生无效页面大小（< 1024 字节或 0），导致后续除法错误或未定义行为。

### 3.4 Bug #4: decompress_page_data() 缺少压缩比检查 (Severity: HIGH)

**问题:** `compressed_table_recovery.cpp:131-187` 的 `uncompress()` 和 `LZ4Helper::decompress()` 调用没有压缩比上限。恶意压缩页面可导致 OOM。

### 3.5 Bug #5: space_id 偏移量不一致 (Severity: HIGH)

**问题:** `compressed_table_recovery.cpp:81` 在偏移量 24 读取 space_id（实际是 `FIL_PAGE_TYPE`），而 `encrypted_table_recovery.cpp:105` 在偏移量 34 读取（正确，`FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID`）。

### 3.6 Bug #6: is_page_encrypted() 读取错误偏移量 (Severity: CRITICAL)

**问题:** `EncryptedTableRecovery::is_page_encrypted()` (encrypted_table_recovery.cpp:127-133) 在 `page_data[0]` 读取 TDE 算法字节——但偏移量 0 是 FIL 校验和。正确偏移量是 38（FIL 头部之后）。`TDEDecryptor::is_encrypted()` (tde_decryptor.hpp:263) 正确使用偏移量 38。

**影响:** 页面加密检测完全失效——随机字节偶尔匹配 0x01-0x09 产生误报，或漏报。

### 3.7 Bug #7: KeyringFile::load() 使用 XOR 占位符 (Severity: MEDIUM)

**问题:** `tde_decryptor.hpp:289-389` 的 `KeyringFile::load()` 使用 XOR 占位符解密（L358-360: `decrypted_key[i] = encrypted_key[i] ^ xor_key[i % xor_key_len]`）。而 `OfflineKeyringParser` (offline_keyring_parser.cpp) 使用真实的 AES-256-CBC。但 `EncryptedTableRecovery::load_keyring()` 使用 XOR 路径，而非真实 AES 路径。

### 3.8 TDE 布局注释不匹配 (Severity: LOW)

**问题:** `tde_decryptor.hpp:262` 的注释描述与实际解析代码 (L205-208) 不匹配。注释过时。

### 3.9 修复建议

| 优先级 | 修复 | 文件:行 |
|--------|------|---------|
| P0 | 将 `inflateInit` 改为 `inflateInit2(&strm, -15)` | `sdi_extractor.hpp:172,351` |
| P0 | 添加最大大小和压缩比限制到 `extract_sdi_payload()` | `sdi_extractor.hpp:135-226` |
| P0 | 修复 `is_page_encrypted()` 偏移量 0→38 | `encrypted_table_recovery.cpp:131` |
| P1 | 添加 `zip_ssize` 范围验证 (>=5 拒绝) | `compressed_table_recovery.cpp:270,318` |
| P1 | 添加 `decompress_page_data()` 压缩比检查 | `compressed_table_recovery.cpp:131-187` |
| P1 | 将 `EncryptedTableRecovery` 切换到 `OfflineKeyringParser` | `encrypted_table_recovery.cpp:41-54` |
| P1 | 修复 space_id 偏移量 24→34 | `compressed_table_recovery.cpp:81` |

---

## 4. Salvage Engine 与 Secondary Index

### 文件: `salvage_engine.hpp/cpp`, `secondary_index_rescuer.hpp/cpp`

### 4.1 Salvage Engine L1-L4 级别 (Severity: INFO, 完整实现)

**现状:** L1/L2/L3/L4 级别正确实现，覆盖从 FIL 头部有效到仅可打印字符串提取的所有场景。

### 4.2 置信度阈值双重过滤 (Severity: LOW)

**问题:** `salvage_engine.hpp` 默认 `min_confidence=0.3`，CLI 覆盖为 `0.15`。L3 (0.4) 和 L4 (0.3) 行都通过 CLI 的 0.15 阈值。实际的 L3/L4 过滤由 CLI 导出层的 `salvage_level` 检查完成，而非置信度。引擎 `salvage_page()` 的过滤和 CLI 的导出过滤冗余且可能不一致。

### 4.3 Secondary Index Rescuer 仅支持 COMPACT 格式 (Severity: MEDIUM)

**问题:** `innodb::SecondaryIndexRescuer::parse_secondary_leaf_records()` (secondary_index_rescuer.hpp:274) 假设 COMPACT 格式，不支持 REDUNDANT 格式（无变长列表，不同的 NULL 位图）。

### 4.4 修复建议

| 优先级 | 修复 | 文件:行 |
|--------|------|---------|
| P2 | 统一 salvage 引擎和 CLI 的置信度过滤逻辑 | `salvage_engine.cpp`, `recovery_cli.hpp` |
| P2 | 为 SecondaryIndexRescuer 添加 REDUNDANT 格式支持 | `secondary_index_rescuer.hpp:274` |

---

## 5. DDL 变更检测与 Partition/Shared Tablespace

### 文件: `ddl_change_detector.hpp/cpp`, `partition_table_recovery.hpp`, `shared_tablespace_recovery.hpp/cpp`

### 5.1 DDL Change Detector 列数偏差 (Severity: MEDIUM)

**问题:** `ddl_change_detector.hpp:147-208` 的 `detect_column_count_from_page()` 使用 `entry_count - 2` (L190) 来排除 TRX_ID 和 ROLL_PTR。但注释 (L145) 说 `- 3`（包括 ROW_ID）。对于无显式 PK 的表，ROW_ID 存在但未排除，导致列数多 1。

### 5.2 Partition Table Recovery 缺少 SDI 检测 (Severity: MEDIUM)

**问题:** `partition_table_recovery.hpp:255` 的 `recover_partition()` 使用 `BPlusTreeParser` 但需要 `--schema-file` 参数。未集成 SDI 页面自动检测。

### 5.3 Shared Tablespace Recovery 缺少缓存 (Severity: LOW)

**问题:** `shared_tablespace_recovery.cpp:58-122` 的 `extract_table()` 按精确 page_no 顺序复制页面。对于大型共享表空间 (>10GB)，每次访问都进行全扫描。

### 5.4 修复建议

| 优先级 | 修复 | 文件:行 |
|--------|------|---------|
| P1 | 修复 DDL 列数计算：检查 ROW_ID 存在性 | `ddl_change_detector.hpp:190` |
| P2 | 为 PartitionTableRecovery 添加 SDI 自动检测 | `partition_table_recovery.hpp:255` |
| P2 | 为 SharedTablespaceRecovery 添加缓存 | `shared_tablespace_recovery.cpp:58-122` |

---

## 6. Redo Log Replayer

### 文件: `redo_log_replayer.hpp/cpp`, `redo_log_enhanced.cpp`

### 6.1 无 CLI 暴露 (Severity: HIGH)

**问题:** `recovery_cli.hpp` 中搜索 `RedoLogReplayer` 得到零匹配。该组件仅在测试文件和性能基准中使用。

### 6.2 schema 参数为死代码 (Severity: MEDIUM)

**问题:** `redo_log_replayer.hpp:64` 的 `replay()` 方法接受 `catalog::TableSchema& schema` 参数，但实现 (redo_log_replayer.cpp:39) 完全忽略该参数。记录以原始字节变异方式应用于页面缓冲区，未通过 `BPlusTreeParser` 解析。

### 6.3 LSN 伪造不正确 (Severity: MEDIUM)

**问题:** `RedoLogFilter::passes_filter()` (redo_log_replayer.cpp:338) 使用 `(r_space_id << 32) | r_page_no` 伪造 LSN，这在语义上不正确。

### 6.4 链构建使用 space_id 替代事务 ID (Severity: MEDIUM)

**问题:** `RedoLogChainBuilder::build_chains()` (redo_log_replayer.cpp:247) 使用 `r_space_id` 作为分组代理，而非真实的事务 ID。注释 (L252) 承认这是"简化的链构建"。

### 6.5 修复建议

| 优先级 | 修复 | 文件:行 |
|--------|------|---------|
| P1 | 将 RedoLogReplayer 添加到 CLI | `recovery_cli.hpp` |
| P2 | 修复或删除 schema 死参数 | `redo_log_replayer.hpp:64` |
| P2 | 修复 LSN 伪造逻辑 | `redo_log_replayer.cpp:338` |

---

## 7. CLI 集成与 Test Workspace

### 7.1 CLI 集成状态

| 组件 | CLI 已接线? | 状态 |
|------|------------|------|
| Salvage Engine | 是 | `--salvage-mode`, `--auto-salvage` |
| Undo History | 是 | `--undo-history` |
| Flashback | 是 | `--flashback-delete`, `--flashback-update` |
| Partition Recovery | 是 | `--partition-recovery`, `--list-partitions` |
| Shared Tablespace | 是 | `--extract-table`, `--list-tables` |
| DDL Detection | 是 | `--detect-ddl-changes` |
| Secondary Rescue | 是 | `--secondary-rescue` |
| Redo Log Replayer | **否** | 无 CLI 选项 |

### 7.2 Test Workspace 隔离问题 (Severity: HIGH)

**问题:** `p0_regression_results.log` 为空，因为指向了错误的构建目录。`/app/mysql_recovery_test/scripts/` 中的脚本引用了 `/app/mysql_recovery_pro/build_asan/` 而非 `/app/mysql_recovery_test/build/asan/`。

### 7.3 Device Safety Gate 路径问题 (Severity: MEDIUM)

**问题:** `device_safety_gate.sh` 报告 `allowlist_exists: False`，因为报告代码检查的路径与 `manifest/device_allowlist.json` 的实际位置不匹配。

### 7.4 修复建议

| 优先级 | 修复 | 文件:行 |
|--------|------|---------|
| P0 | 修复 p0_regression_results.log 的构建路径 | `scripts/run_p0_regression.sh` |
| P1 | 修复 allowlist 路径检测 | `device_safety_gate.sh` |
| P1 | 将 RedoLogReplayer 加入 CLI | `recovery_cli.hpp` |

---

## 8. 汇总与优先级排序

### 按严重性汇总

| 严重性 | 数量 | 关键问题 |
|--------|------|---------|
| CRITICAL | 3 | SDI 压缩 deflate 格式错误、extract_sdi_payload 无安全限制、is_page_encrypted 偏移量错误 |
| HIGH | 9 | 5.6 过度分类、无 9.0 路径、8.0 仅 SDI 可检测、两套并行 Undo 系统、zip_ssize 未验证、decompress 无压缩比检查、space_id 偏移不一致、Redo 无 CLI、test workspace 路径错误 |
| MEDIUM | 8 | page_version 偏移冲突、FLUSH_LSN 不可靠、多页投票非确定性、read_prev_undo_ptr 死代码、代码重复、DDL 列数偏差、Secondary 仅 COMPACT、XOR 占位符、缺少集成测试 |
| LOW | 4 | 校验和启发式、版本枚举不匹配、TDE 注释过时、双重过滤冗余 |

### 修复优先级排序

| 优先级 | 问题 | 工作量估计 |
|--------|------|-----------|
| **P0 (立即修复, 7项)** | | |
| P0.1 | SDI 压缩路径 inflateInit → inflateInit2(-15) | 2 行 |
| P0.2 | extract_sdi_payload() 添加 max_size / compression_ratio 限制 | 10 行 |
| P0.3 | is_page_encrypted() 偏移量 0 → 38 | 1 行 |
| P0.4 | 修复 p0_regression_results.log 构建路径 | 1 行 |
| P0.5 | 添加 9.0 检测路径到 decide_version() | 10 行 |
| P0.6 | 修复 5.7 非 FSP 页面检测 | 15 行 |
| P0.7 | 将 RecoveryEngine flashback 切换到 DeepUndoAnalyzer | 50 行 |
| **P1 (高优先级, 10项)** | | |
| P1.1 | zip_ssize 范围验证 (>=5 拒绝) | 3 行 |
| P1.2 | decompress_page_data() 添加压缩比检查 | 10 行 |
| P1.3 | 修复 space_id 偏移量 24 → 34 | 1 行 |
| P1.4 | 将 EncryptedTableRecovery 切换到 OfflineKeyringParser | 30 行 |
| P1.5 | 修复 DDL 列数计算 (ROW_ID 感知) | 10 行 |
| P1.6 | 将 RedoLogReplayer 加入 CLI | 50 行 |
| P1.7 | 修复 allowlist 路径检测 | 5 行 |
| P1.8 | 修复多页投票非确定性 | 10 行 |
| P1.9 | 消除 FlashbackGenerator/DeepUndoAnalyzer 代码重复 | 100 行 |
| P1.10 | 添加 trace_version_chain 集成测试 | 200 行 |
| **P2 (中优先级, 6项)** | | |
| P2.1 | 统一 salvage 置信度过滤逻辑 | 20 行 |
| P2.2 | SecondaryIndexRescuer 添加 REDUNDANT 格式支持 | 80 行 |
| P2.3 | PartitionTableRecovery 添加 SDI 自动检测 | 60 行 |
| P2.4 | SharedTablespaceRecovery 添加缓存 | 40 行 |
| P2.5 | 修复 RedoLogReplayer LSN 伪造 | 15 行 |
| P2.6 | 删除或链接 read_prev_undo_ptr 死代码 | 5 行 |

### 总工作量估计

- P0: 约 89 行代码变更
- P1: 约 419 行代码变更  
- P2: 约 220 行代码变更
- **总计: 约 728 行代码变更**

---

*报告结束。下一阶段: Phase 1-6 逐项修复。*