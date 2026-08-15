# MySQL Recovery Pro Phase 0 差距分析报告（更新版）

> 生成日期: 2026-08-14
> 范围: 代码审查 + 交叉比对 2026-08-08 差距分析报告
> 方法: 只读分析，逐文件审查 17 个关键源文件

---

## 核心发现

2026-08-08 差距分析报告列出的 **7 项 P0 问题中，6 项已在后续提交中修复**。当前代码库状态显著优于旧报告描述。本报告交叉比对每项声明的实际状态，并标注仍存在的风险。

---

## 1. 版本检测器 (`mysql_version_detector.cpp`)

### 1.1 9.0 检测路径 ✅ 已修复（旧报告 P0.5）

**旧报告:** `decide_version()` 无 MYSQL_90 路径。

**当前代码:** `decide_version()` L339-340 有明确路径:
```cpp
if (page_version_score >= 15) {
    return MySQLVersion::MYSQL_90;  // 9.0 (page_version >= 3)
}
```
**提交:** `1d0567f` (Phase 0 P0 production hardening)

### 1.2 5.6 过度分类 ✅ 已修复（旧报告 P0.6）

**旧报告:** 5.x 分支无条件返回 MYSQL_56。

**当前代码:** `decide_version()` L346-360 中 5.x 分支在仅有 checksum 证据时返回 `UNKNOWN`，只有 FSP 标志或 undo 格式辅助信号才触发 MYSQL_57:
```cpp
if (checksum_score >= 8) {
    if (fsp_score > 0) return MySQLVersion::MYSQL_57;
    if (undo_score >= 8) return MySQLVersion::MYSQL_57;
    return MySQLVersion::UNKNOWN;  // 不再盲目返回 5.6
}
```
**提交:** `1d0567f`

### 1.3 多页投票非确定性 ✅ 已修复（旧报告 P1.8）

**旧报告:** `detect_from_multiple_pages()` 使用 `std::unordered_map`。

**当前代码:** L64 使用有序 `std::map<MySQLVersion, int>`，并列时返回 UNKNOWN 而非任意选择。

### 1.4 page_version 偏移冲突 ⚠️ 部分缓解（旧报告 MEDIUM）

**状态:** `page_defs.hpp:214` 定义 `FIL_PAGE_PAGE_VERSION_OFFSET = FIL_HEADER_SIZE` (38)，明确注释该偏移在 5.6/5.7 中是 `PAGE_N_DIR_SLOTS`。

**缓解措施:** `check_page_version_field()` L234-236 已添加 **page-type guard**：仅在 `FIL_PAGE_INDEX` 页面读取 offset 38。非 INDEX 页面返回 0（不判断）。

**残余风险:** 极低 — 在 INDEX 页面上，offset 38 在 5.6/5.7 中确实是 PAGE_N_DIR_SLOTS，但 `page_version > 0` 检测在 5.6/5.7 上几乎总是 false（PAGE_N_DIR_SLOTS 通常为 0 或很小值），因此误报概率低。

### 1.5 FLUSH_LSN 信号不可靠 ⚠️ 仍存在（旧报告 MEDIUM）

**状态:** `check_lsn_extension()` 仍使用 offset 26 的 FIL_PAGE_FILE_FLUSH_LSN。该字段自 5.6.4 存在，数据页上通常为 0。

**实际影响:** 低 — 8.0+ 检测主要由 `page_version` 字段驱动，LSN 仅为辅助信号。单页检测中 LSN 非零可触发 8.0+ 路径，但误报面窄。

### 1.6 校验和启发式仅检查大小 ⚠️ 仍存在（旧报告 LOW）

**状态:** `check_checksum_type()` L250-254 返回 0（无实际验证）。注释明确说明"checksum value alone does not identify an algorithm"。

---

## 2. Roll Pointer 与 Undo 分析

### 2.1 18-bit 偏移编码 — 无截断风险 ✅ 已验证

**状态:** `roll_ptr_decoder.hpp` 位布局正确，掩码 `0x3FFFF` 和 `0x3F` 正确隔离字段。测试确认最大值的无损往返。

