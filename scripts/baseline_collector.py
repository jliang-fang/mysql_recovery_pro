#!/usr/bin/env python3
"""Collect conservative InnoDB page evidence.

This tool deliberately does not claim row-level ground truth. Use
``disaster_harness.ground_truth`` for typed MySQL exports.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path
from typing import Any, Dict, List, Optional

FIL_PAGE_PREV_ABS = 8
FIL_PAGE_NEXT_ABS = 12
FIL_PAGE_LSN_ABS = 16
FIL_PAGE_TYPE_ABS = 24
FIL_PAGE_OFFSET_ABS = 4
FIL_PAGE_SPACE_ID_ABS = 34
FIL_HEADER_SIZE = 38
FIL_PAGE_INDEX = 17855
FIL_PAGE_UNDO_LOG = 17842
FIL_PAGE_SDI = 17854
FIL_PAGE_SDI_ZIP = 17853
FIL_PAGE_FSP_HDR = 17848
FIL_PAGE_XDES = 17849
FIL_PAGE_ALLOCATED = 0
VALID_PAGE_TYPES = {
    FIL_PAGE_INDEX, FIL_PAGE_UNDO_LOG, FIL_PAGE_SDI, FIL_PAGE_SDI_ZIP,
    FIL_PAGE_FSP_HDR, FIL_PAGE_XDES, FIL_PAGE_ALLOCATED,
}


def be16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def be64(data: bytes, offset: int) -> int:
    return struct.unpack_from(">Q", data, offset)[0]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def page_type_name(page_type: int) -> str:
    return {
        FIL_PAGE_INDEX: "INDEX",
        FIL_PAGE_UNDO_LOG: "UNDO_LOG",
        FIL_PAGE_SDI: "SDI",
        FIL_PAGE_SDI_ZIP: "SDI_ZIP_OR_BLOB",
        FIL_PAGE_FSP_HDR: "FSP_HDR",
        FIL_PAGE_XDES: "XDES",
        FIL_PAGE_ALLOCATED: "ALLOCATED",
    }.get(page_type, "UNKNOWN")


class BaselineCollector:
    def __init__(self, page_size: int = 16384):
        self.page_size = page_size
        self.baseline: Dict[str, Any] = {
            "evidence_status": "EVIDENCE_ONLY",
            "row_level_ground_truth": False,
            "file_info": {},
            "page_info": [],
            "row_hashes": [],
        }

    def read_page(self, data: bytes, page_no: int) -> Optional[bytes]:
        start = page_no * self.page_size
        end = start + self.page_size
        return data[start:end] if 0 <= start and end <= len(data) else None

    def parse_page_header(self, page: bytes, page_no: int = -1) -> Dict[str, Any]:
        if len(page) < FIL_HEADER_SIZE:
            return {"page_no": page_no, "status": "TRUNCATED"}
        page_type = be16(page, FIL_PAGE_TYPE_ABS)
        return {
            "page_no": page_no,
            "stored_page_no": be32(page, FIL_PAGE_OFFSET_ABS),
            "space_id": be32(page, FIL_PAGE_SPACE_ID_ABS),
            "page_type": page_type,
            "page_type_name": page_type_name(page_type),
            "page_lsn": be64(page, FIL_PAGE_LSN_ABS),
            "prev_page": be32(page, FIL_PAGE_PREV_ABS),
            "next_page": be32(page, FIL_PAGE_NEXT_ABS),
            "identity_valid": be32(page, FIL_PAGE_OFFSET_ABS) == page_no and page_type in VALID_PAGE_TYPES,
            "page_sha256": sha256(page),
        }

    def parse_sdi_page(self, page: bytes, page_no: int = -1) -> Dict[str, Any]:
        header = self.parse_page_header(page, page_no)
        return {
            "page_no": page_no,
            "page_type": header.get("page_type_name"),
            "sdi_header_offset": FIL_HEADER_SIZE,
            "sdi_header_sha256": sha256(page[FIL_HEADER_SIZE:FIL_HEADER_SIZE + 32])
            if len(page) >= FIL_HEADER_SIZE + 32 else None,
            "status": "RAW_PAGE_EVIDENCE_ONLY",
        }

    def extract_record_hashes(self, data: bytes) -> List[Dict[str, Any]]:
        """Return no pseudo-records; raw page hashes are not row hashes."""
        self.baseline["record_extraction_note"] = (
            "disabled: page byte windows cannot establish record boundaries, "
            "types, primary keys, NULL values, or field equality"
        )
        return []

    def collect(self, filepath: str) -> Dict[str, Any]:
        path = Path(filepath)
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"IBD input must be a regular non-symlink file: {path}")
        data = path.read_bytes()
        total_pages, remainder = divmod(len(data), self.page_size)
        self.baseline["file_info"] = {
            "path": str(path.resolve()),
            "size": len(data),
            "total_pages": total_pages,
            "trailing_bytes": remainder,
            "sha256": sha256(data),
        }
        page_types = {}
        for page_no in range(total_pages):
            page = self.read_page(data, page_no)
            evidence = self.parse_page_header(page, page_no)
            self.baseline["page_info"].append(evidence)
            name = evidence["page_type_name"]
            page_types[name] = page_types.get(name, 0) + 1
            if evidence.get("page_type") in (FIL_PAGE_SDI, FIL_PAGE_SDI_ZIP):
                self.baseline.setdefault("sdi_pages", []).append(self.parse_sdi_page(page, page_no))
        self.baseline["row_hashes"] = self.extract_record_hashes(data)
        self.baseline["summary"] = {
            "total_files": 1,
            "total_pages": total_pages,
            "valid_identity_pages": sum(item.get("identity_valid", False) for item in self.baseline["page_info"]),
            "page_types": page_types,
            "raw_page_hash_count": len(self.baseline["page_info"]),
            "row_hash_count": 0,
        }
        return self.baseline


def main():
    parser = argparse.ArgumentParser(description="Collect conservative InnoDB page evidence")
    parser.add_argument("--ibd", required=True)
    parser.add_argument("--output", default="baseline.json")
    parser.add_argument("--page-size", type=int, default=16384)
    args = parser.parse_args()
    baseline = BaselineCollector(args.page_size).collect(args.ibd)
    output = Path(args.output)
    if output.exists():
        raise FileExistsError(f"refusing to overwrite baseline: {output}")
    output.write_text(json.dumps(baseline, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Evidence baseline saved: {output}")
    print(f"Valid identity pages: {baseline['summary']['valid_identity_pages']}")


if __name__ == "__main__":
    main()
