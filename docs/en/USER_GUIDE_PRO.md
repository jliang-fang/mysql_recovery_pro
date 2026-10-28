# MySQL Recovery Pro — User Guide

This guide is the operational reference for MySQL Recovery Pro. It covers installation, input preparation, CLI modes, schema binding, output contracts, and evidence handling.

> **Safety first:** never run a first recovery attempt against a mounted production filesystem. Acquire a sector image or use a verified read-only, unmounted source and write all outputs to another filesystem.

## Contents

1. [Scope and verified support](#1-scope-and-verified-support)
2. [Build and prerequisites](#2-build-and-prerequisites)
3. [Choose a recovery mode](#3-choose-a-recovery-mode)
4. [CLI reference](#4-cli-reference)
5. [Schema and identifier binding](#5-schema-and-identifier-binding)
6. [Output and independent validation](#6-output-and-independent-validation)
7. [Safety checklist and troubleshooting](#7-safety-checklist-and-troubleshooting)

## 1. Scope and verified support

The engine operates on InnoDB physical pages rather than an online SQL connection. It supports two primary inputs:

- a detached or damaged `.ibd` tablespace;
- a disk image or raw block device containing deleted/freed tablespace pages.

The completed qualification campaign covers MySQL 5.6, 5.7, 8.0, 8.4 LTS, and 9.0 Innovation, with ext4 and XFS filesystem scenarios. The dataset families include numeric, temporal, text/LOB, JSON, and composite-key columns. Qualification evidence is stored under `/app/mysql_recovery_artifacts` and is summarized in the [authoritative validation report](../../../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md).

The validated result is a contract, not a row count. A clean result requires structural decoding, strict typed comparison, and—when structured export is claimed—independent fresh-instance import and read-back comparison.

## 2. Build and prerequisites

### Requirements

- Linux kernel 5.4 or later;
- GCC/G++ 11+ or Clang 14+ with C++17 support;
- CMake 3.20+;
- OpenSSL and zlib development packages;
- liburing is optional.

### Build and local tests

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The local test suite validates parser and recovery components. It does not authorize operation on a real production device.

## 3. Choose a recovery mode

| Input or incident | Mode | Required preparation |
|---|---|---|
| Detached `.ibd` | Direct tablespace recovery | Preserve the original and work on a copy. |
| `DROP TABLE` | `--disk-carving` or `.ibd` recovery | Identify `space_id` and clustered `index_id` if possible. |
| `TRUNCATE TABLE` | `--disk-carving` | Stop all writes immediately. |
| Deleted `.ibd` / `rm -rf` | `--disk-carving` | Prefer an image; later writes may overwrite pages. |
| Damaged or truncated input | Bounded salvage | Expect rejected pages or evidence-only output. |

### 3.1 Direct tablespace recovery

```bash
./build/mysql_recovery_pro \
  --ibd=/recovery/input/table.ibd \
  --schema-file=/recovery/schema/table.json \
  --mysql-version=8.4 \
  --output=/recovery/output/table \
  --report-format=json,text \
  --report-path=/recovery/output/table/recovery_report
```

### 3.2 Disk-image or block-device carving

Start with a forensic image whenever possible:

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

For a real device, stop MySQL, unmount the filesystem, confirm the device identity, and apply the kernel read-only lock before starting:

```bash
sudo blockdev --setro /dev/<source-device>
sudo blockdev --getro /dev/<source-device>  # expected: 1
```

## 4. CLI reference

The options below are the options used by the documented recovery paths. Run the built binary's help command for the complete feature inventory.

| Option | Meaning |
|---|---|
| `--ibd=<path>` | Detached `.ibd`, image, or input tablespace path. |
| `--disk-carving` | Enable physical disk/image carving. |
| `--device-path=<path>` | Source image or raw device for carving. |
| `--filesystem=ext4\|xfs` | Filesystem-aware candidate filtering. |
| `--page-size=<bytes>` | InnoDB page size: 4096, 8192, 16384, 32768, or 65536 where supported. |
| `--schema-file=<path>` | JSON schema for strict typed decoding. |
| `--mysql-version=<version>` | Version hint such as `5.6`, `5.7`, `8.0`, `8.4`, or `9.0`. |
| `--target-index-ids=<ids>` | Comma-separated clustered index IDs to retain. |
| `--target-space-ids=<ids>` | Comma-separated tablespace IDs to retain. |
| `--output=<dir>` | Separate output directory. |
| `--result-json=<path>` | Machine-readable result contract. |
| `--report-format=<fmt>` | Comma-separated `json`, `text`, and/or `html`. |
| `--report-path=<path>` | Extensionless report base path. |

Never infer success from process exit code alone. Inspect the result contract and independent comparison report.

## 5. Schema and identifier binding

A schema file describes the physical column order and decoding rules. `physical_pos` is the on-disk position; `ordinal_position` is the logical column order. The primary-key index is used for ordering and duplicate suppression.

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

Common physical type IDs used in fixtures include:

| ID | Physical type |
|---:|---|
| 9 | BIGINT |
| 16 | VARCHAR/VARBINARY family |
| 19 | DATETIME2 |
| 20 | TIME2 |
| 21 | NEWDECIMAL |
| 27 | TEXT/BLOB |
| 31 | Binary JSON |

The `space_id` and clustered `index_id` filters are not cosmetic. They prevent stale pages from another table or an old allocation from becoming a false positive.

## 6. Output and independent validation

A normal output directory contains some or all of:

- `result.json`: structured execution contract;
- CSV/SQL output: emitted only when the decoder proves the contract is safe;
- text/JSON/HTML report files;
- `manifest.json` and `manifest.seal`: input, output, environment, and digest evidence;
- comparison and fresh-import reports produced by the validation harness.

### Verdicts

| Verdict | Interpretation |
|---|---|
| `PASS` | Complete structured result passed strict comparison and the independent validation gate. |
| `PASS_BOUNDED_STRUCTURED` | Intact data is structured correctly; damaged or missing regions are explicitly bounded. |
| `PASS_EVIDENCE_ONLY` | Evidence is retained, but structured/importable export is intentionally refused. |
| `UNSUPPORTED` | The requested physical combination is outside the validated scope. |
| `SETUP_ERROR` | The fixture or environment stopped the run; it is not a product verdict. |

Important contract fields:

- `structured_rows`: rows decoded under schema and structural checks;
- `checksum_rejected_pages`: pages rejected by integrity gates;
- `overflow_records_dropped`: rows whose external LOB could not be proven complete;
- `importable: false`: intentional policy; the carving process does not self-certify importability;
- `decoder_validated: false`: indicates physical carving rather than an online DBMS read.

The final validation step is external: create a clean MySQL instance, apply the expected DDL, import the emitted data, read it back, and compare every field with the sealed truth set. A recovery report must not call a dataset importable without that boundary.

## 7. Safety checklist and troubleshooting

### Before scanning

- [ ] MySQL is stopped and no process can write to the source.
- [ ] The source filesystem is unmounted.
- [ ] The source device model, serial, WWN, and major/minor identity are recorded.
- [ ] A forensic image or read-only source is used.
- [ ] Output, temporary files, and logs are on a different filesystem.
- [ ] Schema, `space_id`, and clustered `index_id` are recorded.

### If no rows are exported

Check the page size, row format/version hint, schema physical order, target IDs, and checksum result. Evidence-only output is expected for encrypted or unsupported compressed layouts; it is safer than exporting shifted values.

### If rows are missing

Check `checksum_rejected_pages`, LOB overflow counters, media coverage, and whether blocks were overwritten after the incident. A missing row is not a field mismatch; the report must keep those quantities separate.

### If fresh import fails

Treat the result as unqualified until the schema, SQL/CSV escaping, character set, NULL handling, and comparison report are inspected. Do not “repair” the recovered data manually and then claim a clean recovery.

## Related documents

- [Disaster recovery field guide](DISASTER_RECOVERY_GUIDE.md)
- [Implementation principles](IMPLEMENTATION_PRINCIPLES.md)
- [Chinese user guide](../zh/USER_GUIDE_PRO.md)
- [Authoritative validation report](../../../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md)