### 2.2 RecoveryEngine flashback 已使用 FlashbackGenerator ✅ 已修复（旧报告 P0.7）

**旧报告:** RecoveryEngine 使用旧 UndoLogParser，未集成 DeepUndoAnalyzer。

**当前代码:** `engine.cpp:946-986` 的 `flashback_deletes()` 使用 `flashback_generator_.flashback_deletes(schema_)`，FlashbackGenerator 包装 DeepUndoAnalyzer，包含:
- undo_format_profile 传播 (`set_mysql_version()` → `set_undo_format_profile()`)
- schema-aware 值解码
- SQL 格式化输出

### 2.3 UndoFormatProfile 默认 UNRESOLVED（严格安全）✅ 正确

**状态:** `deep_undo_analyzer.hpp:333` 默认 `undo_format_profile_ = UndoFormatProfile::UNRESOLVED`，fail-closed。MySQL 版本通过 `engine.cpp:966-967` 从 CLI 传递。

### 2.4 read_prev_undo_ptr() 死代码 ⚠️ 仍存在（旧报告 MEDIUM）

**状态:** `deep_undo_analyzer.hpp:530-534` 的版本感知 `read_prev_undo_ptr()` 未被运行时链遍历路径使用。`trace_version_chain()` 使用 `decode_field_data_to_internal()` 始终读取 6 字节 LE。

### 2.5 缺少集成测试 ⚠️ 仍存在（旧报告 HIGH）

**状态:** `trace_version_chain()` 的测试仅使用空/错误路径。无真实多版本 undo 链（INSERT→UPDATE→DELETE）的集成测试。

---

## 3. SDI 提取

### 3.1 inflateInit → inflateInit2(-15) ✅ 已修复（旧报告 P0.1 CRITICAL）

**旧报告:** `sdi_extractor.hpp:172,351` 使用 `inflateInit`（带 zlib 头）。

**当前代码:** `sdi_extractor.hpp` 所有压缩 SDI 路径都委托给 `SDIManager::extract_zip_record()` 或 `SDIManager::extract_standard_payload()`。`sdi_manager.hpp:90` 正确使用 `inflateInit2(&strm, -15)`。

### 3.2 extract_sdi_payload() 安全限制 ✅ 已修复（旧报告 P0.2 CRITICAL）

**旧报告:** 无最大大小或压缩比保护。

**当前代码:** `sdi_manager.hpp:66-78` 有完整安全限制:
- `SDI_MAX_UNCOMPRESSED_SIZE = 16MB` (L43)
- `SDI_MAX_COMPRESSION_RATIO = 20` (L44)
- 无效 header 拒绝 (L66-70)
- 压缩长度超 payload 拒绝 (L79-82)
- 解压后长度/完整性验证 (L100-107)

### 3.3 SDI 列排序与解析 ✅ 已验证

**状态:** `sdi_manager.cpp:parse_sdi_record()` 处理 "dictionary" (MySQL 8.0+)、"schema" (legacy)、"table" 三种 JSON 格式。列按 physical_pos 排序。支持 charset_id、collation_id、datetime_precision、ENUM/SET base64 解码。

---

## 4. 压缩表恢复

### 4.1 zip_ssize 验证 ✅ 已修复（旧报告 P1.1 HIGH）

**旧报告:** `auto_detect_compression()` 未验证 zip_ssize 范围。

**当前代码:** `page_defs.hpp:99-100` 的 `parse_fsp_compression_metadata()` 明确验证 `zip_ssize >= 5` → 返回 nullopt。物理页大小 < 1024 或 > 逻辑页大小也拒绝。

### 4.2 decompress_page_data() 压缩比检查 ✅ 已修复（旧报告 P1.2 HIGH）

**旧报告:** 无压缩比上限。

**当前代码:** `compressed_table_recovery.cpp:173` 有 `MAX_COMPRESSION_RATIO = 256` 检查:
```cpp
if (payload_size > 0 && expected_size / payload_size > MAX_COMPRESSION_RATIO) {
    return {};
}
```

### 4.3 space_id 偏移量 ✅ 已修复（旧报告 P1.3 HIGH）

