#!/usr/bin/env python3
"""Re-import recovered rows into a fresh MySQL instance and re-verify them.

This is the import gate the in-process comparison cannot provide: the CSV/SQL
artifacts are turned into a CREATE TABLE + INSERT stream, executed on a clean
server of the case's version, then read back with exactly the capture path used
for ground truth and compared field by field.

Usage:
  python3 -m scripts.disaster_validation.fresh_import_verify \
      --case <case-dir> [--case <case-dir> ...]
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional

sys.path.insert(0, "/app/mysql_recovery_pro")

from scripts.disaster_validation.csv_to_canonical import _parse_csv_cell  # noqa: E402
from scripts.disaster_harness.comparator import compare  # noqa: E402

ROOT = Path("/app/mysql_recovery_pro")


def run(argv: list[str], timeout: float = 300, check: bool = False,
        input_text: str = "") -> subprocess.CompletedProcess:
    """Run one command, mapping transport failures onto a non-zero result.

    A readiness probe or an import that hangs must surface as a retryable
    failure: letting `subprocess.TimeoutExpired` escape would abort the whole
    case as a setup error instead of letting the caller classify it.
    """
    try:
        return subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                              input=input_text, check=check)
    except subprocess.TimeoutExpired as error:
        return subprocess.CompletedProcess(
            argv, 124, error.stdout or "",
            f"{error.stderr or ''}\ntimed out after {timeout}s".strip())
    except OSError as error:
        return subprocess.CompletedProcess(argv, 127, "", str(error))


def unwrap_canonical(value: Any) -> Any:
    """Turn a canonical {"type": ..., "value": ...} cell into a plain Python value."""
    if isinstance(value, dict) and "type" in value:
        kind = value.get("type")
        if kind == "null":
            return None
        if "value" not in value:
            return None
        if kind == "binary":
            payload = value.get("value")
            if isinstance(payload, bytes):
                return payload
            if value.get("encoding") == "base64":
                import base64
                return base64.b64decode(payload)
            if isinstance(payload, str) and payload.startswith("0x"):
                return bytes.fromhex(payload[2:])
            return str(payload)
        if kind in ("int", "float", "decimal", "string", "bool"):
            return value.get("value")
        # json and anything else: re-serialise the decoded structure
        return value.get("value")
    return value


def sql_literal(value: Any) -> str:
    """Render one canonical value as SQL text for a clean-server INSERT."""
    value = unwrap_canonical(value)
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, (bytes, bytearray)):
        return "UNHEX('%s')" % bytes(value).hex()
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, dict) or isinstance(value, list):
        return "'" + json.dumps(value, ensure_ascii=False, separators=(",", ":")).replace("'", "''") + "'"
    text = str(value)
    return "'" + text.replace("\\", "\\\\").replace("'", "''") + "'"


def build_ddl(truth: dict[str, Any]) -> str:
    schema = truth["schema"]
    pk = set(schema.get("primary_key") or [])
    parts = []
    for column in schema["columns"]:
        pieces = [f"`{column['name']}` {column['type']}"]
        if column["name"] in pk:
            pieces.append("NOT NULL")
        elif not column.get("nullable", True):
            pieces.append("NOT NULL")
        if column.get("charset"):
            pieces.append(f"CHARACTER SET {column['charset']}")
        if column.get("collation"):
            pieces.append(f"COLLATE {column['collation']}")
        parts.append(" ".join(pieces))
    if pk:
        ordered = [c["name"] for c in schema["columns"] if c["name"] in pk]
        parts.append("PRIMARY KEY (" + ", ".join(f"`{n}`" for n in ordered) + ")")
    return "CREATE TABLE IF NOT EXISTS `recovered_import`.`recovered_table` (" + ", ".join(parts) + ") ENGINE=InnoDB;"


def capture_tsv(container: str, truth: dict[str, Any],
                database: str = "recovered_import") -> str:
    """Read the table back exactly like ground truth was captured."""
    schema = truth["schema"]
    binary_types = {"binary", "varbinary", "blob", "tinyblob", "mediumblob", "longblob"}
    select = []
    for column in schema["columns"]:
        base = str(column["type"]).lower().split("(", 1)[0].strip()
        if base in binary_types:
            select.append(f"CONCAT('0x', HEX(`{column['name']}`)) AS `{column['name']}`")
        else:
            select.append(f"`{column['name']}`")
    sql = (f"SELECT {', '.join(select)} FROM "
           f"`{database}`.`recovered_table`")
    if truth["schema"].get("primary_key"):
        sql += " ORDER BY " + ", ".join(f"`{n}`" for n in truth["schema"]["primary_key"])
    result = run(["podman", "exec", container, "mysql", "-uroot", "-N", "-B", "-e", sql], 600)
    if result.returncode != 0:
        raise RuntimeError(f"select failed: {result.stderr.strip()}")
    return result.stdout


def dataset_from_tsv(tsv: str, truth: dict[str, Any], version: str) -> dict[str, Any]:
    """Build a canonical (typed) dataset from a SELECT TSV, matching ground truth."""
    from scripts.disaster_harness.ground_truth import build_ground_truth

    schema = truth["schema"]
    rows = []
    for line in tsv.splitlines():
        if not line:
            continue
        parts = line.split("\t")
        row = {}
        for index, column in enumerate(schema["columns"]):
            text = parts[index] if index < len(parts) else None
            row[column["name"]] = _parse_csv_cell(text, column)
        rows.append(row)
    return build_ground_truth(truth["database"], truth["table"], schema, rows,
                              version or truth.get("mysql_version", ""))


def rows_from_tsv(tsv: str, truth: dict[str, Any]) -> list[dict[str, Any]]:
    schema = truth["schema"]
    rows = []
    for line in tsv.splitlines():
        if not line:
            continue
        parts = line.split("\t")
        row = {}
        for index, column in enumerate(schema["columns"]):
            text = parts[index] if index < len(parts) else None
            row[column["name"]] = _parse_csv_cell(text, column)
        rows.append(row)
    return rows


_TRANSIENT_IMPORT_ERRORS = (
    "Can't connect to local MySQL server",
    "Lost connection to MySQL server",
    "Server shutdown in progress",
    "system error: 104",
    "empty reply from server",
    "Error in result packet",
)


def _is_transient(error_text: str) -> bool:
    return any(marker in (error_text or "") for marker in _TRANSIENT_IMPORT_ERRORS)


def _import_with_retry(container: str, payload: str, attempts: int = 5):
    """Feed one INSERT stream to the clean server, retrying transport failures.

    Returns the final attempt plus whether every failure looked transient, so
    the caller can report an environment interruption instead of a bad import.
    """
    proc = None
    for attempt in range(attempts):
        proc = run(["podman", "exec", "-i", container, "mysql", "-uroot",
                    "recovered_import"], 1800, input_text=payload)
        if proc.returncode == 0:
            return proc, False
        if not _is_transient(proc.stderr or ""):
            return proc, False
        time.sleep(2 * (attempt + 1))
    return proc, True


def verify_case(case: Path, version: str) -> dict[str, Any]:
    """Import every recovered CSV artifact of a case and compare to truth."""
    truth_path = case / "truth" / "ground_truth.json"
    if not truth_path.is_file():
        return {"case": case.name, "status": "SETUP_ERROR", "reason": "no ground truth"}
    truth = json.loads(truth_path.read_text(encoding="utf-8"))

    csvs = sorted((case / "recovery-output").rglob("*.csv"))
    if not csvs:
        return {"case": case.name, "status": "EVIDENCE_ONLY", "reason": "no CSV artifact"}

    datadir = Path(f"/tmp/fresh-import-{case.name}")
    if datadir.exists():
        shutil.rmtree(datadir)
    datadir.mkdir(parents=True)
    container = f"fresh-import-{case.name}"
    run(["podman", "rm", "-f", container], 60)
    run(["podman", "run", "-d", "--name", container,
         "-v", f"{datadir}:/var/lib/mysql:Z",
         "-e", "MYSQL_ALLOW_EMPTY_PASSWORD=yes", f"localhost/mysql:{version}"], 300)

    ready = False
    for _ in range(120):
        if run(["podman", "exec", container, "mysqladmin", "ping", "-uroot"], 15).returncode == 0:
            ready = True
            break
        time.sleep(1)
    if not ready:
        run(["podman", "rm", "-f", container], 60)
        shutil.rmtree(datadir, ignore_errors=True)
        return {"case": case.name, "status": "SETUP_ERROR", "reason": "fresh server never ready"}

    results: dict[str, Any] = {"case": case.name, "version": version, "artifacts": {}}
    try:
        ddl = build_ddl(truth)
        (case / "import-validation" / "ddl.sql").parent.mkdir(exist_ok=True)
        (case / "import-validation" / "ddl.sql").write_text(ddl + "\n", encoding="utf-8")
        create_db = run(["podman", "exec", container, "mysql", "-uroot",
                         "-e", "CREATE DATABASE IF NOT EXISTS `recovered_import`;"], 120)
        if create_db.returncode != 0:
            return {**results, "status": "FAIL",
                    "reason": "cannot create import database",
                    "stderr": create_db.stderr.strip()[:400]}
        ddl_run = run(["podman", "exec", container, "mysql", "-uroot", "-e", ddl], 120)
        if ddl_run.returncode != 0:
            return {**results, "status": "FAIL",
                    "reason": "DDL rejected by fresh server",
                    "stderr": ddl_run.stderr.strip()[:400]}

        if not truth.get("schema", {}).get("columns"):
            return {**results, "status": "FAIL", "reason": "truth has no schema"}

        for csv_path in csvs:
            label = csv_path.stem
            from scripts.disaster_validation.csv_to_canonical import csv_to_canonical_dataset
            canonical = csv_to_canonical_dataset(csv_path, truth["database"], truth["table"],
                                                 truth["schema"], version)
            statements = []
            columns = [c["name"] for c in truth["schema"]["columns"]]
            for row in canonical["rows"]:
                values = [sql_literal(row["values"].get(name)) for name in columns]
                statements.append(
                    "INSERT INTO `recovered_import`.`recovered_table` (`"
                    + "`,`".join(columns) + "`) VALUES ("
                    + ", ".join(values) + ");")
            statements.insert(0, "TRUNCATE TABLE `recovered_import`.`recovered_table`;")
            payload = "\n".join(statements) + "\n"
            logs = case / "import-validation" / f"{label}.sql"
            logs.write_text(payload, encoding="utf-8")
            proc, transient = _import_with_retry(container, payload)
            if proc.returncode != 0:
                # A dropped connection or a server mid-shutdown says nothing
                # about the recovered rows: record it as an environment
                # interruption, never as a recovery defect.
                results["artifacts"][label] = {
                    "status": "SETUP_ERROR" if transient else "FAIL",
                    "reason": ("import interrupted by a transient server/client failure"
                               if transient else "import rejected"),
                    "stderr": proc.stderr.strip()[:500]}
                continue
            try:
                after_rows = capture_tsv(container, truth)
            except RuntimeError as error:
                transient = _is_transient(str(error))
                results["artifacts"][label] = {
                    "status": "SETUP_ERROR" if transient else "FAIL",
                    "reason": ("post-import read-back interrupted by a transient failure"
                               if transient else "post-import read-back failed"),
                    "stderr": str(error)[:500]}
                continue
            after_dataset = dataset_from_tsv(after_rows, truth, version)
            (case / "import-validation" / f"{label}_server_roundtrip.json").write_text(
                json.dumps(after_dataset, sort_keys=True, ensure_ascii=False), encoding="utf-8")
            comparison = compare(truth, after_dataset, f"{case.name}_server_{label}")
            results["artifacts"][label] = {
                "status": "PASS" if comparison.get("strict_pass") else "FAIL",
                "expected_rows": comparison.get("expected_rows"),
                "actual_rows": comparison.get("actual_rows"),
                "correct_rows": comparison.get("correct_rows"),
                "field_mismatches": len(comparison.get("field_mismatches", [])),
                "false_positives": len(comparison.get("false_positives", [])),
                "false_negatives": len(comparison.get("false_negatives", [])),
                "strict_pass": comparison.get("strict_pass"),
                "stdout_tail": proc.stdout.strip()[-300:],
            }
        statuses = [entry["status"] for entry in results["artifacts"].values()]
        if statuses and all(s == "PASS" for s in statuses):
            results["status"] = "PASS"
        elif any(s == "SETUP_ERROR" for s in statuses):
            # A transport interruption must never be scored as a failed import.
            results["status"] = "SETUP_ERROR"
            results["reason"] = "clean-instance import was interrupted"
        elif any(s == "EVIDENCE_ONLY" for s in statuses):
            results["status"] = "EVIDENCE_ONLY"
        else:
            results["status"] = "FAIL"
        (case / "import-validation" / "fresh_import_result.json").write_text(
            json.dumps(results, indent=1, ensure_ascii=False), encoding="utf-8")
        return results
    finally:
        run(["podman", "rm", "-f", container], 60)
        shutil.rmtree(datadir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Fresh-instance import verification")
    parser.add_argument("--case", action="append", required=True)
    args = parser.parse_args()

    summary = []
    for raw in args.case:
        case = Path(raw)
        truth_path = case / "truth" / "ground_truth.json"
        version = "8.0"
        if truth_path.is_file():
            version = json.loads(truth_path.read_text(encoding="utf-8")).get("mysql_version", "8.0")
        try:
            result = verify_case(case, version)
        except Exception as error:  # noqa: BLE001 - surfaced as a failed case
            result = {"case": case.name, "status": "FAIL",
                      "reason": f"{type(error).__name__}: {error}"}
        print(f"{result.get('status', '?'):>6}  {case.name}  {result.get('reason', '')}")
        summary.append(result)

    out = Path("/app/mysql_recovery_artifacts/fresh_import_summary.json")
    out.write_text(json.dumps(summary, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
