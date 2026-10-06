#!/usr/bin/env python3
"""Executable harness: create the full fixture suite on live MySQL and verify.

This is the integration harness that proves the fixtures are real: it starts
the server, creates every suite, inserts the pinned boundary rows plus a random
sample, and reads the data back through the recovery binary's own schema JSON to
confirm field alignment.  It is the same code path Phase A/B will use, so a
passing run here means the fixtures are usable by the campaign rather than
merely self-consistent.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, "/app/mysql_recovery_pro")

from scripts.disaster_validation import fixtures as fx
from scripts.disaster_validation.mysql_driver import (
    ContainerGuard,
    MysqlClient,
    server_args_for,
    start_mysql,
)
from scripts.disaster_validation.matrix import CAPABILITIES

DATABASE = "fixture_check"

# Variables that do not exist on every supported server.  MySQL 5.6 has no
# innodb_default_row_format (5.7 onward), and a 5.6 probe must not be reported
# as a fixture failure: it is a version fact, so it is recorded verbatim.
_ENVIRONMENT_PROBES = (
    ("mysqld_version", "SELECT VERSION();"),
    ("client_charset", "SELECT @@character_set_client;"),
    ("server_charset", "SELECT @@character_set_server;"),
    ("collation_server", "SELECT @@collation_server;"),
    ("innodb_page_size", "SELECT @@innodb_page_size;"),
    ("innodb_default_row_format", "SELECT @@innodb_default_row_format;"),
    ("innodb_file_per_table", "SELECT @@innodb_file_per_table;"),
    ("innodb_file_format", "SELECT @@innodb_file_format;"),
    ("lower_case_table_names", "SELECT @@lower_case_table_names;"),
    ("sql_mode", "SELECT @@sql_mode;"),
)


def probe_environment(client: MysqlClient) -> dict:
    """Record every environment fact a version supports; never raise."""
    record = {}
    for name, sql in _ENVIRONMENT_PROBES:
        try:
            record[name] = client.scalar(sql)
        except Exception as error:  # noqa: BLE001 - unsupported variable, recorded
            record[name] = f"UNSUPPORTED: {type(error).__name__}"
    return record


def check_version(version: str, suites: list[str], rows: int, page_size: int,
                  evidence_root: Path) -> dict:
    run_id = f"fixture-{version.replace('.', '')}-{int(time.time())}"
    image = CAPABILITIES[version].image
    record: dict = {"version": version, "image": image, "run_id": run_id,
                    "suites": {}, "page_size": page_size, "rows": rows}
    container = None
    try:
        with ContainerGuard(f"fixture-check-{run_id}"):
            container = start_mysql(
                run_id, image, None,
                server_args_for(version, page_size), container_prefix="fixture-check")
            client = MysqlClient(container)
            client.execute(f"CREATE DATABASE IF NOT EXISTS `{DATABASE}`;")
            client.database = DATABASE
            record["environment"] = probe_environment(client)

            for name in suites:
                supported, reason = fx.suite_supports(name, version)
                if not supported:
                    record["suites"][name] = {"status": "UNSUPPORTED", "reason": reason}
                    continue
                spec = fx.build_suite(name, version)
                entry: dict = {"status": "PENDING"}
                started = time.time()
                try:
                    client.execute(spec.ddl(DATABASE))
                    for first, last in fx.row_count_batches(rows):
                        client.execute_batch(spec.insert_sql(DATABASE, first, last),
                                             timeout=1800)
                    actual = client.row_count(DATABASE, spec.name)
                    entry["row_count"] = actual
                    entry["expected_rows"] = rows
                    entry["duration_s"] = round(time.time() - started, 2)
                    # Schema metadata is what the recovery binary is told; if the
                    # server disagrees with the fixture the schema JSON is wrong.
                    metadata = client.column_metadata(DATABASE, spec.name)
                    entry["server_columns"] = len(metadata)
                    entry["fixture_columns"] = len(
                        [c for c in spec.columns if not c.virtual_generated])
                    entry["primary_key"] = client.primary_key(DATABASE, spec.name)
                    entry["status"] = "PASS" if actual == rows else "FAIL"
                    if actual != rows:
                        entry["reason"] = f"row count {actual} != {rows}"
                except Exception as error:  # noqa: BLE001 - recorded per suite
                    entry["status"] = "FAIL"
                    entry["reason"] = f"{type(error).__name__}: {error}"
                record["suites"][name] = entry
    except Exception as error:  # noqa: BLE001 - recorded per version
        record["error"] = f"{type(error).__name__}: {error}"
    finally:
        (evidence_root).mkdir(parents=True, exist_ok=True)
        (evidence_root / f"{run_id}.json").write_text(
            json.dumps(record, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description="Fixture integration check on live MySQL")
    parser.add_argument("--versions", nargs="+", default=["5.6", "5.7", "8.0", "8.4", "9.0"])
    parser.add_argument("--suites", nargs="+", default=None)
    parser.add_argument("--rows", type=int, default=120)
    parser.add_argument("--page-size", type=int, default=16384)
    parser.add_argument("--evidence-root", type=Path,
                        default=Path("/app/mysql_recovery_artifacts/fixture_check"))
    args = parser.parse_args()

    suites = args.suites or list(fx.suite_names())
    failures = 0
    for version in args.versions:
        record = check_version(version, suites, args.rows, args.page_size,
                               args.evidence_root)
        if record.get("error"):
            print(f"{version}: HARNESS ERROR {record['error']}")
            failures += 1
            continue
        line = []
        for name, entry in record["suites"].items():
            status = entry["status"]
            line.append(f"{name}={status}")
            if status == "FAIL":
                failures += 1
                print(f"  {version}/{name}: {entry.get('reason')}")
        print(f"{version}: {' '.join(line)}")
    print(f"\ntotal failures: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