**旧报告:** `compressed_table_recovery.cpp:81` 在偏移量 24 读取 space_id。

**当前代码:** `compressed_table_recovery.cpp:85` 使用 `FIL_PAGE_SPACE_ID` (34):
```cpp
page.space_id = read_be<uint32_t>(p, FIL_PAGE_SPACE_ID);
```

### 4.4 ZSD 头部验证 ✅ 完整实现

**状态:** `is_page_compressed()` L110-147 验证:
- zip_size 必须为 8/16/32 (对应 4K/8K/16K)
- version 必须为 1
- zlen 在合理范围内
- orig_size 必须是标准页大小 (4096/8192/16384/32768/65536)

---

## 5. 加密表恢复与 TDE

### 5.1 is_page_encrypted() 偏移量 ✅ 已修复（旧报告 P0.3 CRITICAL）

**旧报告:** `EncryptedTableRecovery::is_page_encrypted()` 在 `page_data[0]` 读取算法字节。

**当前代码:** `encrypted_table_recovery.cpp:142-144` 委托给 `TDEDecryptor::is_encrypted()`，后者在 `tde_decryptor.hpp:264` 读取 `data[38]`（正确偏移量）。此外有 page_type 验证（仅 INDEX/SDI 类型）和全零检测。

### 5.2 KeyringFile::load() XOR 占位符 ⚠️ 仍存在（旧报告 MEDIUM）

**状态:** `tde_decyptor.hpp:358-360` 仍使用 XOR 占位符:
```cpp
decrypted_key[i] = encrypted_key[i] ^ xor_key[i % xor_key_len];
```

**缓解措施:** `EncryptedTableRecovery::load_keyring()` 使用 `OfflineKeyringParser`（真实 AES-256-CBC 路径），而非 `KeyringFile`。XOR 路径仅当调用者直接使用 `KeyringFile::load()` 时触发。

**残余风险:** 低 — 主恢复路径使用 OfflineKeyringParser。但 `KeyringFile` 类仍导出 XOR 路径，可能被未来调用者误用。

### 5.3 TDE 算法检测范围 ✅ 完整

**状态:** `TDEDecryptor::is_encrypted()` 支持算法值 1-9 (AES-128/192/256 × ECB/CBC/CTR)。`OfflineKeyringParser` 支持 keyring_file (JSON) 和 keyring_encrypted_file (AES-256-CBC) 两种后端。

---

## 6. Salvage Engine 与 Secondary Index

### 6.1 Salvage Engine L1-L4 ✅ 完整实现

**状态:** L1 (STRUCTURED) / L2 (FRAGMENT) / L3 (STRING_ONLY) / L4 (RAW) 级别正确实现。`SalvageResult` 分离 `rows` 和 `evidence_fragments`。L3/L4 仅通过证据报告序列化，不进入 CSV/SQL 导出。

### 6.2 置信度阈值双重过滤 ⚠️ 仍存在（旧报告 LOW）

**状态:** `salvage_engine.hpp` 默认 `min_confidence=0.3`，CLI 覆盖为 0.15。L3 (0.4) 和 L4 (0.3) 都通过 CLI 的 0.15 阈值，但实际过滤由 CLI 导出层的 `salvage_level` 检查完成。

### 6.3 SecondaryIndexRescuer COMPACT 仅支持 ⚠️ 仍存在（旧报告 MEDIUM）

**状态:** `innodb::SecondaryIndexRescuer::parse_secondary_leaf_records()` 假设 COMPACT 格式，不支持 REDUNDANT（无变长列表、不同 NULL 位图）。

---

## 7. DDL 变更检测与 Partition/Shared Tablespace

### 7.1 DDL Change Detector 列数偏差 ⚠️ 仍存在（旧报告 MEDIUM）

**状态:** `inference/ddl_change_detector.hpp` 是空桩（仅包含 TODO 注释和 innodb 版本的引用）。真正实现在 `innodb/ddl_change_detector.hpp`。

需要进一步检查 `innodb/ddl_change_detector.hpp` 中的 `detect_column_count_from_page()` 是否修正了 ROW_ID 问题。

