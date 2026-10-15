# MySQL Recovery Pro — 技术原理与深度实现说明

## 目录
1. [系统架构与分层设计](#1-系统架构与分层设计)
2. [InnoDB 物理存储布局与结构不变量](#2-innodb-物理存储布局与结构不变量)
3. [核心算法与数学模型](#3-核心算法与数学模型)
4. [全字段类型高精度反序列化](#4-全字段类型高精度反序列化)
5. [文件系统物理感知与扇区雕刻](#5-文件系统物理感知与扇区雕刻)
6. [工业级取证与 Fail-Closed 门控](#6-工业级取证与-fail-closed-门控)

---

## 1. 系统架构与分层设计

MySQL Recovery Pro 采用高度解耦的工业级取证分层架构：

```
┌────────────────────────────────────────────────────────────────────────┐
│                        mysql_recovery_pro                              │
├────────────────────────────────────────────────────────────────────────┤
│ 1. 物理介质安全与取证门控 (Kernel RO Lock / WWN Identity / Token Seal)   │
├────────────────────────────────────────────────────────────────────────┤
│ 2. 文件系统 Extent 雕刻层 (Filesystem Physical Extent Carving)          │
│    ├── ext4: 块组描述符解析 (Group Descriptors) + 空闲位图过滤           │
│    └── XFS : 分配组 (AG) 多线程并发 + Inode B+Tree / Free B+Tree 遍历  │
├────────────────────────────────────────────────────────────────────────┤
│ 3. InnoDB 物理页解析引擎 (InnoDB B+Tree & Page Engine)                  │
│    ├── FIL Header / Page Header / Infimum-Supremum 链表追踪            │
│    ├── 聚簇叶子节点拓扑保序验证 (Monotonic Clustered Key Ordering)      │
│    ├── LSN 世代与双写缓冲去重 (Doublewrite & Snapshot Deduplication)   │
│    └── 校验和多态批处理门控 (ChecksumGateBatch: Legacy / CRC32C)       │
├────────────────────────────────────────────────────────────────────────┤
│ 4. 字段反序列化与大对象拼接层 (Decoding & LOB Reassembly)                │
│    ├── Compact / Dynamic 变长数组与 NULL 位图精准解码                  │
│    ├── Antelope 788B 前缀 + 20B 指针反解 (size - 16384)                │
│    ├── Barracuda 20B 指针 + LOB 索引树重组                             │
│    └── 时间/数值合法性多维断言拦截 (Temporal & Domain Sanity Check)    │
├────────────────────────────────────────────────────────────────────────┤
│ 5. 输出与回环验证 (CSV/SQL Stream / Independent Fresh Re-import)       │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. InnoDB 物理存储布局与结构不变量

### 2.1 物理页拓扑（FIL Header & Trailer）

InnoDB 默认物理页大小为 16 KiB（16,384 字节）：

| 偏移区间 | 长度 | 字段名称 | 物理语义与取证判定 |
| :--- | :---: | :--- | :--- |
| `0..3` | 4B | `FIL_PAGE_SPACE_OR_CHKSUM` | 页面校验和（或空间校验和） |
| `4..7` | 4B | `FIL_PAGE_OFFSET` | 表空间内逻辑页号（Page Number） |
| `8..11` | 4B | `FIL_PAGE_PREV` | B+Tree 同层前驱兄弟页号（叶子链表） |
| `12..15` | 4B | `FIL_PAGE_NEXT` | B+Tree 同层后继兄弟页号（叶子链表） |
| `16..23` | 8B | `FIL_PAGE_LSN` | 页面最近一次修改的日志序列号（64位无符号整数） |
| `24..25` | 2B | `FIL_PAGE_TYPE` | 核心页类型：`0x45BF`(INDEX), `0x000A`(BLOB 5.x), `0x0018`(BLOB 8.x), `0x45BD`(SDI) |
| `26..33` | 8B | `FIL_PAGE_FILE_FLUSH_LSN` | 仅在系统表空间 Page 0 有效；普通页通常为 0 |
| `34..37` | 4B | `FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID` | 表空间唯一标识符（Space ID） |
| `38..page_size-9` | 变长 | Page Body | 包含 Page Header(36B)、Infimum/Supremum、User Records、Page Directory |
| `page_size-8..page_size-1` | 8B | `FIL_PAGE_END_LSN_CHECKSUM` | 老式校验和与 LSN 低 32 位折叠值 |

### 2.2 记录头与变长物理存储方言

InnoDB 记录从逻辑原点（`rec_origin`）向两个方向展开：
- **低地址方向（Record Header & Metadata，逆向读取）**：
  `[变长长度列表 (Reverse)] ── [NULL 位图 (Big-Endian)] ── [5字节记录头 (origin - 5)]`
- **高地址方向（Record Data，正向读取）**：
  `[主键字段] ── [DB_TRX_ID (6B)] ── [DB_ROLL_PTR (7B)] ── [其余用户列]`

#### 变长字段长度物理编码算法（rem0rec 规范）
对于变长列（VARCHAR、BLOB、TEXT、VARBINARY、多字节 CHAR）：
1. 若列的最大物理可能长度（`col_length * mb_maxlen`）$\le 255$：
   占用 **1 字节**，直接存储该列物理字节长度。
2. 若列的最大物理可能长度 $> 255$：
   - 若实际存储长度 $< 128$：占用 **1 字节**（最高位为 0）；
   - 若实际存储长度 $\ge 128$：占用 **2 字节**。高字节最高位（Bit 7）必为 1。
   - **离页标志位（BTR_EXTERN_FIELD, Bit 6）**：
     若列存储在外部溢出页中，高字节的 Bit 6（`0x40`）被置为 1。
     此时两字节组合计算为：
     $$\text{size} = \text{byte}_2 + (\text{byte}_1 - 128) \times 256$$
     由于 Bit 6 为 1，$\text{size} \ge 16384$（$0\text{x}4000$）。
     **真实的内联字节数（Inline Bytes）由下式严格给出**：
     $$\text{inline\_bytes} = \text{size} - 16384$$
     - 在 **DYNAMIC** 格式下：$\text{inline\_bytes} = 20$（仅包含 20 字节指针）；
     - 在 **COMPACT (Antelope)** 格式下：$\text{inline\_bytes} = 788$（768 字节前缀 + 20 字节外部指针）。

---

## 3. 核心算法与数学模型

### 3.1 聚簇索引叶子节点单调性断言（Monotonic Key Ordering Invariant）

在关系数据库存储体系中，聚簇索引叶子节点的记录物理序列与逻辑键值之间存在严格的不变量：
$$\forall i \in [1, k-1], \quad \text{Key}(r_i) < \text{Key}(r_{i+1})$$

在物理扇区雕刻中，若某个物理页由损坏块、历史覆盖块拼接而成，记录链可能解码出伪造的主键。MySQL Recovery Pro 建立了基于双向窗口的判定算法：

```cpp
// 伪代码：聚簇索引单调性自适应断决
for (size_t i = 1; i < records.size(); ++i) {
    if (records[i].key <= records[i - 1].key) {
        // 发现逆序违例，构建局部滑动窗口 [i-2, i-1, i, i+1]
        bool drop_prev_restores = (i < 2 || records[i].key > records[i - 2].key) &&
                                  (i + 1 >= records.size() || records[i + 1].key > records[i].key);
        bool drop_curr_restores = (i + 1 >= records.size() || records[i + 1].key > records[i - 1].key);
        
        if (drop_prev_restores && !drop_curr_restores) {
            dropped[i - 1] = true;
        } else if (drop_curr_restores && !drop_prev_restores) {
            dropped[i] = true;
        } else {
            // 无法确证，执行严格 Fail-Closed 双丢弃
            dropped[i - 1] = true;
            dropped[i] = true;
        }
    }
}
```

### 3.2 多版本冲突消歧（LSN Conflict Resolution）
在未 TRIM 的固态介质上，同一 `(space_id, page_no)` 可能存在多次历史快照、双写缓冲区副本或已释放的旧块。系统建立全序冲突决议偏序集：
$$\mathcal{P}_1 \succ \mathcal{P}_2 \iff (\text{LSN}_1 > \text{LSN}_2) \lor (\text{LSN}_1 = \text{LSN}_2 \land \text{N\_RECS}_1 > \text{N\_RECS}_2)$$
在全局物理扫描阶段直接阻断老旧页面的无效展开。

---

## 4. 全字段类型高精度反序列化

引擎提供工业级零拷贝内存解析器，支持 MySQL 5.6 至 9.0 定义的全部存储域：

1. **整数类型（Integer Serialization）**：
   InnoDB 对所有有符号整数执行符号位反转存储（`raw ^ (1ULL << (bits - 1))`），使有符号整数在物理内存上的字典序与数值序完全对齐。反序列化时精准还原。
2. **定点数类型（NEWDECIMAL）**：
   按 9 位十进制数（4 字节）分段打包存储，最高段根据符号位执行掩码反转（正数最高位异或 `0x80`，负数按位全量取反）。
3. **高精度时间类型（Temporal2 Encoding）**：
   - `TIME2`：3 字节基础时间（基准偏移 $8388608$），根据微秒精度补入 1~3 字节小数；
   - `DATETIME2`：5 字节打包年月日时分秒（$19\text{位月} + 5\text{位日} + 5\text{位时} + 6\text{位分} + 6\text{位秒}$），后接变长微秒；
   - `TIMESTAMP2`：4 字节大端 Unix 时间戳 + 变长微秒。
   - **严格域合法性检查（Domain Sanity Assertion）**：
     在物理扇区雕刻时，解析器强制断言 $m \in [1, 12]$、$d \in [1, 31]$、$h \le 838$、$mi \le 59$、$s \le 59$，拦截任何由非结构化字符混淆导致的越界时间假阳性行。
4. **原生二进制 JSON（Type 31）**：
   完整实现 MySQL 8.0/8.4/9.0 二进制 JSON 文档规范（`JSON_TYPE_SMALL_OBJECT`, `JSON_TYPE_LARGE_ARRAY` 等），支持小端变长键值查找与多层嵌套树的无损还原。

---

## 5. 文件系统物理感知与扇区雕刻

### 5.1 ext4 块分配图拓扑雕刻
- 物理范围：读取超级块（`Superblock`）确认块大小（4 KiB）与每组块数（32,768）；
- 空闲扇区捕获：在 `scan_freed_blocks` 中遍历块组描述符（`ext4_group_desc`）定位各块组 `bg_block_bitmap`，将 bit 为 0 的已删除块连续映射为待检物理页；
- 目标空间前置过滤：在收集 `blob_queue_` 及索引页时，前置校验 `FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID == target_space_id`，杜绝跨库扇区串扰。

### 5.2 XFS 分配组（Allocation Group）并行遍历
- 针对百万级 IOPS 的企业级介质，利用 XFS 独立的 AG 架构划分并发工作单元；
- 每个 Worker 独立处理一个 AG，解析 `xfs_agf`（空间空闲 B+Tree）与 `xfs_agi`（空闲 Inode B+Tree），高效召回已执行 `unlink` 但未清零的 Extent 物理空间。

---

## 6. 工业级取证与 Fail-Closed 门控

### 6.1 密码学证据链与状态机
整个恢复流程由确定的有限状态机严格驱动：
$$\text{PLANNED} \longrightarrow \text{PRECHECKED} \longrightarrow \text{MEDIA\_PREPARED} \longrightarrow \text{TRUTH\_SEALED} \longrightarrow \text{PERSISTED} \longrightarrow \text{SCENARIO\_APPLIED} \longrightarrow \text{RECOVERY\_ATTEMPTED} \longrightarrow \text{IMPORT\_VALIDATED} \longrightarrow \text{REPORTED} \longrightarrow \text{SEALED}$$

- 每轮运行的全部输入元数据、输出日志、CSV 导出及校验 JSON 均计算 SHA-256 哈希清单；
- 终态生成包含设备物理指纹（WWN / Serial）的数字封印（`manifest.seal`），实现不可伪造的取证链条。

### 6.2 独立容器重导入回环判决（Round-Trip Re-import Verification）
任何物理扇区雕刻输出的结构化数据，均必须通过独立全新创建的干净目标版本 MySQL 容器进行 DDL 创建与全量 SQL 导入。只有当导入成功、无警告报错、且读回比对与真值实现逐行逐字段零差异时，资格赛方可判定为 **`PASS`**。
