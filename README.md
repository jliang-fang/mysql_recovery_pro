# MySQL Recovery Pro

> **InnoDB physical forensics and disaster recovery for MySQL 5.6–9.0**
> Schema-aware recovery of detached `.ibd` files and deleted data from ext4/XFS media, with an independent verification boundary.

[English](#english) · [中文](#中文)

---

<a id="english"></a>

## English

### What this project does

MySQL Recovery Pro reads InnoDB pages directly. It does not require a running MySQL server, a logical backup, or a healthy copy of the original table. The engine combines:

- direct `.ibd` parsing and bounded salvage;
- ext4 block-group and XFS allocation-group-aware carving;
- InnoDB page, record, clustered-index, checksum, and LOB validation;
- schema-guided decoding for numeric, temporal, character, JSON, ENUM/SET, and BLOB/TEXT columns;
- Antelope/COMPACT and Barracuda/DYNAMIC layout handling;
- deterministic deduplication, primary-key ordering checks, and fail-closed export contracts;
- machine-readable results and sealed forensic evidence.

The intended result is a **recovery evidence set**, not an assertion that an arbitrary output is safe to import. Importability is established separately by loading the emitted SQL/CSV into a clean MySQL instance and comparing it with the sealed ground truth.

### Supported recovery situations

| Situation | Recommended mode | Notes |
|---|---|---|
| Detached or damaged `.ibd` | Direct tablespace recovery | Preserve the original file and work on a forensic copy. |
| `DROP TABLE` | Disk carving or tablespace recovery | Supply the target `space_id` and clustered `index_id` whenever available. |
| `TRUNCATE TABLE` | Disk carving | Stop writes immediately; old pages may remain until overwritten. |
| Deleted `.ibd` / `rm -rf` | Disk carving | The probability of recovery depends on subsequent block reuse. |
| Corrupted pages or partial media | Bounded salvage / evidence mode | Uncertain structures are rejected instead of exported as shifted rows. |

### Verified compatibility scope

The completed qualification campaign covered MySQL **5.6, 5.7, 8.0, 8.4 LTS, and 9.0 Innovation**, InnoDB page sizes used by the test matrix, and both **ext4** and **XFS** disaster paths. The qualification data is retained under `/app/mysql_recovery_artifacts`.

The campaign used multi-column datasets containing numeric, temporal, text/LOB, structured JSON, and composite-key values. Results were checked by strict typed comparison and, where the contract was structured, by independent fresh-instance import. The published qualification summary is intentionally separate from the detailed per-run evidence:

- **Phase A** — offline `.ibd` format and page-size matrix;
- **Phase B** — filesystem scenarios: baseline, `DROP`, `TRUNCATE`, and `rm -rf`;
- **fresh import** — clean MySQL instance, DDL, insert, read-back, and field comparison;
- **corruption matrix** — checksum, page-header, record-chain, truncation, compression, and TDE candidate boundaries;
- **raw-media qualification** — physical-device evidence with device identity and run sealing.

For the exact run list, hashes, environment, and per-case contracts, see:

- [Authoritative validation report (Markdown)](../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md)
- [Authoritative validation report (JSON)](../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.json)
- [Validation evidence root](../mysql_recovery_artifacts/batch_authoritative/)

> The artifact paths above are host-local evidence paths. They are not expected to resolve in a fresh clone unless the evidence directory is supplied separately.

### Build

#### Requirements

- Linux kernel 5.4 or later;
- GCC/G++ 11+ or Clang 14+ with C++17 support;
- CMake 3.20+;
- OpenSSL and zlib development packages;
- liburing is optional and used for asynchronous I/O where available.

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### Safe recovery workflow

**Do not point a first run at a mounted production device. Do not write recovered files to the source device.** The minimum safe sequence is:

1. Stop MySQL and prevent further writes.
2. Acquire a sector image or attach the source read-only.
3. Verify the device identity and unmounted state.
4. Work in a separate output directory and preserve manifests/logs.
5. Run recovery with the schema and physical identifiers.
6. Inspect `result.json`, CSV/SQL contracts, and the run manifest.
7. Perform independent fresh-instance import and typed comparison.
8. Seal and archive the evidence before any repair or reformat operation.

### CLI examples

#### Recover a detached tablespace

```bash
./build/mysql_recovery_pro \
  --ibd=/recovery/input/table.ibd \
  --schema-file=/recovery/schema/table.json \
  --mysql-version=8.4 \
  --output=/recovery/output/table \
  --report-format=json,text \
  --report-path=/recovery/output/table/recovery_report
```

#### Carve an image or read-only device

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

For a real block device, perform the safety checks first and substitute the device path only after it has been unmounted and placed in a read-only workflow:

```bash
sudo blockdev --setro /dev/<source-device>
sudo blockdev --getro /dev/<source-device>  # expected: 1
```

### Schema and physical identifiers

A schema file supplies the physical field order and type information needed for strict decoding. The two identifiers are equally important during carving:

- `--target-space-ids` prevents pages from another tablespace from being accepted;
- `--target-index-ids` prevents stale pages and unrelated clustered indexes from becoming false positives.

Example:

```json
{
  "table_name": "orders",
  "columns": [
    {"name": "id", "type": 9, "is_nullable": false, "is_unsigned": false,
     "physical_pos": 0, "ordinal_position": 0},
    {"name": "created_at", "type": 19, "is_nullable": false,
     "physical_pos": 1, "ordinal_position": 1, "datetime_precision": 6},
    {"name": "payload", "type": 27, "is_nullable": true,
     "physical_pos": 2, "ordinal_position": 2, "char_length": 65535}
  ],
  "indexes": [
    {"name": "PRIMARY", "is_primary": true, "is_unique": true, "columns": ["id"]}
  ]
}
```

The type IDs are InnoDB physical type IDs, not arbitrary application type names. See the [user guide](docs/en/USER_GUIDE_PRO.md) for the complete field reference.

### Output contract and verdicts

Every run should be interpreted from its machine-readable contract rather than from a row count alone.

| Status / field | Meaning |
|---|---|
| `PASS` | Structured output passed strict comparison and the independent validation gate. |
| `PASS_BOUNDED_STRUCTURED` | Structured output is correct for the intact portion; damaged portions are explicitly bounded. |
| `PASS_EVIDENCE_ONLY` | Evidence was preserved, but the engine refused to emit importable structured data. |
| `UNSUPPORTED` | The physical combination is outside the validated decoder scope. |
| `SETUP_ERROR` | The environment or fixture interrupted a run; it is not a product verdict. |
| `importable: false` | Deliberate safety contract: the carving process does not self-certify importability. |
| `structured_rows` | Rows decoded under the active schema and structural checks. |
| `checksum_rejected_pages` | Pages rejected by checksum or integrity gates. |
| `overflow_records_dropped` | Rows rejected because an external LOB could not be proven complete. |

### Documentation map

| Need | Document |
|---|---|
| Emergency procedure and disaster scenarios | [English field guide](docs/en/DISASTER_RECOVERY_GUIDE.md) · [中文实战指南](docs/zh/DISASTER_RECOVERY_GUIDE.md) |
| Install, CLI, schema, output, and safety | [English user guide](docs/en/USER_GUIDE_PRO.md) · [中文用户指南](docs/zh/USER_GUIDE_PRO.md) |
| Page layout and recovery algorithms | [English implementation principles](docs/en/IMPLEMENTATION_PRINCIPLES.md) · [中文技术原理](docs/zh/IMPLEMENTATION_PRINCIPLES.md) |
| Current validation evidence | [Validation reports](docs/reports/) |

### Scope boundaries

Recovery is physically constrained: overwritten blocks, discarded encryption keys, missing schema facts, and data that never reached the persistence barrier cannot be reconstructed by a parser. Compressed tablespaces, encrypted pages, unsupported row formats, and damaged metadata are therefore handled conservatively. The correct output for an uncertain input is a bounded evidence record or an explicit refusal—not fabricated rows.

---

<a id="中文"></a>

## 中文

### 项目定位

MySQL Recovery Pro 面向 MySQL **5.6、5.7、8.0、8.4 LTS 和 9.0 Innovation** 的 InnoDB 物理取证与灾难恢复。工具直接解析 `.ibd` 表空间和 ext4/XFS 物理介质，不要求数据库在线，也不把健康逻辑备份作为恢复前提。

核心能力包括：

- 独立 `.ibd` 解析、页级校验与有界抢救；
- ext4 块组、XFS 分配组感知的物理扇区雕刻；
- InnoDB 页、记录、聚簇索引、校验和与 LOB 链解析；
- 数值、时间、字符、JSON、ENUM/SET、BLOB/TEXT 等字段的 Schema 引导解码；
- Antelope/COMPACT 与 Barracuda/DYNAMIC 行格式处理；
- 主键保序校验、SpaceID/IndexID 双重过滤、确定性去重和 Fail-Closed 导出；
- `result.json`、manifest 和 SHA-256 封存的审计证据。

恢复输出首先是**取证证据集**，不是工具自行宣称“可以直接导入”的数据库。最终导入性必须在全新的 MySQL 实例中独立执行，并与封存的真实数据逐字段比对。

### 已覆盖的灾难场景

| 场景 | 推荐模式 | 说明 |
|---|---|---|
| 独立或损坏 `.ibd` | 独立表空间恢复 | 必须保留原文件，建议对取证副本操作。 |
| `DROP TABLE` | 磁盘雕刻或表空间恢复 | 尽可能提供目标 `space_id` 与 clustered `index_id`。 |
| `TRUNCATE TABLE` | 磁盘雕刻 | 立即停止写入，旧页可能尚未被覆盖。 |
| `.ibd` 被删除 / `rm -rf` | 磁盘雕刻 | 能否恢复取决于后续块是否被复用。 |
| 页损坏或介质截断 | 有界抢救 / evidence-only | 无法证明边界的数据会被拒绝，不输出错位行。 |

### 验证与证据

最终资格验证覆盖五个 MySQL 版本、ext4/XFS、baseline、DROP、TRUNCATE、rm-rf、独立 fresh-import 以及损坏 fail-closed 矩阵。实验数据已保存于 `/app/mysql_recovery_artifacts`。详细用例、环境信息、哈希和每轮合约请查看：

- [权威验证报告（Markdown）](../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md)
- [权威验证报告（JSON）](../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.json)
- [验证证据目录](../mysql_recovery_artifacts/batch_authoritative/)

上述路径是当前主机上的证据路径，在普通代码克隆中不会自动存在。

### 编译

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### 安全操作顺序

> **不要直接对挂载中的生产盘执行恢复，也不要把输出写回源设备。**

1. 停止 MySQL，阻断后续写入；
2. 优先制作扇区镜像，或将源设备置于只读、未挂载状态；
3. 核对设备身份与挂载状态；
4. 使用独立输出目录保存结果、日志和 manifest；
5. 提供 Schema、SpaceID、IndexID 并执行恢复；
6. 检查 `result.json` 与 CSV/SQL 合约；
7. 在全新 MySQL 实例中执行 fresh-import 与逐字段比对；
8. 完成封存后再考虑修复或重新格式化源介质。

### 命令示例

```bash
# 独立 .ibd 恢复
./build/mysql_recovery_pro \
  --ibd=/recovery/input/table.ibd \
  --schema-file=/recovery/schema/table.json \
  --mysql-version=8.4 \
  --output=/recovery/output/table \
  --report-format=json,text \
  --report-path=/recovery/output/table/recovery_report
```

```bash
# 对镜像文件执行 ext4 物理雕刻
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

真实块设备仅可在已停止服务、已卸载且经过只读保护后使用：

```bash
sudo blockdev --setro /dev/<source-device>
sudo blockdev --getro /dev/<source-device>  # 预期：1
```

### 文档导航

- [中文灾难恢复实战指南](docs/zh/DISASTER_RECOVERY_GUIDE.md) · [English field guide](docs/en/DISASTER_RECOVERY_GUIDE.md)
- [中文用户指南](docs/zh/USER_GUIDE_PRO.md) · [English user guide](docs/en/USER_GUIDE_PRO.md)
- [中文技术原理](docs/zh/IMPLEMENTATION_PRINCIPLES.md) · [English implementation principles](docs/en/IMPLEMENTATION_PRINCIPLES.md)

### 边界与状态解释

物理覆盖、丢失的加密密钥、缺失的 Schema 信息，以及尚未落盘的数据无法由解析器凭空重建。压缩表空间、加密页、不支持的行格式和损坏的元数据会进入保守处理路径。`PASS_EVIDENCE_ONLY` 表示证据被保留但结构化导出被拒绝；`PASS_BOUNDED_STRUCTURED` 表示完好部分结构化正确、损坏部分被明确限定；`importable: false` 是取证安全契约，不等于恢复失败。

---

## 许可证 / License

本项目基于 [PolyForm Noncommercial 1.0.0](LICENSE) 发布：仅限个人学习、研究、教学等非商业用途，禁止商业使用。如需商业授权，请联系作者。

This project is licensed under the [PolyForm Noncommercial 1.0.0](LICENSE): noncommercial use only (personal study, research, education). Commercial use requires a separate license from the author.
