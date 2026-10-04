#!/usr/bin/env python3
"""Deterministic, fail-closed corruption validation on disposable media images.

Each case starts with the Phase B persistence barrier and then copies the stopped
filesystem image before changing a single bounded region.  The original image,
ground truth and pre-disaster tablespace remain immutable evidence.  Recovery
always reads the derived image, never a host block device.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable

from scripts.disaster_harness.comparator import compare
from scripts.disaster_validation.lifecycle import RunLifecycleError, ValidationRun
from scripts.disaster_validation.matrix import MatrixItem, matrix_item
from scripts.disaster_validation.phase_b_general import (
    CommandResult,
    ScenarioConfig,
    ScenarioDriver,
    _run,
)


_MUTATIONS = (
    "checksum_body_flip",
    "fil_header_page_no_flip",
    "page_type_flip",
    "record_chain_break",
    "tail_truncate_partial_page",
    "fsp_flags_corrupt",
    "garbage_append",
    "tde_candidate",
    "compression_flags_invalid",
    "compression_stride_mismatch",
)

# Mutations applied to a preserved .ibd copy rather than to the filesystem
# image: they target page-0 metadata that only an offline tablespace carries.
_IBD_MUTATIONS = frozenset({
    "tde_candidate",
    "compression_flags_invalid",
    "compression_stride_mismatch",
})

# Every fail-closed .ibd mutation must land in one of these classifications.
_FORENSIC_CLASSIFICATIONS = frozenset({"forensic_evidence", "expected_reject"})

_FIL_PAGE_OFFSET = 4
_FIL_PAGE_TYPE = 24
_FIL_PAGE_LSN = 16
_FIL_PAGE_SPACE_ID = 34
_FSP_SPACE_FLAGS = 54
_INFIMUM_ORIGIN_COMPACT = 99
_REC_HEADER_SIZE_NEW = 5
_TDE_HEADER_SIZE = 45


class CorruptionError(RuntimeError):
    """Raised when a requested mutation cannot be applied safely."""


@dataclass(frozen=True)
class MutationResult:
    name: str
    source_sha256: str
    target_sha256: str
    target_path: str
    before: str
    after: str
    page_no: int | None
    byte_offset: int | None
    page_metadata: dict[str, int] | None
    selection_rule: str

    def document(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "source_sha256": self.source_sha256,
            "target_sha256": self.target_sha256,
            "target_path": self.target_path,
            "before_hex": self.before,
            "after_hex": self.after,
            "page_no": self.page_no,
            "byte_offset": self.byte_offset,
            "page_metadata": self.page_metadata,
            "selection_rule": self.selection_rule,
        }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _require_image(path: Path) -> None:
    if not path.is_file() or path.is_symlink():
        raise CorruptionError(f"mutation source must be a regular non-symlink image: {path}")


def _read_u16_be(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def _read_u32_be(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def _read_u64_be(data: bytes, offset: int) -> int:
    return struct.unpack_from(">Q", data, offset)[0]


def _page_metadata(page: bytes) -> dict[str, int]:
    if len(page) < _FIL_PAGE_SPACE_ID + 4:
        raise CorruptionError("selected page is shorter than its FIL header")
    return {
        "page_number": _read_u32_be(page, _FIL_PAGE_OFFSET),
        "page_type": _read_u16_be(page, _FIL_PAGE_TYPE),
        "lsn": _read_u64_be(page, _FIL_PAGE_LSN),
        "space_id": _read_u32_be(page, _FIL_PAGE_SPACE_ID),
    }


def _find_index_page(image: Path, page_size: int,
                     target_index_id: int | None = None) -> tuple[int, bytes]:
    """Return one complete FIL header from an INDEX page in the raw image.

    The target is a filesystem image, not an ``.ibd`` file: InnoDB logical page
    number is metadata, never a physical byte offset.  We therefore scan at the
    filesystem's 4 KiB allocation granularity and return the actual image byte
    position of a candidate header.

    When the caller knows the clustered index ID, only that index's leaf pages
    are eligible; mutating a foreign system tablespace would prove nothing about
    the table under test.
    """
    _require_image(image)
    if page_size not in (4096, 8192, 16384, 32768, 65536):
        raise CorruptionError(f"unsupported page size: {page_size}")
    page_header_index_id = 66
    page_header_level = 64
    candidates: list[tuple[int, bytes]] = []
    with image.open("rb") as stream:
        offset = 0
        while True:
            header = stream.read(4096)
            if len(header) != 4096:
                break
            if _read_u16_be(header, _FIL_PAGE_TYPE) == 0x45BF:
                if target_index_id is not None:
                    if _read_u64_be(header, page_header_index_id) != target_index_id:
                        offset += 4096
                        continue
                    # Only leaf pages hold user rows; corrupting an internal
                    # node would not exercise record decoding.
                    if _read_u16_be(header, page_header_level) != 0:
                        offset += 4096
                        continue
                candidates.append((offset, header))
            offset += 4096
    if not candidates:
        qualifier = "target index" if target_index_id is not None else "index"
        raise CorruptionError(f"no 4 KiB-aligned {qualifier} leaf page found in image")
    # Pick the median so the corrupted page is neither the first nor the last
    # row carrier: surrounding pages stay intact as a control sample.
    return candidates[len(candidates) // 2]


def _find_fsp_header(image: Path) -> tuple[int, bytes]:
    """Return a physical FSP header page associated with a unique space ID.

    An image may retain historical page-0 copies.  Selecting the first FSP page
    would silently mutate stale data; selecting by the most common FSP
    ``space_id`` makes the choice deterministic and records the full identity.
    """
    _require_image(image)
    candidates: list[tuple[int, bytes]] = []
    with image.open("rb") as stream:
        offset = 0
        while True:
            header = stream.read(4096)
            if len(header) != 4096:
                break
            if (_read_u16_be(header, _FIL_PAGE_TYPE) in {0x0008, 0x45BB} and
                    _read_u32_be(header, _FIL_PAGE_OFFSET) == 0):
                candidates.append((offset, header))
            offset += 4096
    if not candidates:
        raise CorruptionError("no physical FSP header page found in image")
    counts: dict[int, int] = {}
    for _, header in candidates:
        space_id = _read_u32_be(header, _FIL_PAGE_SPACE_ID)
        counts[space_id] = counts.get(space_id, 0) + 1
    selected_space = max(counts, key=lambda space_id: (counts[space_id], space_id))
    selected = [(offset, header) for offset, header in candidates
                if _read_u32_be(header, _FIL_PAGE_SPACE_ID) == selected_space]
    # A retained duplicate is legitimate; mutate the highest physical copy so
    # the low-offset one remains a control sample for the carver.
    return max(selected, key=lambda item: item[0])


def _copy_to_derived_image(source: Path, target: Path) -> None:
    _require_image(source)
    target_parent = target.parent.resolve()
    if not target_parent.is_dir() or target.exists():
        raise CorruptionError(f"derived image target must be a new file: {target}")
    shutil.copyfile(source, target)
    os.chmod(target, 0o600)


def apply_mutation(source: Path, target: Path, page_size: int, name: str,
                   target_index_id: int | None = None) -> MutationResult:
    """Copy ``source`` to ``target`` then mutate only one bounded target region."""
    if name not in _MUTATIONS:
        raise CorruptionError(f"unknown mutation: {name}")
    _copy_to_derived_image(source, target)
    source_digest = sha256_file(source)
    source_size = source.stat().st_size
    if source_size < page_size:
        raise CorruptionError("image is smaller than one logical page")

    selected_page: int | None = None
    metadata: dict[str, int] | None = None
    offset: int | None = None
    before = b""
    after = b""
    rule = ""

    if name in {"checksum_body_flip", "fil_header_page_no_flip", "page_type_flip", "record_chain_break"}:
        image_offset, page = _find_index_page(source, page_size, target_index_id)
        metadata = _page_metadata(page)
        selected_page = metadata["page_number"]
        scope = (f"clustered index {target_index_id}" if target_index_id is not None
                 else "first index")
        if name == "checksum_body_flip":
            offset = image_offset + 128
            rule = f"median 4 KiB-aligned leaf page of {scope}, body offset +128"
            before = page[128:129]
            after = bytes((before[0] ^ 0x01,))
        elif name == "fil_header_page_no_flip":
            offset = image_offset + _FIL_PAGE_OFFSET
            rule = f"median 4 KiB-aligned leaf page of {scope}, FIL_PAGE_OFFSET"
            before = page[_FIL_PAGE_OFFSET:_FIL_PAGE_OFFSET + 4]
            after = struct.pack(">I", (metadata["page_number"] ^ 1) & 0xFFFFFFFF)
        elif name == "page_type_flip":
            offset = image_offset + _FIL_PAGE_TYPE
            rule = f"median 4 KiB-aligned leaf page of {scope}, FIL_PAGE_TYPE"
            before = page[_FIL_PAGE_TYPE:_FIL_PAGE_TYPE + 2]
            after = b"\x00\x00"
        else:
            rec_next = _INFIMUM_ORIGIN_COMPACT - _REC_HEADER_SIZE_NEW + 3
            offset = image_offset + rec_next
            rule = f"median 4 KiB-aligned leaf page of {scope}, COMPACT infimum REC_NEXT"
            before = page[rec_next:rec_next + 2]
            after = b"\x7f\xff"
    elif name == "fsp_flags_corrupt":
        image_offset, page0 = _find_fsp_header(source)
        offset = image_offset + _FSP_SPACE_FLAGS
        rule = "selected highest physical FSP header copy for dominant space ID, canonical FSP_SPACE_FLAGS"
        metadata = _page_metadata(page0)
        before = page0[_FSP_SPACE_FLAGS:_FSP_SPACE_FLAGS + 4]
        after = bytes(value ^ 0xFF for value in before)
        selected_page = 0
    elif name == "tail_truncate_partial_page":
        truncate_to = source_size - max(1, page_size // 2)
        if truncate_to <= page_size:
            raise CorruptionError("image cannot retain page 0 after partial-page truncation")
        offset = truncate_to
        rule = "remove the final half logical page from copied image"
        with target.open("r+b") as stream:
            stream.truncate(truncate_to)
        before = struct.pack(">Q", source_size)
        after = struct.pack(">Q", truncate_to)
    elif name == "garbage_append":
        offset = source_size
        rule = "append one non-page-aligned deterministic garbage trailer"
        before = b""
        after = (b"DBREC-GARBAGE-TRAILER\x00" * 17) + b"\xff"
        with target.open("ab") as stream:
            stream.write(after)
    elif name == "tde_candidate":
        raise CorruptionError("TDE candidates are derived from the preserved .ibd, not a raw image")
    else:  # pragma: no cover - _MUTATIONS is exhaustively validated above
        raise AssertionError(name)

    if name not in {"tail_truncate_partial_page", "garbage_append"}:
        assert offset is not None
        if offset < 0 or offset + len(after) > source_size:
            raise CorruptionError("mutation range exceeds copied image")
        with target.open("r+b") as stream:
            stream.seek(offset)
            stream.write(after)

    return MutationResult(
        name=name,
        source_sha256=source_digest,
        target_sha256=sha256_file(target),
        target_path=str(target),
        before=before.hex(),
        after=after.hex(),
        page_no=selected_page,
        byte_offset=offset,
        page_metadata=metadata,
        selection_rule=rule,
    )


def apply_tde_candidate(source: Path, target: Path, page_size: int) -> MutationResult:
    """Create a bounded page-0 TDE-routing fixture from a preserved `.ibd`.

    This is deliberately a candidate heuristic test, not an assertion that the
    original tablespace is encrypted.  It exercises the common CLI gate before
    any generic plaintext parsing can emit artifacts.
    """
    _copy_to_derived_image(source, target)
    source_digest = sha256_file(source)
    if source.stat().st_size < page_size or page_size < _FIL_PAGE_SPACE_ID + 4:
        raise CorruptionError("preserved tablespace is too short for a TDE candidate mutation")
    with source.open("rb") as stream:
        page0 = stream.read(page_size)
    candidate = bytearray(page0)
    candidate[_FIL_PAGE_TYPE:_FIL_PAGE_TYPE + 2] = b"\x45\xbf"
    candidate[38] = 6  # valid algorithm marker (AES-256-CBC)
    candidate[55:83] = b"\xab" * 28
    offset = _FIL_PAGE_TYPE
    before = page0[offset:83]
    after = bytes(candidate[offset:83])
    with target.open("r+b") as stream:
        stream.seek(offset)
        stream.write(after)
    return MutationResult(
        name="tde_candidate",
        source_sha256=source_digest,
        target_sha256=sha256_file(target),
        target_path=str(target),
        before=before.hex(),
        after=after.hex(),
        page_no=0,
        byte_offset=offset,
        page_metadata=_page_metadata(page0),
        selection_rule="preserved tablespace page 0, bounded TDE candidate routing fixture",
    )


def _apply_compression_metadata_mutation(source: Path, target: Path,
                                         page_size: int, name: str) -> MutationResult:
    """Tamper with FSP compression metadata on a real compressed `.ibd` copy.

    Only page 0's four FSP_SPACE_FLAGS bytes change: either ZIP_SSIZE becomes
    unparseable, or it redeclares a physical stride that the file layout does
    have. Both must end in a fail-closed forensic classification with no
    importable artifact.
    """
    _copy_to_derived_image(source, target)
    source_digest = sha256_file(source)
    if source.stat().st_size < page_size:
        raise CorruptionError("preserved tablespace is shorter than one page")
    with source.open("rb") as stream:
        page0 = stream.read(page_size)
    if len(page0) < _FSP_SPACE_FLAGS + 4:
        raise CorruptionError("tablespace page 0 is too short to carry FSP flags")
    flags = int.from_bytes(page0[_FSP_SPACE_FLAGS:_FSP_SPACE_FLAGS + 4], "big")
    zip_ssize = (flags >> 1) & 0xF
    if zip_ssize == 0 or zip_ssize > 4:
        raise CorruptionError(
            f"source is not a compressed tablespace (FSP zip_ssize={zip_ssize})")
    if name == "compression_flags_invalid":
        # zip_ssize >= 5 is rejected by parse_fsp_compression_metadata.
        new_zip, rule = 5, "compressed tablespace page 0, FSP_SPACE_FLAGS zip_ssize -> 5"
    else:
        new_zip = 1 if zip_ssize != 1 else 2
        rule = ("compressed tablespace page 0, FSP_SPACE_FLAGS zip_ssize redeclared "
                "so the declared stride cannot match the layout")
    new_flags = (flags & ~0x1E) | (new_zip << 1)
    before = page0[_FSP_SPACE_FLAGS:_FSP_SPACE_FLAGS + 4]
    after = new_flags.to_bytes(4, "big")
    with target.open("r+b") as stream:
        stream.seek(_FSP_SPACE_FLAGS)
        stream.write(after)
    return MutationResult(
        name=name,
        source_sha256=source_digest,
        target_sha256=sha256_file(target),
        target_path=str(target),
        before=before.hex(),
        after=after.hex(),
        page_no=0,
        byte_offset=_FSP_SPACE_FLAGS,
        page_metadata=_page_metadata(page0),
        selection_rule=rule,
    )


def apply_ibd_mutation(source: Path, target: Path, page_size: int,
                       name: str) -> MutationResult:
    """Apply a page-0 mutation to a preserved `.ibd` copy."""
    if name not in _IBD_MUTATIONS:
        raise CorruptionError(f"not an offline-tablespace mutation: {name}")
    if name == "tde_candidate":
        return apply_tde_candidate(source, target, page_size)
    return _apply_compression_metadata_mutation(source, target, page_size, name)


class CorruptionScenarioDriver(ScenarioDriver):
    """Phase B setup and recovery with a derived-image mutation in between."""

    def __init__(self, config: ScenarioConfig, mutation: str,
                 runner: Callable[..., CommandResult] | None = None) -> None:
        super().__init__(config, runner or _run)
        self.mutation = mutation
        self.mutation_result: MutationResult | None = None

    def _verify(self, recovery: dict[str, Any]) -> dict[str, Any]:
        if self.mutation not in _IBD_MUTATIONS:
            return super()._verify(recovery)
        payload = recovery["runs"][self.mutation]
        entry = {
            "returncode": payload.get("returncode"),
            "contract_complete": payload.get("contract_complete", False),
            "contract_error": payload.get("contract_error"),
        }
        if entry["contract_complete"]:
            contract = payload["contract"]
            entry.update({
                "classification": contract.get("classification"),
                "structured_rows": contract.get("structured_rows"),
                "evidence_fragments": contract.get("evidence_fragments"),
                "export_success": contract.get("export_success"),
                "importable": contract.get("importable"),
                "recovery_method": contract.get("recovery_method"),
                "compression_status": contract.get("compression_status"),
                "compression_rejection_reason": contract.get("compression_rejection_reason"),
                "reason": contract.get("reason"),
                "status": "EVIDENCE_ONLY",
            })
        else:
            entry.update({"status": "FAIL", "reason": entry["contract_error"]})
        return {"runs": {self.mutation: entry}, "blockers": []}

    def _prepare_mutated_input(self) -> tuple[Path, bool]:
        if self.media is None:
            raise CorruptionError("media is unavailable after shutdown")
        source = self.media.path
        baseline = self.run_dir / "persistent-input" / "baseline-media.img"
        if baseline.exists():
            raise CorruptionError(f"refusing to overwrite baseline evidence: {baseline}")
        shutil.copyfile(source, baseline)
        os.chmod(baseline, 0o400)
        if self.mutation in _IBD_MUTATIONS:
            ibd_candidates = sorted((self.run_dir / "persistent-input").glob("*.ibd"))
            if len(ibd_candidates) != 1:
                raise CorruptionError(
                    f"{self.mutation} requires exactly one preserved tablespace")
            target = self.run_dir / "disaster-input" / f"{self.mutation}.ibd"
            self.mutation_result = apply_ibd_mutation(
                ibd_candidates[0], target, self.item.page_size, self.mutation)
            source_path = ibd_candidates[0]
            direct_ibd = True
        else:
            target = self.run_dir / "disaster-input" / f"{self.mutation}.img"
            # Only this table's clustered leaf pages are legitimate mutation
            # targets: the image also holds MySQL system tablespaces.
            index_id = self._discover_index_id()
            self.mutation_result = apply_mutation(
                baseline, target, self.item.page_size, self.mutation, index_id)
            source_path = baseline
            direct_ibd = False
        manifest = {
            "mutation_version": 1,
            "created_at": datetime.now(timezone.utc).isoformat(),
            "page_size": self.item.page_size,
            "source_path": str(source_path),
            "source_sha256": sha256_file(source_path),
            "mutation": self.mutation_result.document(),
        }
        path = self.run_dir / "manifests" / "mutation.json"
        path.write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        return target, direct_ibd

    def _recover(self) -> dict[str, Any]:
        target, direct_ibd = self._prepare_mutated_input()
        if direct_ibd:
            out_dir = self.run_dir / "recovery-output" / self.mutation
            out_dir.mkdir(parents=True, exist_ok=True)
            run = self._run_one(self.mutation, [
                str(self.config.recovery_binary), "--ibd", str(target),
                "--page-size", str(self.item.page_size), "--schema", "id:BIGINT:pk",
                "--format", "csv", "--output", str(out_dir),
                "--result-json", str(out_dir / "result.json"),
            ], out_dir)
            return {
                "input_kind": "ibd",
                "image": str(target),
                "truth": str(self.run_dir / "truth" / "ground_truth.json"),
                "runs": {self.mutation: run},
                "baseline_image": str(self.run_dir / "persistent-input" / "baseline-media.img"),
                "mutation": self.mutation_result.document() if self.mutation_result else None,
            }
        if self.media is None:
            raise CorruptionError("media vanished before derived recovery")
        original = self.media.path
        self.media.path = target
        try:
            recovery = super()._recover()
        finally:
            self.media.path = original
        recovery["baseline_image"] = str(self.run_dir / "persistent-input" / "baseline-media.img")
        recovery["mutation"] = self.mutation_result.document() if self.mutation_result else None
        return recovery

    def execute(self) -> dict[str, Any]:
        self.run = ValidationRun.create(self.config.evidence_root, self.config.run_id, self.item)
        # "setup" failures are environmental; "verify" failures happen after the
        # recovery binary has produced its artifacts and are product defects.
        stage = "setup"
        self.run.transition("PRECHECKED", {
            "version": self.item.version,
            "image": self.capability.image,
            "scenario": self.item.scenario,
            "filesystem": self.item.filesystem,
            "rows": self.config.rows,
            "mutation": self.mutation,
        })
        try:
            self._start_mysql()
            self.run.transition("MEDIA_PREPARED", {"image": str(self.media.path),
                                                   "filesystem": self.item.filesystem,
                                                   "mount_point": str(self.media.mount_point)})
            self._populate()
            self.run.transition("DATASET_CREATED", {"row_count": self.config.rows,
                                                      "database": self.database,
                                                      "table": self.table})
            truth = self._seal_truth()
            self.run.transition("TRUTH_SEALED", {"row_count": truth.get("row_count"),
                                                 "dataset_sha256": truth.get("dataset_sha256")})
            self._persist()
            self._snapshot_before()
            self.run.transition("PERSISTED", self.record.get("before", {}))
            # The baseline has no logical deletion: corruption is the disaster.
            # Keep the source tablespace allocated so mutation placement is
            # attributable; filesystem deletion is tested separately by Phase B.
            if self.item.scenario != "baseline":
                raise CorruptionError("corruption matrix currently supports baseline media only")
            self._apply_scenario()
            self.run.transition("SCENARIO_APPLIED", {"scenario": self.item.scenario,
                                                       "mutation": self.mutation})
            self._stop_mysql()
            recovery = self._recover()
            self.run.transition("RECOVERY_ATTEMPTED", {
                "returncodes": {key: value.get("returncode")
                                for key, value in (recovery.get("runs") or {}).items()}})
            stage = "verify"
            verification = self._verify(recovery)
            verdict, reasons = _corruption_verdict(verification, self.mutation)
            self.record.update({
                "kind": "corruption_matrix",
                "mutation": self.mutation,
                "scenario": self.item.scenario,
                "filesystem": self.item.filesystem,
                "version": self.item.version,
                "row_format": self.item.row_format,
                "page_size": self.item.page_size,
                "rows_written": truth.get("row_count"),
                "media": self.record.get("before", {}),
                "recovery": recovery,
                "verification": verification,
                "status": verdict,
                "verdict_reasons": reasons,
            })
            self.run.transition("REPORTED", {"verdict": verdict, "reasons": reasons})
        except Exception as error:  # noqa: BLE001 - classify, then seal diagnostics
            # Once recovery has produced its artifacts, anything that breaks the
            # verification (unparseable CSV, comparator crash, contract mismatch)
            # is a product defect; earlier failures are environmental.
            failed_after_recovery = stage == "verify"
            status = "PRODUCT_FAIL" if failed_after_recovery else "SETUP_ERROR"
            self.record.update({
                "kind": "corruption_matrix", "mutation": self.mutation,
                "scenario": self.item.scenario, "filesystem": self.item.filesystem,
                "version": self.item.version, "status": status,
                "error": f"{type(error).__name__}: {error}",
                "verdict_reasons": [f"verification raised {type(error).__name__}: {error}"],
            })
            try:
                self._stop_mysql()
            except Exception:
                pass
            try:
                if failed_after_recovery and self.run.state == "RECOVERY_ATTEMPTED":
                    self.run.transition("REPORTED", {"verdict": status,
                                                     "error": self.record["error"]})
                else:
                    self.run.transition("FAILED_SETUP", {"error": self.record["error"]})
            except RunLifecycleError:
                pass
        finally:
            self._teardown_media()

        (self.run_dir / "reports" / "corruption.json").write_text(
            json.dumps(self.record, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        if self.run.state == "REPORTED":
            self.run.seal({"corruption": self.record})
        return self.record


def _corruption_verdict(verification: dict[str, Any], mutation: str) -> tuple[str, list[str]]:
    """Classify a corruption run without treating expected loss as a tool defect."""
    runs = verification.get("runs") or {}
    reasons: list[str] = []
    if not runs:
        return "SETUP_ERROR", ["no recovery run was recorded"]
    entries = list(runs.values())
    if any(entry.get("returncode") is None or int(entry.get("returncode", 0)) < 0
           for entry in entries):
        return "PRODUCT_FAIL", ["recovery process timed out or was signal-terminated"]
    if any(not entry.get("contract_complete") for entry in entries):
        return "PRODUCT_FAIL", ["recovery result contract is missing or malformed"]
    if mutation in _IBD_MUTATIONS:
        for entry in entries:
            if entry.get("classification") not in _FORENSIC_CLASSIFICATIONS:
                return "PRODUCT_FAIL", [
                    f"{mutation}: expected a forensic classification, got "
                    f"{entry.get('classification')}"]
            if entry.get("export_success") or entry.get("importable"):
                return "PRODUCT_FAIL", [
                    f"{mutation}: unvalidated input claims exportable output"]
            if mutation == "tde_candidate" and entry.get("returncode") == 0:
                return "PRODUCT_FAIL", ["unvalidated TDE candidate exited successfully"]
            if mutation.startswith("compression_"):
                status = entry.get("compression_status")
                if status in (None, "", "NOT_COMPRESSED", "UNKNOWN"):
                    return "PRODUCT_FAIL", [
                        f"{mutation}: compression metadata was not rejected ({status})"]
                if not entry.get("compression_rejection_reason"):
                    return "PRODUCT_FAIL", [
                        f"{mutation}: rejected without a compression rejection reason"]
        return "PASS_EVIDENCE_ONLY", [
            f"{mutation}: rejected before any importable artifact could be written"]
    if any(entry.get("classification") != "structured_recovery" for entry in entries):
        for entry in entries:
            if entry.get("classification") != "structured_recovery":
                if entry.get("export_success") or entry.get("importable"):
                    return "PRODUCT_FAIL", ["forensic evidence contract claims exportable output"]
        return "PASS_EVIDENCE_ONLY", ["damage triggered a non-exportable forensic-evidence gate"]
    # From here the contract claims structured rows.  Physical damage is
    # allowed to cost rows on the mutated page; it is never allowed to
    # fabricate, shift or corrupt a single recovered value.
    for label, entry in runs.items():
        comparison = entry.get("comparison") or {}
        if not comparison:
            return "PRODUCT_FAIL", [f"{label}: structured output was not compared to truth"]
        if comparison.get("status") != "COMPARABLE":
            return "PRODUCT_FAIL", [
                f"{label}: comparison not comparable: "
                f"{comparison.get('prerequisites') or comparison.get('status')}"]
        mismatches = int(comparison.get("field_mismatches") or 0)
        false_positives = int(comparison.get("false_positives") or 0)
        if mismatches or false_positives:
            return "PRODUCT_FAIL", [
                f"{label}: {mismatches} field mismatches, {false_positives} fabricated rows"]
        lost = int(comparison.get("false_negatives") or 0)
        if lost:
            reasons.append(f"{label}: {lost} rows lost with the physically damaged page")
        else:
            reasons.append(f"{label}: all {comparison.get('actual_rows')} rows match truth")
    if mutation in {"garbage_append"} and any(
            "rows lost" in reason for reason in reasons):
        return "PRODUCT_FAIL", ["trailing garbage must not lose table rows"] + reasons
    if mutation in {"garbage_append"}:
        return "PASS_BOUNDED_STRUCTURED", reasons or ["trailing garbage was ignored"]
    return "PASS_BOUNDED_STRUCTURED", reasons


def run_matrix(evidence_root: Path, versions: Iterable[str], filesystems: Iterable[str],
               mutations: Iterable[str], scenarios: Iterable[str], rows: int,
               image_mb: int, fresh_import: bool,
               row_formats: Iterable[str] = ("DYNAMIC",)) -> list[dict[str, Any]]:
    evidence_root.mkdir(parents=True, exist_ok=True)
    (evidence_root / "media").mkdir(exist_ok=True)
    results: list[dict[str, Any]] = []
    for version in versions:
        for filesystem in filesystems:
            for scenario in scenarios:
                for row_format in row_formats:
                    item = matrix_item(version, filesystem, scenario, "smoke", 16384, row_format)
                    if item.status != "PLANNED":
                        results.append({"status": "UNSUPPORTED", "reason": item.reason,
                                        "version": version, "filesystem": filesystem,
                                        "scenario": scenario, "row_format": row_format})
                        continue
                    for mutation in mutations:
                        if mutation not in _MUTATIONS:
                            raise CorruptionError(f"unknown mutation requested: {mutation}")
                        run_id = (f"corruption-{version.replace('.', '')}-{filesystem}-{scenario}-"
                                  f"{row_format.lower()}-{mutation}-{int(time.time())}")
                        driver = CorruptionScenarioDriver(
                            ScenarioConfig(evidence_root=evidence_root, run_id=run_id, item=item,
                                           rows=rows, image_mb=image_mb, fresh_import=fresh_import),
                            mutation)
                        record = driver.execute()
                        results.append({"run_id": run_id, **record})
    return results


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run bounded corruption validation on disposable loop-backed images")
    parser.add_argument("--versions", nargs="+", default=["5.7"])
    parser.add_argument("--filesystems", nargs="+", default=["ext4"])
    parser.add_argument("--scenarios", nargs="+", choices=("baseline",), default=["baseline"])
    parser.add_argument("--row-formats", nargs="+", default=["DYNAMIC"],
                        help="Row format for the baseline tablespace; compression "
                             "metadata mutations require COMPRESSED")
    parser.add_argument("--mutations", nargs="+", choices=_MUTATIONS,
                        default=list(_MUTATIONS))
    parser.add_argument("--rows", type=int, default=20000)
    parser.add_argument("--image-mb", type=int, default=2048)
    parser.add_argument("--evidence-root", type=Path,
                        default=Path("/app/mysql_recovery_artifacts/corruption"))
    parser.add_argument("--fresh-import", dest="fresh_import", action="store_true", default=True)
    parser.add_argument("--no-fresh-import", dest="fresh_import", action="store_false")
    args = parser.parse_args()

    results = run_matrix(args.evidence_root, args.versions, args.filesystems, args.mutations,
                         args.scenarios, args.rows, args.image_mb, args.fresh_import,
                         row_formats=args.row_formats)
    summary_path = args.evidence_root / "corruption_summary.json"
    if summary_path.exists():
        raise CorruptionError(f"refusing to overwrite evidence summary: {summary_path}")
    summary_path.write_text(json.dumps(results, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    for result in results:
        print(f"{result.get('run_id', 'matrix-item')}: {result.get('status')}")
    print(f"wrote {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
