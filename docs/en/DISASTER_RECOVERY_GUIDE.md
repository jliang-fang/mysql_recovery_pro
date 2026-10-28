# MySQL Disaster Recovery Field Guide

This guide describes the first-response procedure and the two supported physical recovery paths: detached tablespace parsing and filesystem-aware carving. It is deliberately conservative: preserve the source, isolate writes, and treat every recovery as evidence until an independent clean-instance import proves otherwise.

## Contents

1. [Incident response and physical invariants](#1-incident-response-and-physical-invariants)
2. [Common preparation](#2-common-preparation)
3. [Recovery procedures](#3-recovery-procedures)
4. [Filesystem-aware carving](#4-filesystem-aware-carving)
5. [Schema, SpaceID, and IndexID](#5-schema-spaceid-and-indexid)
6. [Verification and evidence sealing](#6-verification-and-evidence-sealing)
7. [Format support and fail-closed boundaries](#7-format-support-and-fail-closed-boundaries)

## 1. Incident response and physical invariants

`DROP TABLE`, `TRUNCATE TABLE`, and `rm -rf` change InnoDB and filesystem metadata. They do not guarantee that the old sectors are immediately zeroed. Recovery is possible only while the old pages remain physically readable and structurally identifiable.

```text
Stop writers
    │
    ├── acquire a sector image, or lock the source read-only
    ├── unmount the source filesystem
    ├── record device identity and evidence hashes
    └── recover into a different output filesystem
             │
             ├── strict page/record/schema checks
             ├── bounded CSV/SQL evidence, when safe
             └── independent clean-instance import and comparison
```

### What each incident changes

- **`TRUNCATE TABLE`**: a new tablespace or a fast-truncate free-page path may be created; old page bytes can remain until reuse.
- **`DROP TABLE`**: the tablespace file and its extents are released; the physical blocks may still contain old pages.
- **`rm -rf`**: the directory entry and file metadata are removed; subsequent allocation can overwrite the freed blocks.
- **Continuing writes**: redo, journal replay, allocator activity, and ordinary workload writes can destroy the evidence. Treat every minute before isolation as material.

## 2. Common preparation

### 2.1 Stop and isolate

```bash
sudo systemctl stop mysqld
# Use only when the service cannot be stopped cleanly:
sudo pkill -9 mysqld

# Confirm the source is not mounted before imaging or carving.
findmnt /dev/<source-device>
sudo umount /mnt/data
```

Do not run `fsck`, mount the source read-write, copy recovered output back to it, or start MySQL against the damaged data directory.

### 2.2 Prefer a forensic image

A block image makes retries reproducible and avoids competing reads/writes against the original. Store the image, source identity, command line, UTC timestamp, and SHA-256 digest in the case record. If an image is not possible, use a verified read-only source:

```bash
sudo blockdev --setro /dev/<source-device>
sudo blockdev --getro /dev/<source-device>  # expected: 1
```

The safety gate also checks the source identity and mounted state. A read-only flag is necessary but does not replace device identification.

### 2.3 Collect metadata without changing the source

Retain the last known MySQL version, row format, page size, database/table name, schema definition, `space_id`, clustered `index_id`, and any `.frm`/SDI metadata already available. Do not guess these values when a reliable source can be recovered from logs, DDL, or a separate metadata export.

## 3. Recovery procedures

### 3.1 Detached `.ibd` recovery

Use this path when a tablespace copy exists, even if its tail or individual pages are damaged:

```bash
./build/mysql_recovery_pro \
  --ibd=/recovery/input/table.ibd \
  --schema-file=/recovery/schema/table.json \
  --mysql-version=8.4 \
  --output=/recovery/output/table \
  --report-format=json,text \
  --report-path=/recovery/output/table/recovery_report
```

Keep the original input immutable. If the input is truncated, the parser must distinguish complete pages from a partial tail; do not pad or modify the file to make its size appear valid.

### 3.2 `DROP TABLE` and `TRUNCATE TABLE`

Use an image or read-only device and enable filesystem-aware carving:

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

For XFS, change only the filesystem value after verifying the image:

```bash
--filesystem=xfs
```

`DROP` and `TRUNCATE` are not interchangeable at the metadata level, but both require the same first response: stop writes, preserve the medium, and identify the target page population.

### 3.3 Deleted `.ibd` / `rm -rf`

If the server is still alive, do not restart it before checking whether an open deleted file descriptor can be preserved as a separate copy. If the file is no longer open, carve the image or freed extents. The command is the same as above; use the correct filesystem and target identifiers.

## 4. Filesystem-aware carving

| Property | ext4 | XFS |
|---|---|---|
| Allocation model | Block groups and block bitmaps | Allocation groups and free-space trees |
| Candidate discovery | Freed block/extent filtering | AG-parallel extent and free-space traversal |
| Main benefit | Efficient filtering of unlinked blocks | Parallel scanning of independent AGs |
| Main caveat | Journal and allocator activity can reuse blocks | Extent reuse and stale pages require strict page identity checks |

The filesystem layer only proposes physical candidates. The InnoDB layer still validates page size, page type, space identity, index identity, checksum policy, record chain, key ordering, field boundaries, and external LOB pointers.

## 5. Schema, SpaceID, and IndexID

A schema file is the decoding authority for structured output. It should contain physical column positions, nullability, signedness, lengths/precision, charset information, and the primary-key index. `--target-space-ids` and `--target-index-ids` constrain carving before records are exported.

The most important anti-contamination rule is:

```text
candidate page
  → target SpaceID
  → target clustered IndexID
  → valid page and record structure
  → schema decode
  → key order and duplicate checks
  → output contract
```

For external values, the decoder distinguishes the 20-byte Barracuda pointer from the Antelope COMPACT layout containing a 768-byte inline prefix plus the pointer. Unsupported or incomplete chains are rejected rather than shortened silently.

## 6. Verification and evidence sealing

### 6.1 Inspect the result contract

Review at least:

- `structured_rows`;
- `checksum_rejected_pages`;
- `overflow_records_dropped`;
- duplicate and ordering diagnostics;
- `importable` and `decoder_validated`;
- output file digests and manifest state.

`importable: false` is intentional. It means the carving process has not self-attested the output, not that every recovered row is invalid.

### 6.2 Independent fresh-instance validation

A qualified recovery run must use a separate clean MySQL instance:

1. create the expected schema;
2. import the emitted SQL/CSV without manual edits;
3. read back every table and compare typed values with the sealed ground truth;
4. record expected rows, actual rows, field mismatches, duplicates, and rejected/missing rows;
5. retain the import logs and result JSON beside the recovery output.

A process exit code or a plausible row count is not a substitute for this comparison.

### 6.3 Evidence location

The qualification artifacts for this repository are under `/app/mysql_recovery_artifacts`. The report and JSON contract are linked from the repository README. Host-local artifact paths may not exist in a normal clone; copy or mount the evidence root when reproducing an audit.

## 7. Format support and fail-closed boundaries

The validated structured paths include the tested COMPACT/DYNAMIC layouts and page sizes for the supported MySQL versions. REDUNDANT layouts without an authoritative field-boundary descriptor, compressed tablespaces without a fixture-validated decompressor, encrypted/TDE candidates without keys, and malformed pages are handled as evidence-only or unsupported inputs.

Typical corruption outcomes are intentionally bounded:

- checksum/body flips reject affected pages rather than exporting corrupted fields;
- broken `REC_NEXT` links are bounded to prevent loops and field shifts;
- partial tails stop at the last complete page;
- invalid compression metadata or TDE candidates produce no importable SQL/CSV;
- incomplete BLOB chains are counted as dropped/uncertain, not silently truncated.

### Related documentation

- [User guide](USER_GUIDE_PRO.md)
- [Implementation principles](IMPLEMENTATION_PRINCIPLES.md)
- [Chinese field guide](../zh/DISASTER_RECOVERY_GUIDE.md)
- [Authoritative validation report](../../../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md)
