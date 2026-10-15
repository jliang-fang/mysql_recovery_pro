# MySQL Recovery Pro — Implementation Principles & Theoretical Foundations

## Table of Contents
1. [Architecture & Layering](#1-architecture--layering)
2. [InnoDB Physical Storage Layout & Structural Invariants](#2-innodb-physical-storage-layout--structural-invariants)
3. [Core Algorithms & Mathematical Models](#3-core-algorithms--mathematical-models)
4. [High-Precision Field Deserialization Engine](#4-high-precision-field-deserialization-engine)
5. [Filesystem-Aware Physical Sector Carving](#5-filesystem-aware-physical-sector-carving)
6. [Forensic Auditability & Fail-Closed Safety Gates](#6-forensic-auditability--fail-closed-safety-gates)

---

## 1. Architecture & Layering

MySQL Recovery Pro implements a decoupled, forensic-grade layered architecture:

```
┌────────────────────────────────────────────────────────────────────────┐
│                        mysql_recovery_pro                              │
├────────────────────────────────────────────────────────────────────────┤
│ 1. Physical Media Safety & Forensics Gate (Kernel RO / WWN / Seal)      │
├────────────────────────────────────────────────────────────────────────┤
│ 2. Filesystem Extent Carving Layer (ext4 / XFS Allocation Trees)       │
│    ├── ext4: Block Group Descriptors + Free Block Bitmap Parsing       │
│    └── XFS : Allocation Group (AG) Multithreading + Free B+Trees       │
├────────────────────────────────────────────────────────────────────────┤
│ 3. InnoDB Page Engine (B+Tree & Page Parser)                           │
│    ├── FIL Header / Page Header / Infimum-Supremum Chain Traversal     │
│    ├── Monotonic Clustered Key Invariant Verification                  │
│    ├── LSN Epoch & Doublewrite Buffer Conflict Resolution              │
│    └── Polymorphic Checksum Gate (ChecksumGateBatch: Legacy / CRC32C)  │
├────────────────────────────────────────────────────────────────────────┤
│ 4. Field Deserialization & LOB Reassembly                              │
│    ├── Compact / Dynamic Variable-Length Arrays & NULL Bitmaps         │
│    ├── Antelope 788-Byte Prefix + 20-Byte Pointer Parsing              │
│    ├── Barracuda 20-Byte Pointer + LOB Tree Reassembly                 │
│    └── Temporal & Domain Sanity Checking (Out-of-Bounds Interception)  │
├────────────────────────────────────────────────────────────────────────┤
│ 5. Emission & Verification (CSV/SQL Stream / Fresh-Instance Import)   │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. InnoDB Physical Storage Layout & Structural Invariants

### 2.1 Physical Page Topology (FIL Header & Trailer)

InnoDB default page size is 16 KiB (16,384 bytes):

| Byte Range | Length | Field Name | Physical Semantics & Forensic Purpose |
| :--- | :---: | :--- | :--- |
| `0..3` | 4B | `FIL_PAGE_SPACE_OR_CHKSUM` | Page checksum (or tablespace checksum) |
| `4..7` | 4B | `FIL_PAGE_OFFSET` | Logical page number within tablespace |
| `8..11` | 4B | `FIL_PAGE_PREV` | Predecessor page pointer at current B+Tree level |
| `12..15` | 4B | `FIL_PAGE_NEXT` | Successor page pointer at current B+Tree level |
| `16..23` | 8B | `FIL_PAGE_LSN` | Log Sequence Number of the last modification |
| `24..25` | 2B | `FIL_PAGE_TYPE` | Type identifier: `0x45BF`(INDEX), `0x000A`(BLOB 5.x), `0x0018`(BLOB 8.x), `0x45BD`(SDI) |
| `26..33` | 8B | `FIL_PAGE_FILE_FLUSH_LSN` | System tablespace Page 0 only; 0 on data pages |
| `34..37` | 4B | `FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID` | Tablespace Space ID |
| `38..page_size-9` | Variable | Page Body | Page Header (36B), Infimum, User Records, Supremum, Page Directory |
| `page_size-8..page_size-1` | 8B | `FIL_PAGE_END_LSN_CHECKSUM` | Old-style checksum + lower 32 bits of LSN |

### 2.2 Record Header & Variable-Length Storage Dialects

InnoDB records expand in two directions from the record origin (`rec_origin`):
- **Negative Direction (Record Header & Metadata, traversed backwards)**:
  `[Variable Lengths (Reverse)] ── [NULL Bitmap (Big-Endian)] ── [5-Byte Header (origin - 5)]`
- **Positive Direction (Record Data, traversed forward)**:
  `[Primary Key] ── [DB_TRX_ID (6B)] ── [DB_ROLL_PTR (7B)] ── [User Columns]`

#### Variable-Length Field Encoding (rem0rec Specification)
For variable columns (`VARCHAR`, `BLOB`, `TEXT`, `VARBINARY`, multi-byte `CHAR`):
1. If max possible byte length ($\text{col\_length} \times \text{mb\_maxlen}$) $\le 255$:
   Encoded in **1 byte**, storing the exact physical byte length.
2. If max possible byte length $> 255$:
   - If actual length $< 128$: encoded in **1 byte** (high bit = 0);
   - If actual length $\ge 128$: encoded in **2 bytes** (high bit = 1).
   - **Off-Page Flag (`BTR_EXTERN_FIELD`, Bit 6)**:
     If the column data is stored externally, Bit 6 (`0x40`) of the high byte is set.
     The combined 2-byte value is:
     $$\text{size} = \text{byte}_2 + (\text{byte}_1 - 128) \times 256$$
     Since Bit 6 is set, $\text{size} \ge 16384$ ($0\text{x}4000$).
     **The exact inline bytes stored in the clustered leaf page are strictly computed as**:
     $$\text{inline\_bytes} = \text{size} - 16384$$
     - In **DYNAMIC** row format: $\text{inline\_bytes} = 20$ (20-byte pointer only);
     - In **COMPACT (Antelope)** format: $\text{inline\_bytes} = 788$ (768-byte inline prefix + 20-byte pointer).

---

## 3. Core Algorithms & Mathematical Models

### 3.1 Monotonic Key Ordering Invariant

In InnoDB B+Tree clustered indexes, physical leaf records traversed via `REC_NEXT` must preserve strict lexicographical order:
$$\forall i \in [1, k-1], \quad \text{Key}(r_i) < \text{Key}(r_{i+1})$$

When carving across sectors containing remnants of stale tables or overwritten blocks, invalid record chains can emerge. MySQL Recovery Pro resolves ordering violations dynamically:

```cpp
// Sliding window resolution for B-tree key order violations
for (size_t i = 1; i < records.size(); ++i) {
    if (records[i].key <= records[i - 1].key) {
        bool drop_prev_restores = (i < 2 || records[i].key > records[i - 2].key) &&
                                  (i + 1 >= records.size() || records[i + 1].key > records[i].key);
        bool drop_curr_restores = (i + 1 >= records.size() || records[i + 1].key > records[i - 1].key);
        
        if (drop_prev_restores && !drop_curr_restores) {
            dropped[i - 1] = true;
        } else if (drop_curr_restores && !drop_prev_restores) {
            dropped[i] = true;
        } else {
            // Fail closed: drop both to guarantee zero corrupted records
            dropped[i - 1] = true;
            dropped[i] = true;
        }
    }
}
```

### 3.2 LSN Conflict Resolution (Doublewrite & Stale Pages)
On media without TRIM, identical `(space_id, page_no)` tuples may appear across multiple block groups. The engine evaluates a strict partial order:
$$\mathcal{P}_1 \succ \mathcal{P}_2 \iff (\text{LSN}_1 > \text{LSN}_2) \lor (\text{LSN}_1 = \text{LSN}_2 \land \text{N\_RECS}_1 > \text{N\_RECS}_2)$$
This guarantees that only the newest persistent snapshot is parsed.

---

## 4. High-Precision Field Deserialization Engine

1. **Integer Deserialization**:
   InnoDB flips the sign bit for signed integers (`raw ^ (1ULL << (bits - 1))`) so that physical byte order matches numerical sort order. The engine reverses this mapping cleanly.
2. **Fixed-Point Decimal (`NEWDECIMAL`)**:
   Decimals are packed into 9-digit segments (4 bytes each). Negative numbers are fully bit-inverted; positive numbers have the sign bit inverted on the first segment.
3. **Temporal2 Encodings (`TIME2`, `DATETIME2`, `TIMESTAMP2`)**:
   - `TIME2`: 3-byte base time with $+8388608$ offset, followed by fractional bytes;
   - `DATETIME2`: 5-byte packed date and time ($19\text{-bit months} + 5\text{-bit day} + 5\text{-bit hour} + 6\text{-bit minute} + 6\text{-bit second}$);
   - `TIMESTAMP2`: 4-byte big-endian Unix timestamp + fractional bytes.
   - **Domain Sanity Checks**:
     Asserts $m \in [1, 12]$, $d \in [1, 31]$, $h \le 838$, $mi \le 59$, $s \le 59$. Any decoded record violating these bounds is rejected as structural corruption.
4. **Binary JSON (Type 31)**:
   Full parsing of MySQL 8.0/8.4/9.0 binary JSON documents (`JSON_TYPE_SMALL_OBJECT`, `JSON_TYPE_LARGE_ARRAY`, etc.), extracting keys, offsets, and nested hierarchies losslessly.

---

## 5. Filesystem-Aware Physical Sector Carving

### 5.1 ext4 Extent & Block Group Carving
- Reads superblock (`Superblock`) to confirm 4 KiB block size and 32,768 blocks per group;
- Traverses `ext4_group_desc` to read `bg_block_bitmap`, mapping bits with value 0 (unlinked extents) into candidates;
- Filters candidate pages against `target_space_ids` prior to enqueuing into BLOB and INDEX queues, preventing cross-tablespace contamination.

### 5.2 XFS Allocation Group (AG) Carving
- Leverages XFS AG partitioning to distribute parallel workers across independent storage zones;
- Each worker inspects `xfs_agf` (Free Space B+Tree) and `xfs_agi` (Free Inode B+Tree) to retrieve unlinked extents directly from physical sectors.

---

## 6. Forensic Auditability & Fail-Closed Safety Gates

### 6.1 State Machine Lifecycle
Recovery transitions deterministically through a verifiable lifecycle:
$$\text{PLANNED} \longrightarrow \text{PRECHECKED} \longrightarrow \text{MEDIA\_PREPARED} \longrightarrow \text{TRUTH\_SEALED} \longrightarrow \text{PERSISTED} \longrightarrow \text{SCENARIO\_APPLIED} \longrightarrow \text{RECOVERY\_ATTEMPTED} \longrightarrow \text{IMPORT\_VALIDATED} \longrightarrow \text{REPORTED} \longrightarrow \text{SEALED}$$

- Every input, log, CSV, and validation result is recorded with its SHA-256 digest in `manifest.json`;
- The run directory is sealed with `manifest.seal` binding to device hardware identity (WWN / Serial).

### 6.2 Round-Trip Re-Import Validation Boundary
Every carved dataset is piped into an independent clean-instance MySQL container. Recovery is granted **`PASS`** status only when DDL creation and SQL insertion succeed with zero errors, and read-back yields zero field mismatches against the canonical ground truth.
