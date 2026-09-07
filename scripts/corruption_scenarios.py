#!/usr/bin/env python3
"""Deterministic, manifest-producing InnoDB damage generator.

This utility only reads the source regular file and writes a new artifact. It
never operates on devices, symlinks, or the source path itself. Damage is
recorded as exact byte ranges and is not interpreted as a recovery result.
"""

import argparse
import hashlib
import json
import os
import random
import stat
import struct
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

FIL_HEADER_SIZE = 38
FIL_PAGE_PREV_ABS = 8
FIL_PAGE_NEXT_ABS = 12
FIL_PAGE_LSN_ABS = 16
FIL_PAGE_TYPE_ABS = 24
FIL_PAGE_OFFSET_ABS = 4
FIL_PAGE_SPACE_ID_ABS = 34
FIL_PAGE_INDEX = 17855
FIL_PAGE_UNDO_LOG = 17842
FIL_PAGE_SDI = 17854
FIL_PAGE_SDI_ZIP = 17853
FIL_PAGE_BLOB = FIL_PAGE_SDI_ZIP
FIL_PAGE_TYPE_FSP_HDR = 17848
FIL_PAGE_TYPE_XDES = 17849
FIL_PAGE_TYPE_ALLOCATED = 0
PAGE_INFIMUM_REC_OFFSET_ABS = 54
FIL_NULL = 0xFFFFFFFF
VALID_PAGE_TYPES = {
    FIL_PAGE_INDEX, FIL_PAGE_UNDO_LOG, FIL_PAGE_SDI, FIL_PAGE_SDI_ZIP,
    FIL_PAGE_TYPE_FSP_HDR, FIL_PAGE_TYPE_XDES, FIL_PAGE_TYPE_ALLOCATED,
}
LEVEL_I_SCENARIOS = {
    "level1_redo_missing",
    "level1_redo_truncated",
    "level1_redo_checksum",
}
TABLESPACE_INPUT_ROLE = "tablespace"
REDO_INPUT_ROLE = "redo"
PAGE_SIZES = (4096, 8192, 16384, 32768, 65536)


def read_be_uint16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def read_be_uint32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def read_be_uint64(data: bytes, offset: int) -> int:
    return struct.unpack_from(">Q", data, offset)[0]


def write_be_uint16(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">H", data, offset, value & 0xFFFF)


def write_be_uint32(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">I", data, offset, value & 0xFFFFFFFF)


def write_be_uint64(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">Q", data, offset, value & 0xFFFFFFFFFFFFFFFF)


# Compatibility names retained for callers that used the old helpers.
read_le_uint16 = read_be_uint16
read_le_uint32 = read_be_uint32
read_le_uint64 = read_be_uint64
write_le_uint16 = write_be_uint16
write_le_uint32 = write_be_uint32
write_le_uint64 = write_be_uint64


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _regular_file(path: Path, label: str) -> Path:
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{label} must be a regular non-symlink file: {path}")
    mode = path.stat().st_mode
    if not stat.S_ISREG(mode):
        raise ValueError(f"{label} must be a regular file: {path}")
    return path.resolve()


