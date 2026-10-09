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
from datetime import timedelta
from decimal import Decimal
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
        proc = subprocess.run(argv, capture_output=True, timeout=timeout,
                              input=input_text.encode("utf-8") if input_text else None, check=check)
        stdout = proc.stdout.decode("utf-8", errors="replace") if proc.stdout else ""
        stderr = proc.stderr.decode("utf-8", errors="replace") if proc.stderr else ""
        return subprocess.CompletedProcess(argv, proc.returncode, stdout, stderr)
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode("utf-8", errors="replace") if error.stdout else ""
        stderr = error.stderr.decode("utf-8", errors="replace") if error.stderr else ""
        return subprocess.CompletedProcess(
            argv, 124, stdout,
            f"{stderr}\ntimed out after {timeout}s".strip())
    except OSError as error:
        return subprocess.CompletedProcess(argv, 127, "", str(error))


def unwrap_canonical(value: Any) -> Any:
    """Turn a canonical {"type": ..., "value": ...} cell into a plain Python value."""
    if isinstance(value, dict) and "type" in value:
        kind = value.get("type")
        if kind == "null":
            return None
        if kind == "set":
            # Canonical SET is {"type": "set", "values": [...]}; join back to the
            # comma-separated text MySQL expects.  Checked before the "value"-key
            # guard below, which would otherwise silently import NULL for every
            # SET cell because the canonical document uses "values".
            members = value.get("values")
            if not isinstance(members, list):
                return None
            return ",".join(str(member) for member in members)
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
        if kind == "duration_us":
            # TIME columns canonicalise to microseconds; render as a timedelta so
            # sql_literal can emit a real TIME literal instead of a bare integer
            # the server rejects with ERROR 1292.
            return timedelta(microseconds=int(value.get("value") or 0))
        if kind in ("int", "float", "decimal", "string", "bool"):
            return value.get("value")
        # json and anything else: re-serialise the decoded structure
        return value.get("value")
    return value


