# MySQL Recovery Pro — 技术原理与实现说明

本文档说明 MySQL Recovery Pro 使用的物理模型，面向开发、审计和取证复核。它不是“所有理论路径都对所有版本开放”的承诺；当前已验证边界以[验证证据报告](../../../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md)为准。

## 目录

1. [架构与证据边界](#1-架构与证据边界)
2. [InnoDB 页与记录布局](#2-innodb-页与记录布局)
3. [结构校验与冲突消歧](#3-结构校验与冲突消歧)
4. [字段解码与外部 LOB](#4-字段解码与外部-lob)
5. [文件系统感知雕刻](#5-文件系统感知雕刻)
6. [审计生命周期与 Fail-Closed 策略](#6-审计生命周期与-fail-closed-策略)

## 1. 架构与证据边界

恢复流水线按层次解耦：

```text
源介质安全与设备身份
          │
文件系统候选发现（ext4 / XFS）
          │
InnoDB 页身份和完整性门控
          │
记录链与聚簇主键校验
          │
Schema 引导的字段解码与 LOB 拼接
          │
CSV/SQL 证据合约 + manifest
          │
独立干净实例导入与比对
```

前五层处理物理证据，最后一层明确置于解码器之外。这样可以避免解析器把自己的输出自证为“可安全导入”。

## 2. InnoDB 页与记录布局

对于页大小 `N`，基本页结构如下：

```text
FIL Header（38 字节）
  页校验和 / space-or-checksum
  页号
  前驱 / 后继页
  页 LSN
  页类型
  Space ID
Page Body
  Page Header、Infimum、Records、Supremum、Directory
FIL Trailer（8 字节）
```

在解码记录前，先校验 FIL Header 的关键字段：

| 偏移 | 长度 | 字段 | 取证用途 |
|---:|---:|---|---|
| 0 | 4 | checksum 或 space/checksum | 完整性门。 |
| 4 | 4 | page number | 页身份与重复页消歧。 |
| 8 | 4 | previous page | 链一致性。 |
| 12 | 4 | next page | 链一致性。 |
| 16 | 8 | page LSN | 新旧页冲突消歧。 |
| 24 | 2 | page type | INDEX、BLOB、SDI 等布局选择。 |
| 34 | 4 | Space ID | 跨表空间污染过滤。 |

### 2.1 COMPACT/DYNAMIC 记录元数据

COMPACT 和 DYNAMIC 记录的变长元数据与 NULL 位图位于记录数据之前。记录数据还包含 InnoDB 隐藏事务字段和用户列。解码器必须依据物理排列解析，不能简单假设物理顺序等于 SQL 列顺序。

对于最大字节长度大于 255 的变长字段：

- 短值可以使用一个长度字节；
- 较长值使用两个字节；
- 外部存储标志属于编码长度的一部分；
- 设置外部标志时，内联字节数必须由编码长度推导，不能只看指针猜测。

已验证的 Antelope 规则为：

```text
inline_bytes = encoded_size - 16384 = 788
              = 768 字节内联前缀 + 20 字节 BTR_EXTERN 指针
```

DYNAMIC/Barracuda 行通常在聚簇记录中保留 20 字节指针，再从外部 LOB 链拼接数据。

## 3. 结构校验与冲突消歧

### 3.1 聚簇主键保序

聚簇叶子页中通过记录链连接的记录必须严格按聚簇键递增：

\[
K(r_1) < K(r_2) < \dots < K(r_n)
\]

物理雕刻可能遇到旧页、异表页或单条损坏记录。实现采用有界的局部判定，并拒绝无法消歧的碰撞；不会通过编造主键或移动字段来“修复”保序违例。

### 3.2 SpaceID 与 IndexID 过滤

候选页只有在物理身份匹配目标时才有资格继续处理：

\[
SpaceID(P) \in S_{target} \land IndexID(P) \in I_{target}
\]

该过滤在结构化导出前完成，用于阻止旧分配和无关系统表污染结果。

### 3.3 页和 LSN 冲突

在未执行 TRIM 或发生分配复用的介质上，同一 `(space_id, page_no)` 可能出现多个副本。系统依据页身份、完整性、LSN 和记录证据消歧：有效的新页优先于旧副本；相同 LSN 时采取保守策略，不按扫描顺序任意选择。

### 3.4 校验和门

校验和不是提示信息，而是结构化输出门。无法满足适用校验策略的页会被拒绝。损坏矩阵因此区分：

- 健康的结构化行；
- 可归因于被拒绝页的有界缺失；
- 无法安全声明结构化行的 evidence-only 证据。

## 4. 字段解码与外部 LOB

Schema 引导解码器覆盖资格验证夹具中使用的物理编码：

- 有符号/无符号整数排序变换；
- 分段打包的 `NEWDECIMAL`；
- 带小数精度的 `TIME2`、`DATETIME2`、`TIMESTAMP2`；
- 字符/二进制值以及 NULL/空字符串区分；
- ENUM/SET 值映射；
- MySQL 二进制 JSON 树；
- 内联和外部 TEXT/BLOB 数据。

时间类型会执行范围校验。月份、日期、小时、分钟或秒越界说明字段边界可能错误，必须拒绝而不是格式化成看似合理的时间。

### 4.1 LOB 链拼接

只有当指针和片段都符合对应物理格式时才重建外部值：

```text
聚簇记录
  └── 20 字节外部指针
        ├── 旧版 0x000A 链（5.x）
        ├── 压缩/旧版变体（按夹具结果进入支持或证据路径）
        └── 现代 LOB entry/fragment 链（8.x/9.x）
```

内联前缀必须保持原始顺序。缺失、循环、越界或截断片段会被记录为丢弃/不确定，不能静默替换成较短值。

## 5. 文件系统感知雕刻

### 5.1 ext4

ext4 扫描器读取文件系统几何信息和块组元数据，识别可能释放的块/Extent，再把物理页候选交给 InnoDB 身份过滤器。日志回放和事故后的重新分配都可能使候选无效；文件系统层发现本身不等于可恢复。

### 5.2 XFS

XFS 扫描器按分配组划分任务，并利用 AG 元数据与空闲空间结构查找候选 Extent。并行化只影响发现速度，不改变校验规则；每个候选仍需经过页身份、SpaceID/IndexID、校验和、记录和 Schema 校验。

## 6. 审计生命周期与 Fail-Closed 策略

封存运行通常遵循如下状态：

```text
PLANNED
  → PRECHECKED
  → MEDIA_PREPARED
  → TRUTH_SEALED
  → RECOVERY_ATTEMPTED
  → CONTRACT_PERSISTED
  → IMPORT_VALIDATED
  → REPORTED
  → SEALED
```

验证夹具记录实际生命周期字段。输入、输出、调用元数据和比对结果会写入 manifest 并计算摘要；封印将证据绑定到记录中的源设备身份。

### 6.1 合约语义

- `structured_recovery`：Schema 和物理校验允许输出类型化数据；
- `forensic_evidence`：发现了物理证据，但拒绝结构化导出；
- `PASS_BOUNDED_STRUCTURED`：完好区域通过，损坏区域被明确限定；
- `PASS_EVIDENCE_ONLY`：拒绝导出是预期安全结果；
- `UNSUPPORTED`：组合超出已验证解码边界；
- `importable: false`：物理恢复工具不自行证明可导入。

### 6.2 独立重导入边界

最终资格判断由独立干净 MySQL 实例执行：创建预期 Schema，不做人工修复地导入恢复数据，读回并与封存真值逐字段比较。没有该边界，不能把看似合理的行数转化为 PASS。

### 6.3 保守失败行为

Fail-Closed 是正确性能力：

- 拒绝不明确的字段边界；
- 拒绝无效校验和与不可能的时间值；
- 拒绝断裂或不完整的外部链；
- 缺少已验证后端或密钥时拒绝压缩/加密数据；
- 保存诊断和证据供后续分析。

操作流程和参数说明见[用户操作指南](USER_GUIDE_PRO.md)与[灾难恢复实战指南](DISASTER_RECOVERY_GUIDE.md)。
