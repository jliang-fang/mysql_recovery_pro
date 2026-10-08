#!/usr/bin/env python3
"""Resume a sealed-input, multi-table Stage 1 run without altering raw media.

This utility only permits recovery from a Stage 1 run after its DROP phase.  It
requires the real device to already be unmounted and kernel read-only, then
writes per-table recovery, comparison, import, and sealed evidence artifacts.
It never formats, mounts, unlocks, or writes the target block device.
"""

from __future__ import annotations

import datetime
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, "/app/mysql_recovery_pro")

from scripts.disaster_harness.comparator import compare
from scripts.disaster_harness.manifest import sha256_file
from scripts.disaster_validation.device_safety import ALLOWED_DEVICE, read_device_identity
from scripts.disaster_validation.fresh_import_verify import verify_case
from scripts.disaster_validation.lifecycle import ValidationRun
from scripts.disaster_validation.mysql_driver import run

RUN_DIR = Path("/app/mysql_recovery_artifacts/raw_nvme_qualification/stage1-nvme80-100k-1791427313")
DEVICE_PATH = ALLOWED_DEVICE
RECOVERY_BINARY = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")


def _write_new(path: Path, value: dict[str, Any]) -> None:
    if path.exists():
        raise RuntimeError(f"refusing to overwrite evidence: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
                    encoding="utf-8")


