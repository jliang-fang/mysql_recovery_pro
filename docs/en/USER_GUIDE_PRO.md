# MySQL Recovery Pro — User Guide

## Overview

MySQL Recovery Pro provides MySQL/InnoDB inspection, page scanning, schema-assisted parsing, disk-carving, and recovery-report code paths for tablespaces associated with MySQL 5.6, 5.7, 8.0, 8.4, and 9.0. Recovery is case-specific: source-media condition, overwrite history, corruption, available metadata, schema accuracy, and the selected path determine what can be recovered.

## Validation boundary

Fresh validation completed on 2026-09-16: **1,548 CTest tests excluding `REAL_MYSQL`** and **four `REAL_MYSQL` CTest suites** passed.

The real-MySQL fixture suites validate tablespace integrity, version-family page markers, metadata, partition names, and undo-page presence. They do **not** demonstrate end-to-end row recovery, undo recovery, or flashback recovery.

MySQL 8.4 and 9.0 route to the `MYSQL8_7BYTE` undo-format profile and have unit/regression coverage for that mapping. This is not evidence of real-MySQL undo or flashback recovery.

No filesystem-, version-, scenario-, or field-type-specific recovery rate is claimed while fresh results are pending. Independently validate every recovered artifact against source evidence or known-good data before use.

## Build

```bash
cmake -B build -S .
cmake --build build -j"$(nproc)"
```

## Basic inspection and recovery paths

```bash
# Inspect a copied tablespace. Output defaults to /tmp/recovered.
./build/mysql_recovery_pro --ibd=/path/to/table.ibd --output=/recovery/output

# Supply the version when automatic detection is ambiguous.
./build/mysql_recovery_pro --ibd=/path/to/table.ibd \
  --mysql-version=8.4 --output=/recovery/output
```

Automatic version detection is fail-closed: strong SDI evidence can identify an 8.0-family tablespace, while ambiguous evidence remains unknown. Supply `--mysql-version` when the source version is known and automatic detection is inconclusive. A selected profile guides code-path routing; it is not a recovery guarantee.

## Schema-assisted parsing

When compatible metadata or an accurate manual schema is available, use the supported compact schema form:

```bash
./build/mysql_recovery_pro --ibd=/path/to/table.ibd \
  --schema='id:INT:4:pk,name:VARCHAR:100' --output=/recovery/output
```

A supplied schema is operator-provided evidence. Validate column definitions and parsed values independently. Do not assume SQL fragments or `CREATE TABLE` syntax are accepted as `--schema` input.

## Recovery reports

The CLI can generate text, JSON, and HTML recovery-report artifacts:

```bash
./build/mysql_recovery_pro --ibd=/path/to/table.ibd --output=/recovery/output \
  --report-format=json,text,html --report-path=/recovery/report

# Display the text artifact.
./build/mysql_recovery_pro --show-last-report --output=/recovery/output \
  --report-path=/recovery/report
```

`--report-path` is an extensionless base path. The command writes `<base>.json`, `<base>.txt`, and/or `<base>.html`; when omitted, the base is `<output>/recovery_report`. Accepted `--report-format` values are `json`, `text`, and `html`. Values are trimmed, case-normalized, and deduplicated; the default is `text`. `--show-last-report` reads `<base>.txt`, so a text report must have been requested.

A report describes the tool’s observations and must not be treated as proof that records, undo history, or flashback output were recovered correctly.

## Disk carving and raw-device safety

For disk carving, acquire a read-only forensic image when possible:

```bash
./build/mysql_recovery_pro --disk-carving --device-path=/path/to/image \
  --filesystem=ext4 --output=/recovery/output
```

The filesystem selector accepts `ext4` or `xfs`. A raw block-device command requires both `--disk-carving` and `--device-path`; privileges do not bypass the safety gate. Do not operate on a mounted, writable, or otherwise unapproved device. Preserve the source, record provenance, and validate all artifacts on a separate target.

The safety gate is fail-closed. Before any raw block device is opened, every mandatory fact must be proven: device identity verified, kernel-level read-only confirmed, exclusive access acquired, and the device not mounted. Loop devices additionally require verified backing-file identity under a trusted root; a loop device without a discoverable, policy-accepted backing file is refused. Regular files and directories do not pass through the block-device gate.

## Operational workflow

1. Stop writes to the affected data and preserve the source or acquire a forensic image.
2. Record the MySQL version, table definition, tablespace identity, page size, and overwrite/corruption history.
3. Use an explicit `--mysql-version` when detection is ambiguous.
4. Provide compatible metadata or a verified manual schema when needed.
5. Run the applicable inspection or recovery path against a copy or approved image.
6. Review reports, compare output with source evidence or known-good data, and only then import into an isolated target.

DROP TABLE, TRUNCATE, deleted-row, `rm -rf`, and corruption cases have materially different evidence availability. The listed code paths and examples are operational guidance, not validated recovery-result guarantees.

## Verification

Run non-fixture and real-fixture cohorts separately:

```bash
# Non-fixture tests
ctest --test-dir build --output-on-failure -LE REAL_MYSQL

# Real-MySQL fixture suites; missing fixtures are skipped
ctest --test-dir build --output-on-failure -L REAL_MYSQL
```

A passing unit, regression, or fixture test does not by itself establish end-to-end recovery for a different source version, filesystem, schema, or incident scenario.
