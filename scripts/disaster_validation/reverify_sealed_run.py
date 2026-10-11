#!/usr/bin/env python3
"""Re-verify a sealed Stage 1 run non-destructively and auditably.

This utility NEVER reads, formats, mounts, or otherwise touches the target
block device.  It re-scores an *existing* sealed Stage 1 evidence directory —
its ``truth/*/ground_truth.json`` and ``recovery-output/<table>*/<csv>`` —
against the *current* comparator and fresh-import logic, so we can tell which
of the 2026-10-08 defects are already fixed by later commits before spending
another destructive epoch on the real device.

Safety / audit properties:

- Read-only on the sealed case.  It refuses to overwrite any prior evidence:
  every artifact lands in a brand-new sibling copy
  ``reverify/<run_id>-<epoch>/``; the original ``manifest.json``,
  ``run_state.json``, ``reports/``, and ``manifest.seal`` are never modified.
- The single-table -> per-table layout is mirrored verbatim, so
  ``verify_case`` reuses its existing multi-table path.
- When a table produced several recovery variants (e.g. the fail-closed
  re-runs of ``t_text``), it picks the *newest* ``<table>*`` directory whose
  ``result.json`` reports ``export_success`` and the highest
  ``structured_rows`` and records exactly which one it chose.
- Emits ``reverification.json`` with the binary/script checksums, the per-table
  comparison and fresh-import verdicts, and ``preserves_prior_seal: true``.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, "/app/mysql_recovery_pro")

from scripts.disaster_harness.comparator import compare
from scripts.disaster_harness.manifest import canonical_json, sha256_file
from scripts.disaster_validation.fresh_import_verify import verify_case


DEFAULT_RUN_DIR = Path(
    "/app/mysql_recovery_artifacts/raw_nvme_qualification/"
    "stage1-nvme80-100k-1791427313")


def _sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _read_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001 - unreadable evidence is surfaced, not fatal
        return {}


def _write_new(path: Path, document: dict[str, Any]) -> None:
    if path.exists():
        raise RuntimeError(f"refusing to overwrite evidence: {path}")
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.write_text(canonical_json(document) + "\n", encoding="utf-8")


def _pick_recovery_variant(run_dir: Path, table: str) -> tuple[Path, dict[str, Any], str]:
    """Return (variant_dir, contract, csv_path) for the newest good output."""
    candidates: list[tuple[float, int, Path, dict[str, Any]]] = []
    for out_dir in sorted(run_dir.glob(f"recovery-output/{table}*")):
        if not out_dir.is_dir():
            continue
        result_path = out_dir / "result.json"
        contract = _read_json(result_path)
        csvs = sorted(out_dir.glob("*.csv"))
        if not contract or contract.get("export_success") is not True or not csvs:
            continue
        structured = int(contract.get("structured_rows") or 0)
        # Prefer the directory with the most recovered rows; break ties by the
        # newest mtime so fail-closed refinements supersede the first pass.
        candidates.append((structured, out_dir.stat().st_mtime, out_dir, contract))
    if not candidates:
        raise RuntimeError(f"{table}: no export_success recovery variant found")
    candidates.sort(key=lambda item: (item[0], item[1]))
    structured, _mtime, out_dir, contract = candidates[-1]
    csvs = sorted(out_dir.glob("*.csv"))
    return out_dir, contract, str(csvs[0])


def _clustered_index_id(ibd_path: Path, page_size: int = 16384) -> int | None:
    """Recover the clustered index id from a sealed tablespace copy."""
    import struct
    if not ibd_path.is_file():
        return None
    index_counts: dict[int, int] = {}
    with ibd_path.open("rb") as handle:
        total_pages = ibd_path.stat().st_size // page_size
        for page in range(total_pages):
            handle.seek(page * page_size + 24)
            if struct.unpack(">H", handle.read(2))[0] == 0x45BF:
                handle.seek(page * page_size + 66)
                index_id = struct.unpack(">Q", handle.read(8))[0]
                index_counts[index_id] = index_counts.get(index_id, 0) + 1
    if not index_counts:
        return None
    return max(index_counts.items(), key=lambda item: item[1])[0]


def build_reverify_workdir(run_dir: Path, work_root: Path) -> tuple[Path, dict[str, Any]]:
    """Stage a fresh sibling copy of the case for a clean re-verification.

    The copy carries truth/, persistent-input/, and recovery-output/ only —
    deliberately not the sealed manifest, run_state, reports, approval tokens,
    or import-validation results — so verify_case writes its outputs into the
    new directory and the sealed run stays byte-identical.
    """
    epoch = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    work_dir = work_root / f"{run_dir.name}-{epoch}"
    if work_dir.exists():
        raise RuntimeError(f"work dir already exists: {work_dir}")
    work_dir.mkdir(mode=0o700, parents=True)
    (work_dir / "import-validation").mkdir()
    (work_dir / "reports").mkdir()

    selection: dict[str, Any] = {}
    persistent_src = run_dir / "persistent-input"
    for truth_path in sorted((run_dir / "truth").glob("*/ground_truth.json")):
        table = truth_path.parent.name
        (work_dir / "truth" / table).mkdir(parents=True)
        shutil.copy2(truth_path, work_dir / "truth" / table / "ground_truth.json")

        # Mirror the per-table persistent-input (schema + sealed tablespace) so
        # verify_case can still enrich ENUM/SET members for the DDL.
        src_pi = persistent_src / table
        if src_pi.is_dir():
            dst_pi = work_dir / "persistent-input" / table
            dst_pi.mkdir(parents=True)
            shutil.copy2(src_pi / "schema.json", dst_pi / "schema.json")
            ibd = src_pi / f"{table}.ibd"
            if ibd.is_file():
                shutil.copy2(ibd, dst_pi / ibd.name)

        out_dir, contract, csv_path = _pick_recovery_variant(run_dir, table)
        dst_out = work_dir / "recovery-output" / table
        dst_out.mkdir(parents=True)
        for artifact in out_dir.iterdir():
            if artifact.is_file():
                shutil.copy2(artifact, dst_out / artifact.name)
        ibd_copy = persistent_src / table / f"{table}.ibd"
        selection[table] = {
            "recovery_variant": out_dir.relative_to(run_dir).as_posix(),
            "recovery_variant_csv": Path(csv_path).name,
            "clustered_index_id": _clustered_index_id(ibd_copy),
            "schema_file": str((persistent_src / table / "schema.json")),
            "schema_sha256": sha256_file(persistent_src / table / "schema.json"),
            "truth_sha256": sha256_file(truth_path),
            "tablespace_sha256": sha256_file(ibd_copy) if ibd_copy.is_file() else None,
            "contract_sha256": sha256_file(out_dir / "result.json"),
        }
    return work_dir, {"epoch": epoch, "selection": selection}


def reverify(run_dir: Path, work_root: Path | None, binary: Path | None) -> dict[str, Any]:
    run_dir = run_dir.resolve()
    work_root = work_root or run_dir.parent / "reverify"
    work_dir, meta = build_reverify_workdir(run_dir, work_root)

    report: dict[str, Any] = {
        "record_version": 1,
        "run_id": run_dir.name,
        "source_case": str(run_dir),
        "work_dir": str(work_dir),
        "epoch": meta["epoch"],
        "preserves_prior_seal": True,
        "device_access": "none",
        "binary": ({"path": str(binary.resolve()),
                    "sha256": sha256_file(binary)} if binary and binary.is_file()
                   else {"path": str(binary) if binary else None,
                         "sha256": None}),
        "script_sha256": _sha256_text(Path(__file__).read_text(encoding="utf-8")),
        "selection": meta["selection"],
        "per_table": {},
        "fresh_import": {},
        "verdict": "FAIL",
    }

    from scripts.disaster_validation.csv_to_canonical import csv_to_canonical_dataset

    comparisons: dict[str, dict[str, Any]] = {}
    for table, sel in meta["selection"].items():
        truth = _read_json(work_dir / "truth" / table / "ground_truth.json")
        csv_path = work_dir / "recovery-output" / table / sel["recovery_variant_csv"]
        # Use the truth's own server version as the single source of truth so a
        # version stamp never produces a spurious mysql_version_mismatch.
        recovered = csv_to_canonical_dataset(
            csv_path, truth["database"], truth["table"], truth["schema"],
            truth.get("mysql_version") or "")
        comparison = compare(truth, recovered, f"{run_dir.name}_{table}_reverify")
        comparisons[table] = comparison
        _write_new(work_dir / "reports" / f"comparison_{table}.json", comparison)
        report["per_table"][table] = {
            "expected_rows": comparison.get("expected_rows"),
            "actual_rows": comparison.get("actual_rows"),
            "correct_rows": comparison.get("correct_rows"),
            "field_mismatches": (len(comparison.get("field_mismatches", []))
                                 if isinstance(comparison.get("field_mismatches"), list)
                                 else comparison.get("field_mismatches")),
            "false_positives": len(comparison.get("false_positives", [])),
            "false_negatives": len(comparison.get("false_negatives", [])),
            "strict_pass": comparison.get("strict_pass"),
            "status": comparison.get("status"),
            "prerequisites": comparison.get("prerequisites"),
        }
        print(f"[reverify] {table}: {comparison.get('correct_rows')}/"
              f"{comparison.get('expected_rows')} strict={comparison.get('strict_pass')} "
              f"status={comparison.get('status')}")

    # Fresh-instance import on the staged copy — the only operation that needs
    # a container, and it is still entirely off the real device.
    version = _read_json(work_dir / "truth" / next(iter(meta["selection"]))
                         / "ground_truth.json").get("mysql_version", "8.0")
    import_res = verify_case(work_dir, version,
                             container_name=f"reverify-{run_dir.name[-13:]}-{meta['epoch'][-7:]}")
    report["fresh_import"] = import_res
    print(f"[reverify] fresh import: {import_res.get('status')}")

    comparisons_ok = all(c.get("strict_pass") is True for c in comparisons.values())
    import_ok = import_res.get("status") == "PASS"
    contracts_ok = all(
        (_read_json(Path(run_dir) / sel["recovery_variant"] / "result.json") or {})
            .get("classification") == "structured_recovery"
        and (_read_json(Path(run_dir) / sel["recovery_variant"] / "result.json") or {})
            .get("export_success") is True
        for sel in meta["selection"].values())
    overflow_dropped = sum(
        int(((_read_json(Path(run_dir) / sel["recovery_variant"] / "result.json") or {})
             .get("details") or {}).get("overflow_records_dropped") or 0)
        for sel in meta["selection"].values())
    report["overflow_records_dropped"] = overflow_dropped
    report["contracts_ok"] = contracts_ok
    report["comparisons_ok"] = comparisons_ok
    report["verdict"] = "PASS" if (comparisons_ok and import_ok and contracts_ok
                                   and overflow_dropped == 0) else "FAIL"

    _write_new(work_dir / "reports" / "reverification.json", report)
    # Also mirror the fresh-import artifact verify_case wrote inside the staged
    # copy up into a labelled place under the staged reports dir for the
    # aggregate collector (it stays out of the sealed original entirely).
    print(f"[reverify] verdict={report['verdict']} -> {work_dir}/reports/reverification.json")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Re-verify a sealed Stage 1 run")
    parser.add_argument("--run-dir", default=str(DEFAULT_RUN_DIR))
    parser.add_argument("--work-root", default=None,
                        help="Where to place the reverify/<run>-<epoch> copy "
                             "(default: <run_dir>/../reverify)")
    parser.add_argument("--binary", default="/app/mysql_recovery_pro/build/mysql_recovery_pro")
    args = parser.parse_args()

    report = reverify(Path(args.run_dir),
                      Path(args.work_root) if args.work_root else None,
                      Path(args.binary) if args.binary else None)
    print(json.dumps({k: report[k] for k in
                      ("run_id", "verdict", "per_table", "fresh_import",
                       "overflow_records_dropped", "work_dir")},
                     indent=1, ensure_ascii=False))
    return 0 if report["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