def detect_page_size(data: bytes, requested: Optional[int] = None) -> int:
    if requested:
        if requested not in PAGE_SIZES:
            raise ValueError(f"unsupported page size: {requested}")
        return requested
    candidates = []
    for page_size in PAGE_SIZES:
        if len(data) < page_size or len(data) % page_size:
            continue
        score = 0
        for page_no in range(min(len(data) // page_size, 16)):
            page = data[page_no * page_size:(page_no + 1) * page_size]
            page_type = read_be_uint16(page, FIL_PAGE_TYPE_ABS)
            stored_no = read_be_uint32(page, FIL_PAGE_OFFSET_ABS)
            if page_type in VALID_PAGE_TYPES:
                score += 1
            if stored_no == page_no:
                score += 2
        candidates.append((score, page_size))
    if not candidates:
        return 16384
    return max(candidates)[1]


def _validate_input_role(scenario: str, input_role: Optional[str]) -> str:
    if scenario in LEVEL_I_SCENARIOS:
        if input_role != REDO_INPUT_ROLE:
            raise ValueError(
                f"{scenario} requires input_role='redo'; refusing unclassified input"
            )
        return REDO_INPUT_ROLE
    if input_role not in (None, TABLESPACE_INPUT_ROLE):
        raise ValueError(f"{scenario} does not accept input_role={input_role!r}")
    return TABLESPACE_INPUT_ROLE


@dataclass(frozen=True)
class PageIdentity:
    page_no: int
    offset: int
    space_id: int
    page_type: int
    lsn: int

    def as_dict(self) -> dict:
        return {
            "page_no": self.page_no,
            "byte_offset": self.offset,
            "space_id": self.space_id,
            "page_type": self.page_type,
            "lsn": self.lsn,
        }


def page_identity(data: bytes, page_no: int, page_size: int) -> Optional[PageIdentity]:
    offset = page_no * page_size
    if offset < 0 or offset + page_size > len(data):
        return None
    page = data[offset:offset + page_size]
    if len(page) < FIL_HEADER_SIZE:
        return None
    stored_no = read_be_uint32(page, FIL_PAGE_OFFSET_ABS)
    page_type = read_be_uint16(page, FIL_PAGE_TYPE_ABS)
    if stored_no != page_no or page_type not in VALID_PAGE_TYPES:
        return None
    return PageIdentity(
        page_no=page_no,
        offset=offset,
        space_id=read_be_uint32(page, FIL_PAGE_SPACE_ID_ABS),
        page_type=page_type,
        lsn=read_be_uint64(page, FIL_PAGE_LSN_ABS),
    )


def validated_pages(data: bytes, page_size: int, include_allocated: bool = False) -> List[PageIdentity]:
    result = []
    for page_no in range(len(data) // page_size):
        identity = page_identity(data, page_no, page_size)
        if identity and (include_allocated or identity.page_type != FIL_PAGE_TYPE_ALLOCATED):
            result.append(identity)
    return result


@dataclass
class DamageRange:
    start: int
    end: int
    reason: str
    before_sha256: str
    after_sha256: str
    page: Optional[dict] = None

    def as_dict(self) -> dict:
        result = {
            "start": self.start,
            "end": self.end,
            "length": self.end - self.start,
            "reason": self.reason,
            "before_sha256": self.before_sha256,
            "after_sha256": self.after_sha256,
        }
        if self.page:
            result["page"] = self.page
        return result


@dataclass
class FileStats:
    total_pages: int = 0
    page_size: int = 0
    file_size: int = 0
    page_types: Dict[int, int] = None

    def __post_init__(self):
        if self.page_types is None:
            self.page_types = {}


def analyze_file(filepath: str, page_size: Optional[int] = None) -> FileStats:
    path = _regular_file(Path(filepath), "input")
    data = path.read_bytes()
    size = detect_page_size(data, page_size)
    stats = FileStats(len(data) // size, size, len(data), {})
    for identity in validated_pages(data, size, include_allocated=True):
        stats.page_types[identity.page_type] = stats.page_types.get(identity.page_type, 0) + 1
    return stats


def _set_range(result: bytearray, start: int, replacement: bytes, reason: str,
               page: Optional[PageIdentity], ranges: List[DamageRange], original: bytes):
    end = start + len(replacement)
    if start < 0 or end > len(result):
        raise ValueError(f"damage range outside file: [{start}, {end})")
    before = original[start:end]
    result[start:end] = replacement
    ranges.append(DamageRange(start, end, reason, sha256(before), sha256(replacement),
                              page.as_dict() if page else None))


def _random_page_list(pages: Sequence[PageIdentity], count: int, rng: random.Random) -> List[PageIdentity]:
    eligible = [page for page in pages if page.page_no != 0]
    if count < 1 or not eligible:
        return []
    return rng.sample(eligible, min(count, len(eligible)))


class CorruptionEngine:
    """Apply exact damage to a byte buffer using a local deterministic RNG."""

    def __init__(self, seed: int = 42):
        self.seed = seed
        self.rng = random.Random(seed)

    def _pages(self, data: bytes, page_size: int) -> List[PageIdentity]:
        return validated_pages(data, page_size)

    def corrupt_random_pages(self, data: bytearray, num_pages: int, page_size: int,
                             exclude_pages: Optional[List[int]] = None):
        ranges = []
        original = bytes(data)
        excluded = set(exclude_pages or [0])
        pages = [page for page in self._pages(original, page_size) if page.page_no not in excluded]
        for page in self.rng.sample(pages, min(num_pages, len(pages))):
            _set_range(data, page.offset, b"\0" * page_size, "zero_page", page, ranges, original)
        return data, [item.page["page_no"] for item in ranges]

    def corrupt_page_headers(self, data: bytearray, page_size: int, page_numbers=None):
        original = bytes(data); ranges = []
        pages = self._pages(original, page_size)
        selected = [p for p in pages if page_numbers is None or p.page_no in page_numbers]
        if page_numbers is None:
            selected = self._random_page_list(pages, max(1, len(pages) * 5 // 100), self.rng)
        for page in selected:
            _set_range(data, page.offset + FIL_PAGE_TYPE_ABS, b"\xff\xff", "page_type", page, ranges, original)
        return data, [item.page["page_no"] for item in ranges]

    def corrupt_infimum_records(self, data, page_size, page_numbers=None):
        return self._damage_byte(data, page_size, page_numbers, PAGE_INFIMUM_REC_OFFSET_ABS, "infimum")

    def corrupt_supremum_records(self, data, page_size, page_numbers=None):
        return self._damage_byte(data, page_size, page_numbers, PAGE_INFIMUM_REC_OFFSET_ABS + 13, "supremum")

    def _damage_byte(self, data, page_size, page_numbers, relative, reason):
        original = bytes(data); ranges = []
        pages = self._pages(original, page_size)
        selected = [p for p in pages if page_numbers is None or p.page_no in page_numbers]
        if page_numbers is None:
            selected = self._random_page_list(pages, max(1, len(pages) * 5 // 100), self.rng)
        for page in selected:
            _set_range(data, page.offset + relative, b"\xff", reason, page, ranges, original)
        return data, [item.page["page_no"] for item in ranges]

    def break_page_chain(self, data, page_size, page_numbers=None):
        original = bytes(data); ranges = []
        pages = self._pages(original, page_size)
        selected = [p for p in pages if page_numbers is None or p.page_no in page_numbers]
        if page_numbers is None:
            selected = self._random_page_list(pages, max(1, len(pages) * 3 // 100), self.rng)
        for page in selected:
            _set_range(data, page.offset + FIL_PAGE_PREV_ABS, b"\xde\xad\xbe\xef", "prev_page", page, ranges, original)
            _set_range(data, page.offset + FIL_PAGE_NEXT_ABS, b"\xde\xad\xbe\xef", "next_page", page, ranges, original)
        return data, [item.page["page_no"] for item in ranges]

    def corrupt_record_chain(self, data, page_size, page_numbers=None):
        # Record next pointers require row-format parsing; damage only a declared
        # record-header byte and preserve this as forensic damage, not a row result.
        return self._damage_byte(data, page_size, page_numbers, PAGE_INFIMUM_REC_OFFSET_ABS + 2, "record_chain")

    def corrupt_metadata(self, data, page_size):
        return self.corrupt_page_headers(data, page_size, [0])

    def truncate_file(self, data, page_size, truncate_ratio=0.3):
        if not 0 < truncate_ratio < 1:
            raise ValueError("truncate_ratio must be between 0 and 1")
        pages = len(data) // page_size
        count = max(1, int(pages * truncate_ratio))
        new_size = len(data) - count * page_size
        return data[:new_size], list(range(pages - count, pages))

    def corrupt_checksums(self, data, page_size, page_numbers=None):
        original = bytes(data); ranges = []
        pages = self._pages(original, page_size)
        selected = [p for p in pages if page_numbers is None or p.page_no in page_numbers]
        if page_numbers is None:
            selected = self._random_page_list(pages, max(1, len(pages) * 10 // 100), self.rng)
        for page in selected:
            _set_range(data, page.offset, b"\xde\xad\xbe\xef", "header_checksum", page, ranges, original)
            _set_range(data, page.offset + page_size - 4, b"\xde\xad\xbe\xef", "trailer_checksum", page, ranges, original)
        return data, [item.page["page_no"] for item in ranges]

    def corrupt_btree_level(self, data, page_size, level_pages=100):
        pages = [p for p in self._pages(bytes(data), page_size) if p.page_type == FIL_PAGE_INDEX and p.page_no != 0]
        return self.corrupt_page_headers(data, page_size, [p.page_no for p in pages[:level_pages]])


def _apply_scenario(data: bytes, scenario: str, engine: CorruptionEngine, page_size: int, kwargs: dict):
    if scenario == "level1_redo_checksum":
        result = bytearray(data); ranges = []
        if len(result) < 4:
            raise ValueError("redo input is too small")
        _set_range(result, 0, b"\xde\xad\xbe\xef", "redo_checksum", None, ranges, data)
        _set_range(result, len(result) - 4, b"\xde\xad\xbe\xef", "redo_trailer_checksum", None, ranges, data)
        return bytes(result), ranges, "LEVEL_I_REDO_CHECKSUM_DAMAGE"
    if scenario == "level1_redo_truncated":
        ratio = kwargs.get("truncate_ratio", 0.1)
        if not 0 < ratio < 1:
            raise ValueError("truncate_ratio must be between 0 and 1")
        size = max(1, len(data) - max(1, int(len(data) * ratio)))
        ranges = [DamageRange(size, len(data), "redo_truncation", sha256(data[size:]), sha256(b""), None)]
        return data[:size], ranges, "LEVEL_I_REDO_TRUNCATION"
    if scenario == "level1_redo_missing":
        ranges = [DamageRange(0, len(data), "redo_missing", sha256(data), sha256(b""), None)]
        return b"", ranges, "LEVEL_I_REDO_MISSING"
    if scenario == "level2_data_page":
        page_numbers = kwargs.get("page_numbers")
        selected = page_numbers or [page.page_no for page in engine._pages(data, page_size)
                                    if page.page_type == FIL_PAGE_INDEX and page.page_no != 0][:1]
        original = bytes(data); result = bytearray(data); ranges = []
        for page_no in selected:
            page = page_identity(original, int(page_no), page_size)
            if page is None or page.page_type != FIL_PAGE_INDEX:
                raise ValueError(f"page {page_no} is not a validated INDEX page")
            start = page.offset + FIL_HEADER_SIZE
            length = min(int(kwargs.get("damage_bytes", 64)), page_size - FIL_HEADER_SIZE - 8)
            _set_range(result, start, b"\0" * length, "data_page_local_damage", page, ranges, original)
        return bytes(result), ranges, "LEVEL_II_DATA_PAGE_DAMAGE"
    if scenario == "level3_sdi_damage":
        original = bytes(data); result = bytearray(data); ranges = []
        pages = [p for p in engine._pages(original, page_size)
                 if p.page_type in (FIL_PAGE_SDI, FIL_PAGE_SDI_ZIP)]
        if not pages:
            raise ValueError("no validated SDI page found; refusing to label dictionary damage")
        for page in pages[:int(kwargs.get("num_pages", 1))]:
            start = page.offset + FIL_HEADER_SIZE
            length = min(int(kwargs.get("damage_bytes", 128)), page_size - FIL_HEADER_SIZE - 8)
            _set_range(result, start, b"\0" * length, "sdi_evidence_damage", page, ranges, original)
        return bytes(result), ranges, "LEVEL_III_SDI_EVIDENCE_DAMAGE"
    scenarios = {
        "random_pages": lambda: engine.corrupt_random_pages(bytearray(data), kwargs.get("num_pages", 1), page_size),
        "page_headers": lambda: engine.corrupt_page_headers(bytearray(data), page_size, kwargs.get("page_numbers")),
        "infimum": lambda: engine.corrupt_infimum_records(bytearray(data), page_size, kwargs.get("page_numbers")),
        "supremum": lambda: engine.corrupt_supremum_records(bytearray(data), page_size, kwargs.get("page_numbers")),
        "chain": lambda: engine.break_page_chain(bytearray(data), page_size, kwargs.get("page_numbers")),
        "record_chain": lambda: engine.corrupt_record_chain(bytearray(data), page_size, kwargs.get("page_numbers")),
        "metadata": lambda: engine.corrupt_metadata(bytearray(data), page_size),
        "truncate": lambda: engine.truncate_file(bytearray(data), page_size, kwargs.get("truncate_ratio", .3)),
        "checksum": lambda: engine.corrupt_checksums(bytearray(data), page_size, kwargs.get("page_numbers")),
        "btree_level": lambda: engine.corrupt_btree_level(bytearray(data), page_size, kwargs.get("level_pages", 100)),
    }
    if scenario not in scenarios:
        raise ValueError(f"unknown scenario: {scenario}")
    result, pages = scenarios[scenario]()
    ranges = [DamageRange(page * page_size, min((page + 1) * page_size, len(data)),
                          scenario, sha256(data[page * page_size:min((page + 1) * page_size, len(data))]),
                          sha256(result[page * page_size:min((page + 1) * page_size, len(result))]),
                          page_identity(data, page, page_size).as_dict() if page_identity(data, page, page_size) else None)
              for page in pages]
    return bytes(result), ranges, "LEGACY_FORENSIC_DAMAGE"


def run_corruption_scenario(input_path: str, output_path: str, scenario: str, seed: int = 42,
                            page_size: Optional[int] = None, manifest_path: Optional[str] = None,
                            input_role: Optional[str] = None, **kwargs) -> Dict:
    input_role = _validate_input_role(scenario, input_role)
    source = _regular_file(Path(input_path), "input")
    target = Path(output_path).resolve()
    if target == source:
        raise ValueError("output must differ from input")
    if target.exists() or target.is_symlink():
        raise FileExistsError(f"refusing to overwrite output: {target}")
    data = source.read_bytes()
    actual_page_size = None if input_role == REDO_INPUT_ROLE else detect_page_size(data, page_size)
    engine = CorruptionEngine(seed)
    damaged, ranges, damage_class = _apply_scenario(data, scenario, engine, actual_page_size, kwargs)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(damaged)
    result = {
        "manifest_version": 1,
        "scenario": scenario,
        "input_role": input_role,
        "damage_class": damage_class,
        "input_file": str(source),
        "output_file": str(target),
        "input_size": len(data),
        "output_size": len(damaged),
        "input_sha256": sha256(data),
        "output_sha256": sha256(damaged),
        "page_size": actual_page_size,
        "seed": seed,
        "ranges": [item.as_dict() for item in ranges],
        "created_at": datetime.now(timezone.utc).isoformat(),
        "source_unchanged_sha256": file_sha256(source),
        "status": "SEALED_DAMAGE_INPUT",
    }
    manifest = Path(manifest_path) if manifest_path else Path(str(target) + ".manifest.json")
    if manifest.exists():
        raise FileExistsError(f"refusing to overwrite manifest: {manifest}")
    manifest.write_text(json.dumps(result, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description="Generate sealed disposable InnoDB damage inputs")
    parser.add_argument("--input", "-i", required=True)
    parser.add_argument("--output", "-o", required=True)
    parser.add_argument("--scenario", "-s", required=True, choices=[
        "level1_redo_missing", "level1_redo_truncated", "level1_redo_checksum",
        "level2_data_page", "level3_sdi_damage", "random_pages", "page_headers",
        "infimum", "supremum", "chain", "record_chain", "metadata", "truncate",
        "checksum", "btree_level"])
    parser.add_argument("--input-role", choices=(REDO_INPUT_ROLE, TABLESPACE_INPUT_ROLE),
                        help="Evidence role; Level I scenarios require redo")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--page-size", type=int, choices=PAGE_SIZES)
    parser.add_argument("--num-pages", type=int, default=1)
    parser.add_argument("--damage-bytes", type=int, default=64)
    parser.add_argument("--truncate-ratio", type=float, default=.3)
    parser.add_argument("--level-pages", type=int, default=100)
    parser.add_argument("--manifest", help="Explicit damage manifest path")
    args = parser.parse_args()
    result = run_corruption_scenario(
        input_path=args.input,
        output_path=args.output,
        scenario=args.scenario,
        seed=args.seed,
        page_size=args.page_size,
        manifest_path=args.manifest,
        input_role=args.input_role,
        num_pages=args.num_pages,
        damage_bytes=args.damage_bytes,
        truncate_ratio=args.truncate_ratio,
        level_pages=args.level_pages,
    )
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
