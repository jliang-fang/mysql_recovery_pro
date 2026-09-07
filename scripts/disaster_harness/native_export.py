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


def export_table(socket_path: str, database: str, table: str, output: Path,
                 user: str, password: str) -> dict:
    import mysql.connector

    conn = mysql.connector.connect(
        unix_socket=socket_path,
        user=user,
        password=password,
        use_pure=True,
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
    parser.add_argument("--password", default="root")
    args = parser.parse_args()
    try:
        document = export_table(args.socket, args.database, args.table,
                                Path(args.output), args.user, args.password)
    except Exception as error:  # noqa: BLE001 - surfaced as a failed export
        sys.stderr.write(f"native_export_failed: {type(error).__name__}: {error}\n")
        return 1
    sys.stderr.write(f"native_export_rows: {document.get('row_count')}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
