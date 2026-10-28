# MySQL 灾难恢复实战指南

本指南介绍事故发生后的第一响应流程，以及两条主要物理恢复路径：独立表空间解析和文件系统感知雕刻。原则只有一个：先保护源介质，再恢复；所有恢复结果在独立干净 MySQL 实例验证前都只能视为取证证据。

## 目录

1. [事故响应与物理不变量](#1-事故响应与物理不变量)
2. [通用准备流程](#2-通用准备流程)
3. [恢复操作](#3-恢复操作)
4. [文件系统感知雕刻](#4-文件系统感知雕刻)
5. [Schema、SpaceID 与 IndexID](#5-schema-spaceid-与-indexid)
6. [验证与证据封存](#6-验证与证据封存)
7. [格式支持与 Fail-Closed 边界](#7-格式支持与-fail-closed-边界)

## 1. 事故响应与物理不变量

`DROP TABLE`、`TRUNCATE TABLE` 和 `rm -rf` 会改变 InnoDB 与文件系统元数据，但不保证旧扇区立即被清零。只有在旧页仍可读取且结构仍可识别时，物理恢复才有意义。

```text
停止写入
   │
   ├── 制作扇区镜像，或锁定源设备为只读
   ├── 卸载源文件系统
   ├── 记录设备身份与证据哈希
   └── 在另一文件系统中执行恢复
           │
           ├── 页/记录/Schema 严格校验
           ├── 仅在安全时输出有界 CSV/SQL
           └── 独立干净实例导入与逐字段比对
```

### 不同事故的物理变化

- **`TRUNCATE TABLE`**：可能创建新的表空间或走 fast-truncate 释放页路径；旧页在再次分配前可能仍然存在。
- **`DROP TABLE`**：表空间文件和 Extent 被释放；物理块中可能仍残留旧页。
- **`rm -rf`**：目录项和文件元数据被删除；后续分配可能覆盖释放块。
- **持续写入**：Redo、日志回放、分配器活动和业务写入都会破坏证据。隔离前的每一次 I/O 都可能影响结果。

## 2. 通用准备流程

### 2.1 停止并隔离

```bash
sudo systemctl stop mysqld
# 只有在服务无法正常停止时才使用：
sudo pkill -9 mysqld

# 确认源设备未挂载后再进行镜像或雕刻。
findmnt /dev/<source-device>
sudo umount /mnt/data
```

不要对源介质运行 `fsck`，不要以读写方式挂载，不要把恢复结果写回源设备，也不要直接使用损坏的数据目录启动 MySQL。

### 2.2 优先制作取证镜像

扇区镜像便于重复实验，也避免对原始介质产生竞争读写。案件记录中应保存镜像摘要、源设备身份、命令行、UTC 时间和 SHA-256。如果无法制作镜像，则使用已核实的只读设备：

```bash
sudo blockdev --setro /dev/<source-device>
sudo blockdev --getro /dev/<source-device>  # 预期：1
```

安全门还会检查设备身份和挂载状态。只读标志是必要条件，但不能替代设备身份核验。

### 2.3 只读收集元数据

保留最后已知的 MySQL 版本、行格式、页大小、库表名、Schema、`space_id`、clustered `index_id`，以及已经取得的 `.frm`/SDI 元数据。不要在存在可靠来源时凭空猜测这些值。

## 3. 恢复操作

### 3.1 独立 `.ibd` 恢复

当表空间副本存在，即使尾部或部分页损坏，也可使用此路径：

```bash
./build/mysql_recovery_pro \
  --ibd=/recovery/input/table.ibd \
  --schema-file=/recovery/schema/table.json \
  --mysql-version=8.4 \
  --output=/recovery/output/table \
  --report-format=json,text \
  --report-path=/recovery/output/table/recovery_report
```

原始输入必须保持不变。如果文件被截断，解析器应识别完整页和不完整尾页；不要为了凑整文件长度而填充或修改输入。

### 3.2 `DROP TABLE` 与 `TRUNCATE TABLE`

对镜像文件或只读设备执行文件系统感知雕刻：

```bash
./build/mysql_recovery_pro \
  --disk-carving \
  --device-path=/recovery/media/disk.img \
  --filesystem=ext4 \
  --page-size=16384 \
  --schema-file=/recovery/schema/table.json \
  --target-index-ids=158 \
  --target-space-ids=2 \
  --output=/recovery/output/table \
  --result-json=/recovery/output/table/result.json
```

XFS 镜像只需在确认输入后更改文件系统参数：

```bash
--filesystem=xfs
```

`DROP` 和 `TRUNCATE` 的元数据变化不同，但第一响应相同：停止写入、保护介质，并识别目标页集合。

### 3.3 删除 `.ibd` / `rm -rf`

如果服务仍在运行，不要先重启；先确认是否存在可独立复制的 deleted 文件句柄。如果文件不再打开，则对镜像或释放 Extent 进行雕刻。命令与上例相同，但必须使用正确的文件系统和目标标识。

## 4. 文件系统感知雕刻

| 属性 | ext4 | XFS |
|---|---|---|
| 分配模型 | 块组和块位图 | 分配组和空闲空间树 |
| 候选发现 | 释放块/Extent 过滤 | 分配组并行遍历和空闲空间遍历 |
| 主要优势 | 快速筛选已解除链接的块 | 可并行扫描相互独立的 AG |
| 主要注意事项 | 日志和分配器活动可能复用块 | Extent 复用和旧页必须经过严格页身份校验 |

文件系统层只负责提出物理候选。InnoDB 层仍须校验页大小、页类型、SpaceID、IndexID、校验和、记录链、主键顺序、字段边界和外部 LOB 指针。

## 5. Schema、SpaceID 与 IndexID

Schema 是结构化输出的解码依据，应包含物理列位置、可空性、符号属性、长度/精度、字符集以及主键索引。`--target-space-ids` 和 `--target-index-ids` 在导出前约束候选页。

最重要的防污染链路是：

```text
候选页
  → 目标 SpaceID
  → 目标 clustered IndexID
  → 有效页和记录结构
  → Schema 解码
  → 主键保序与重复检查
  → 输出合约
```

对于外部字段，解码器区分 Barracuda 的 20 字节指针和 Antelope COMPACT 的“768 字节内联前缀 + 指针”布局。无法证明链完整时，必须拒绝或记录为不确定，不能静默截短。

## 6. 验证与证据封存

### 6.1 检查结果合约

至少检查：

- `structured_rows`；
- `checksum_rejected_pages`；
- `overflow_records_dropped`；
- 重复和保序诊断；
- `importable` 与 `decoder_validated`；
- 输出文件摘要和 manifest 状态。

`importable: false` 是设计要求，表示雕刻器没有自行证明输出可导入，不表示所有恢复行都无效。

### 6.2 独立 fresh-import 验证

合格的恢复运行应使用独立的干净 MySQL 实例：

1. 创建预期 Schema；
2. 不做人工编辑，直接导入恢复出的 SQL/CSV；
3. 读回所有表，与封存真值进行逐字段类型比对；
4. 记录期望行数、实际行数、字段错误、重复行和拒绝/缺失行；
5. 将导入日志和结果 JSON 与恢复输出放在同一证据目录。

进程退出码或“行数看起来正确”不能替代逐字段比较。

### 6.3 证据位置

本仓库资格验证证据位于 `/app/mysql_recovery_artifacts`，README 中提供报告和 JSON 合约链接。普通代码克隆不会自动包含这些主机本地路径；复核时应单独复制或挂载证据目录。

## 7. 格式支持与 Fail-Closed 边界

已验证的结构化路径包含支持版本和测试矩阵中的 COMPACT/DYNAMIC 行格式与页大小。没有权威字段边界描述的 REDUNDANT、没有夹具验证解压后端的压缩表空间、没有密钥的加密/TDE 候选以及畸形页会进入 evidence-only 或 unsupported 路径。

典型损坏结果会被明确限定：

- checksum/body 翻转会拒绝受影响页，不输出损坏字段；
- `REC_NEXT` 断裂会受到边界保护，防止死循环和字段移位；
- 尾部半页会在最后一个完整页处停止；
- 无效压缩元数据或 TDE 候选不会生成可导入 SQL/CSV；
- 不完整 BLOB 链会计为丢弃/不确定，不会静默截短。

### 相关文档

- [用户操作指南](USER_GUIDE_PRO.md)
- [技术原理与实现说明](IMPLEMENTATION_PRINCIPLES.md)
- [English field guide](../en/DISASTER_RECOVERY_GUIDE.md)
- [权威验证报告](../../../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md)
