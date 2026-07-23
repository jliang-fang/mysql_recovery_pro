# MySQL Recovery Pro v2.0 "超越 ODU" 完整架构设计蓝图

> **文档版本**: v2.0.0-draft
> **创建日期**: 2026-07-19
> **目标**: 在全部维度上超越 Oracle ODU，成为 MySQL 数据恢复的终极工具

---

## 目录

1. [现状分析与差距评估](#1-现状分析与差距评估)
2. [深度 Undo 分析引擎](#2-深度-undo-分析引擎)
3. [智能模式推断引擎](#3-智能模式推断引擎)
4. [抢救性恢复引擎](#4-抢救性恢复引擎)
5. [极端磁盘雕刻器 v2.0](#5-极端磁盘雕刻器-v20)
6. [全场景覆盖增强](#6-全场景覆盖增强)
7. [恢复报告系统](#7-恢复报告系统)
8. [优先级矩阵](#8-优先级矩阵)
9. [实施路线图](#9-实施路线图)

---

## 1. 现状分析与差距评估

### 1.1 现有能力总结

| 维度 | 当前状态 | 评分 |
|------|---------|------|
| **版本兼容** | MySQL 5.6 - 9.0 全系列，含 FRM/SDI 双路径 | ⭐⭐⭐⭐⭐ |
| **DROP TABLE 恢复** | ext4/XFS 磁盘雕刻 + 文件系统感知恢复，100% 成功率 | ⭐⭐⭐⭐⭐ |
| **TRUNCATE 恢复** | FLUSH TABLES 后 100%，未 FLUSH 受限于延迟分配 | ⭐⭐⭐⭐ |
| **rm -rf 恢复** | ext4/XFS 文件系统遍历 + 裸盘扫描，600K+ 记录规模验证 | ⭐⭐⭐⭐⭐ |
| **行格式解析** | COMPACT/DYNAMIC/REDUNDANT/COMPRESSED 全支持 | ⭐⭐⭐⭐⭐ |
| **字段解码** | 全部 MySQL 类型，含 ENUM/SET/JSON/GEOMETRY/BLOB | ⭐⭐⭐⭐⭐ |
| **Schemaless 恢复** | 基于 Page Directory 的模式无关扫描，基础启发式 | ⭐⭐⭐ |
| **BLOB 恢复** | 完整的 BLOB 链遍历和重建 | ⭐⭐⭐⭐ |
| **损坏修复** | 页头修复、checksum 修复、页链修复 | ⭐⭐⭐ |
| **Undo 解析** | 基础 TRUNCATE 逆向恢复，undo 页解析框架存在 | ⭐⭐ |
| **Redo 解析** | 基础 redo record 解析和 replay 框架 | ⭐⭐ |
| **加密支持** | TDE 解密框架 (AES-256-CBC)，keyring 解析 | ⭐⭐⭐ |
| **分区表** | 基础分区识别，无自动聚合 | ⭐⭐ |
| **共享表空间** | 未实现 | ⭐ |
| **报告系统** | RecoveryStats 基础统计 | ⭐⭐ |

### 1.2 关键差距 (vs ODU)

1. **Undo 深度不够**: ODU 支持完整的 undo 链回溯和多版本恢复，我们仅有基础的 undo 页扫描，`apply_rollback()` 是空壳 (只有 switch-case 框架)。
2. **模式推断弱**: ODU 在无元数据时可以智能推断列类型，我们只有基础的 pattern matching (email/user/数字)。
3. **抢救能力有限**: 页头损坏时无逐行抢救 (row-by-row salvage)，无辅助索引反查。
4. **碎片重组缺失**: 裸盘雕刻模式未做 SPACE_ID + PAGE_NO 逻辑重组。
5. **无分区/共享表空间支持**: 这两类是企业级部署的常见场景。
6. **无闪回 DELETE**: 无法从 undo 生成反向 INSERT。
7. **报告简陋**: 无专业的恢复报告，无损坏分析、策略说明。

### 1.3 架构债务

- **双命名空间**: `dbrecovery` vs `mysql_recovery` 存在概念重复，需要统一但不可破坏现有功能。
- **大文件**: `recovery_cli.hpp` (2671行), `main.cpp` (2973行) 需要模块化。
- **Undo/Redo 集成松散**: redo replayer 和 btree parser 之间的集成是弱耦合的。

---

## 2. 深度 Undo 分析引擎

### 2.1 理论背景

InnoDB 的 MVCC 通过 undo log 实现。每条记录的 DB_ROLL_PTR 指向 undo log 中的前一个版本。关键数据结构：

```
Undo Log Segment Header (FSEG Header in undo page):
  - TRX_UNDO_STATE: 活跃状态
  - TRX_UNDO_LAST_LOG: 最后一条 undo log 偏移
  - TRX_UNDO_TRX_ID: 事务 ID
  - TRX_UNDO_XID: XA 事务 ID

Undo Record (TRX_UNDO_UPDATE 类型):
  [2B] next_offset    - 下一条记录偏移
  [2B] type_cmpl      - 类型和压缩标志
  [1B] undo_no        - undo 编号
  [4B] table_id       - 表 ID
  [nB] primary_key    - 主键字段 (变长)
  [2B] info_bits      - 信息位
  [1B] n_updated      - 更新的字段数
  [nB] old_values     - 旧值 (各字段)
  
  对于 TRX_UNDO_DEL_MARK: 不存储旧值，仅标记删除
  对于 TRX_UNDO_INSERT: 存储主键用于删除回滚
```

**Undo 链的遍历**:

```
当前记录 (B+Tree 叶子页)
  └─ DB_ROLL_PTR ──→ Undo Record #1 (最新 undo)
                       └─ prev_undo_ptr ──→ Undo Record #2
                                              └─ ... ──→ Undo Record #N (最旧)
```

### 2.2 架构设计

```
┌──────────────────────────────────────────────────────────────────┐
│                    DeepUndoAnalyzer (新增)                        │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────────┐  ┌──────────────────┐  ┌────────────────┐  │
│  │ UndoChainTracer │  │ HistoryRebuilder │  │ FlashbackEngine│  │
│  │                 │  │                  │  │                │  │
│  │ - trace_chain() │  │ - rebuild_row()  │  │ - gen_reverse()│  │
│  │ - find_all_ver()│  │ - merge_fields() │  │ - filter_time()│  │
│  │ - validate()    │  │ - detect_ddl()   │  │ - filter_trx() │  │
│  └────────┬────────┘  └────────┬─────────┘  └───────┬────────┘  │
│           │                    │                     │           │
│  ┌────────┴────────────────────┴─────────────────────┴────────┐  │
│  │               UndoPageManager (重构)                        │  │
│  │  - 统一 undo 页解析 (替换分散的实现)                         │  │
│  │  - 支持 ibdata1 + 独立 undo 表空间                          │  │
│  │  - 缓存热 undo 页                                           │  │
│  └─────────────────────────────────────────────────────────────┘  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 2.3 核心数据结构

```cpp
// ==================== include/dbrecovery/innodb/deep_undo_analyzer.hpp ====================

namespace dbrecovery::innodb {

/**
 * @brief Undo 记录中的字段信息（旧值）
 */
struct UndoFieldValue {
    uint16_t field_no;              // 字段编号 (1-based)
    std::vector<uint8_t> old_value; // 旧值（原始字节）
    bool is_null;                   // 是否为 NULL
    bool is_extern;                 // 是否为外部存储引用
};

/**
 * @brief 完整的 undo 记录（增强版）
 */
struct UndoRecordV2 {
    // 定位信息
    uint32_t undo_page_no;          // undo 页号
    uint16_t undo_page_offset;      // 页内偏移
    
    // 事务信息
    uint64_t trx_id;                // 事务 ID
    uint64_t undo_no;               // undo 编号（同一事务内递增）
    
    // 类型
    enum Type { INSERT=1, UPDATE=2, DELETE_MARK=3, DELETE=4 };
    Type type;
    
    // 目标记录定位
    uint32_t table_id;
    uint64_t index_id;
    uint32_t page_no;               // 原始记录所在页
    uint16_t page_offset;           // 原始记录页内偏移
    uint64_t roll_ptr;              // 指向前一个 undo 版本的指针
    uint64_t prev_undo_ptr;         // 从 undo 记录头解析的前一个 undo 指针
    
    // 数据
    std::vector<uint8_t> primary_key_raw;   // 主键原始数据
    std::vector<UndoFieldValue> old_fields; // 旧字段值
    uint16_t info_bits;                     // 信息位
    
    // 时间戳 (从 trx 系统字段或 redo 推断)
    std::optional<uint64_t> commit_lsn;
    std::optional<std::chrono::system_clock::time_point> approx_timestamp;
};

/**
 * @brief 一行的完整版本历史
 */
struct RowVersionHistory {
    // 当前数据页中的版本（最新提交版本）
    std::vector<uint8_t> current_data;
    bool current_is_deleted;        // 当前是否已删除
    
    // 历史版本，按时间降序 (最新 undo → 最旧 undo)
    std::vector<UndoRecordV2> undo_chain;
    
    // 元信息
    uint64_t index_id;
    std::vector<uint8_t> primary_key;
    uint32_t n_versions;            // 总版本数（含当前）
    
    /**
     * @brief 获取指定时间点的最近版本
     */
    std::optional<std::vector<uint8_t>> get_version_at(
        std::chrono::system_clock::time_point point) const;
    
    /**
     * @brief 重建完整 INSERT 语句（最新非删除版本）
     */
    std::string to_insert_sql(const catalog::TableSchema& schema) const;
};

/**
 * @brief DELETE 闪回结果
 */
struct FlashbackResult {
    std::string reverse_sql;        // 反向 SQL (INSERT/DELETE/UPDATE)
    uint64_t trx_id;
    uint64_t undo_no;
    std::optional<std::chrono::system_clock::time_point> timestamp;
    double confidence;              // 0.0 - 1.0，数据完整度
};

// ==================== 核心引擎类 ====================

class DeepUndoAnalyzer {
public:
    /**
     * @brief 为指定记录遍历完整的 undo 版本链
     * 
     * 算法:
     * 1. 从当前记录的 DB_ROLL_PTR 开始
     * 2. 解析 roll_ptr → (undo_page_no, undo_page_offset)
     * 3. 读取 undo 记录，提取旧值
     * 4. 从 undo 记录头解析 prev_undo_ptr
     * 5. 重复步骤 2-4 直到 prev_undo_ptr = 0 或 IS_INSERT 标记
     * 6. 按 undo_no 排序，构建完整版本链
     * 
     * @param current_record 当前 B+Tree 中的记录数据
     * @param roll_ptr 当前记录的 DB_ROLL_PTR 值
     * @param schema 表结构（用于字段解码）
     * @param index_id 索引 ID
     * @return 完整版本历史
     */
    RowVersionHistory trace_version_chain(
        const std::vector<uint8_t>& current_record,
        uint64_t roll_ptr,
        const catalog::TableSchema& schema,
        uint64_t index_id);
    
    /**
     * @brief 从指定表的所有 undo 链中重建历史版本
     * 
     * 扫描 ibdata1 和独立 undo 表空间中属于目标 space_id 的 undo 记录，
     * 构建所有行的版本历史。
     * 
     * @param space_id 目标表空间 ID
     * @param schema 表结构
     * @param index_id 聚集索引 ID
     * @param time_range 可选时间范围过滤
     * @return 按主键分组的版本历史映射
     */
    std::map<std::string, RowVersionHistory> rebuild_all_histories(
        uint32_t space_id,
        const catalog::TableSchema& schema,
        uint64_t index_id,
        std::optional<std::pair<uint64_t, uint64_t>> lsn_range = std::nullopt);
    
    /**
     * @brief 闪回 DELETE 操作 - 生成反向 INSERT 语句
     * 
     * 算法:
     * 1. 扫描 undo log 找到所有 TRX_UNDO_DEL_MARK 类型的记录
     * 2. 对每条删除记录:
     *    a. 解析 undo 记录中的主键
     *    b. 如果有 update undo 链，从链中重建完整行数据
     *    c. 如果没有，标记为 "主键已知但数据不完整"
     * 3. 生成 INSERT INTO ... VALUES (...) 语句
     * 4. 按事务 ID 和时间戳排序输出
     * 
     * @param space_id 表空间 ID
     * @param schema 表结构
     * @param filter 过滤条件（时间/事务ID）
     * @return 闪回 SQL 语句列表
     */
    std::vector<FlashbackResult> flashback_deletes(
        uint32_t space_id,
        const catalog::TableSchema& schema,
        const FlashbackFilter& filter = {});
    
    /**
     * @brief 启发式重组断裂的 undo 链
     * 
     * 当 undo 页部分损坏时：
     * 1. 按 table_id + primary_key 匹配 undo 记录
     * 2. 利用 undo_no 序列重建顺序
     * 3. 用 LSN 辅助排序
     * 4. 标记断裂点并给出置信度
     */
    std::vector<RowVersionHistory> heuristic_reconstruct_chains(
        uint32_t space_id,
        const catalog::TableSchema& schema);
    
private:
    std::unique_ptr<UndoPageManager> page_manager_;
    UndoRecordV2Parser record_parser_;
    FieldReconstructor field_reconstructor_;
};

/**
 * @brief 闪回过滤器
 */
struct FlashbackFilter {
    std::optional<uint64_t> start_lsn;
    std::optional<uint64_t> end_lsn;
    std::optional<uint64_t> trx_id;
    std::optional<std::chrono::system_clock::time_point> start_time;
    std::optional<std::chrono::system_clock::time_point> end_time;
    uint32_t max_records = 10000;   // 安全上限
};

} // namespace dbrecovery::innodb
```

### 2.4 Undo 链遍历算法 (伪代码)

```
Algorithm: trace_version_chain(current_record, roll_ptr, schema, index_id)
Input:  当前B+Tree记录, DB_ROLL_PTR值, 表结构, 索引ID
Output: 完整版本历史 RowVersionHistory

1. history ← RowVersionHistory()
2. history.current_data ← current_record
3. current_roll_ptr ← roll_ptr
4. visited ← HashSet<uint64_t>()  // 防止循环
5. 
6. WHILE current_roll_ptr != 0 AND current_roll_ptr ∉ visited:
7.     visited.insert(current_roll_ptr)
8.     
9.     // 解析 roll_ptr: [47:32] undo_page_no | [31:0] undo_page_offset
10.    undo_page_no ← (current_roll_ptr >> 32) & 0xFFFFFFFF
11.    undo_offset  ← current_roll_ptr & 0xFFFFFFFF
12.    
13.    // 读取 undo 页
14.    undo_page ← page_manager_.read_undo_page(undo_page_no)
15.    IF undo_page == null: BREAK  // 页不可读
16.    
17.    // 解析 undo 记录
18.    undo_rec ← record_parser_.parse(undo_page, undo_offset)
19.    IF undo_rec == null: BREAK   // 记录损坏
20.    
21.    // 提取旧值
22.    IF undo_rec.type == TRX_UNDO_UPDATE:
23.        // 更新类型：提取被修改字段的旧值
24.        FOR EACH updated_field IN undo_rec.updated_fields:
25.            field_value ← decode_field(updated_field.data, schema, updated_field.field_no)
26.            undo_rec.old_fields.push_back(field_value)
27.    ELIF undo_rec.type == TRX_UNDO_INSERT:
28.        // 插入类型：记录主键，标记为插入版本
29.        undo_rec.primary_key_raw ← extract_pk_from_undo(undo_rec, schema)
30.    
31.    history.undo_chain.push_back(undo_rec)
32.    
33.    // 移动到前一个版本
34.    current_roll_ptr ← undo_rec.prev_undo_ptr
35.    
36.    // 如果到达 INSERT 版本，停止
37.    IF undo_rec.type == TRX_UNDO_INSERT: BREAK
38.    
39.    // 安全限制：最多回溯 1000 个版本
40.    IF history.undo_chain.size() >= 1000:
41.        history.truncated ← true
42.        BREAK
43. 
44. history.n_versions ← 1 + history.undo_chain.size()
45. RETURN history
```

### 2.5 DELETE 闪回算法 (伪代码)

```
Algorithm: flashback_deletes(space_id, schema, filter)
Input:  表空间ID, 表结构, 过滤条件
Output: 闪回SQL语句列表

1. results ← []
2. 
3. // 第一步：收集所有 delete-mark undo 记录
4. undo_pages ← page_manager_.scan_all_undo_pages()
5. delete_records ← []
6. 
7. FOR EACH page IN undo_pages:
8.     FOR EACH record IN page.records:
9.         IF record.type == TRX_UNDO_DEL_MARK:
10.            // 检查 space_id 匹配（从 undo 记录头或关联推断）
11.            IF record.table_id matches space_id:
12.                delete_records.push_back(record)
13. 
14. // 第二步：按事务分组并排序
15. delete_records.sort(by: trx_id, then: undo_no)
16. 
17. // 第三步：为每条删除记录重建行数据
18. FOR EACH del_record IN delete_records:
19.     IF results.size() >= filter.max_records: BREAK
20.     
21.     // 时间过滤
22.     IF filter.start_time AND del_record.approx_timestamp < filter.start_time:
23.         CONTINUE
24.     
25.     // 重建行数据
26.     row_data ← reconstruct_row_from_undo_chain(del_record, schema)
27.     
28.     flashback ← FlashbackResult()
29.     flashback.trx_id ← del_record.trx_id
30.     flashback.timestamp ← del_record.approx_timestamp
31.     
32.     IF row_data.complete:
33.         flashback.reverse_sql ← generate_insert_sql(row_data, schema)
34.         flashback.confidence ← 1.0
35.     ELSE:
36.         // 不完整数据：至少生成带主键的 INSERT（部分列可能为 NULL）
37.         flashback.reverse_sql ← generate_partial_insert_sql(row_data, schema)
38.         flashback.confidence ← row_data.completeness_ratio
39.     
40.     results.push_back(flashback)
41. 
42. RETURN results

Algorithm: reconstruct_row_from_undo_chain(del_record, schema)
Input:  删除类型的 undo 记录, 表结构
Output: 重建的行数据

1. row ← RowData()
2. 
3. // 主键从 undo 记录中获取
4. row.primary_key ← del_record.primary_key_raw
5. 
6. // 检查删除记录之前的 undo 链
7. prev_ptr ← del_record.prev_undo_ptr
8. 
9. WHILE prev_ptr != 0:
10.    prev_undo ← read_undo_record(prev_ptr)
11.    IF prev_undo == null: BREAK
12.    
13.    IF prev_undo.type == TRX_UNDO_UPDATE:
14.        // 从 update undo 中恢复旧值
15.        FOR EACH field IN prev_undo.old_fields:
16.            IF field.field_no NOT IN row.fields:
17.                row.fields[field.field_no] ← field.value
18.    
19.    IF prev_undo.type == TRX_UNDO_INSERT:
20.        // 找到了 INSERT 版本，数据最完整
21.        row.complete ← true
22.        BREAK
23.    
24.    prev_ptr ← prev_undo.prev_undo_ptr
25. 
26. // 填充缺失的字段为 NULL（带警告标记）
27. FOR i ← 1 TO schema.columns.size():
28.     IF i NOT IN row.fields:
29.         row.fields[i] ← NULL
30.         row.missing_fields.insert(i)
31. 
32. row.completeness_ratio ← |row.fields - row.missing_fields| / |schema.columns|
33. RETURN row
```

### 2.6 CLI 接口设计

```bash
# 完整历史版本回溯
mysql_recovery_pro --ibd /path/to/table.ibd \
    --undo-history \
    --schema-file /path/to/schema.json \
    --output /path/to/output/ \
    --format sql

# DELETE 闪回 (时间范围)
mysql_recovery_pro --ibd /path/to/table.ibd \
    --flashback-delete \
    --start-time "2026-07-18 00:00:00" \
    --end-time "2026-07-19 00:00:00" \
    --trx-id 12345678 \
    --output /path/to/flashback.sql

# 指定 ibdata1 和独立 undo 表空间
mysql_recovery_pro --data-dir /path/to/mysql/data \
    --undo-tablespaces "undo_001,undo_002" \
    --flashback-delete \
    --table-name mydb.mytable

# 交互式版本浏览
mysql_recovery_pro --ibd /path/to/table.ibd \
    --undo-history \
    --interactive  # 允许用户选择特定版本导出
```

---

## 3. 智能模式推断引擎

### 3.1 理论背景

当 `frm`/`SDI` 完全丢失时，必须从数据页本身推断表结构。InnoDB 数据页中每行记录的物理存储格式提供了丰富的线索：

**COMPACT/DYNAMIC 行格式物理布局**:
```
[offset table] [NULL bitmap] [TRX_ID 6B] [ROLL_PTR 7B] [FIXED fields...] [VARIABLE fields...]
```

**可提取的特征**:
- NULL bitmap 大小 → 可 NULL 列数
- offset table 条目数 → 总列数 (含系统列)
- 每个字段的字节模式 → 类型线索
- 字段间的值分布 → 类型置信度

### 3.2 架构设计

```
┌──────────────────────────────────────────────────────────────────┐
│              IntelligentSchemaInferenceEngine (新增)              │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌───────────────────┐  ┌─────────────────┐  ┌───────────────┐  │
│  │ PhysicalLayout    │  │ StatisticalType │  │ Interactive   │  │
│  │ Analyzer          │  │ Classifier      │  │ Schema Builder│  │
│  │                   │  │                 │  │               │  │
│  │ - detect_col_cnt()│  │ - classify_int()│  │ - propose_ddl │  │
│  │ - find_null_map() │  │ - classify_str()│  │ - user_confirm│  │
│  │ - calc_fixed_size │  │ - classify_time │  │ - refine()    │  │
│  │ - find_var_offsets│  │ - classify_enum │  │ - iterate()   │  │
│  └────────┬──────────┘  └────────┬────────┘  └───────┬───────┘  │
│           │                      │                    │          │
│  ┌────────┴──────────────────────┴────────────────────┴────────┐  │
│  │                    FeatureExtractor                          │  │
│  │  - 采样 N 个数据页 (默认 100)                                 │  │
│  │  - 对每列提取 M 个特征向量 (默认 1000 行)                      │  │
│  │  - 特征: [min, max, mean_len, stddev_len, entropy,           │  │
│  │           null_ratio, zero_ratio, ascii_ratio, digit_ratio,   │  │
│  │           has_sign, has_dot, has_colon, has_dash, ...]        │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 3.3 概率模型

```cpp
/**
 * @brief 列类型概率推断模型
 * 
 * 为每种候选类型计算后验概率:
 *   P(Type | Features) ∝ P(Features | Type) × P(Type)
 * 
 * 其中 P(Type) 是先验概率（常见类型的先验更高），
 * P(Features | Type) 是给定类型的特征似然度。
 */

namespace dbrecovery::inference {

/**
 * @brief 单列的特征向量
 */
struct ColumnFeatures {
    // 基本统计
    uint32_t sample_count;          // 采样行数
    double null_ratio;              // NULL 比例 (0.0 - 1.0)
    double zero_ratio;              // 全零字节比例
    double empty_ratio;             // 空字符串比例
    
    // 长度统计
    double len_min;                 // 最小字节数
    double len_max;                 // 最大字节数
    double len_mean;                // 平均字节数
    double len_median;              // 中位数字节数
    double len_stddev;              // 长度标准差
    std::vector<uint32_t> len_distribution; // 长度分布直方图 (桶=10)
    
    // 内容分析
    double ascii_ratio;             // 全 ASCII 可打印比例
    double digit_ratio;             // 全数字比例
    double alpha_ratio;             // 纯字母比例
    double alphanum_ratio;          // 字母数字混合比例
    double has_sign_ratio;          // 带符号(-)比例
    double has_dot_ratio;           // 含小数点比例
    double has_colon_ratio;         // 含冒号比例 (时间戳)
    double has_dash_ratio;          // 含破折号比例 (日期)
    double has_space_ratio;         // 含空格比例
    double has_at_ratio;            // 含@比例 (email)
    double has_slash_ratio;         // 含/比例 (路径/URL)
    
    // 数值特征
    std::optional<uint64_t> numeric_min;
    std::optional<uint64_t> numeric_max;
    double numeric_ratio;           // 可解析为数字的比例
    double integer_ratio;           // 可解析为整数的比例
    double float_ratio;             // 可解析为浮点数的比例
    double signed_ratio;            // 负数的比例
    
    // 字节模式
    double entropy;                 // 香农熵 (0.0 - 8.0)
    std::vector<uint8_t> byte_histogram; // 256 字节值分布
    double repeated_pattern_score;  // 重复模式得分 (ENUM 特征)
    
    // InnoDB 内部特征
    bool is_fixed_length;           // 字段是否定长
    uint32_t fixed_size;            // 如果定长，字节数
};

/**
 * @brief 候选类型及置信度
 */
struct TypeCandidate {
    std::string type_name;          // e.g., "BIGINT", "VARCHAR(255)"
    uint32_t innodb_type_id;        // InnoDB 内部类型 ID
    double confidence;              // 0.0 - 1.0
    std::string reasoning;          // 人类可读的判断理由
};

/**
 * @brief 推断结果
 */
struct InferenceResult {
    std::vector<std::vector<TypeCandidate>> column_candidates; // 每列的候选类型列表
    std::string proposed_ddl;       // 推荐的 CREATE TABLE 语句
    double overall_confidence;      // 总体置信度
    std::vector<std::string> warnings;
    uint32_t detected_column_count;
    uint32_t detected_pk_column_count;
    bool has_auto_increment;
};

/**
 * @brief 统计分类器
 */
class StatisticalTypeClassifier {
public:
    /**
     * @brief 为单列特征计算所有可能类型的得分
     */
    std::vector<TypeCandidate> classify(const ColumnFeatures& features);
    
private:
    // 各类型的似然函数
    double likelihood_tinyint(const ColumnFeatures& f);
    double likelihood_smallint(const ColumnFeatures& f);
    double likelihood_int(const ColumnFeatures& f);
    double likelihood_bigint(const ColumnFeatures& f);
    double likelihood_float(const ColumnFeatures& f);
    double likelihood_double(const ColumnFeatures& f);
    double likelihood_decimal(const ColumnFeatures& f);
    double likelihood_char(const ColumnFeatures& f);
    double likelihood_varchar(const ColumnFeatures& f);
    double likelihood_text(const ColumnFeatures& f);
    double likelihood_blob(const ColumnFeatures& f);
    double likelihood_datetime(const ColumnFeatures& f);
    double likelihood_date(const ColumnFeatures& f);
    double likelihood_time(const ColumnFeatures& f);
    double likelihood_timestamp(const ColumnFeatures& f);
    double likelihood_enum(const ColumnFeatures& f);
    double likelihood_set(const ColumnFeatures& f);
    double likelihood_json(const ColumnFeatures& f);
    double likelihood_year(const ColumnFeatures& f);
    double likelihood_bit(const ColumnFeatures& f);
    double likelihood_binary(const ColumnFeatures& f);
    double likelihood_varbinary(const ColumnFeatures& f);
};

/**
 * @brief 似然函数实现策略
 * 
 * 每个 likelihood_xxx() 函数使用特征向量计算加权得分:
 * 
 * TINYINT:
 *   - len_max <= 1 AND integer_ratio > 0.95 AND signed_ratio < 0.5
 *   - 如果 signed_ratio > 0.3，建议 TINYINT SIGNED
 * 
 * BIGINT:
 *   - 4 <= len_max <= 8 AND integer_ratio > 0.95
 *   - numeric_max > 2^32 → BIGINT UNSIGNED
 *   - signed_ratio > 0.3 → BIGINT SIGNED
 * 
 * VARCHAR(N):
 *   - ascii_ratio > 0.8 AND len_stddev > 0
 *   - N = min(len_max * 2, 255) 对于普通 VARCHAR
 *   - N = min(len_max * 2, 65535) 对于 TEXT
 *   - 如果 len_max > 255，建议 TEXT
 * 
 * DATETIME:
 *   - len_mean ≈ 19 AND has_dash_ratio > 0.8 AND has_colon_ratio > 0.8
 *   - 模式匹配: YYYY-MM-DD HH:MM:SS
 * 
 * DATE:
 *   - len_mean ≈ 10 AND has_dash_ratio > 0.8 AND has_colon_ratio < 0.1
 *   - 模式匹配: YYYY-MM-DD
 * 
 * ENUM:
 *   - len_stddev < 2 AND repeated_pattern_score > 0.7
 *   - 唯一值数量少 (distinct_count < 20)
 *   - ascii_ratio > 0.9
 * 
 * JSON:
 *   - len_stddev > 5 AND (has_brace OR has_bracket) AND entropy > 3.0
 *   - 模式匹配: 以 { 或 [ 开头
 * 
 * BLOB:
 *   - entropy > 6.0 AND ascii_ratio < 0.5
 *   - 字节直方图分布均匀
 */

} // namespace dbrecovery::inference
```

### 3.4 物理布局分析器

```cpp
/**
 * @brief 从原始数据页分析物理列布局
 * 
 * 不依赖元数据，通过分析 COMPACT/DYNAMIC 行格式的物理结构
 * 来确定列数、NULL bitmap 大小和每列的大致类型。
 */
class PhysicalLayoutAnalyzer {
public:
    struct LayoutInfo {
        uint32_t total_columns;         // 含系统列的总列数
        uint32_t user_columns;          // 用户列数 (= total - 3)
        uint32_t null_bitmap_bytes;     // NULL bitmap 字节数
        uint32_t fixed_size_total;      // 固定长度部分总字节数
        std::vector<uint32_t> var_len_offsets; // 变长字段偏移
        bool is_compact_format;         // COMPACT 还是 REDUNDANT
    };
    
    /**
     * @brief 分析单页的物理布局
     * 
     * 算法:
     * 1. 读取页目录，找到所有记录偏移
     * 2. 对每条记录:
     *    a. 读取 offset table (反向)
     *    b. 计算 NULL bitmap 大小 = ceil((offset_count - 3) / 8)
     *    c. 定位 TRX_ID (6B) + ROLL_PTR (7B) = 13B 固定开销
     *    d. 分析固定长度部分和变长部分的边界
     * 3. 汇总所有记录的布局信息，投票决定最可能的列数
     */
    LayoutInfo analyze_page(const uint8_t* page_data, size_t page_size);
    
    /**
     * @brief 分析多个页，投票确定最终布局
     */
    LayoutInfo analyze_multiple_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);
};
```

### 3.5 交互确认流程

```cpp
/**
 * @brief 交互式 Schema 构建器
 * 
 * 允许用户以交互方式确认或微调推断结果
 */
class InteractiveSchemaBuilder {
public:
    /**
     * @brief 启动交互式 Schema 构建
     * 
     * 流程:
     * 1. 显示推断的列数和每列的 Top-3 候选类型（含置信度百分比）
     * 2. 用户可以:
     *    a. 接受全部推荐 → 直接生成 DDL
     *    b. 修改某一列的类型 → 重新生成 DDL
     *    c. 指定列名 → 替换自动生成的 col_1, col_2, ...
     *    d. 指定主键 → 标记主键列
     *    e. 指定字符集 → 设置表级/列级字符集
     *    f. 添加额外列 → 手动添加推断遗漏的列
     * 3. 每次修改后显示更新后的 CREATE TABLE 语句
     * 4. 用户确认后，生成最终的 schema JSON 文件
     * 
     * CLI 输出示例:
     * 
     * ┌─ Schema Inference Results (confidence: 87.3%) ─┐
     * │ Table: recovered_table (12 columns detected)    │
     * │                                                  │
     * │ C1  [95%] BIGINT UNSIGNED    (auto_increment)   │
     * │ C2  [88%] VARCHAR(64)        (user_001 pattern) │
     * │ C3  [82%] VARCHAR(128)       (email pattern)    │
     * │ C4  [91%] INT UNSIGNED       (range: 0-99999)   │
     * │ C5  [79%] DECIMAL(10,2)      (XXX.XX pattern)   │
     * │ C6  [97%] DATETIME           (ISO format)       │
     * │ C7  [85%] VARCHAR(255)       (mixed text)       │
     * │ C8  [73%] ENUM('active','inactive','pending')   │
     * │ C9  [66%] TINYINT            (0/1 values)       │
     * │ C10 [94%] TIMESTAMP          (unix timestamps)  │
     * │ C11 [60%] TEXT               (long text, mixed) │
     * │ C12 [55%] JSON               (starts with {)    │
     * │                                                  │
     * │ Proposed DDL saved to: ./recovered_table.sql    │
     * │ Commands: a(accept) e(edit) r(reject) q(quit)   │
     * └──────────────────────────────────────────────────┘
     */
    InferenceResult run_interactive(const InferenceResult& initial);
    
private:
    void display_candidates(const InferenceResult& result);
    bool handle_user_input(const std::string& cmd, InferenceResult& result);
    std::string generate_ddl(const InferenceResult& result);
};
```

### 3.6 DDL 变更链检测

```cpp
/**
 * @brief DDL 变更链检测器
 * 
 * 检测同一个表空间中可能存在多个行格式版本的情况。
 * 这通常发生在:
 * 1. INSTANT ADD COLUMN (MySQL 8.0+)
 * 2. ALTER TABLE ... ADD/DROP COLUMN (重建表之前)
 * 3. 表空间碎片中的旧版本数据
 * 
 * 检测方法:
 * 1. 扫描数据页，对每条记录的列数进行统计
 * 2. 如果发现多个不同的列数，可能存在多个版本
 * 3. 对每个版本独立推断 schema
 * 4. 让用户选择使用哪个版本的 schema 或尝试自动合并
 */
class DDLChangeChainDetector {
public:
    struct SchemaVersion {
        uint32_t column_count;
        uint32_t page_count;        // 使用此版本列数的页数
        std::vector<uint32_t> page_numbers;
        InferredSchema inferred_schema;
    };
    
    std::vector<SchemaVersion> detect_versions(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);
};
```

### 3.7 CLI 接口设计

```bash
# 自动推断模式
mysql_recovery_pro --ibd /path/to/table.ibd \
    --infer-schema \
    --sample-pages 200 \
    --sample-rows 5000 \
    --output /path/to/output/

# 交互式推断
mysql_recovery_pro --ibd /path/to/table.ibd \
    --infer-schema \
    --interactive

# 指定已知约束 (提高推断精度)
mysql_recovery_pro --ibd /path/to/table.ibd \
    --infer-schema \
    --known-pk "id" \
    --known-types "col3:DATETIME,col5:DECIMAL(10,2)" \
    --charset utf8mb4

# 检测 DDL 变更链
mysql_recovery_pro --ibd /path/to/table.ibd \
    --detect-ddl-changes
```

---

## 4. 抢救性恢复引擎

### 4.1 理论背景

传统恢复方法在页头损坏时直接放弃整个页 (16384 字节)。但对于用户来说，即使能救回一行也是巨大的价值。抢救性恢复的核心思想是：**放弃结构化的页解析，转而使用启发式逐行扫描**。

InnoDB 数据行在页内的物理特征:
- COMPACT 格式: 每条记录以反向 offset table 条目开始，后面跟 NULL bitmap
- REDUNDANT 格式: 每条记录以 6 字节定长头开始
- 记录的第一个字段通常是主键 (整数或字符串)
- 有效的记录头 (info_bits + next_offset) 有特定模式
- 记录的字段值通常落在合理的范围内

### 4.2 架构设计

```
┌──────────────────────────────────────────────────────────────────┐
│                 SalvageRecoveryEngine (新增)                      │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌───────────────────┐  ┌─────────────────┐  ┌───────────────┐  │
│  │ RowSalvageScanner │  │ SecondaryIndex  │  │ DataCorrelation│  │
│  │ (逐行抢救扫描器)   │  │ Rescuer         │  │ Engine         │  │
│  │                   │  │ (二级索引抢救)   │  │ (数据关联引擎) │  │
│  │ - scan_raw()      │  │ - extract_pk()  │  │ - correlate()  │  │
│  │ - find_records()  │  │ - extract_sec() │  │ - merge()      │  │
│  │ - validate_row()  │  │ - build_pk_map()│  │ - dedup()      │  │
│  └────────┬──────────┘  └────────┬────────┘  └───────┬───────┘  │
│           │                      │                    │          │
│  ┌────────┴──────────────────────┴────────────────────┴────────┐  │
│  │                  DamagedPageHandler                          │  │
│  │  - 页头损坏程度分级 (Level 0-4)                               │  │
│  │  - 分级处理策略选择                                           │  │
│  │  - 损坏日志记录                                               │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 4.3 页损坏程度分级

```
Level 0: 完全健康
  - FIL header OK, Page header OK, checksum OK
  → 使用正常 B+Tree 解析

Level 1: 轻微损坏
  - FIL header OK, checksum OK, 但 page header 部分字段损坏
  - 例如: PAGE_N_RECS 不正确, PAGE_HEAP_TOP 偏差
  → 使用页目录槽位扫描, 忽略 page header 统计

Level 2: 中度损坏
  - FIL header 部分损坏 (page type 仍可识别)
  - checksum 不匹配但 page type = FIL_PAGE_INDEX
  - 页目录可能损坏
  → 使用逐行扫描模式

Level 3: 严重损坏
  - FIL header 完全损坏, 无法识别页类型
  - 但页面中仍有连续的记录结构
  → 使用裸记录模式扫描 (见下文算法)

Level 4: 灾难性损坏
  - 页面大部分被覆盖或全零
  - 仅能识别少量数据片段
  → 使用最大努力数据提取, 输出占位符
```

### 4.4 逐行抢救扫描算法

```
Algorithm: salvage_scan_page(page_data, page_size, damage_level, schema_hint)
Input:  页数据, 页大小, 损坏级别, 可选的 schema 提示
Output: 抢救出的记录列表 + 损坏日志

1. records ← []
2. damage_log ← []
3. 
4. SWITCH damage_level:
5. 
6. CASE Level 2 (中度损坏, 页目录可能损坏):
7.     // 从页的 heap 区域开始线性扫描
8.     // COMPACT 格式: heap 区域从 FIL_HEADER+PAGE_HEADER 之后开始
9.     scan_start ← FIL_HEADER_SIZE + PAGE_HEADER_SIZE  // ≈ 38 + 56 = 94
10.    
11.    // 首先找到 INFIMUM 和 SUPREMUM 伪记录
12.    infimum_pos ← scan_start
13.    supremum_pos ← find_supremum(page_data, scan_start, page_size)
14.    
15.    IF supremum_pos == NOT_FOUND:
16.        supremum_pos ← page_size - 8  // 猜测 SUPREMUM 在页尾附近
17.    
18.    pos ← infimum_pos + 5  // 跳过 INFIMUM (5字节 COMPACT 头)
19.    
20.    WHILE pos < supremum_pos AND pos < page_size - 20:
21.        record ← try_parse_compact_record(page_data, pos, schema_hint)
22.        
23.        IF record.valid:
24.            records.push_back(record)
25.            pos ← record.next_offset  // 从记录头获取
26.        ELSE:
27.            // 记录解析失败, 尝试前进到下一个可能的记录
28.            next_pos ← find_next_likely_record(page_data, pos, page_size)
29.            IF next_pos == NOT_FOUND:
30.                BREAK
31.            damage_log.push_back({pos, "skipped corrupted bytes", next_pos - pos})
32.            pos ← next_pos
33. 
34. CASE Level 3 (严重损坏, 裸记录扫描):
35.     // 完全放弃结构化假设, 扫描整个页面寻找记录特征
36.     pos ← 0
37.     WHILE pos < page_size - 20:
38.         // 尝试在当前位置解析记录
39.         record ← try_parse_record_heuristic(page_data, pos, schema_hint)
40.         
41.         IF record.valid AND record.confidence > 0.5:
42.             records.push_back(record)
43.             pos ← pos + record.estimated_size
44.         ELSE:
45.             pos ← pos + 1  // 逐字节前进
46.             
47.             // 如果前进太多没有找到记录, 尝试更大步长
48.             IF consecutive_failures > 100:
49.                 pos ← pos + 16  // 跳过大块损坏区域
50.                 damage_log.push_back({pos, "large corruption gap", 16})
51.                 consecutive_failures ← 0
52. 
53. CASE Level 4 (灾难性损坏, 片段提取):
54.     // 查找任何可能的数据片段
55.     fragments ← extract_data_fragments(page_data, page_size, schema_hint)
56.     FOR EACH fragment IN fragments:
57.         damage_log.push_back({fragment.pos, "partial data fragment", fragment.size})
58.         records.push_back(create_placeholder_record(fragment))
59. 
60. RETURN {records, damage_log}

Algorithm: try_parse_record_heuristic(page_data, pos, schema_hint)
Input:  页数据, 起始位置, schema 提示
Output: 解析结果 (valid + confidence)

1. result ← RecordParseResult()
2. result.valid ← false
3. result.confidence ← 0.0
4. 
5. // 检查 1: offset table 模式 (COMPACT 格式特征)
6. // COMPACT 记录的 offset table 是反向的 2-byte 偏移数组
7. // 第一个偏移应该指向记录起始位置
8. offset_table_start ← pos
9. first_offset ← read_le_uint16(page_data, offset_table_start)
10. 
11. IF first_offset > pos AND first_offset < page_size:
12.     // 检查第二个偏移是否也合理 (递减)
13.     second_offset ← read_le_uint16(page_data, offset_table_start + 2)
14.     IF second_offset > 0 AND second_offset < first_offset:
15.         result.confidence ← result.confidence + 0.3
16. 
17. // 检查 2: NULL bitmap 模式
18. // NULL bitmap 字节的高位 bit 通常不是全 1
19. null_bitmap_pos ← pos + 2  // 跳过 offset table 的第一个条目
20. null_byte ← page_data[null_bitmap_pos]
21. IF null_byte != 0xFF:  // 全 1 的 bitmap 很少见
22.     result.confidence ← result.confidence + 0.1
23. 
24. // 检查 3: info_bits 有效性
25. // info_bits 只有低 5 位有意义, 高 3 位必须为 0
26. info_pos ← ...  // 根据格式计算
27. info_byte ← page_data[info_pos]
28. IF (info_byte & 0xE0) == 0:  // 高 3 位为 0
29.     result.confidence ← result.confidence + 0.2
30. 
31. // 检查 4: 如果有 schema 提示, 验证第一个字段是否像主键
32. IF schema_hint.has_pk_type:
33.     pk_value ← extract_possible_pk(page_data, pos, schema_hint)
34.     IF pk_value.is_plausible:
35.         result.confidence ← result.confidence + 0.2
36. 
37. // 检查 5: 记录长度合理
38. estimated_len ← estimate_record_length(page_data, pos)
39. IF estimated_len >= 10 AND estimated_len <= page_size - pos:
40.     result.confidence ← result.confidence + 0.1
41. 
42. result.valid ← (result.confidence >= 0.5)
43. IF result.valid:
44.     result.data ← extract_record_data(page_data, pos, estimated_len)
45.     result.estimated_size ← estimated_len
46. 
47. RETURN result
```

### 4.5 辅助索引反查算法

```
Algorithm: secondary_index_rescue(secondary_ibd_path, primary_ibd_path, schema)
Input:  辅助索引 IBD 文件, 聚集索引 IBD 文件, 表结构
Output: 通过辅助索引拼凑出的数据记录

1. // 第一步: 从辅助索引叶子页提取数据
2. sec_records ← []
3. FOR EACH leaf_page IN secondary_ibd:
4.     FOR EACH record IN leaf_page:
5.         sec_rec ← SecondaryIndexRecord()
6.         
7.         // 辅助索引记录格式: [索引列值...] [主键列值...]
8.         // 根据索引定义确定字段边界
9.         offset ← 0
10.        FOR EACH index_col IN schema.secondary_indexes[target_index]:
11.            value, size ← decode_field(record.data, offset, index_col.type)
12.            sec_rec.index_values.push_back(value)
13.            offset ← offset + size
14.        
15.        // 剩余部分是主键值
16.        pk_offset ← offset
17.        FOR EACH pk_col IN schema.primary_key:
18.            value, size ← decode_field(record.data, pk_offset, pk_col.type)
19.            sec_rec.pk_values.push_back(value)
20.            pk_offset ← pk_offset + size
21.        
22.        sec_records.push_back(sec_rec)
23. 
24. // 第二步: 构建主键 → 索引值的映射
25. pk_to_index_values ← Map<PK, Vector<IndexValues>>()
26. FOR EACH sec_rec IN sec_records:
27.     pk_key ← serialize_pk(sec_rec.pk_values)
28.     pk_to_index_values[pk_key].push_back(sec_rec.index_values)
29. 
30. // 第三步: 尝试从聚集索引中查找匹配行
31. results ← []
32. FOR EACH (pk_key, index_values_list) IN pk_to_index_values:
33.     // 尝试在聚集索引中查找
34.     cluster_record ← search_clustered_index(primary_ibd, pk_key)
35.     
36.     IF cluster_record.found:
37.         // 找到了完整行, 直接使用
38.         results.push_back(cluster_record)
39.     ELSE:
40.         // 聚集索引中缺失, 用辅助索引数据构建部分行
41.         partial_row ← PartialRow()
42.         partial_row.primary_key ← pk_key
43.         
44.         // 填充辅助索引中有的列值
45.         FOR EACH index_values IN index_values_list:
46.             FOR j, col IN enumerate(schema.secondary_indexes[target_index].columns):
47.                 partial_row.fields[col.name] ← index_values[j]
48.         
49.         // 其他列标记为 NULL
50.         FOR EACH col IN schema.columns:
51.             IF col.name NOT IN partial_row.fields:
52.                 partial_row.fields[col.name] ← NULL
53.                 partial_row.missing_fields.insert(col.name)
54.         
55.         partial_row.source ← "secondary_index_only"
56.         results.push_back(partial_row)
57. 
58. // 第四步: 去重和排序
59. results ← deduplicate_and_sort(results)
60. 
61. RETURN results
```

### 4.6 CLI 接口设计

```bash
# 抢救性恢复 (页头损坏时)
mysql_recovery_pro --ibd /path/to/corrupted.ibd \
    --salvage-mode \
    --damage-threshold 0.5 \
    --schema-file /path/to/schema.json \
    --output /path/to/salvaged/ \
    --damage-log /path/to/damage_report.log

# 辅助索引反查 (聚集索引损坏时)
mysql_recovery_pro --ibd /path/to/corrupted_primary.ibd \
    --secondary-index /path/to/secondary_idx.ibd \
    --secondary-rescue \
    --schema-file /path/to/schema.json \
    --output /path/to/rescued/

# 全自动抢救 (尝试所有策略)
mysql_recovery_pro --ibd /path/to/corrupted.ibd \
    --auto-salvage \
    --all-indexes /path/to/data/dir/ \
    --output /path/to/output/
```

---

## 5. 极端磁盘雕刻器 v2.0

### 5.1 理论背景

当文件系统元数据完全丢失 (如 `rm -rf` 后大量覆写)，文件系统感知雕刻 (ext4 inode 遍历 / XFS extent btree 遍历) 可能无法找到所有数据。此时必须在裸磁盘扇区上进行"深空雕刻"。

当前实现已支持基础的裸盘扫描 (raw_disk_carver.cpp 1581 行)，但缺少一个关键能力：**碎片重组**。当文件系统删除文件后，InnoDB 页可能散布在磁盘的任意位置。v2.0 需要将这些物理上不连续的页，按 SPACE_ID 和 PAGE_NO 重组成逻辑上连续的表空间。

### 5.2 架构设计

```
┌──────────────────────────────────────────────────────────────────┐
│                ExtremeDiskCarver v2.0 (增强)                      │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌───────────────────┐  ┌─────────────────┐  ┌───────────────┐  │
│  │ DeepSpaceScanner  │  │ PageFragment    │  │ Tablespace    │  │
│  │ (深空扫描器)       │  │ Reassembler     │  │ Composer      │  │
│  │                   │  │ (碎片重组器)     │  │ (表空间合成器) │  │
│  │ - scan_sectors()  │  │ - group_by_sid()│  │ - compose_ibd()│  │
│  │ - detect_page()   │  │ - sort_by_pno() │  │ - fill_gaps()  │  │
│  │ - verify_header() │  │ - detect_gaps() │  │ - write_file() │  │
│  │ - extract_page()  │  │ - fill_blanks() │  │ - export_data()│  │
│  └────────┬──────────┘  └────────┬────────┘  └───────┬───────┘  │
│           │                      │                    │          │
│  ┌────────┴──────────────────────┴────────────────────┴────────┐  │
│  │               PageIdentityEngine (增强)                       │  │
│  │  - SPACE_ID 提取 (FIL header offset 34, 4B)                  │  │
│  │  - PAGE_NO 提取 (FIL header offset 4, 4B)                    │  │
│  │  - PAGE_TYPE 提取 (FIL header offset 24, 2B)                 │  │
│  │  - LSN 提取 (FIL header offset 16, 8B)                       │  │
│  │  - checksum 验证 (CRC32-C / InnoDB legacy)                   │  │
│  │  - 压缩页检测 (FIL_PAGE_COMPRESSED, LZ4 解压)                │  │
│  │  - TDE 加密页检测和标记                                       │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 5.3 碎片重组算法

```
Algorithm: reassemble_tablespace(identified_pages)
Input:  已识别的 InnoDB 页列表 (每页含 SPACE_ID, PAGE_NO, 页数据)
Output: 重组后的逻辑表空间

1. // 第一步: 按 SPACE_ID 分组
2. space_groups ← Map<SPACE_ID, Vector<Page>>()
3. FOR EACH page IN identified_pages:
4.     space_groups[page.space_id].push_back(page)
5. 
6. // 第二步: 对每个 SPACE_ID 组，检测并去重
7. FOR EACH (space_id, pages) IN space_groups:
8.     // 按 PAGE_NO 分组，处理重复页
9.     page_map ← Map<PAGE_NO, Vector<Page>>()
10.    FOR EACH page IN pages:
11.        page_map[page.page_no].push_back(page)
12.    
13.    // 对每个 PAGE_NO，选择最佳版本
14.    best_pages ← Map<PAGE_NO, Page>()
15.    FOR EACH (page_no, candidates) IN page_map:
16.        IF candidates.size() == 1:
17.            best_pages[page_no] ← candidates[0]
18.        ELSE:
19.            // 多个候选: 选择 checksum 正确、LSN 最新的版本
20.            best_pages[page_no] ← select_best_candidate(candidates)
21.    
22.    // 第三步: 按 PAGE_NO 排序
23.    sorted_pages ← sort(best_pages.values(), by: page_no)
24.    
25.    // 第四步: 检测间隙
26.    gaps ← []
27.    FOR i ← 0 TO sorted_pages.size() - 2:
28.        expected_next ← sorted_pages[i].page_no + 1
29.        actual_next ← sorted_pages[i+1].page_no
30.        IF actual_next > expected_next:
31.            FOR missing_pno ← expected_next TO actual_next - 1:
32.                gaps.push_back({space_id, missing_pno})
33.    
34.    // 第五步: 分析缺失页的影响
35.    // 如果缺失的是:
36.    //   - Page 0 (FSP_HDR): 影响严重，缺少 space flags、page size 等
37.    //   - Page 1 (IBUF_BITMAP): 影响小
38.    //   - Page 2 (INODE): 影响中等，缺少文件段信息
39.    //   - Page 3 (SDI): 缺少元数据 (MySQL 8.0+)
40.    //   - 数据页: 影响数据完整性
41.    gap_analysis ← analyze_gap_impact(gaps, sorted_pages)
42.    
43.    // 第六步: 生成填充页
44.    filled_pages ← []
45.    current_pno ← 0
46.    FOR EACH page IN sorted_pages:
47.        WHILE current_pno < page.page_no:
48.            // 生成占位页 (全零或特定标记)
49.            placeholder ← create_placeholder_page(space_id, current_pno)
50.            filled_pages.push_back(placeholder)
51.            current_pno ← current_pno + 1
52.        
53.        filled_pages.push_back(page)
54.        current_pno ← page.page_no + 1
55.    
56.    // 第七步: 写入合成的 .ibd 文件
57.    output_path ← "recovered_space_" + to_string(space_id) + ".ibd"
58.    write_composed_ibd(output_path, filled_pages, gap_analysis)
59.    
60.    // 第八步: 记录重组报告
61.    reassembly_report ← {
62.        space_id: space_id,
63.        total_pages_found: sorted_pages.size(),
64.        total_gaps: gaps.size(),
65.        critical_gaps: count_critical_gaps(gap_analysis),
66.        completeness: sorted_pages.size() / (sorted_pages.size() + gaps.size()),
67.        data_pages_found: count_data_pages(sorted_pages),
68.        index_pages_found: count_index_pages(sorted_pages),
69.        estimated_row_count: estimate_rows(sorted_pages),
70.    }
71. 
72. RETURN {composed_files, reassembly_reports}

Algorithm: select_best_candidate(candidates)
Input:  同一 PAGE_NO 的多个候选页
Output: 最佳候选

1. // 优先级 1: checksum 正确的优先
2. valid_checksum ← filter(candidates, c → verify_checksum(c))
3. IF valid_checksum.size() > 0:
4.     candidates ← valid_checksum
5. 
6. // 优先级 2: LSN 最新的优先 (数据最新)
7. sort(candidates, by: LSN, descending: true)
8. 
9. // 优先级 3: 非全零页优先
10. FOR EACH candidate IN candidates:
11.     IF !is_all_zero(candidate.data):
12.         RETURN candidate
13. 
14. RETURN candidates[0]
```

### 5.4 页身份识别增强

```cpp
/**
 * @brief 增强的 InnoDB 页识别引擎
 * 
 * 在裸盘扫描中，不仅识别页边界，还要确认页的"身份"：
 * - 属于哪个表空间 (SPACE_ID)
 * - 在表空间中的位置 (PAGE_NO)
 * - 页的类型 (INDEX/UNDO/SDI/BLOB/FSP_HDR/INODE)
 * - 压缩状态
 * - 加密状态
 * - 数据新鲜度 (LSN)
 */
class PageIdentityEngine {
public:
    struct PageIdentity {
        uint32_t space_id;
        uint32_t page_no;
        uint16_t page_type;
        uint64_t lsn;
        uint32_t checksum;
        bool checksum_valid;
        bool is_compressed;
        bool is_encrypted;
        uint32_t original_size;     // 压缩前的原始大小
        uint64_t disk_offset;       // 在磁盘上的物理偏移
        std::string page_type_name; // 人类可读的页类型名
    };
    
    /**
     * @brief 从磁盘扇区数据识别 InnoDB 页
     * 
     * 扫描策略:
     * 1. 512 字节步进扫描 (非对齐页检测)
     * 2. 每个位置检查 FIL header 特征:
     *    a. checksum 有效性 (CRC32-C 和 InnoDB legacy)
     *    b. page_type 有效性 (已知的 FIL_PAGE_* 值)
     *    c. SPACE_ID 合理性 (非全零, 非全FF)
     *    d. PAGE_NO 合理性 (相对于同一 SPACE_ID 的其他页)
     * 3. 压缩页检测: 检查压缩页头特征 (FIL_PAGE_COMPRESSED)
     * 4. 加密页检测: 检查 TDE 头特征
     */
    std::vector<PageIdentity> identify_pages(
        const std::vector<uint8_t>& disk_image,
        const ScanConfig& config);
    
    /**
     * @brief 验证页身份的一致性
     * 
     * 检查同一 SPACE_ID 的页集合是否自洽:
     * - PAGE_NO 不重复 (去重)
     * - 必须有 Page 0 (FSP_HDR)
     * - 页链的一致性 (FIL_PAGE_PREV / FIL_PAGE_NEXT)
     * - 索引页的 INDEX_ID 是否一致
     */
    IdentityVerification verify_identity_consistency(
        const std::vector<PageIdentity>& identities);
};

struct ScanConfig {
    uint32_t sector_size = 512;
    uint32_t step_size = 512;       // 扫描步长
    bool check_checksum = true;
    bool detect_compression = true;
    bool detect_encryption = true;
    bool deep_scan = false;         // 尝试所有偏移 (更慢但更全)
    uint64_t max_disk_size = 0;     // 0 = 自动检测
    uint32_t max_pages = 0;         // 0 = 无限制
};
```

### 5.5 CLI 接口设计

```bash
# 裸盘深空雕刻 + 碎片重组
mysql_recovery_pro --disk /dev/sdb \
    --deep-carve \
    --reassemble \
    --output /path/to/recovered_tablespaces/ \
    --sector-size 512 \
    --deep-scan \
    --skip-checksum  # 如果磁盘有 bit rot

# 从磁盘镜像恢复
mysql_recovery_pro --disk /path/to/disk_image.img \
    --deep-carve \
    --reassemble \
    --target-space-id 1234 \
    --output /path/to/output/

# 仅扫描识别，不恢复
mysql_recovery_pro --disk /dev/sdb \
    --deep-carve \
    --scan-only \
    --output /path/to/page_map.json
```

---

## 6. 全场景覆盖增强

### 6.1 分区表支持

```cpp
/**
 * @brief 分区表恢复器
 * 
 * 自动识别分区结构并聚合恢复
 */
class PartitionRecovery {
public:
    /**
     * @brief 检测分区表
     * 
     * 通过以下方式识别:
     * 1. SDI 中的 partition_info 字段
     * 2. 文件命名模式: table#P#p0.ibd, table#P#p1.ibd
     * 3. 每个分区的独立 SPACE_ID
     */
    struct PartitionInfo {
        std::string partition_name;
        std::string partition_method;   // RANGE/LIST/HASH/KEY
        std::string partition_expression;
        std::vector<std::string> partition_values;
        uint32_t space_id;
        std::string ibd_file_path;
    };
    
    std::vector<PartitionInfo> detect_partitions(const std::string& data_dir,
                                                   const std::string& table_name);
    
    /**
     * @brief 恢复单个分区
     */
    RecoveryResult recover_partition(const PartitionInfo& partition,
                                      const catalog::TableSchema& schema);
    
    /**
     * @brief 聚合恢复所有分区
     * 
     * 将所有分区的数据合并到一个输出中
     */
    RecoveryResult recover_all_partitions(
        const std::vector<PartitionInfo>& partitions,
        const catalog::TableSchema& schema);
};
```

### 6.2 共享/通用表空间支持

```cpp
/**
 * @brief 共享表空间恢复器
 * 
 * 处理 ibdata1 及 CREATE TABLESPACE 创建的文件
 */
class SharedTablespaceRecovery {
public:
    /**
     * @brief 从共享表空间中提取目标表的数据
     * 
     * 算法:
     * 1. 解析 ibdata1 的 SDI 页 (MySQL 8.0+)
     *    或从 mysql.innodb_table_stats 推断 (MySQL 5.7)
     * 2. 找到目标表的 SPACE_ID
     * 3. 在共享表空间中定位属于该 SPACE_ID 的页
     * 4. 提取数据页并重组为逻辑 IBD
     */
    RecoveryResult extract_table_from_shared(
        const std::string& shared_tablespace_path,
        uint32_t target_space_id,
        const catalog::TableSchema& schema);
    
    /**
     * @brief 列出共享表空间中的所有表
     */
    std::vector<TableIdentity> list_tables_in_shared(
        const std::string& shared_tablespace_path);
};
```

### 6.3 TDE 加密增强

```cpp
/**
 * @brief 离线 Keyring 解析器
 * 
 * 支持多种 keyring 后端:
 * - keyring_file (JSON 格式)
 * - keyring_encrypted_file (加密的 keyring)
 * - keyring_vault (HashiCorp Vault, 离线模式)
 * - keyring_aws (AWS KMS, 离线模式)
 */
class OfflineKeyringParser {
public:
    struct MasterKey {
        uint32_t key_id;
        std::vector<uint8_t> key_data;
        std::string key_type;   // "AES", "DSA", "RSA"
        std::string algorithm;  // "AES-256-CBC", "AES-256-GCM"
    };
    
    /**
     * @brief 解析 keyring_file
     */
    std::vector<MasterKey> parse_keyring_file(const std::string& path);
    
    /**
     * @brief 解析加密的 keyring 文件
     */
    std::vector<MasterKey> parse_encrypted_keyring(
        const std::string& path, const std::string& password);
    
    /**
     * @brief 从 master key 推导表级加密密钥
     * 
     * InnoDB TDE 密钥派生:
     *   table_key = AES_ENCRYPT(master_key, space_id || page_no || LSN)
     *   或
     *   table_key = HMAC_SHA256(master_key, space_id)
     * 
     * 具体派生方式取决于 MySQL 版本和 keyring 插件。
     */
    std::vector<uint8_t> derive_table_key(
        const MasterKey& master_key,
        uint32_t space_id,
        uint32_t page_no);
};
```

### 6.4 CLI 接口汇总

```bash
# 分区表恢复
mysql_recovery_pro --data-dir /path/to/mysql/data \
    --table-name mydb.partitioned_table \
    --partition-recovery \
    --output /path/to/output/

# 指定分区恢复
mysql_recovery_pro --ibd "mydb/part_table#P#p202401.ibd" \
    --partition-name p202401 \
    --output /path/to/output/

# 共享表空间提取
mysql_recovery_pro --ibdata /path/to/ibdata1 \
    --extract-table mydb.mytable \
    --output /path/to/output/

# 列出共享表空间中的表
mysql_recovery_pro --ibdata /path/to/ibdata1 \
    --list-tables

# TDE 加密恢复
mysql_recovery_pro --ibd /path/to/encrypted.ibd \
    --keyring-file /path/to/keyring.json \
    --tde-password "master_key_password" \
    --output /path/to/output/
```

---

## 7. 恢复报告系统

### 7.1 报告结构

```cpp
/**
 * @brief 专业恢复报告
 * 
 * 每次恢复完成后自动生成，包含:
 * - 执行摘要
 * - 数据统计
 * - 恢复策略和参数
 * - 损坏分析
 * - 无法恢复行的原因
 * - 重建的 DDL
 */
struct RecoveryReport {
    // === 执行摘要 ===
    std::string report_id;              // UUID
    std::chrono::system_clock::time_point report_time;
    std::string recovery_type;          // "DROP_TABLE", "TRUNCATE", "RM_RF", "CORRUPTION"
    std::string mysql_version;
    std::string overall_status;         // "SUCCESS", "PARTIAL", "FAILED"
    
    // === 输入信息 ===
    struct InputInfo {
        std::string source_type;        // "IBD_FILE", "DISK_DEVICE", "DATA_DIR"
        std::string source_path;
        uint64_t source_size_bytes;
        bool has_schema;                // 是否有 schema 元数据
        std::string schema_source;      // "SDI", "FRM", "INFERRED", "MANUAL", "NONE"
        bool has_undo_logs;
        bool has_redo_logs;
        std::vector<std::string> files_processed;
    } input;
    
    // === 恢复策略 ===
    struct StrategyInfo {
        std::vector<std::string> strategies_attempted;
        std::string primary_strategy;
        std::vector<std::string> fallback_strategies;
        std::map<std::string, std::string> parameters;
    } strategy;
    
    // === 数据统计 ===
    struct DataStats {
        uint64_t total_pages_scanned;
        uint64_t index_pages_found;
        uint64_t blob_pages_found;
        uint64_t corrupted_pages;
        uint64_t skipped_pages;
        uint64_t total_records_attempted;
        uint64_t records_fully_recovered;
        uint64_t records_partially_recovered;
        uint64_t records_failed;
        double recovery_rate_percent;
        uint64_t blob_records_recovered;
        uint64_t history_versions_recovered;   // undo 版本
        uint64_t flashback_statements_generated;
        std::map<std::string, uint64_t> records_by_type; // DROP/TRUNCATE/DELETE
    } data;
    
    // === 损坏分析 ===
    struct DamageAnalysis {
        uint32_t pages_with_header_damage;
        uint32_t pages_with_checksum_mismatch;
        uint32_t pages_with_record_corruption;
        uint32_t pages_with_blob_corruption;
        std::vector<DamageDetail> details;
    } damage;
    
    // === 失败原因分析 ===
    struct FailureAnalysis {
        struct FailureCategory {
            std::string category;       // "PAGE_HEADER_DESTROYED", "RECORD_OVERWRITTEN", etc.
            uint64_t count;
            double percentage;
            std::string recommendation;
        };
        std::vector<FailureCategory> categories;
    } failures;
    
    // === 重建的 DDL ===
    std::vector<std::string> reconstructed_ddl;
    
    // === 输出 ===
    struct OutputInfo {
        std::string output_directory;
        std::vector<std::string> output_files;
        uint64_t total_output_size_bytes;
        std::string format;             // "CSV", "SQL", "JSON", "IBD"
    } output;
    
    // === 时间统计 ===
    struct TimingInfo {
        double scan_time_seconds;
        double parse_time_seconds;
        double export_time_seconds;
        double total_time_seconds;
    } timing;
    
    /**
     * @brief 生成 JSON 报告
     */
    std::string to_json() const;
    
    /**
     * @brief 生成人类可读的文本报告
     */
    std::string to_text() const;
    
    /**
     * @brief 生成 HTML 报告 (含可视化图表)
     */
    std::string to_html() const;
};
```

### 7.2 CLI 接口

```bash
# 生成详细报告
mysql_recovery_pro --ibd /path/to/table.ibd \
    --output /path/to/output/ \
    --report-format json,html,text \
    --report-path /path/to/recovery_report.html

# 查看上次恢复的报告
mysql_recovery_pro --show-last-report
```

---

## 8. 优先级矩阵

### P0 (核心差异化, 立即启动)

| 功能 | 理由 | 复杂度 | 预计工作量 |
|------|------|--------|-----------|
| **深度 Undo 链遍历** | ODU 核心优势，我们没有 | 高 | 2-3 周 |
| **DELETE 闪回** | 用户最高频需求之一 | 高 | 1-2 周 |
| **逐行抢救扫描** | 页头损坏时唯一救命手段 | 中 | 1-2 周 |
| **碎片重组 (SPACE_ID+PAGE_NO)** | 裸盘雕刻的根本性增强 | 中 | 2-3 周 |
| **UndoPageManager 统一** | 现有 undo 实现分散且不完整 | 中 | 1 周 |

### P1 (高价值, 短期)

| 功能 | 理由 | 复杂度 | 预计工作量 |
|------|------|--------|-----------|
| **统计类型推断引擎** | 无元数据场景的核心能力 | 高 | 2-3 周 |
| **辅助索引反查** | 聚集索引损坏时的救命稻草 | 中 | 1-2 周 |
| **交互式 Schema 构建器** | 推断结果的用户确认和微调 | 中 | 1 周 |
| **专业恢复报告** | 工业级产品的必备 | 低 | 1 周 |

### P2 (重要, 中期)

| 功能 | 理由 | 复杂度 | 预计工作量 |
|------|------|--------|-----------|
| **分区表自动聚合** | 企业级部署常见 | 中 | 1-2 周 |
| **共享表空间提取** | ibdata1 场景 | 中 | 1-2 周 |
| **DDL 变更链检测** | INSTANT DDL 场景 | 中 | 1 周 |
| **Undo 片段启发式重组** | undo 页损坏时的兜底 | 高 | 1-2 周 |

### P3 (增强, 长期)

| 功能 | 理由 | 复杂度 | 预计工作量 |
|------|------|--------|-----------|
| **离线 Keyring 多后端** | TDE 加密场景 | 中 | 1-2 周 |
| **辅助索引 B+Tree 断裂重组** | 辅助索引本身损坏时 | 高 | 2-3 周 |
| **多版本数据并行导出** | 性能优化 | 中 | 1 周 |

### P4 (锦上添花)

| 功能 | 理由 | 复杂度 |
|------|------|--------|
| **HTML 可视化报告** | 用户体验 | 低 |
| **恢复策略推荐引擎** | 自动选择最佳策略 | 中 |
| **分布式恢复** | 超大磁盘并行扫描 | 高 |

---

## 9. 实施路线图

```
Phase 1 (Week 1-2): Undo 引擎重构
├── UndoPageManager 统一实现
├── UndoRecordV2 完整解析器
├── 基础 undo 链遍历 (trace_version_chain)
└── 单元测试

Phase 2 (Week 3-4): 闪回与历史版本
├── DELETE 闪回引擎 (flashback_deletes)
├── 历史版本重建 (rebuild_all_histories)
├── CLI --flashback-delete, --undo-history 参数
└── 集成测试 (MySQL 5.6-9.0)

Phase 3 (Week 5-6): 抢救性恢复
├── DamagedPageHandler (4 级损坏分类)
├── RowSalvageScanner (逐行抢救)
├── SecondaryIndexRescuer (辅助索引反查)
├── CLI --salvage-mode, --secondary-rescue
└── 损坏场景测试

Phase 4 (Week 7-9): 智能模式推断
├── PhysicalLayoutAnalyzer
├── StatisticalTypeClassifier (20 种类型似然函数)
├── InteractiveSchemaBuilder
├── DDLChangeChainDetector
├── CLI --infer-schema --interactive
└── 基准测试 (已知 schema 的恢复对比)

Phase 5 (Week 10-12): 极端磁盘雕刻 v2.0
├── PageIdentityEngine 增强
├── PageFragmentReassembler
├── TablespaceComposer
├── CLI --deep-carve --reassemble
└── 裸盘碎片重组测试

Phase 6 (Week 13-14): 全场景覆盖
├── PartitionRecovery
├── SharedTablespaceRecovery
├── OfflineKeyringParser 增强
└── 企业级场景测试

Phase 7 (Week 15-16): 报告系统与打磨
├── RecoveryReport (JSON/Text/HTML)
├── 恢复策略推荐
├── 性能优化 (并行扫描, 页缓存)
├── 文档和示例
└── 全面回归测试
```

---

## 附录 A: 文件结构变更计划

```
include/dbrecovery/
├── undo/
│   ├── deep_undo_analyzer.hpp      (新增: 深度 undo 分析)
│   ├── undo_page_manager.hpp       (新增: 统一 undo 页管理)
│   ├── flashback_engine.hpp        (新增: 闪回引擎)
│   └── undo_record_v2.hpp          (新增: 增强 undo 记录)
├── inference/
│   ├── statistical_classifier.hpp  (新增: 统计类型分类器)
│   ├── feature_extractor.hpp       (新增: 特征提取器)
│   ├── layout_analyzer.hpp         (新增: 物理布局分析)
│   ├── interactive_builder.hpp     (新增: 交互式构建器)
│   └── ddl_change_detector.hpp     (新增: DDL 变更检测)
├── salvage/
│   ├── row_salvage_scanner.hpp     (新增: 逐行抢救扫描)
│   ├── secondary_index_rescuer.hpp (新增: 辅助索引抢救)
│   ├── damaged_page_handler.hpp    (新增: 损坏页处理器)
│   └── data_correlation.hpp        (新增: 数据关联引擎)
├── carve/
│   ├── page_identity_engine.hpp    (新增: 页身份识别)
│   ├── fragment_reassembler.hpp    (新增: 碎片重组器)
│   └── tablespace_composer.hpp     (新增: 表空间合成器)
├── enterprise/
│   ├── partition_recovery.hpp      (新增: 分区表恢复)
│   ├── shared_tablespace.hpp       (新增: 共享表空间)
│   └── offline_keyring.hpp         (增强: keyring 多后端)
└── report/
    └── recovery_report.hpp         (新增: 专业恢复报告)

src/
├── undo/
│   ├── deep_undo_analyzer.cpp
│   ├── undo_page_manager.cpp
│   └── flashback_engine.cpp
├── inference/
│   ├── statistical_classifier.cpp
│   ├── feature_extractor.cpp
│   ├── layout_analyzer.cpp
│   └── interactive_builder.cpp
├── salvage/
│   ├── row_salvage_scanner.cpp
│   ├── secondary_index_rescuer.cpp
│   └── damaged_page_handler.cpp
├── carve/
│   ├── page_identity_engine.cpp
│   ├── fragment_reassembler.cpp
│   └── tablespace_composer.cpp
├── enterprise/
│   ├── partition_recovery.cpp
│   ├── shared_tablespace.cpp
│   └── offline_keyring.cpp
└── report/
    └── recovery_report.cpp

tests/
├── undo/
│   ├── test_deep_undo_analyzer.cpp
│   ├── test_flashback_engine.cpp
│   └── test_undo_page_manager.cpp
├── inference/
│   ├── test_statistical_classifier.cpp
│   ├── test_layout_analyzer.cpp
│   └── test_ddl_change_detector.cpp
├── salvage/
│   ├── test_row_salvage_scanner.cpp
│   └── test_secondary_index_rescuer.cpp
├── carve/
│   ├── test_fragment_reassembler.cpp
│   └── test_tablespace_composer.cpp
└── enterprise/
    ├── test_partition_recovery.cpp
    └── test_shared_tablespace.cpp
```

---

## 附录 B: 关键技术参考

### InnoDB 内部常量

```
FIL_HEADER:
  Offset 0:  CHECKSUM (4B)
  Offset 4:  PAGE_NO / OFFSET (4B) - 压缩页时为 offset
  Offset 8:  PREV_PAGE (4B)
  Offset 12: NEXT_PAGE (4B)
  Offset 16: LSN (8B)
  Offset 24: PAGE_TYPE (2B)
  Offset 26: FLUSH_LSN (8B) - MySQL 8.0+
  Offset 34: SPACE_ID (4B)

UNDO PAGE HEADER (offset 38 = FIL_HEADER end):
  Offset 38+8:  TRX_UNDO_PAGE_TYPE (2B)
  Offset 38+10: TRX_UNDO_PAGE_START (2B)
  Offset 38+12: TRX_UNDO_PAGE_FREE (2B)
  Offset 38+14: TRX_UNDO_PAGE_NODE (12B) - flush list node

UNDO RECORD HEADER:
  [0-1]: next_offset (2B)
  [2-3]: type_cmpl (2B)
  [4]:   undo_no (1B)
  [5-8]: table_id (4B)
  [9+]:  primary_key (variable)
  [ ]:   info_bits (2B)
  [ ]:   n_updated (1B) - for UPDATE
  [ ]:   old_values (variable) - for UPDATE

ROLL_PTR 格式:
  [47:32]: undo_page_no (16 bits = 最大 65535 页)
  [31:0]:  undo_page_offset (32 bits)

DB_ROLL_PTR 存储在每条记录的固定部分:
  COMPACT: TRX_ID (6B) + ROLL_PTR (7B) = 13B
  REDUNDANT: 6B 头中不包含, 在字段数据中
```

---

*蓝图完成。这份文档定义了 v2.0 超越 ODU 的完整技术路线。每个模块的接口设计都考虑了与现有代码的兼容性和渐进式集成策略。建议从 Phase 1 (Undo 引擎重构) 开始实施。*