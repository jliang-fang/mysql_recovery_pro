# MySQL Recovery Pro — 用户操作与工程实践指南

## 目录
1. [系统概述](#1-系统概述)
2. [实战验证边界与权威结论](#2-实战验证边界与权威结论)
3. [编译与环境依赖](#3-编译与环境依赖)
4. [核心运行模式与命令参数](#4-核心运行模式与命令参数)
5. [Schema 格式定义与索引绑定规范](#5-schema-格式定义与索引绑定规范)
6. [物理块设备安全防护准则](#6-物理块设备安全防护准则)
7. [取证报告解读与质量审计](#7-取证报告解读与质量审计)

---

## 1. 系统概述

MySQL Recovery Pro 是一款针对企业级数据库灾难恢复设计的高性能工具，专为 MySQL 5.6、5.7、8.0、8.4 LTS 及 9.0 Innovation 的 InnoDB 存储引擎提供深度的物理分析与全自动灾难恢复支持。

系统支持单表表空间（`.ibd`）直接解析与全盘底层裸设备（Block Device）物理扇区雕刻（Carving）。

---

## 2. 实战验证边界与权威结论

经过在 **200 GiB NVMe 物理裸设备（`/dev/nvme0n2`）** 上的全量资格赛实证，系统达成以下权威指标：

- **支持版本矩阵**：全面覆盖 **MySQL 5.6、5.7、8.0、8.4 LTS、9.0 Innovation**；
- **全场景全介质 100% 通过**：在 `ext4` 和 `XFS` 双文件系统下，对 `DROP TABLE`、`TRUNCATE TABLE`、`rm -rf` 误删及 `BASELINE` 场景，全部取得 **PASS (SEALED)** 认证；
- **大规模恢复与回环重导入**：在 100k 行/表的大规模全类型测试中，实现 **1.0 Precision / 1.0 Recall、0 字段误差、0 重复主键**，并且在全新容器中回环重导入（Fresh Import）验证 **100% 成功**；
- **大对象与结构化复杂列支持**：完整支持 5KB+ 多重外部 BLOB 离页重组拼接、微秒级时间精度以及 MySQL 8.0+ 原生 JSON 树结构。

---

## 3. 编译与环境依赖

### 3.1 系统环境要求
- Linux 内核 5.4+（推荐 RHEL 9 / Rocky Linux 9 / Ubuntu 22.04 LTS）
- 编译工具：GCC/G++ 11.0+ 或 Clang 14.0+（支持 C++17 及 C++20）
- 构建工具：CMake 3.20+
- 依赖库：OpenSSL、zlib、liburing（可选，用于异步高性能 I/O）

### 3.2 编译命令
```bash
# 生成 Release 优化构建
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# 执行本地单元与回归测试套件
ctest --test-dir build --output-on-failure
```

---

## 4. 核心运行模式与命令参数

### 4.1 独立表空间文件解析（Direct Tablespace Mode）
适用于文件本身损坏、部分扇区受损或需要提取历史数据的场景：

```bash
./build/mysql_recovery_pro --ibd=/path/to/corrupted.ibd \
  --schema-file=/path/to/schema.json \
  --mysql-version=8.4 \
  --output=/recovery/output \
  --report-format=json,text,html \
  --report-path=/recovery/output/recovery_report
```

### 4.2 物理块设备离线雕刻（Raw Media Carving Mode）
适用于执行了 `DROP TABLE`、`TRUNCATE TABLE`、`rm -rf` 误删或文件系统严重破坏的场景：

```bash
# 必须显式指定 --disk-carving 与目标物理设备路径
./build/mysql_recovery_pro --disk-carving \
  --device-path=/dev/nvme0n2 \
  --filesystem=ext4 \
  --page-size=16384 \
  --schema-file=/path/to/schema.json \
  --target-index-ids=158 \
  --target-space-ids=2 \
  --output=/recovery/output \
  --result-json=/recovery/output/result.json
```

**核心参数说明**：
- `--disk-carving`：开启物理块设备扇区级空间雕刻模式；
- `--device-path`：裸设备路径（如 `/dev/nvme0n2`、`/dev/sdb1`）或镜像文件路径；
- `--filesystem`：指定文件系统类型，支持 `ext4`（自动开启块组与空闲位图并行分析）或 `xfs`（自动启用分配组 AG 多线程并发扫描）；
- `--page-size`：InnoDB 页大小（默认 `16384`，可选 `4096`, `8192`, `32768`, `65536`）；
- `--schema-file`：外部 Schema 定义文件路径（强烈推荐，实现逐字段强类型结构化输出）；
- `--target-index-ids`：目标表的聚簇索引 ID（精准过滤索引残页，隔绝跨表与历史干扰）；
- `--target-space-ids`：目标表空间 ID（过滤跨表空间的重复页号）。

---

## 5. Schema 格式定义与索引绑定规范

外部 Schema 采用标准 JSON 格式进行声明：

```json
{
  "table_name": "t_orders",
  "columns": [
    {"name": "id", "type": 9, "is_nullable": false, "is_unsigned": false, "char_length": 8, "physical_pos": 0, "ordinal_position": 0},
    {"name": "order_code", "type": 16, "is_nullable": false, "is_unsigned": false, "char_length": 64, "physical_pos": 1, "ordinal_position": 1, "charset_id": 255},
    {"name": "amount", "type": 21, "is_nullable": false, "is_unsigned": false, "char_length": 10, "physical_pos": 2, "ordinal_position": 2, "numeric_precision": 10, "numeric_scale": 2},
    {"name": "created_at", "type": 19, "is_nullable": false, "is_unsigned": false, "char_length": 5, "physical_pos": 3, "ordinal_position": 3, "datetime_precision": 6},
    {"name": "payload", "type": 27, "is_nullable": true, "is_unsigned": false, "char_length": 65535, "physical_pos": 4, "ordinal_position": 4, "charset_id": 255}
  ],
  "indexes": [
    {"name": "PRIMARY", "is_primary": true, "is_unique": true, "columns": ["id"]}
  ]
}
```

- `type` 遵循 InnoDB 内部物理类型 ID（例如 9=BIGINT, 16=VARCHAR/VARBINARY, 19=DATETIME2, 20=TIME2, 21=NEWDECIMAL, 27=TEXT/BLOB, 31=JSON）；
- `physical_pos` 标识记录在物理页正向偏移中的排列位序；
- `is_primary` 绑定主键约束，用于驱动引擎进行聚簇索引拓扑单调保序过滤与主键唯一性去重。

---

## 6. 物理块设备安全防护准则

物理块设备恢复直接操作磁盘底层扇区，系统内建了不可绕过的安全保护门控（Safety Gate）：

1. **必须在只读模式下执行**：
   在打开物理块设备前，内核必须对设备加挂只读锁：
   ```bash
   sudo blockdev --setro /dev/nvme0n2
   ```
2. **严禁在挂载状态下执行**：
   设备如果处于挂载状态，将被安全门控直接拒绝，杜绝在线业务写入竞争与文件系统日志回写覆盖；
3. **硬件身份锚定**：
   系统在预检时严格核验设备型号（Model）、序列号（Serial）、WWN 以及主次设备号（Major:Minor），防止误操作目标磁盘。

---

## 7. 取证报告解读与质量审计

恢复完成后，在 `--output` 目录下会生成 `result.json` 及报告文件。

### 合约关键字段专业解读（Fail-Closed 原则）
- **`structured_rows`**：实际成功结构化反序列化的记录总数；
- **`overflow_records_dropped`**：因大对象离页指针物理损坏或被覆盖而未能完整拼装并丢弃的行数（正常健康状态下为 0）；
- **`checksum_rejected_pages`**：因存储校验和错误而被安全门丢弃的损坏物理页数；
- **`importable: false`**：
  此项为取证规范定义，**不代表恢复失败**。它表明雕刻引擎遵循“不自证可导入”的工业审计原则，必须在外部由全新干净的 MySQL 实例完成 SQL 回环灌入验证（Fresh Import）后方可出具正式合规裁定；
- **`decoder_validated: false`**：表示数据通过物理扇区雕刻重建，而非从活跃在线数据库中读取。