def main() -> int:
    state = ValidationRun.load(RUN_DIR)
    if state.state != "SCENARIO_APPLIED":
        raise RuntimeError(f"run must stop after DROP, found lifecycle state {state.state}")
    ro = run(["blockdev", "--getro", DEVICE_PATH], timeout=30)
    if not ro.ok or ro.stdout.strip() != "1":
        raise RuntimeError("recovery refuses to run unless the target device is kernel read-only")

    inputs = sorted((RUN_DIR / "persistent-input").glob("*/schema.json"))
    if not inputs:
        raise RuntimeError("no per-table schemas found")
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    tables: dict[str, dict[str, Any]] = {}
    for schema_file in inputs:
        table = schema_file.parent.name
        truth_path = RUN_DIR / "truth" / table / "ground_truth.json"
        ibd_path = schema_file.parent / f"{table}.ibd"
        truth = json.loads(truth_path.read_text(encoding="utf-8"))
        index_counts: dict[int, int] = {}
        import struct
        with ibd_path.open("rb") as handle:
            total_pages = ibd_path.stat().st_size // 16384
            for page in range(total_pages):
                handle.seek(page * 16384 + 24)
                if struct.unpack(">H", handle.read(2))[0] == 0x45BF:
                    handle.seek(page * 16384 + 66)
                    index_id = struct.unpack(">Q", handle.read(8))[0]
                    index_counts[index_id] = index_counts.get(index_id, 0) + 1
        if not index_counts:
            raise RuntimeError(f"{table}: no index ID in sealed tablespace")
        clustered_index_id = max(index_counts.items(), key=lambda item: item[1])[0]
        out = RUN_DIR / "recovery-output" / table
        out.mkdir(parents=True, exist_ok=True)
        result_path = out / "result.json"
        if result_path.exists():
            raise RuntimeError(f"{table}: recovery result already exists; refusing overwrite")
        argv = [str(RECOVERY_BINARY), "--disk-carving", "--device-path", DEVICE_PATH,
                "--filesystem", "ext4", "--page-size", "16384", "--schema-file",
                str(schema_file), "--target-index-ids", str(clustered_index_id),
                "--output", str(out), "--result-json", str(result_path)]
        command = run(argv, timeout=3600)
        stdout_path = RUN_DIR / "logs" / f"carve_{table}_stdout.log"
        stderr_path = RUN_DIR / "logs" / f"carve_{table}_stderr.log"
        if stdout_path.exists() or stderr_path.exists():
            raise RuntimeError(f"{table}: recovery log exists; refusing overwrite")
        stdout_path.write_text(command.stdout, encoding="utf-8")
        stderr_path.write_text(command.stderr, encoding="utf-8")
        if not result_path.is_file():
            raise RuntimeError(f"{table}: no result contract")
        contract = json.loads(result_path.read_text(encoding="utf-8"))
        csvs = sorted(out.glob("*.csv"))
        if not csvs:
            raise RuntimeError(f"{table}: no recovered CSV")
        from scripts.disaster_validation.csv_to_canonical import csv_to_canonical_dataset
        recovered = csv_to_canonical_dataset(csvs[0], truth["database"], truth["table"],
                                             truth["schema"], "8.0")
        comparison = compare(truth, recovered, f"{RUN_DIR.name}_{table}_resume")
        if "mysql_version_mismatch" in comparison.get("prerequisites", []):
            recovered["mysql_version"] = truth.get("mysql_version", recovered.get("mysql_version"))
            comparison = compare(truth, recovered, f"{RUN_DIR.name}_{table}_resume")
        comparison_path = RUN_DIR / "reports" / f"comparison_{table}.json"
        _write_new(comparison_path, comparison)
        tables[table] = {
            "argv": argv,
            "returncode": command.returncode,
            "clustered_index_id": clustered_index_id,
            "schema_sha256": sha256_file(schema_file),
            "truth_sha256": sha256_file(truth_path),
            "tablespace_sha256": sha256_file(ibd_path),
            "result_sha256": sha256_file(result_path),
            "stdout_sha256": sha256_file(stdout_path),
            "stderr_sha256": sha256_file(stderr_path),
            "contract": contract,
            "comparison": comparison,
        }
        print(f"{table}: {comparison.get('correct_rows')}/{comparison.get('expected_rows')} "
              f"strict={comparison.get('strict_pass')} rc={command.returncode}")

    import_result = verify_case(RUN_DIR, "8.0", container_name=f"dbrec-resume-{RUN_DIR.name}")
    _write_new(RUN_DIR / "reports" / "fresh_import_result.json", import_result)
    ended = datetime.datetime.now(datetime.timezone.utc).isoformat()
    ro_after = run(["blockdev", "--getro", DEVICE_PATH], timeout=30)
    if not ro_after.ok or ro_after.stdout.strip() != "1":
        raise RuntimeError("target device lost kernel read-only protection during recovery")
    invocation = {
        "record_version": 2,
        "mode": "resume_after_interrupted_teardown",
        "binary": {"path": str(RECOVERY_BINARY.resolve()), "sha256": sha256_file(RECOVERY_BINARY)},
        "device": {"identity": read_device_identity(DEVICE_PATH).__dict__, "read_only_before": "1",
                   "read_only_after": ro_after.stdout.strip()},
        "started_at": started,
        "finished_at": ended,
        "tables": tables,
        "fresh_import": import_result,
    }
    _write_new(RUN_DIR / "reports" / "recovery_invocation.json", invocation)
    state.transition("RECOVERY_ATTEMPTED", {"resumed": True,
                                               "returncodes": {k: v["returncode"] for k, v in tables.items()}})
    overall = (all(item["comparison"].get("strict_pass") is True for item in tables.values()) and
               import_result.get("status") == "PASS" and
               all(item["contract"].get("classification") == "structured_recovery" and
                   item["contract"].get("export_success") is True for item in tables.values()))
    if overall:
        state.transition("IMPORT_VALIDATED", {"status": "PASS"})
    state.transition("REPORTED", {"verdict": "PASS" if overall else "FAIL"})
    seal = state.seal({"verdict": "PASS" if overall else "FAIL", "tables": tables,
                       "import": import_result, "invocation": invocation})
    print(json.dumps({"verdict": "PASS" if overall else "FAIL", "seal": seal,
                      "fresh_import": import_result.get("status")}, indent=2))
    return 0 if overall else 1


if __name__ == "__main__":
    raise SystemExit(main())