def sql_literal(value: Any, charset: str = "utf8mb4") -> str:
    """Render one canonical value as SQL text for a clean-server INSERT.

    Text containing control characters (especially NUL \x00, but also \r, \n,
    \x1a) cannot be passed raw via stdin to `mysql` CLI without --binary-mode.
    Instead of risking client truncation or encoding corruption, render such
    strings using CONVERT(0x<hex> USING <charset>).
    """
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
    if isinstance(value, timedelta):
        # MySQL TIME literals are H:MM:SS[.ffffff] with an optional sign; str()
        # of a timedelta yields forms MySQL rejects for TIME ("1 day, 6:00:00"
        # for >=24h negatives), so format from the microsecond total directly.
        # Integer arithmetic only: total_seconds() returns a float and the
        # round-trip through double loses one microsecond on ~0.5% of TIME(6)
        # values, which then shows up as field mismatches after read-back.
        total_us = ((value.days * 86400 + value.seconds) * 1_000_000
                    + value.microseconds)
        sign = "-" if total_us < 0 else ""
        total_us = abs(total_us)
        micro = total_us % 1_000_000
        hours, remainder = divmod(total_us // 1_000_000, 3600)
        minutes, seconds = divmod(remainder, 60)
        fraction = f".{micro:06d}".rstrip("0") if micro else ""
        # Quoted: a bare 7:30:30 is a syntax error (ERROR 1064) in an VALUES list.
        return f"'{sign}{hours}:{minutes:02d}:{seconds:02d}{fraction}'"
    if isinstance(value, (dict, list)):
        return "'" + json.dumps(value, ensure_ascii=False, separators=(",", ":")).replace("'", "''") + "'"
    text = str(value)
    effective_charset = charset or "utf8mb4"
    is_latin1 = "latin1" in effective_charset.lower()
    # Check if text contains control characters (< 0x20) or latin1 high bytes that would break CLI pipe or require binary-mode
    if any(ord(c) < 0x20 for c in text) or (is_latin1 and any(ord(c) >= 0x80 for c in text)):
        py_encoding = "latin1" if is_latin1 else (
            "ascii" if "ascii" in effective_charset.lower() else "utf-8"
        )
        try:
            raw_bytes = text.encode(py_encoding, errors="surrogatepass")
        except Exception:
            raw_bytes = text.encode("utf-8", errors="replace")
        return f"CONVERT(0x{raw_bytes.hex()} USING {effective_charset})"
    return "'" + text.replace("\\", "\\\\").replace("'", "''") + "'"


def build_ddl(truth: dict[str, Any]) -> str:
    schema = truth["schema"]
    pk = set(schema.get("primary_key") or [])
    parts = []
    for column in schema["columns"]:
        col_type = column["type"]
        # If column has elements (e.g. from schema.json), reconstruct ENUM/SET elements losslessly
        if column.get("enum_values"):
            elements_str = ",".join("'" + str(e).replace("'", "''") + "'" for e in column["enum_values"])
            col_type = f"enum({elements_str})"
        elif column.get("set_values"):
            elements_str = ",".join("'" + str(e).replace("'", "''") + "'" for e in column["set_values"])
            col_type = f"set({elements_str})"

        pieces = [f"`{column['name']}` {col_type}"]
        if column.get("charset"):
            pieces.append(f"CHARACTER SET {column['charset']}")
            if column.get("collation"):
                pieces.append(f"COLLATE {column['collation']}")
        if column["name"] in pk:
            pieces.append("NOT NULL")
        elif not column.get("nullable", True):
            pieces.append("NOT NULL")
        parts.append(" ".join(pieces))
    if pk:
        ordered = [c["name"] for c in schema["columns"] if c["name"] in pk]
        parts.append("PRIMARY KEY (" + ", ".join(f"`{n}`" for n in ordered) + ")")
    return "CREATE TABLE IF NOT EXISTS `recovered_import`.`recovered_table` (" + ", ".join(parts) + ") ENGINE=InnoDB;"


def capture_tsv(container: str, truth: dict[str, Any],
                database: str = "recovered_import") -> str:
    """Read the table back exactly like ground truth was captured.

    Text fields may contain arbitrary control characters (newlines, tabs, NULs)
    that would break tab-separated / line-separated TSV framing if emitted raw.
    Therefore, text columns (CHAR, VARCHAR, TEXT) are also selected as HEX:
    `HEX(`col`)`, which dataset_from_tsv decodes back to the exact string.
    """
    schema = truth["schema"]
    binary_types = {"binary", "varbinary", "blob", "tinyblob", "mediumblob", "longblob"}
    select = []
    for column in schema["columns"]:
        base = str(column["type"]).lower().split("(", 1)[0].strip()
        if " " in base:
            base = base.split()[0]
        if base in binary_types:
            select.append(f"CONCAT('0x', HEX(`{column['name']}`)) AS `{column['name']}`")
        elif base == "bit":
            select.append(f"CONV(HEX(`{column['name']}`), 16, 10) AS `{column['name']}`")
        elif base in ("char", "varchar", "text", "tinytext", "mediumtext", "longtext"):
            # Emit text as HEX-wrapped so newlines and tabs do not disrupt TSV row/col parsing.
            select.append(f"IF(`{column['name']}` IS NULL, 'NULL', CONCAT('__HEX__', HEX(`{column['name']}`))) AS `{column['name']}`")
        else:
            select.append(f"`{column['name']}`")
    sql = (f"SELECT {', '.join(select)} FROM "
           f"`{database}`.`recovered_table`")
    if truth["schema"].get("primary_key"):
        sql += " ORDER BY " + ", ".join(f"`{n}`" for n in truth["schema"]["primary_key"])
    result = run(["podman", "exec", container, "mysql", "-uroot",
                  "--default-character-set=utf8mb4", "-N", "-B", "-e", sql], 600)
    if result.returncode != 0:
        raise RuntimeError(f"select failed: {result.stderr.strip()}")
    return result.stdout


_MICROS_PER_SECOND = Decimal(1_000_000)


class _ExactTimedelta(timedelta):
    """A timedelta whose ``total_seconds()`` is exact instead of a float.

    ``ground_truth.canonical_value`` renders TIME cells as
    ``int(td.total_seconds() * 1_000_000)``.  Going through double precision
    loses one microsecond on roughly 0.5% of TIME(6) values, so that conversion
    is not idempotent: the recovered CSV is canonicalised once (sealing truth at
    value ``L``) and the imported table is canonicalised again on read-back
    (turning ``L`` into ``L - 1``).  Returning an exact ``Decimal`` makes the
    read-back conversion lossless.  The CSV path deliberately keeps the
    original float behaviour: the sealed truth was produced through it, and
    changing it would make the offline comparison disagree with truth by the
    same one microsecond.
    """

    def total_seconds(self) -> Decimal:
        microseconds = ((self.days * 86400 + self.seconds) * 1_000_000
                        + self.microseconds)
        return Decimal(microseconds) / _MICROS_PER_SECOND


def _decode_tsv_cell(text: Optional[str], column: dict[str, Any]) -> Any:
    """Helper to decode a TSV cell, handling __HEX__-wrapped strings."""
    if text is None or text == "NULL":
        return None
    if text.startswith("__HEX__"):
        hex_data = text[7:]
        charset = column.get("charset", "utf8mb4") or "utf8mb4"
        py_encoding = "latin1" if "latin1" in charset.lower() else (
            "ascii" if "ascii" in charset.lower() else "utf-8"
        )
        try:
            return bytes.fromhex(hex_data).decode(py_encoding, errors="surrogatepass")
        except Exception:
            return bytes.fromhex(hex_data).decode("utf-8", errors="replace")
    value = _parse_csv_cell(text, column)
    if type(value) is timedelta:
        return _ExactTimedelta(days=value.days, seconds=value.seconds,
                               microseconds=value.microseconds)
    return value


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
            row[column["name"]] = _decode_tsv_cell(text, column)
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
            row[column["name"]] = _decode_tsv_cell(text, column)
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

    Uses --default-character-set=utf8mb4 and --binary-mode so that multi-byte
    UTF-8 strings and statements with binary representations are decoded
    properly instead of falling back to default latin1.
    """
    proc = None
    for attempt in range(attempts):
        proc = run(["podman", "exec", "-i", container, "mysql", "-uroot",
                    "--default-character-set=utf8mb4", "--binary-mode=1",
                    "recovered_import"], 1800, input_text=payload)
        if proc.returncode == 0:
            return proc, False
        if not _is_transient(proc.stderr or ""):
            return proc, False
        time.sleep(2 * (attempt + 1))
    return proc, True


def verify_case(case: Path, version: str, *, container_name: str | None = None) -> dict[str, Any]:
    """Import recovered CSV artifacts into a fresh server and compare to truth.

    A legacy single-table case stores ``truth/ground_truth.json``.  Stage 1
    qualification stores one sealed truth document per table below ``truth/``;
    each table's recovery CSV stays in its corresponding recovery-output
    directory.  Both layouts deliberately use the same clean-server gate.
    """
    legacy_truth = case / "truth" / "ground_truth.json"
    truth_paths = ([legacy_truth] if legacy_truth.is_file() else
                   sorted((case / "truth").glob("*/ground_truth.json")))
    if not truth_paths:
        return {"case": case.name, "status": "SETUP_ERROR", "reason": "no ground truth"}

    multi_table = len(truth_paths) > 1
    csv_groups: list[tuple[Path, list[Path]]] = []
    for truth_path in truth_paths:
        table = truth_path.parent.name if multi_table else ""
        csvs = (sorted((case / "recovery-output" / table).glob("*.csv"))
                if table else sorted((case / "recovery-output").rglob("*.csv")))
        csv_groups.append((truth_path, csvs))
    if not any(csvs for _, csvs in csv_groups):
        return {"case": case.name, "status": "EVIDENCE_ONLY", "reason": "no CSV artifact"}

    datadir = Path(f"/tmp/fresh-import-{case.name}")
    if datadir.exists():
        shutil.rmtree(datadir)
    datadir.mkdir(parents=True)
    container = container_name or f"fresh-import-{case.name}"
    if container_name is None:
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
        for truth_path, csvs in csv_groups:
            truth = json.loads(truth_path.read_text(encoding="utf-8"))
            table_label = truth_path.parent.name if multi_table else ""
            if not truth.get("schema", {}).get("columns"):
                results["artifacts"][table_label or "schema"] = {
                    "status": "FAIL", "reason": "truth has no schema"}
                continue
            if not csvs:
                results["artifacts"][table_label or "recovery"] = {
                    "status": "FAIL", "reason": "no recovered CSV artifact for table"}
                continue

            # Enrich a *copy* of the schema with ENUM/SET elements from
            # persistent-input schema.json.  The copy is only used for the DDL:
            # mutating `truth` itself would change the schema fingerprint that
            # csv_to_canonical_dataset derives from truth["schema"], which no
            # longer matches the sealed fingerprint (schema_mismatch).  The
            # elements are also lossless (base64) where the sealed type string
            # carries utf8mb3-mangled members such as 'emoji?'.
            ddl_truth = truth
            schema_file = case / "persistent-input" / table_label / "schema.json" if table_label else case / "persistent-input" / "schema.json"
            if schema_file.is_file():
                try:
                    import base64
                    s_json = json.loads(schema_file.read_text(encoding="utf-8"))
                    key_by_column: dict[str, str] = {}
                    members_by_column: dict[str, list] = {}
                    for col_entry in s_json.get("columns", []):
                        cname = col_entry.get("name")
                        if not cname or not col_entry.get("elements"):
                            continue
                        if col_entry.get("type") == 22:
                            key_by_column[cname] = "enum_values"
                        elif col_entry.get("type") == 23:
                            key_by_column[cname] = "set_values"
                        else:
                            continue
                        members_by_column[cname] = [
                            base64.b64decode(elem["name"]).decode("utf-8")
                            for elem in col_entry["elements"] if "name" in elem
                        ]
                    if key_by_column:
                        ddl_truth = json.loads(json.dumps(truth, ensure_ascii=False))
                        for col in ddl_truth["schema"]["columns"]:
                            key = key_by_column.get(col["name"])
                            if key:
                                col[key] = members_by_column[col["name"]]
                except Exception:
                    ddl_truth = truth

            ddl = build_ddl(ddl_truth)
            import_dir = case / "import-validation"
            import_dir.mkdir(exist_ok=True)
            ddl_name = f"{table_label}_ddl.sql" if table_label else "ddl.sql"
            (import_dir / ddl_name).write_text(ddl + "\n", encoding="utf-8")
            create_db = run(["podman", "exec", container, "mysql", "-uroot",
                             "-e", "CREATE DATABASE IF NOT EXISTS `recovered_import`;"], 120)
            if create_db.returncode != 0:
                results["artifacts"][table_label or "database"] = {
                    "status": "FAIL", "reason": "cannot create import database",
                    "stderr": create_db.stderr.strip()[:400]}
                continue
            reset_table = run(["podman", "exec", container, "mysql", "-uroot", "-e",
                               "DROP TABLE IF EXISTS `recovered_import`.`recovered_table`;"], 120)
            # The DDL must be executed under a utf8mb4 connection: the client's
            # default charset is latin1, and a CREATE TABLE sent without the flag
            # reinterprets every non-ASCII ENUM/SET member as latin1 bytes, so a
            # later utf8mb4 INSERT of the real value is rejected with ERROR 1265.
            ddl_run = run(["podman", "exec", container, "mysql", "-uroot",
                           "--default-character-set=utf8mb4", "-e", ddl], 120)
            if reset_table.returncode != 0 or ddl_run.returncode != 0:
                failed = reset_table if reset_table.returncode != 0 else ddl_run
                results["artifacts"][table_label or "ddl"] = {
                    "status": "FAIL", "reason": "DDL rejected by fresh server",
                    "stderr": failed.stderr.strip()[:400]}
                continue

            columns = [c["name"] for c in truth["schema"]["columns"]]
            col_charsets = {c["name"]: c.get("charset", "utf8mb4") or "utf8mb4"
                            for c in truth["schema"]["columns"]}
            for csv_path in csvs:
                base_label = csv_path.stem
                label = f"{table_label}_{base_label}" if table_label else base_label
                from scripts.disaster_validation.csv_to_canonical import csv_to_canonical_dataset
                canonical = csv_to_canonical_dataset(csv_path, truth["database"], truth["table"],
                                                     truth["schema"], version)
                statements = []
                for row in canonical["rows"]:
                    values = [sql_literal(row["values"].get(name),
                                          charset=col_charsets.get(name, "utf8mb4"))
                              for name in columns]
                    statements.append(
                        "INSERT INTO `recovered_import`.`recovered_table` (`"
                        + "`,`".join(columns) + "`) VALUES ("
                        + ", ".join(values) + ");")
                statements.insert(0, "TRUNCATE TABLE `recovered_import`.`recovered_table`;")
                payload = "\n".join(statements) + "\n"
                (import_dir / f"{label}.sql").write_text(payload, encoding="utf-8")
                proc, transient = _import_with_retry(container, payload)
                if proc.returncode != 0:
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
                if truth.get("mysql_version") and after_dataset.get("mysql_version") != truth["mysql_version"]:
                    after_dataset["mysql_version"] = truth["mysql_version"]
                (import_dir / f"{label}_server_roundtrip.json").write_text(
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