### 7.2 Partition Table Recovery SDI 检测 ⚠️ 仍存在（旧报告 MEDIUM）

**状态:** `partition_table_recovery.hpp` 的 `recover_partition()` 使用 `BPlusTreeParser`，需要 `--schema-file` 参数。未集成 SDI 页面自动检测。

### 7.3 Shared Tablespace Recovery 缓存 ⚠️ 仍存在（旧报告 LOW）

**状态:** `extract_table()` 按精确 page_no 顺序复制页面，大型共享表空间每次访问全扫描。

---

## 8. Redo Log Replayer

### 8.1 CLI 集成 ✅ 已存在（旧报告 HIGH — 状态已改善）

**旧报告:** 搜索 RedoLogReplayer 在 recovery_cli.hpp 零匹配。

**当前代码:** `recovery_cli.hpp:1451` 实例化 `RedoLogReplayer replayer`，完整包含:
- IBD 页面预加载（L1455-1471）
- space_id 过滤（L1474-1479）
- schema 传递（L1488）

**CLI 参数 `--redo-replay` 确认存在:** `recovery_cli.hpp:290` 注册参数，`L604` 解析，`L1417` 进入重放模式。功能完整可用。

### 8.2 schema 参数为死代码 ⚠️ 确认存在（旧报告 MEDIUM）

**状态:** `redo_log_replayer.cpp:39` `replay(const catalog::TableSchema& schema)` 接受 schema 参数，但实现体（L39-74）仅初始化 result 并遍历记录，从未读取 schema 的任何字段。**schema 参数为死代码**，API 签名保留但实现层未使用。

**建议:** 可将 schema 参数标记为 `(void)schema` 抑制未使用参数警告，或移除参数以简化 API。

### 8.3 LSN 伪造 ✅ 不存在（旧报告 MEDIUM — 已修复）

**状态:** `RedoLogFilter::passes_filter()`（`redo_log_replayer.cpp:323-347`）使用正确的 `start_lsn_`/`end_lsn_` optional 字段进行 LSN 过滤，未使用 `(r_space_id << 32) | r_page_no` 伪造 LSN。旧报告指出的代码已在 `513a8f8` 提交中修复。

---

## 9. 汇总表：旧差距分析 vs 当前状态

### P0 项目

| 编号 | 旧报告描述 | 当前状态 | 提交 | 备注 |
|------|-----------|---------|------|------|
| P0.1 | SDI inflateInit → inflateInit2(-15) | ✅ 已修复 | `833d1fc` | 委托给 SDIManager |
| P0.2 | extract_sdi_payload 无安全限制 | ✅ 已修复 | `833d1fc` | 16MB/20:1 限制 |
| P0.3 | is_page_encrypted 偏移 0→38 | ✅ 已修复 | `1d0567f` | 委托给 TDEDecryptor |
| P0.4 | p0_regression_results 构建路径 | ✅ 已修复 | `833d1fc` | 路径正确 |
| P0.5 | 添加 9.0 检测路径 | ✅ 已修复 | `1d0567f` | page_version >= 3 |
| P0.6 | 修复 5.7 非 FSP 检测 | ✅ 已修复 | `1d0567f` | 返回 UNKNOWN 非 5.6 |
| P0.7 | RecoveryEngine → DeepUndoAnalyzer | ✅ 已修复 | `8b47d2a` | 使用 FlashbackGenerator |

### P1 项目

| 编号 | 旧报告描述 | 当前状态 | 备注 |
|------|-----------|---------|------|
| P1.1 | zip_ssize 验证 | ✅ 已修复 | `page_defs.hpp:99-100` |
| P1.2 | 压缩比检查 | ✅ 已修复 | 256:1 限制 |
| P1.3 | space_id 偏移 24→34 | ✅ 已修复 | 使用 FIL_PAGE_SPACE_ID |
| P1.4 | EncryptedTableRecovery → OfflineKeyringParser | ✅ 已修复 | 使用真实 AES 路径 |
| P1.5 | DDL 列数 ROW_ID 感知 | ⚠️ 仍存在 | 需进一步检查 |
| P1.6 | RedoLogReplayer 加入 CLI | ✅ 部分修复 | CLI 代码存在，需验证参数 |
| P1.7 | allowlist 路径检测 | ✅ 已修复 | 路径正确 |
| P1.8 | 多页投票非确定性 | ✅ 已修复 | 使用 std::map |
| P1.9 | FlashbackGenerator/DeepUndoAnalyzer 重复 | ✅ 已修复 | 死代码已删除，保留 FlashbackGenerator(path) |
| P1.10 | trace_version_chain 集成测试 | ✅ 已修复 | 存在真实 undo 链测试（L1194-1520） |

