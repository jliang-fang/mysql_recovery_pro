# MySQL Recovery Pro — 用户操作指南

本指南用于日常操作、事故响应和审计复核，覆盖安装编译、输入准备、CLI 模式、Schema 绑定、输出合约和证据保存。

> **安全优先：** 第一次恢复不得直接针对挂载中的生产文件系统执行。优先制作扇区镜像；如果必须读取源设备，应确认设备已卸载、只读，并把全部输出写入另一块介质。

## 目录

1. [范围与已验证支持](#1-范围与已验证支持)
2. [编译与依赖](#2-编译与依赖)
3. [选择恢复模式](#3-选择恢复模式)
4. [CLI 参数参考](#4-cli-参数参考)
5. [Schema 与标识绑定](#5-schema-与标识绑定)
6. [输出与独立验证](#6-输出与独立验证)
7. [安全清单与故障排查](#7-安全清单与故障排查)

## 1. 范围与已验证支持

工具直接读取 InnoDB 物理页，不依赖在线 SQL 连接。主要支持两类输入：

- 脱离数据库的数据表空间 `.ibd`；
- 包含已删除/已释放表空间页的磁盘镜像或裸块设备。

最终资格验证覆盖 MySQL 5.6、5.7、8.0、8.4 LTS、9.0 Innovation，以及 ext4/XFS 文件系统场景。数据集包含数值、时间、文本/LOB、JSON 和复合主键字段。验证证据保存在 `/app/mysql_recovery_artifacts`，详见[权威验证报告](../../../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md)。

验证结论不是“行数看起来正确”，而是完整合约：结构化解码、严格字段比对，以及在声明结构化导出时执行独立 fresh-import 和读回比对。

## 2. 编译与依赖

### 环境要求

- Linux 内核 5.4 或更高版本；
- GCC/G++ 11+ 或 Clang 14+，支持 C++17；
- CMake 3.20+；
- OpenSSL、zlib 开发库；
- liburing 可选。

### 编译与本地测试

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

本地测试验证解析器和恢复组件，不代表可以未经授权操作真实生产设备。

## 3. 选择恢复模式

| 输入或事故 | 模式 | 必要准备 |
|---|---|---|
| 脱离数据库的 `.ibd` | 独立表空间恢复 | 保留原文件，使用副本操作。 |
| `DROP TABLE` | `--disk-carving` 或 `.ibd` 恢复 | 尽可能确定 `space_id` 和 clustered `index_id`。 |
| `TRUNCATE TABLE` | `--disk-carving` | 立即停止全部写入。 |
| `.ibd` 删除 / `rm -rf` | `--disk-carving` | 优先使用镜像；后续写入可能覆盖原页。 |
| 页损坏或介质截断 | 有界抢救 | 允许出现拒绝页或 evidence-only 输出。 |

### 3.1 独立表空间恢复

```bash
./build/mysql_recovery_pro \
  --ibd=/recovery/input/table.ibd \
  --schema-file=/recovery/schema/table.json \
  --mysql-version=8.4 \
  --output=/recovery/output/table \
  --report-format=json,text \
  --report-path=/recovery/output/table/recovery_report
```

### 3.2 磁盘镜像或裸设备雕刻

优先对取证镜像执行：

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

如果必须读取真实设备，应先停止 MySQL、卸载文件系统、核对设备身份，再施加内核只读锁：

```bash
sudo blockdev --setro /dev/<source-device>
sudo blockdev --getro /dev/<source-device>  # 预期：1
```

## 4. CLI 参数参考

下表是本文档恢复路径使用的主要参数。完整功能清单以构建后的帮助文本为准。

| 参数 | 含义 |
|---|---|
| `--ibd=<path>` | 脱离的 `.ibd`、镜像或表空间输入路径。 |
| `--disk-carving` | 开启物理磁盘/镜像雕刻。 |
| `--device-path=<path>` | 雕刻源镜像或裸设备路径。 |
| `--filesystem=ext4\|xfs` | 文件系统感知的候选页过滤。 |
| `--page-size=<bytes>` | InnoDB 页大小：4096、8192、16384、32768 或 65536（按组合支持）。 |
| `--schema-file=<path>` | 严格类型解码使用的 JSON Schema。 |
| `--mysql-version=<version>` | 版本提示，例如 `5.6`、`5.7`、`8.0`、`8.4`、`9.0`。 |
| `--target-index-ids=<ids>` | 要保留的聚簇索引 ID，逗号分隔。 |
| `--target-space-ids=<ids>` | 要保留的表空间 ID，逗号分隔。 |
| `--output=<dir>` | 独立输出目录。 |
| `--result-json=<path>` | 机器可读的结果合约。 |
| `--report-format=<fmt>` | `json`、`text`、`html` 的逗号分隔列表。 |
| `--report-path=<path>` | 无扩展名的报告基路径。 |

不要只依据进程退出码判断成功。必须检查结果合约和独立比对报告。

## 5. Schema 与标识绑定

Schema 文件描述物理字段顺序和解码规则。`physical_pos` 是磁盘记录中的物理位置，`ordinal_position` 是逻辑列顺序。主键索引用于保序检查和重复数据抑制。

```json
{
  "table_name": "orders",
  "columns": [
    {"name": "id", "type": 9, "is_nullable": false,
     "is_unsigned": false, "physical_pos": 0, "ordinal_position": 0},
    {"name": "created_at", "type": 19, "is_nullable": false,
     "physical_pos": 1, "ordinal_position": 1, "datetime_precision": 6},
    {"name": "payload", "type": 27, "is_nullable": true,
     "physical_pos": 2, "ordinal_position": 2, "char_length": 65535}
  ],
  "indexes": [
    {"name": "PRIMARY", "is_primary": true, "is_unique": true,
     "columns": ["id"]}
  ]
}
```

常用物理类型 ID：

| ID | 物理类型 |
|---:|---|
| 9 | BIGINT |
| 16 | VARCHAR/VARBINARY 类 |
| 19 | DATETIME2 |
| 20 | TIME2 |
| 21 | NEWDECIMAL |
| 27 | TEXT/BLOB |
| 31 | 二进制 JSON |

`space_id` 和 clustered `index_id` 过滤不是可选的装饰信息：它们用于阻止其他表或旧分配中的残页产生假阳性。

## 6. 输出与独立验证

通常的输出目录包含以下文件中的一部分：

- `result.json`：结构化执行合约；
- CSV/SQL：只有在解码器证明合约安全时才会生成；
- text/JSON/HTML 报告；
- `manifest.json` 和 `manifest.seal`：输入、输出、环境与摘要证据；
- 验证脚本生成的 comparison 和 fresh-import 报告。

### 结果状态

| 状态 | 含义 |
|---|---|
| `PASS` | 完整结构化结果通过严格比对和独立验证门。 |
| `PASS_BOUNDED_STRUCTURED` | 完好数据结构化正确，损坏/缺失范围被明确限定。 |
| `PASS_EVIDENCE_ONLY` | 保留证据，但主动拒绝结构化/可导入导出。 |
| `UNSUPPORTED` | 请求的物理组合不在已验证解码范围内。 |
| `SETUP_ERROR` | 夹具或环境中断运行，不代表产品缺陷。 |

关键合约字段：

- `structured_rows`：通过 Schema 和结构校验后解码的行数；
- `checksum_rejected_pages`：被完整性门拒绝的页数；
- `overflow_records_dropped`：外部 LOB 无法证明完整而丢弃的行数；
- `importable: false`：设计上的安全策略，雕刻器不自行证明可导入；
- `decoder_validated: false`：表示数据来自物理雕刻，而不是在线数据库读取。

最终验证必须在外部完成：创建干净 MySQL 实例，执行预期 DDL，导入恢复数据，读回并与封存真值逐字段比较。没有该边界，不应在报告中称数据“可导入”。

## 7. 安全清单与故障排查

### 扫描前清单

- [ ] MySQL 已停止，源设备不会继续写入。
- [ ] 源文件系统已卸载。
- [ ] 已记录设备型号、序列号、WWN 和主次设备号。
- [ ] 使用取证镜像或只读源。
- [ ] 输出、临时文件和日志位于另一文件系统。
- [ ] 已记录 Schema、`space_id` 和 clustered `index_id`。

### 没有结构化行输出

检查页大小、行格式/版本提示、Schema 物理顺序、目标 ID 和校验和结果。加密页或未验证的压缩布局进入 evidence-only 是预期的安全行为，优于导出错位字段。

### 恢复行数不足

检查 `checksum_rejected_pages`、LOB 溢出计数、介质覆盖情况以及事故后是否有块复用。缺失行与字段错误必须分开统计。

### fresh-import 失败

在确认 Schema、SQL/CSV 转义、字符集、NULL 处理和比对报告之前，不要把结果视为合格。不要手工修改恢复数据后再宣称恢复通过。

## 相关文档

- [灾难恢复实战指南](DISASTER_RECOVERY_GUIDE.md)
- [技术原理与实现说明](IMPLEMENTATION_PRINCIPLES.md)
- [English user guide](../en/USER_GUIDE_PRO.md)
- [权威验证报告](../../../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md)
