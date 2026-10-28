# MySQL Recovery Pro — Implementation Principles

This document explains the physical model used by MySQL Recovery Pro. It is an implementation and audit reference, not a promise that every theoretical path is enabled for every MySQL version. The tested support boundary is maintained in the [validation evidence report](../../../mysql_recovery_artifacts/batch_authoritative/disaster_recovery_validation_report.md).

## Contents

1. [Architecture and evidence boundary](#1-architecture-and-evidence-boundary)
2. [InnoDB page and record layout](#2-innodb-page-and-record-layout)
3. [Structural checks and conflict resolution](#3-structural-checks-and-conflict-resolution)
4. [Typed field decoding and external LOBs](#4-typed-field-decoding-and-external-lobs)
5. [Filesystem-aware carving](#5-filesystem-aware-carving)
6. [Audit lifecycle and fail-closed policy](#6-audit-lifecycle-and-fail-closed-policy)

## 1. Architecture and evidence boundary

The recovery pipeline is deliberately layered:

```text
source safety and device identity
            │
filesystem candidate discovery (ext4 / XFS)
            │
InnoDB page identity and integrity gates
            │
record chain and clustered-key validation
            │
Schema-driven field decoding and LOB assembly
            │
CSV/SQL evidence contract + manifest
            │
independent clean-instance import and comparison
```

The first five layers operate on physical evidence. The last layer is intentionally outside the decoder. This separation prevents a parser from certifying its own output as safe to import.

## 2. InnoDB page and record layout

For a page size `N`, the basic page shape is:

```text
FIL header (38 bytes)
  page checksum / space-or-checksum
  page number
  previous / next page
  page LSN
  page type
  space ID
page body
  page header, infimum, records, supremum, directory
FIL trailer (8 bytes)
```

Important FIL header fields are validated before records are decoded:

| Offset | Size | Field | Recovery use |
|---:|---:|---|---|
| 0 | 4 | checksum or space/checksum | Integrity gate. |
| 4 | 4 | page number | Page identity and duplicate resolution. |
| 8 | 4 | previous page | Chain consistency. |
| 12 | 4 | next page | Chain consistency. |
| 16 | 8 | page LSN | Newer/older page conflict resolution. |
| 24 | 2 | page type | INDEX, BLOB, SDI, and related layout selection. |
| 34 | 4 | space ID | Cross-tablespace contamination filter. |

### 2.1 Compact/Dynamic record metadata

For COMPACT and DYNAMIC records, variable-length metadata and the NULL bitmap are stored before the record data. The record data includes hidden InnoDB transaction fields and user columns. The decoder must account for the physical order rather than assuming the logical SQL order.

For a variable-length field whose maximum byte length exceeds 255:

- a short value can use one length byte;
- a longer value uses two bytes;
- the external-storage flag is part of the encoded value;
- when the external flag is set, the inline byte count is derived from the encoded length, not guessed from the pointer alone.

The tested Antelope rule is:

```text
inline_bytes = encoded_size - 16384 = 788
              = 768-byte inline prefix + 20-byte BTR_EXTERN pointer
```

For DYNAMIC/Barracuda rows, the clustered record normally retains the 20-byte pointer and the payload is assembled from the external LOB chain.

## 3. Structural checks and conflict resolution

### 3.1 Clustered-key ordering

Records linked through a clustered leaf page must be strictly ordered by the clustered key:

\[
K(r_1) < K(r_2) < \dots < K(r_n)
\]

Carving can encounter stale pages, foreign pages, or a single corrupted record. The implementation uses bounded local resolution and rejects ambiguous collisions. It never “fixes” an ordering violation by inventing a key or shifting fields.

### 3.2 Space and index filtering

A candidate page is eligible only when its physical identity matches the requested target:

\[
SpaceID(P) \in S_{target} \land IndexID(P) \in I_{target}
\]

This filter is applied before structured export so that old allocations and unrelated system tables do not contaminate the result set.

### 3.3 Page and LSN conflicts

The same `(space_id, page_no)` can appear more than once on media without TRIM or after allocator reuse. Candidates are compared using page identity, integrity state, LSN, and record evidence. A newer valid page outranks an older duplicate; equal-LSN ties are resolved conservatively rather than by arbitrary scan order.

### 3.4 Checksum gate

Checksum validation is a gate, not a warning. A page that cannot satisfy the applicable checksum/integrity policy is rejected from structured output. Corruption matrices therefore distinguish:

- healthy structured rows;
- bounded loss attributable to rejected pages;
- evidence-only artifacts where no safe structured row can be claimed.

## 4. Typed field decoding and external LOBs

The schema-driven decoder handles the physical encodings exercised by the qualification fixtures:

- signed and unsigned integer sort transformations;
- packed `NEWDECIMAL` groups;
- `TIME2`, `DATETIME2`, and `TIMESTAMP2` with fractional precision;
- character and binary values with NULL/empty distinction;
- ENUM/SET value mapping;
- MySQL binary JSON trees;
- inline and external TEXT/BLOB data.

Temporal values are range-checked. Invalid month/day/hour/minute/second values are structural evidence of a bad boundary and are rejected rather than formatted as plausible timestamps.

### 4.1 LOB chain assembly

External values are reconstructed only when every pointer and fragment is consistent with the selected physical format:

```text
clustered record
  └── 20-byte external pointer
        ├── legacy 0x000A-style chain (5.x)
        ├── compressed/legacy variants (evidence or supported path by fixture)
        └── modern LOB entry/fragment chain (8.x/9.x)
```

Inline prefixes are preserved in their original order. Missing, cyclic, out-of-range, or truncated fragments are recorded as dropped/uncertain and never silently replaced with a shorter value.

## 5. Filesystem-aware carving

### 5.1 ext4

The ext4 scanner reads filesystem geometry and block-group metadata, identifies candidate freed blocks/Extents, and passes physical page candidates into InnoDB identity filters. Journal replay and post-incident allocation can invalidate a candidate; filesystem discovery is therefore not proof of recoverability.

### 5.2 XFS

The XFS scanner partitions work by allocation group and consults AG metadata/free-space structures to locate candidate Extents. Parallelism changes discovery speed, not the validation rules. Every candidate still requires page identity, SpaceID/IndexID, checksum, record, and Schema checks.

## 6. Audit lifecycle and fail-closed policy

A sealed run follows a state progression similar to:

```text
PLANNED
  → PRECHECKED
  → MEDIA_PREPARED
  → TRUTH_SEALED
  → RECOVERY_ATTEMPTED
  → CONTRACT_PERSISTED
  → IMPORT_VALIDATED
  → REPORTED
  → SEALED
```

The exact lifecycle fields are recorded by the validation harness. Inputs, outputs, invocation metadata, and comparison results are hashed into the manifest; the seal binds the evidence to the recorded source identity.

### 6.1 Contract semantics

- `structured_recovery`: schema and physical checks allowed typed output;
- `forensic_evidence`: physical evidence was found but structured export was refused;
- `PASS_BOUNDED_STRUCTURED`: intact regions passed while damaged regions were explicitly bounded;
- `PASS_EVIDENCE_ONLY`: refusal was the expected safe outcome;
- `UNSUPPORTED`: the combination is outside the verified decoder boundary;
- `importable: false`: the physical recovery tool does not self-attest importability.

### 6.2 Independent re-import boundary

The final qualification is performed by a separate clean MySQL instance. It creates the expected schema, imports the emitted data without manual repair, reads it back, and compares typed values against the sealed ground truth. A recovery tool must not convert a plausible row count into a PASS without this boundary.

### 6.3 Conservative failure behavior

The fail-closed rule is a correctness feature:

- reject ambiguous field boundaries;
- reject invalid checksums and impossible temporal values;
- reject broken or incomplete external chains;
- reject unsupported compression/encryption unless the required validated backend and keys exist;
- preserve diagnostics and evidence for later analysis.

For operational procedures and parameter details, see the [user guide](USER_GUIDE_PRO.md) and [field guide](DISASTER_RECOVERY_GUIDE.md).