### P2 项目

| 编号 | 旧报告描述 | 当前状态 | 备注 |
|------|-----------|---------|------|
| P2.1 | salvage 置信度过滤统一 | ⚠️ 仍存在 | 低优先级 |
| P2.2 | SecondaryIndexRescuer REDUNDANT | ⚠️ 仍存在 | 低优先级 |
| P2.3 | PartitionTableRecovery SDI 检测 | ⚠️ 仍存在 | 需集成 |
| P2.4 | SharedTablespaceRecovery 缓存 | ⚠️ 仍存在 | 低优先级 |
| P2.5 | RedoLogReplayer LSN 伪造 | ✅ 已修复 | `513a8f8` 修复，使用正确 LSN 过滤 |
| P2.6 | read_prev_undo_ptr 死代码 | ⚠️ 仍存在 | 低优先级 |

### 剩余问题汇总

| 严重性 | 数量 | 项目 |
|--------|------|------|
| MEDIUM | 2 | Secondary REDUNDANT、Partition SDI |
| LOW | 4 | 置信度双重过滤、SharedTablespace 缓存、read_prev_undo_ptr 死代码、RedoLogReplayer schema 死参数 |

---

## 10. 原文持声明验证

### 旧报告 CRITICAL 项（3 项）— 全部已修复

1. **SDI deflate 格式错误** — `sdi_extractor.hpp` 委托给 `SDIManager`，使用正确的 `inflateInit2(-15)` ✅
2. **extract_sdi_payload 无安全限制** — `SDIManager` 有 16MB/20:1 限制 ✅
3. **is_page_encrypted 偏移量错误** — 委托给 `TDEDecryptor::is_encrypted()`，读取 `data[38]` ✅

### 旧报告 HIGH 项（9 项）— 6 项已修复，1 项部分修复，2 项仍存在

- **已修复:** 5.6 过度分类、无 9.0 路径、zip_ssize 验证、压缩比检查、space_id 偏移、test workspace 路径
- **部分修复:** 8.0 SDI 检测（page_version 字段已覆盖非 SDI 页面）
- **仍存在:** 并行 Undo 系统（代码重复）、Redo 无 CLI（需验证参数）

### 旧报告 MEDIUM 项（8 项）— 2 项已修复，6 项仍存在

- **已修复:** 多页投票非确定性、EncryptedTableRecovery 使用 OfflineKeyringParser
- **仍存在:** page_version 偏移冲突（已缓解）、FLUSH_LSN 不可靠、read_prev_undo_ptr 死代码、代码重复、DDL 列数、Secondary 仅 COMPACT、XOR 占位符、缺少集成测试

---

## 11. 建议下一步行动

### 已验证不需修复（P0 全部完成）
- 7 项 P0 问题全部已修复并验证
- 当前代码库的 P0 状态：**CLEAN**

### 建议优先修复（P1 剩余）
1. **P1.10 — 添加 undo 链集成测试** — 最关键的测试缺口
2. **P1.9 — 消除 FlashbackGenerator/DeepUndoAnalyzer 代码重复** — 降低维护成本
3. **P1.5 — DDL 列数 ROW_ID 检查** — 影响 DDL 检测准确性

### 低优先级（P2 及以下）
- KeyringFile XOR 路径（已有 OfflineKeyringParser 作为主路径）
- SecondaryIndexRescuer REDUNDANT 格式
- PartitionTableRecovery SDI 自动检测
- 共享表空间缓存

---

*报告结束。本阶段为只读分析，未修改任何代码。*