#!/usr/bin/env python3
"""Phase A: Offline .ibd Baseline Orchestrator.

Executes baseline disaster recovery validation on isolated .ibd tablespaces
across MySQL versions, page sizes, row formats, and data volumes.
Operating strictly in userspace without touching raw block devices.

Every recovery output is gated on the CLI result contract (format_version 2).
CSV and SQL exports are validated independently and must strictly agree with
sealed ground truth before a case may report PASS.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import asdict
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence

from scripts.disaster_harness.comparator import compare, write_comparison
from scripts.disaster_harness.ground_truth import build_ground_truth, write_ground_truth
from scripts.disaster_harness.manifest import canonical_json, sha256_file
from scripts.disaster_validation.csv_to_canonical import csv_to_canonical_dataset, _parse_csv_cell
from scripts.disaster_validation.dataset_values import hex_literal
from scripts.disaster_validation.lifecycle import RunLifecycleError, ValidationRun
from scripts.disaster_validation.matrix import CAPABILITIES, MatrixItem, matrix_item

EVIDENCE_ROOT = Path("/app/mysql_recovery_artifacts")
RECOVERY_BINARY = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")

REQUIRED_CONTRACT_KEYS = frozenset({
    "format_version", "table_name", "success", "classification", "structured_rows",
    "evidence_fragments", "schema_source", "schema_trust_level", "damage_level",
    "recovery_method", "ddl_variation_detected", "export_success",
    "export_rejection_reason", "total_pages_scanned", "skipped_pages",
    "damaged_pages", "checksum_mismatches", "header_damage_pages",
    "importable", "reason", "source_ibd", "compression_status",
    "logical_page_size", "physical_page_size",
    "compression_classification_basis", "decoder_validated",
    "compression_rejection_reason",
})


class CommandResult:
    """Minimal subprocess result for the injectable runner seam."""

    def __init__(self, argv: Sequence[str], returncode: Optional[int],
                 stdout: str, stderr: str) -> None:
        self.argv = list(argv)
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _subprocess_runner(argv: Sequence[str], timeout: float, input_text: str = "") -> CommandResult:
    try:
        completed = subprocess.run(
            list(argv), input=input_text, capture_output=True, text=True,
            timeout=timeout, check=False,
        )
        return CommandResult(argv, completed.returncode, completed.stdout, completed.stderr)
    except subprocess.TimeoutExpired as error:
        return CommandResult(argv, None, error.stdout or "", error.stderr or "")
    except OSError as error:
        return CommandResult(argv, None, "", str(error))


Runner = Callable[[Sequence[str], float, str], CommandResult]


def _run_cmd(argv: list[str], timeout: float = 300, check: bool = True,
             runner: Runner = _subprocess_runner) -> CommandResult:
    res = runner(argv, timeout, "")
    if check and res.returncode != 0:
        raise RuntimeError(
            f"Command failed ({res.returncode}): {' '.join(str(a) for a in argv)}\n"
            f"STDERR: {res.stderr}\nSTDOUT: {res.stdout}")
    return res


def _mysql_exec(container_name: str, sql: str, timeout: float = 120,
                runner: Runner = _subprocess_runner, retries: int = 4) -> str:
    """Execute SQL with bounded retries for transient mysqld/socket loss.

    This is for idempotent statements (DDL, COUNT, SELECT). Non-idempotent
    batches must pass ``retries=1`` and own their retry after proving with a
    count whether the statement landed, otherwise a blind retry can duplicate
    rows.
    """
    last: Optional[CommandResult] = None
    for attempt in range(max(1, retries)):
        last = runner(
            ["podman", "exec", "-i", container_name, "mysql", "-uroot", "-e", sql],
            timeout, "",
        )
        if last.returncode == 0:
            return last.stdout
        transient = any(marker in (last.stderr or "") for marker in (
            "Can't connect to local MySQL server",
            "Lost connection to MySQL server",
            "Server shutdown in progress",
            "Error in result packet",
            "empty reply from server",
        ))
        if not transient or attempt == retries - 1:
            break
        time.sleep(2 * (attempt + 1))
    assert last is not None
    raise RuntimeError(
        f"MySQL query failed ({last.returncode}): {sql}\n"
        f"STDERR: {last.stderr}\nSTDOUT: {last.stdout}")


def _mysql_exec_file(container_name: str, sql_path: Path, database: str,
                     timeout: float = 300, runner: Runner = _subprocess_runner) -> CommandResult:
    """Execute a SQL file inside the container via stdin redirection.

    The file is piped through `mysql <db>` so that recovered.sql INSERT
    statements land in an orchestrator-owned disposable schema only.
    """
    res = runner(
        ["podman", "exec", "-i", container_name, "mysql", "-uroot", database],
        timeout,
        sql_path.read_text(encoding="utf-8"),
    )
    return res


def _wait_for_mysql(container_name: str, timeout: float = 60,
                    runner: Runner = _subprocess_runner) -> None:
    start = time.time()
    while time.time() - start < timeout:
        res = runner(
            ["podman", "exec", container_name, "mysqladmin", "ping", "-uroot"],
            10, "",
        )
        if res.returncode == 0:
            return
        time.sleep(1)
    raise TimeoutError(f"MySQL container {container_name} failed to become ready in {timeout}s")


def build_schema_argument(schema: Dict[str, Any]) -> str:
    primary_key = set(schema["primary_key"])
    tokens = []
    for column in schema["columns"]:
        token = f"{column['name']}:{column['type']}"
        if column["name"] in primary_key:
            token += ":pk"
        if not column["nullable"]:
            token += ":notnull"
        if column.get("charset"):
            token += f":{column['charset']}"
        tokens.append(token)
    return ",".join(tokens)


def _load_result_contract(path: Path) -> Dict[str, Any]:
    """Parse and structurally validate a format_version 2 result contract."""
    if not path.is_file():
        raise RuntimeError(f"result contract missing: {path}")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"result contract is not valid JSON: {path}") from error
    if not isinstance(document, dict):
        raise RuntimeError("result contract must be a JSON object")
    missing = REQUIRED_CONTRACT_KEYS - set(document)
    if missing:
        raise RuntimeError(f"result contract missing fields: {', '.join(sorted(missing))}")
    if document.get("format_version") != 2:
        raise RuntimeError(f"unsupported result contract format_version: {document.get('format_version')}")
    return document


def _contract_allows_import(contract: Dict[str, Any]) -> bool:
    """Return True only when the contract itself reports a successful, unrejected export."""
    return (
        contract.get("success") is True
        and contract.get("export_success") is True
        and not contract.get("export_rejection_reason")
    )


def _parse_sql_values(text: str) -> List[Any]:
    """Parse the VALUES (...) payload of one INSERT statement.

    Every non-NULL token is a single-quoted string literal produced by
    SQLOutputter. Returns Python objects: str for quoted literals, None for
    SQL NULL, and bytes for UNHEX('...') / X'...' forms if present.
    """
    values: List[Any] = []
    i = 0
    n = len(text)
    while i < n:
        while i < n and text[i] in " \t\n\r,":
            i += 1
        if i >= n:
            break
        if text.startswith("NULL", i):
            values.append(None)
            i += 4
            continue
        if i + 7 < n and (text.startswith("UNHEX('", i) or text.startswith("unhex('", i)):
            end = text.find("')", i + 7)
            if end == -1:
                values.append(text[i:])
                break
            try:
                values.append(bytes.fromhex(text[i + 7:end]))
            except ValueError:
                values.append(text[i:end + 2])
            i = end + 2
            continue
        if text[i] in "Xx" and i + 1 < n and text[i + 1] == "'":
            end = text.find("'", i + 2)
            if end == -1:
                values.append(text[i:])
                break
            try:
                values.append(bytes.fromhex(text[i + 2:end]))
            except ValueError:
                values.append(text[i:end + 1])
            i = end + 1
            continue
        if text[i] == "'":
            i += 1
            buf = []
            while i < n:
                if text[i] == "'":
                    if i + 1 < n and text[i + 1] == "'":
                        buf.append("'")
                        i += 2
                        continue
                    i += 1
                    break
                if text[i] == "\\" and i + 1 < n:
                    nxt = text[i + 1]
                    if nxt == "0":
                        buf.append("\0")
                    elif nxt == "n":
                        buf.append("\n")
                    elif nxt == "r":
                        buf.append("\r")
                    elif nxt == "\\":
                        buf.append("\\")
                    elif nxt == "Z":
                        buf.append("\x1a")
                    elif nxt == '"':
                        buf.append('"')
                    else:
                        buf.append(nxt)
                    i += 2
                    continue
                buf.append(text[i])
                i += 1
            values.append("".join(buf))
            continue
        # Bare numeric / other literal
        end = i
        while end < n and text[end] not in ",) \t\n\r":
            end += 1
        token = text[i:end]
        try:
            values.append(int(token))
        except ValueError:
            try:
                values.append(float(token))
            except ValueError:
                values.append(token)
        i = end if end > i else i + 1
    return values


def parse_recovered_sql(sql_path: Path) -> List[Dict[str, Any]]:
    """Parse recovered.sql into row dicts keyed by column name.

    Only INSERT INTO ... (`col`,...) VALUES (...); statements are understood.
    Statements without a VALUES payload are skipped. Returns list of dicts
    preserving emitted column order.
    """
    if not sql_path.is_file():
        raise FileNotFoundError(f"recovered SQL file does not exist: {sql_path}")
    text = sql_path.read_text(encoding="utf-8", errors="replace")
    rows: List[Dict[str, Any]] = []
    # Split on INSERT boundaries, not just ';': the exporter emits a bare
    # "INSERT INTO `t` (cols) VALUES" header line with no semicolon, which
    # would otherwise glue to the first row statement and lose it.
    for stmt in re.split(r"(?i)(?=INSERT\s+INTO)", text):
        stmt = stmt.strip().rstrip(";").strip()
        if not stmt:
            continue
        if not stmt.upper().startswith("INSERT INTO"):
            continue
        col_start = stmt.find("(")
        col_end = stmt.find(")", col_start)
        if col_start == -1 or col_end == -1:
            continue
        columns = [c.strip().strip("`") for c in stmt[col_start + 1:col_end].split(",")]
        values_pos = stmt.upper().find("VALUES", col_end)
        if values_pos == -1:
            continue
        payload = stmt[values_pos + 6:].strip()
        if payload.startswith("(") and payload.endswith(")"):
            payload = payload[1:-1]
        else:
            # VALUES keyword without a parenthesised payload: malformed.
            continue
        values = _parse_sql_values(payload)
        rows.append(dict(zip(columns, values)))
    return rows


def sql_rows_to_canonical_dataset(
    rows: List[Dict[str, Any]],
    database: str,
    table: str,
    schema: Dict[str, Any],
    mysql_version: str = "",
    metadata: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """Convert parsed SQL row dicts into a canonical typed dataset.

    SQL export encodes all values as quoted strings; the schema types are
    applied so the result is strictly comparable to ground truth.
    """
    from scripts.disaster_harness.ground_truth import (
        GROUND_TRUTH_VERSION, TYPED_ENCODING_VERSION,
        canonical_schema, canonical_value, row_hash, schema_fingerprint,
    )
    from scripts.disaster_harness.manifest import sha256_bytes

    canonical_sch = canonical_schema(schema["columns"], schema["primary_key"])
    fingerprint = schema_fingerprint(canonical_sch)
    by_name = {col["name"]: col for col in canonical_sch["columns"]}

    canonical_rows = []
    duplicate_keys = []
    seen = set()
    for row in rows:
        parsed_values = {}
        for col in canonical_sch["columns"]:
            name = col["name"]
            parsed_values[name] = _parse_csv_cell(row.get(name), col)
        pk_vals = [
            canonical_value(parsed_values[k], by_name[k].get("type", ""))
            for k in canonical_sch["primary_key"]
        ]
        pk_key = canonical_json(pk_vals)
        if pk_key in seen:
            duplicate_keys.append(pk_key)
        seen.add(pk_key)
        canonical_rows.append({
            "pk": pk_vals,
            "values": {
                name: canonical_value(parsed_values.get(name), by_name[name].get("type", ""))
                for name in [c["name"] for c in canonical_sch["columns"]]
            },
            "row_hash": row_hash(canonical_sch, parsed_values),
        })

    canonical_rows.sort(key=lambda r: canonical_json(r["pk"]))
    valid = bool(canonical_sch["primary_key"]) and not duplicate_keys
    document = {
        "ground_truth_version": GROUND_TRUTH_VERSION,
        "typed_encoding_version": TYPED_ENCODING_VERSION,
        "database": database,
        "table": table,
        "mysql_version": mysql_version,
        "schema": canonical_sch,
        "schema_fingerprint": fingerprint,
        "rows": canonical_rows,
        "row_count": len(canonical_rows),
        "duplicate_primary_keys": sorted(set(duplicate_keys)),
        "status": "SEALED_READY" if valid else "NOT_COMPARABLE",
        "metadata": metadata or {},
    }
    document["dataset_sha256"] = sha256_bytes(canonical_json({
        "schema_fingerprint": fingerprint,
        "rows": canonical_rows,
    }).encode("utf-8"))
    return document


class PhaseAOrchestrator:
    """Baseline .ibd recovery validation with dual-format export gating."""

    def __init__(self, run_root: Path, item: MatrixItem,
                 binary: Path = RECOVERY_BINARY,
                 run_id: Optional[str] = None,
                 runner: Runner = _subprocess_runner):
        self.item = item
        self.binary = binary
        self.runner = runner
        self.run_id = run_id or (
            f"phase-a-{item.version.replace('.', '')}-{item.row_format.lower()}-"
            f"{item.dataset_profile}-p{item.page_size}-{int(time.time())}"
        )
        self.evidence_root = run_root
        self.capability = CAPABILITIES[item.version]
        self.container_name = f"phase-a-box-{self.run_id}"
        self.run: Optional[ValidationRun] = None
        self.database = "phase_a_db"
        self.table = f"t_{item.dataset_profile}"

    def execute(self) -> Dict[str, Any]:
        self.run = ValidationRun.create(self.evidence_root, self.run_id, self.item)
        run_dir = self.run.root

        self.run.transition("PRECHECKED", {
            "version": self.item.version,
            "image": self.capability.image,
            "binary_sha256": sha256_file(self.binary),
        })

        data_dir = run_dir / "environment" / "mysql_datadir"
        data_dir.mkdir(mode=0o777, exist_ok=True)
        self.run.transition("MEDIA_PREPARED", {"data_dir": str(data_dir)})

        try:
            podman_argv = [
                "podman", "run", "-d",
                "--name", self.container_name,
                "-v", f"{data_dir}:/var/lib/mysql:Z",
                "-e", "MYSQL_ALLOW_EMPTY_PASSWORD=yes",
                self.capability.image,
                "--innodb-file-per-table=1",
                f"--innodb-page-size={self.item.page_size}",
                # MySQL 5.6 defaults to the Antelope file format, which silently
                # downgrades ROW_FORMAT=DYNAMIC/COMPRESSED to COMPACT. Pin
                # Barracuda so the requested row format is the one tested.
                *(["--innodb-file-format=Barracuda",
                   "--innodb-file-format-max=Barracuda",
                   "--innodb-large-prefix=ON"]
                  if self.item.version.startswith("5.") else []),
            ]
            _run_cmd(podman_argv, timeout=120, runner=self.runner)
            _wait_for_mysql(self.container_name, timeout=60, runner=self.runner)

            self._create_and_populate_table()
            self.run.transition("DATASET_CREATED", {
                "database": self.database,
                "table": self.table,
                "row_format": self.item.row_format,
            })

            truth_doc = self._seal_ground_truth(run_dir)
            self.run.transition("TRUTH_SEALED", {
                "ground_truth_sha256": truth_doc.get("dataset_sha256"),
                "row_count": truth_doc.get("row_count"),
            })

            _mysql_exec(
                self.container_name,
                f"FLUSH TABLES `{self.database}`.`{self.table}` FOR EXPORT;",
                runner=self.runner,
            )
            table_ibd_src = data_dir / self.database / f"{self.table}.ibd"
            table_ibd_dest = run_dir / "persistent-input" / f"{self.table}.ibd"
            shutil.copy2(table_ibd_src, table_ibd_dest)
            _mysql_exec(self.container_name, "UNLOCK TABLES;", runner=self.runner)

            if "FRM" in self.capability.schema_sources:
                frm_src = data_dir / self.database / f"{self.table}.frm"
                if frm_src.is_file():
                    shutil.copy2(frm_src, run_dir / "persistent-input" / f"{self.table}.frm")

            self.run.transition("PERSISTED", {"ibd_sha256": sha256_file(table_ibd_dest)})
            self.run.transition("SCENARIO_APPLIED", {"scenario": self.item.scenario})

            schema_arg = build_schema_argument(truth_doc["schema"])
            frm_file = run_dir / "persistent-input" / f"{self.table}.frm"
            frm_arg = str(frm_file) if frm_file.is_file() else None

            csv_result = self._run_export(
                run_dir, "csv", table_ibd_dest, schema_arg, frm_arg)
            sql_result = self._run_export(
                run_dir, "sql", table_ibd_dest, schema_arg, frm_arg)

            self.run.transition("RECOVERY_ATTEMPTED", {
                "csv": csv_result["returncode"],
                "sql": sql_result["returncode"],
            })

            csv_contract = None
            sql_contract = None
            csv_ok = False
            sql_ok = False

            csv_doc: Optional[Dict[str, Any]] = None
            sql_doc: Optional[Dict[str, Any]] = None
            comparisons: Dict[str, Any] = {}
            blockers: List[str] = []

            try:
                csv_contract = _load_result_contract(csv_result["result_json"])
                csv_ok = _contract_allows_import(csv_contract)
            except RuntimeError:
                blockers.append("csv_contract_rejected")
            try:
                sql_contract = _load_result_contract(sql_result["result_json"])
                sql_ok = _contract_allows_import(sql_contract)
            except RuntimeError:
                blockers.append("sql_contract_rejected")

            if csv_contract is not None and not csv_ok:
                blockers.append("csv_contract_rejected")
            if sql_contract is not None and not sql_ok:
                blockers.append("sql_contract_rejected")

            if csv_ok and csv_result["artifact"].is_file():
                # A CSV that the canonical decoder cannot parse is itself a
                # failed case: record it instead of aborting the whole batch.
                try:
                    csv_doc = csv_to_canonical_dataset(
                        csv_path=csv_result["artifact"],
                        database=self.database,
                        table=self.table,
                        schema=truth_doc["schema"],
                        mysql_version=self.item.version,
                    )
                except (ValueError, TypeError, KeyError, csv.Error) as error:
                    csv_doc = None
                    blockers.append(f"csv_parse_failed:{type(error).__name__}")
                if csv_doc is not None:
                    (run_dir / "recovery-output" / "csv" / "recovered_typed.json").write_text(
                        canonical_json(csv_doc) + "\n", encoding="utf-8")
                    comparisons["truth_vs_csv"] = compare(
                        truth_doc, csv_doc, f"{self.run_id}_truth_vs_csv")
                    if not comparisons["truth_vs_csv"]["strict_pass"]:
                        blockers.append("csv_strict_compare_failed")
            elif csv_ok:
                blockers.append("csv_artifact_missing")

            if sql_ok and sql_result["artifact"].is_file():
                try:
                    sql_rows = parse_recovered_sql(sql_result["artifact"])
                    sql_doc = sql_rows_to_canonical_dataset(
                        sql_rows, self.database, self.table,
                        truth_doc["schema"], self.item.version)
                except (ValueError, TypeError, KeyError) as error:
                    sql_doc = None
                    blockers.append(f"sql_parse_failed:{type(error).__name__}")
                if sql_doc is not None:
                    (run_dir / "import-validation" / "sql_typed.json").write_text(
                        canonical_json(sql_doc) + "\n", encoding="utf-8")
                    comparisons["truth_vs_sql"] = compare(
                        truth_doc, sql_doc, f"{self.run_id}_truth_vs_sql")
                    if not comparisons["truth_vs_sql"]["strict_pass"]:
                        blockers.append("sql_strict_compare_failed")
                    if csv_doc is not None:
                        comparisons["csv_vs_sql"] = compare(
                            csv_doc, sql_doc, f"{self.run_id}_csv_vs_sql")
                        if not comparisons["csv_vs_sql"]["strict_pass"]:
                            blockers.append("csv_sql_disagreement")
            elif sql_ok:
                blockers.append("sql_artifact_missing")

            import_validated = not blockers
            if import_validated:
                self.run.transition("IMPORT_VALIDATED", {
                    "csv_rows": csv_doc["row_count"] if csv_doc else 0,
                    "sql_rows": sql_doc["row_count"] if sql_doc else 0,
                })

            comp_report_path = run_dir / "reports" / "comparison.json"
            write_comparison(comp_report_path, comparisons)
            all_pass = bool(comparisons) and all(
                c.get("strict_pass") for c in comparisons.values())
            self.run.transition("REPORTED", {
                "strict_pass": all_pass,
                "blockers": blockers,
            })
            seal_digest = self.run.seal({
                "comparisons": comparisons,
                "blockers": blockers,
            })

            if blockers:
                return {
                    "run_id": self.run_id,
                    "status": "NOT_COMPUTABLE",
                    "reason": ";".join(blockers),
                    "comparison": comparisons,
                    "manifest_sha256": seal_digest,
                }
            return {
                "run_id": self.run_id,
                "status": "PASS" if all_pass else "FAIL",
                "comparison": comparisons,
                "manifest_sha256": seal_digest,
            }

        finally:
            self._cleanup()

    def _run_export(self, run_dir: Path, fmt: str, ibd: Path,
                    schema_arg: str, frm_arg: Optional[str]) -> Dict[str, Any]:
        out_dir = run_dir / "recovery-output" / fmt
        out_dir.mkdir(parents=True, exist_ok=True)
        result_json = out_dir / "result.json"
        artifact = out_dir / ("recovered.csv" if fmt == "csv" else "recovered.sql")
        argv = [
            str(self.binary),
            f"--ibd={ibd}",
            f"--output={out_dir}",
            f"--format={fmt}",
            f"--page-size={self.item.page_size}",
            f"--schema={schema_arg}",
            f"--result-json={result_json}",
        ]
        if frm_arg:
            argv.append(f"--frm={frm_arg}")
        proc = _run_cmd(argv, timeout=300, check=False, runner=self.runner)
        (run_dir / "logs" / f"recovery_{fmt}.stdout").write_text(proc.stdout, encoding="utf-8")
        (run_dir / "logs" / f"recovery_{fmt}.stderr").write_text(proc.stderr, encoding="utf-8")
        return {
            "returncode": proc.returncode,
            "result_json": result_json,
            "artifact": artifact,
        }

    def _create_and_populate_table(self) -> None:
        _mysql_exec(self.container_name,
                    f"CREATE DATABASE IF NOT EXISTS `{self.database}`;",
                    runner=self.runner)
        row_fmt = f"ROW_FORMAT={self.item.row_format}"
        if self.item.row_format == "COMPRESSED":
            # KEY_BLOCK_SIZE must never exceed innodb_page_size (MySQL rejects
            # the table outright), so derive it and keep a 2:1 ratio on every
            # page size the matrix supports.
            key_block = max(1, min(8, self.item.page_size // 2048))
            row_fmt += f" KEY_BLOCK_SIZE={key_block}"
        has_json = self.capability.supports_json
        col_json = "`payload` JSON," if has_json else ""
        ddl = (
            f"CREATE TABLE `{self.database}`.`{self.table}` ("
            f"`id` BIGINT NOT NULL,"
            f"`signed_value` INT NOT NULL,"
            f"`decimal_value` DECIMAL(12,4) NOT NULL,"
            f"`nullable_text` VARCHAR(128),"
            f"{col_json}"
            f"`created_at` DATETIME NOT NULL,"
            f"`blob_value` BLOB,"
            f"PRIMARY KEY (`id`)"
            f") ENGINE=InnoDB {row_fmt};"
        )
        _mysql_exec(self.container_name, ddl, runner=self.runner)
        row_count = 1000 if self.item.dataset_profile == "smoke" else (
            100000 if self.item.dataset_profile == "100k" else 1000000)
        self._insert_rows(row_count, has_json)

    @staticmethod
    def _row_values(ids: Iterable[int], has_json: bool) -> str:
        values = []
        for i in ids:
            dt_str = "2026-05-01 12:00:00"
            txt_val = f"'sample_text_{i}'" if i % 10 != 0 else "NULL"
            json_val = f"'{{\"k\": {i}}}'," if has_json else ""
            # X'...' must carry an even number of hex digits; see
            # dataset_values.hex_literal for the 65536 boundary it fixes.
            values.append(
                f"({i}, -{i}, {i}.5000, {txt_val}, {json_val} '{dt_str}', "
                f"{hex_literal(i)})")
        return ",".join(values)

    def _present_ids(self, first_id: int, last_id: int) -> set:
        rows = _mysql_exec(
            self.container_name,
            f"SELECT id FROM `{self.database}`.`{self.table}` "
            f"WHERE id BETWEEN {first_id} AND {last_id};",
            timeout=120, runner=self.runner)
        present = set()
        for line in rows.splitlines():
            token = line.strip()
            if token.isdigit():
                present.add(int(token))
        return present

    def _insert_rows(self, total: int, has_json: bool) -> None:
        """Insert the profile in bounded batches, tolerating partial applies.

        A failed client can race with mysqld applying the statement, so a
        blind retry would either duplicate rows or give up. Each retry queries
        which ids actually landed and re-inserts only the missing ones. This
        matters at 100k/1m, where one socket blip must not turn a valid long
        run into a setup error. `retries=1` keeps _mysql_exec's blind retry
        out of the non-idempotent path: this caller owns idempotency.
        """
        batch_size = 500
        for start_id in range(1, total + 1, batch_size):
            end_id = min(start_id + batch_size, total + 1)
            pending = list(range(start_id, end_id))
            last_error = ""
            for attempt in range(4):
                insert_sql = (
                    f"INSERT INTO `{self.database}`.`{self.table}` "
                    f"VALUES {self._row_values(pending, has_json)};")
                try:
                    _mysql_exec(self.container_name, insert_sql, timeout=300,
                                runner=self.runner, retries=1)
                    pending = []
                    break
                except RuntimeError as error:
                    last_error = str(error)
                    recoverable = any(marker in last_error for marker in (
                        "Can't connect to local MySQL server",
                        "Lost connection to MySQL server",
                        "Error in result packet",
                        # A retry after partial application surfaces as a
                        # duplicate key, which the id probe below resolves.
                        "Duplicate entry",
                    ))
                    if not recoverable:
                        raise
                    landed = self._present_ids(start_id, end_id - 1)
                    pending = [i for i in pending if i not in landed]
                    if not pending:
                        last_error = ""
                        break
                    time.sleep(2 * (attempt + 1))
            if pending:
                raise RuntimeError(
                    f"insert of ids {pending[0]}..{pending[-1]} incomplete after "
                    f"retries: {last_error}")

    def _seal_ground_truth(self, run_dir: Path) -> Dict[str, Any]:
        dump_cols = _mysql_exec(
            self.container_name,
            f"SELECT COLUMN_NAME, ORDINAL_POSITION, COLUMN_TYPE, IS_NULLABLE, "
            f"CHARACTER_MAXIMUM_LENGTH, NUMERIC_PRECISION, NUMERIC_SCALE, "
            f"CHARACTER_SET_NAME, COLLATION_NAME "
            f"FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='{self.database}' "
            f"AND TABLE_NAME='{self.table}' ORDER BY ORDINAL_POSITION;",
            runner=self.runner,
        )
        columns = []
        for line in dump_cols.strip().splitlines()[1:]:
            parts = line.split("\t")
            if len(parts) >= 9:
                columns.append({
                    "name": parts[0], "ordinal": int(parts[1]) - 1, "type": parts[2],
                    "nullable": parts[3] == "YES",
                    "length": int(parts[4]) if parts[4] != "NULL" else None,
                    "precision": int(parts[5]) if parts[5] != "NULL" else None,
                    "scale": int(parts[6]) if parts[6] != "NULL" else None,
                    "charset": parts[7] if parts[7] != "NULL" else None,
                    "collation": parts[8] if parts[8] != "NULL" else None,
                })

        pk_cols = _mysql_exec(
            self.container_name,
            f"SELECT COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE "
            f"WHERE TABLE_SCHEMA='{self.database}' AND TABLE_NAME='{self.table}' "
            f"AND CONSTRAINT_NAME='PRIMARY' ORDER BY ORDINAL_POSITION;",
            runner=self.runner,
        )
        pk = [l.strip() for l in pk_cols.strip().splitlines()[1:] if l.strip()]
        schema = {"columns": columns, "primary_key": pk}

        binary_types = {"binary", "varbinary", "blob", "tinyblob", "mediumblob", "longblob"}
        select_columns = []
        for column in columns:
            name = column["name"]
            base_type = column["type"].lower().split("(", 1)[0].strip()
            if base_type in binary_types:
                select_columns.append(f"CONCAT('0x', HEX(`{name}`)) AS `{name}`")
            else:
                select_columns.append(f"`{name}`")
        col_sql = ", ".join(select_columns)
        tsv_data = _mysql_exec(
            self.container_name,
            f"SELECT {col_sql} FROM `{self.database}`.`{self.table}` ORDER BY `id`;",
            runner=self.runner,
        )
        rows = []
        lines = tsv_data.splitlines()
        if lines:
            for line in lines[1:]:
                if not line.strip():
                    continue
                parts = line.split("\t")
                row_dict = {}
                for idx, col in enumerate(columns):
                    val = parts[idx] if idx < len(parts) else "NULL"
                    if val == "NULL":
                        row_dict[col["name"]] = None
                    else:
                        row_dict[col["name"]] = _parse_csv_cell(val, col)
                rows.append(row_dict)

        truth_doc = build_ground_truth(
            self.database, self.table, schema, rows, self.item.version)
        truth_path = run_dir / "truth" / "ground_truth.json"
        write_ground_truth(truth_path, truth_doc)
        return truth_doc

    def _cleanup(self) -> None:
        try:
            _run_cmd(["podman", "rm", "-f", self.container_name],
                     check=False, runner=self.runner)
        except Exception:
            pass


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Phase A: Offline .ibd Baseline Orchestrator")
    parser.add_argument("--versions", nargs="+", default=["8.0"],
                        help="MySQL versions to validate")
    parser.add_argument("--scales", nargs="+", default=["smoke"],
                        help="Dataset profiles: smoke, 100k, 1m")
    parser.add_argument("--row-formats", nargs="+", default=["DYNAMIC"],
                        help="Row formats: DYNAMIC, COMPACT, REDUNDANT, COMPRESSED")
    parser.add_argument("--page-sizes", nargs="+", type=int, default=[16384],
                        help="InnoDB page sizes to validate")
    parser.add_argument("--evidence-root", type=Path, default=EVIDENCE_ROOT,
                        help="Artifact evidence root")
    args = parser.parse_args()

    args.evidence_root.mkdir(parents=True, exist_ok=True)

    summary_results = []
    for version in args.versions:
        for scale in args.scales:
            for rf in args.row_formats:
                for page_size in args.page_sizes:
                    item = matrix_item(version=version, filesystem="ext4",
                                       scenario="baseline", dataset_profile=scale,
                                       row_format=rf, page_size=page_size)
                    if item.status == "UNSUPPORTED":
                        print(f"Skipping unsupported matrix combination: "
                              f"{version} / {rf} / {scale} / {page_size} ({item.reason})")
                        continue
                    print(f"\n=======================================================")
                    print(f"Executing Phase A: MySQL {version} | Format: {rf} | "
                          f"Page: {page_size} | Scale: {scale}")
                    print(f"=======================================================")
                    orchestrator = PhaseAOrchestrator(args.evidence_root, item)
                    # One unusable matrix cell must never abort the batch: the
                    # evidence for every other cell is already on disk.
                    try:
                        res = orchestrator.execute()
                    except Exception as error:  # noqa: BLE001 - recorded, not fatal
                        res = {"run_id": f"phase-a-{version.replace('.', '')}-{rf}-"
                                         f"{scale}-p{page_size}",
                               "status": "SETUP_ERROR",
                               "reason": f"{type(error).__name__}: {error}"}
                    print(f"Result for {res['run_id']}: {res['status']}")
                    if res.get("reason"):
                        print(f"  reason: {res['reason']}")
                    summary_results.append(res)

    print("\n================== PHASE A SUMMARY ==================")
    all_pass = all(r["status"] == "PASS" for r in summary_results)
    for r in summary_results:
        line = f"  [{r['status']}] {r['run_id']}"
        if r.get("reason"):
            line += f" :: {r['reason']}"
        print(line)
    print(f"Overall Phase A: {'ALL PASS' if all_pass else 'SOME FAILED'}\n")


if __name__ == "__main__":
    main()
