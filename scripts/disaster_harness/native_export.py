#!/usr/bin/env python3
"""Capture a running mysqld table into canonical typed ground-truth JSON.

This runs inside the native MySQL container over the private socket so that the
native baseline uses the exact same typed encoding as ground truth. It never
parses client text output.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .ground_truth import capture_mysql_table

try:
    from disaster_validation.matrix import CAPABILITIES
except ModuleNotFoundError:
    from scripts.disaster_validation.matrix import CAPABILITIES


def connection_collation(mysql_version: str) -> str:
    """Return the known compatible collation for an exact MySQL major version."""
    major_minor = ".".join(mysql_version.split(".")[:2])
    capability = CAPABILITIES.get(major_minor)
    if capability is None:
        raise ValueError(f"unsupported MySQL version for native export: {mysql_version}")
    return capability.connection_collation


def export_table(socket_path: str, database: str, table: str, output: Path,
                 user: str, password: str, mysql_version: str = "") -> dict:
    import mysql.connector

    conn = mysql.connector.connect(
        unix_socket=socket_path,
        user=user,
        password=password,
        use_pure=True,
        charset="utf8mb4",
        connection_timeout=30,
    )
    try:
        version = ""
        cursor = conn.cursor()
        try:
            cursor.execute("SELECT VERSION()")
            fetched = cursor.fetchone()
            version = str(fetched[0]) if fetched else ""
        finally:
            cursor.close()
        expected_collation = connection_collation(mysql_version or version)
        collation_cursor = conn.cursor()
        try:
            collation_cursor.execute(f"SET NAMES utf8mb4 COLLATE {expected_collation}")
        finally:
            collation_cursor.close()
        document = capture_mysql_table(conn, database, table, version)
    finally:
        conn.close()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, sort_keys=True, separators=(",", ":"),
                                 ensure_ascii=False) + "\n", encoding="utf-8")
    return document


def main() -> int:
    parser = argparse.ArgumentParser(description="Typed native recovery-result exporter")
    parser.add_argument("--socket", required=True)
    parser.add_argument("--database", required=True)
    parser.add_argument("--table", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--user", default="root")
    parser.add_argument("--password", default=None)
    parser.add_argument("--password-stdin", action="store_true")
    parser.add_argument("--mysql-version", default="",
                        help="expected server major version; detected version is used when omitted")
    args = parser.parse_args()
    password = args.password
    if args.password_stdin:
        password = sys.stdin.readline().rstrip("\n")
    if password is None:
        password = ""
    try:
        document = export_table(args.socket, args.database, args.table,
                                Path(args.output), args.user, password, args.mysql_version)
    except Exception as error:  # noqa: BLE001 - surfaced as a failed export
        sys.stderr.write(f"native_export_failed: {type(error).__name__}: {error}\n")
        return 1
    sys.stderr.write(f"native_export_rows: {document.get('row_count')}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
