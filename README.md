# MySQL Recovery Pro

**[English](#english) | [中文](#中文)**

---

## English

### Overview

MySQL Recovery Pro provides MySQL/InnoDB inspection, recovery, and recovery-report code paths for tablespaces from MySQL 5.6 through 9.0. Recovery is case-specific: source-media condition, overwrite history, corruption, schema accuracy, and the selected path determine what can be recovered.

### Validation boundary

Fresh validation completed on 2026-09-16: **1,548 CTest tests excluding `REAL_MYSQL`** and **four `REAL_MYSQL` CTest suites** passed. The real-MySQL fixture suites check tablespace integrity, version-family page markers, metadata, partition names, and undo-page presence. They do **not** demonstrate end-to-end row recovery, undo recovery, or flashback recovery.

MySQL 8.4 and 9.0 route to the `MYSQL8_7BYTE` undo-format profile and have unit/regression coverage for that mapping. This is not evidence of real-MySQL undo or flashback recovery.

No filesystem- or version-specific recovery rate is claimed while fresh results are pending.

### Supported code paths

- Tablespace inspection and page scanning for MySQL 5.6, 5.7, 8.0, 8.4, and 9.0 profiles
- Schema-assisted parsing when compatible metadata or a correct manual schema is available
- Recovery-report generation in text, JSON, and HTML formats
- Disk-carving paths for ext4 and XFS sources, subject to raw-device safety approval

Component availability is not a recovery guarantee. Independently validate every recovered artifact against source evidence or known-good data.

### Quick start

```bash
# Configure and build
cmake -B build -S .
cmake --build build -j"$(nproc)"

# Inspect a copied .ibd file. Output defaults to /tmp/recovered.
./build/mysql_recovery_pro --ibd=/path/to/table.ibd --output=/recovery/output

# Provide an explicit version when automatic detection is ambiguous.
./build/mysql_recovery_pro --ibd=/path/to/table.ibd \
  --mysql-version=8.4 --output=/recovery/output

# Generate report artifacts at /recovery/report.json, .txt, and .html.
./build/mysql_recovery_pro --ibd=/path/to/table.ibd --output=/recovery/output \
  --report-format=json,text,html --report-path=/recovery/report

# Display the text artifact at /recovery/report.txt.
./build/mysql_recovery_pro --show-last-report --output=/recovery/output \
  --report-path=/recovery/report
```

`--report-path` is an extensionless base path. Allowed `--report-format` values are `json`, `text`, and `html`; the default is `text`.

### Raw-device handling

For disk carving, acquire a read-only forensic image when possible. A raw block-device command requires `--disk-carving` and `--device-path`; privileges alone do not bypass the safety gate.

```bash
./build/mysql_recovery_pro --disk-carving --device-path=/path/to/image \
  --filesystem=ext4 --output=/recovery/output
```

The filesystem selector accepts `ext4` or `xfs`. Do not operate on mounted, writable, or otherwise unapproved devices.

### Manual schema

When source metadata is unavailable, provide a schema in the supported compact form:

```bash
./build/mysql_recovery_pro --ibd=/path/to/table.ibd \
  --schema='id:INT:4:pk,name:VARCHAR:100' --output=/recovery/output
```

A supplied schema is evidence supplied by the operator; verify parsed values independently. Do not assume SQL fragments or `CREATE TABLE` syntax are accepted schema input.

### Verification

```bash
# Non-fixture suite
ctest --test-dir build --output-on-failure -LE REAL_MYSQL

# Real-MySQL fixture suite (missing fixtures are skipped)
ctest --test-dir build --output-on-failure -L REAL_MYSQL
```

### Documentation

- [English User Guide](docs/en/USER_GUIDE_PRO.md)
- [中文用户指南](docs/zh/USER_GUIDE_PRO.md)
- [English Disaster Recovery Guide](docs/en/DISASTER_RECOVERY_GUIDE.md)
- [中文灾难恢复指南](docs/zh/DISASTER_RECOVERY_GUIDE.md)

---

## 中文

### 概述

MySQL Recovery Pro 提供面向 MySQL 5.6 至 9.0 表空间的 MySQL/InnoDB 检查、恢复和恢复报告代码路径。恢复结果因具体案例而异，取决于源介质状态、覆写历史、损坏程度、Schema 准确性以及所选路径。

### 验证边界

新一轮验证结果待定。既有已验证覆盖包括 **1,544 个排除 `REAL_MYSQL` 的 CTest 测试**，以及 **4 个 `REAL_MYSQL` CTest 套件**。真实 MySQL fixture 套件验证表空间完整性、版本族页面标记、元数据、分区名称和 undo 页面存在性；它们**不**证明端到端行恢复、undo 恢复或闪回恢复。

MySQL 8.4 和 9.0 已映射到 `MYSQL8_7BYTE` undo 格式 profile，并有该映射的单元/回归测试覆盖。这不构成真实 MySQL undo 或闪回恢复能力的证明。

在新结果完成前，不声明按文件系统或版本划分的恢复率。

### 已实现代码路径

- 面向 MySQL 5.6、5.7、8.0、8.4 和 9.0 profile 的表空间检查与页面扫描
- 在有兼容元数据或正确手动 Schema 时进行 Schema 辅助解析
- 生成文本、JSON 和 HTML 格式的恢复报告
- 面向 ext4 与 XFS 源的磁盘雕刻路径，但须通过原始设备安全审批

组件存在不构成恢复保证。应将所有恢复产物与源证据或已知正确的数据独立比对验证。

### 快速开始

```bash
# 配置并编译
cmake -B build -S .
cmake --build build -j"$(nproc)"

# 检查已复制的 .ibd 文件；默认输出目录为 /tmp/recovered。
./build/mysql_recovery_pro --ibd=/path/to/table.ibd --output=/recovery/output

# 自动识别存在歧义时，显式指定版本。
./build/mysql_recovery_pro --ibd=/path/to/table.ibd \
  --mysql-version=8.4 --output=/recovery/output

# 在 /recovery/report 生成 .json、.txt 和 .html 报告。
./build/mysql_recovery_pro --ibd=/path/to/table.ibd --output=/recovery/output \
  --report-format=json,text,html --report-path=/recovery/report

# 显示 /recovery/report.txt 文本报告。
./build/mysql_recovery_pro --show-last-report --output=/recovery/output \
  --report-path=/recovery/report
```

`--report-path` 是不带扩展名的基础路径。`--report-format` 仅接受 `json`、`text` 和 `html`；默认值为 `text`。

### 原始设备处理

执行磁盘雕刻时，应尽可能先制作只读取证镜像。对原始块设备，必须同时指定 `--disk-carving` 与 `--device-path`；提升权限本身不能绕过安全门控。

```bash
./build/mysql_recovery_pro --disk-carving --device-path=/path/to/image \
  --filesystem=ext4 --output=/recovery/output
```

文件系统选择器接受 `ext4` 或 `xfs`。不得对已挂载、可写或其他未经批准的设备执行操作。

### 手动 Schema

源元数据不可用时，可以使用已支持的紧凑格式提供 Schema：

```bash
./build/mysql_recovery_pro --ibd=/path/to/table.ibd \
  --schema='id:INT:4:pk,name:VARCHAR:100' --output=/recovery/output
```

手动 Schema 是操作人员提供的证据；解析结果必须独立验证。不要假设 SQL 片段或 `CREATE TABLE` 语法可作为 Schema 输入。

### 验证

```bash
# 非 fixture 测试集
ctest --test-dir build --output-on-failure -LE REAL_MYSQL

# 真实 MySQL fixture 测试集（fixture 缺失时测试会跳过）
ctest --test-dir build --output-on-failure -L REAL_MYSQL
```

### 文档

- [English User Guide](docs/en/USER_GUIDE_PRO.md)
- [中文用户指南](docs/zh/USER_GUIDE_PRO.md)
- [English Disaster Recovery Guide](docs/en/DISASTER_RECOVERY_GUIDE.md)
- [中文灾难恢复指南](docs/zh/DISASTER_RECOVERY_GUIDE.md)

## License

Copyright © 2026. All rights reserved.
