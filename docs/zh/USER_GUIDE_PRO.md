# MySQL Recovery Pro — 用户指南

## 概述

MySQL Recovery Pro 提供面向 MySQL 5.6、5.7、8.0、8.4 和 9.0 相关表空间的 MySQL/InnoDB 检查、页面扫描、Schema 辅助解析、磁盘雕刻和恢复报告代码路径。恢复结果因具体案例而异：源介质状态、覆写历史、损坏程度、可用元数据、Schema 准确性和所选路径都会影响可恢复内容。

## 验证边界

新一轮验证已于 2026-09-16 完成：**1,548 个排除 `REAL_MYSQL` 的 CTest 测试**和 **4 个 `REAL_MYSQL` CTest 套件**均通过。

真实 MySQL fixture 套件验证表空间完整性、版本族页面标记、元数据、分区名称和 undo 页面存在性；它们**不**证明端到端行恢复、undo 恢复或闪回恢复。

MySQL 8.4 和 9.0 已路由到 `MYSQL8_7BYTE` undo 格式 profile，并有该映射的单元/回归测试覆盖。这不构成真实 MySQL undo 或闪回恢复能力的证明。

在新结果完成前，不声明按文件系统、版本、场景或字段类型划分的恢复率。所有恢复产物均须与源证据或已知正确的数据独立比对后再使用。

## 构建

```bash
cmake -B build -S .
cmake --build build -j"$(nproc)"
```

## 基本检查与恢复路径

```bash
# 检查已复制的表空间；默认输出目录为 /tmp/recovered。
./build/mysql_recovery_pro --ibd=/path/to/table.ibd --output=/recovery/output

# 自动识别存在歧义时显式指定版本。
./build/mysql_recovery_pro --ibd=/path/to/table.ibd \
  --mysql-version=8.4 --output=/recovery/output
```

版本自动识别采用 fail-closed 策略：SDI 强证据可以识别 8.0 系列表空间；证据不充分时结果保持未知。已知源版本而自动识别不明确时，请提供 `--mysql-version`。所选 profile 仅用于代码路径路由，不构成恢复保证。

## Schema 辅助解析

当存在兼容元数据或正确的手动 Schema 时，可使用已支持的紧凑格式：

```bash
./build/mysql_recovery_pro --ibd=/path/to/table.ibd \
  --schema='id:INT:4:pk,name:VARCHAR:100' --output=/recovery/output
```

手动 Schema 是操作人员提供的证据。必须独立核对列定义和解析值。不要假设 SQL 片段或 `CREATE TABLE` 语法可作为 `--schema` 输入。

## 恢复报告

CLI 可生成文本、JSON 和 HTML 恢复报告：

```bash
./build/mysql_recovery_pro --ibd=/path/to/table.ibd --output=/recovery/output \
  --report-format=json,text,html --report-path=/recovery/report

# 显示文本报告。
./build/mysql_recovery_pro --show-last-report --output=/recovery/output \
  --report-path=/recovery/report
```

`--report-path` 是不带扩展名的基础路径。命令会写入 `<base>.json`、`<base>.txt` 和/或 `<base>.html`；未指定时基础路径为 `<output>/recovery_report`。`--report-format` 仅接受 `json`、`text` 和 `html`；输入会去除空白、规范化大小写并去重，默认值为 `text`。`--show-last-report` 读取 `<base>.txt`，因此必须先请求文本报告。

报告仅描述工具观察到的信息，不能视为记录、undo 历史或闪回输出已被正确恢复的证明。

## 磁盘雕刻与原始设备安全

执行磁盘雕刻时，应尽可能先制作只读取证镜像：

```bash
./build/mysql_recovery_pro --disk-carving --device-path=/path/to/image \
  --filesystem=ext4 --output=/recovery/output
```

文件系统选择器接受 `ext4` 或 `xfs`。原始块设备命令必须同时指定 `--disk-carving` 与 `--device-path`；提升权限本身不能绕过安全门控。不得对已挂载、可写或其他未经批准的设备执行操作。应保留源证据、记录来源，并在独立目标位置验证所有产物。

安全门控采用 fail-closed 策略。任何原始块设备在被打开前，必须满足全部强制条件：设备身份已验证、内核级只读已确认、独占访问已获取、且设备未被挂载。Loop 设备还需在可信根目录下通过 backing 文件身份验证；缺少可发现且符合策略的 backing 文件的 loop 设备将被拒绝。普通文件与目录不经过块设备门控。

## 操作流程

1. 停止对受影响数据的写入，保留源文件或制作取证镜像。
2. 记录 MySQL 版本、表定义、表空间标识、页面大小及覆写/损坏历史。
3. 自动识别存在歧义时，显式指定 `--mysql-version`。
4. 需要时提供兼容元数据或已核实的手动 Schema。
5. 在副本或经批准的镜像上执行相应检查或恢复路径。
6. 审阅报告，并与源证据或已知正确数据对比；仅在此后导入隔离目标。

DROP TABLE、TRUNCATE、误删行、`rm -rf` 和页面损坏的证据条件存在实质差异。所列代码路径和示例仅为操作指引，不构成已验证的恢复结果保证。

## 验证

应分别运行非 fixture 和真实 fixture 测试：

```bash
# 非 fixture 测试
ctest --test-dir build --output-on-failure -LE REAL_MYSQL

# 真实 MySQL fixture 套件；fixture 缺失时会跳过
ctest --test-dir build --output-on-failure -L REAL_MYSQL
```

单元、回归或 fixture 测试通过，本身并不能证明另一源版本、文件系统、Schema 或事故场景中的端到端恢复能力。
