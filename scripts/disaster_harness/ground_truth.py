#!/usr/bin/env python3
"""Canonical, typed ground truth for MySQL recovery comparisons."""

import base64
import hashlib
import json
import math
import re
from datetime import date, datetime, time, timedelta
from decimal import Decimal
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from .manifest import canonical_json, sha256_bytes

GROUND_TRUTH_VERSION = 1


def _decimal_text(value: Decimal) -> str:
    if not value.is_finite():
        raise ValueError("non-finite Decimal is not canonical ground-truth data")
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    if text in ("", "-0"):
        return "0"
    return text


def _json_value(value: Any) -> Any:
    if value is None:
        return None
    if isinstance(value, bool):
        return value
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("non-finite JSON number is not canonical")
        return value
    if isinstance(value, Decimal):
        return _decimal_text(value)
    if isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        return [_json_value(item) for item in value]
    if isinstance(value, dict):
        return {str(k): _json_value(v) for k, v in sorted(value.items(), key=lambda kv: str(kv[0]))}
    raise TypeError(f"unsupported JSON value: {type(value).__name__}")


def canonical_value(value: Any, declared_type: str = "") -> Dict[str, Any]:
    """Return a lossless tagged representation distinguishing NULL and empty."""
    if value is None:
        return {"type": "null"}
    normalized_type = declared_type.lower().split("(", 1)[0].strip()
    if isinstance(value, bool):
        return {"type": "bool", "value": value}
    if isinstance(value, bytes):
        return {"type": "binary", "encoding": "base64", "value": base64.b64encode(value).decode("ascii")}
    if isinstance(value, Decimal) or normalized_type in {"decimal", "numeric"}:
        return {"type": "decimal", "value": _decimal_text(Decimal(str(value)))}
    if isinstance(value, datetime):
        return {"type": "datetime", "value": value.isoformat(sep=" ")}
    if isinstance(value, date):
        return {"type": "date", "value": value.isoformat()}
    if isinstance(value, time):
        return {"type": "time", "value": value.isoformat()}
    if isinstance(value, timedelta):
        return {"type": "duration_us", "value": int(value.total_seconds() * 1000000)}
    if normalized_type == "json" or isinstance(value, (dict, list, tuple)):
        return {"type": "json", "value": _json_value(value)}
    if normalized_type == "enum":
        return {"type": "enum", "value": str(value)}
    if normalized_type == "set":
        return {"type": "set", "value": str(value)}
    if isinstance(value, int) and not isinstance(value, bool):
        return {"type": "int", "value": value}
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("non-finite float is not canonical")
        return {"type": "float", "value": repr(value)}
    if isinstance(value, str):
        return {"type": "string", "value": value}
    raise TypeError(f"unsupported value type: {type(value).__name__}")


def canonical_schema(columns: Iterable[Dict[str, Any]], primary_key: Iterable[str]) -> Dict[str, Any]:
    normalized = []
    for ordinal, column in enumerate(columns):
        item = {
            "name": str(column["name"]),
            "ordinal": int(column.get("ordinal", ordinal)),
            "type": str(column.get("type", column.get("data_type", ""))).lower(),
            "nullable": bool(column.get("nullable", True)),
        }
        for key in ("length", "precision", "scale", "charset", "collation", "enum_values", "set_values"):
            if key in column and column[key] is not None:
                item[key] = column[key]
        normalized.append(item)
    normalized.sort(key=lambda item: (item["ordinal"], item["name"]))
    pk = [str(name) for name in primary_key]
    return {"columns": normalized, "primary_key": pk}


def schema_fingerprint(schema: Dict[str, Any]) -> str:
    return sha256_bytes(canonical_json(schema).encode("utf-8"))


def _row_payload(schema: Dict[str, Any], values: Dict[str, Any]) -> Dict[str, Any]:
    by_name = {column["name"]: column for column in schema["columns"]}
    return {
        name: canonical_value(values[name], by_name[name].get("type", ""))
        for name in [column["name"] for column in schema["columns"]]
    }


def row_hash(schema: Dict[str, Any], values: Dict[str, Any]) -> str:
    return sha256_bytes(canonical_json(_row_payload(schema, values)).encode("utf-8"))


def _pk_key(schema: Dict[str, Any], values: Dict[str, Any]) -> str:
    if not schema["primary_key"]:
        raise ValueError("a verifiable primary key is required")
    payload = []
    by_name = {column["name"]: column for column in schema["columns"]}
    for name in schema["primary_key"]:
        if name not in values:
            raise ValueError(f"primary-key column missing: {name}")
        payload.append(canonical_value(values[name], by_name[name].get("type", "")))
    return canonical_json(payload)


def build_ground_truth(database: str, table: str, schema: Dict[str, Any], rows: Iterable[Dict[str, Any]],
                       mysql_version: str = "", metadata: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    schema = canonical_schema(schema["columns"], schema["primary_key"])
    fingerprint = schema_fingerprint(schema)
    canonical_rows = []
    duplicate_keys = []
    seen = set()
    if not schema["primary_key"]:
        return {
            "ground_truth_version": GROUND_TRUTH_VERSION,
            "database": database,
            "table": table,
            "mysql_version": mysql_version,
            "schema": schema,
            "schema_fingerprint": fingerprint,
            "rows": [],
            "row_count": 0,
            "duplicate_primary_keys": [],
            "status": "NOT_COMPARABLE",
            "metadata": metadata or {},
            "dataset_sha256": sha256_bytes(canonical_json({
                "schema_fingerprint": fingerprint,
                "rows": [],
            }).encode("utf-8")),
        }
    for values in rows:
        values = dict(values)
        key = _pk_key(schema, values)
        if key in seen:
            duplicate_keys.append(key)
        seen.add(key)
        canonical_rows.append({
            "pk": json.loads(key),
            "values": _row_payload(schema, values),
            "row_hash": row_hash(schema, values),
        })
    canonical_rows.sort(key=lambda row: canonical_json(row["pk"]))
    valid = bool(schema["primary_key"]) and not duplicate_keys
    document = {
        "ground_truth_version": GROUND_TRUTH_VERSION,
        "database": database,
        "table": table,
        "mysql_version": mysql_version,
        "schema": schema,
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


def write_ground_truth(path: Path, document: Dict[str, Any]) -> Path:
    path = Path(path)
    if path.exists():
        raise FileExistsError(f"refusing to overwrite ground truth: {path}")
    path.write_text(canonical_json(document) + "\n", encoding="utf-8")
    return path


def load_ground_truth(path: Path) -> Dict[str, Any]:
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)


def _quote_identifier(identifier: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_$]+", identifier):
        raise ValueError(f"unsafe SQL identifier: {identifier}")
    return "`" + identifier.replace("`", "``") + "`"


def capture_mysql_table(conn, database: str, table: str, mysql_version: str = "") -> Dict[str, Any]:
    """Capture a table through a DB-API connection using information_schema metadata."""
    db = _quote_identifier(database)
    tbl = _quote_identifier(table)
    cursor = conn.cursor()
    try:
        cursor.execute(
            "SELECT COLUMN_NAME, ORDINAL_POSITION, COLUMN_TYPE, IS_NULLABLE, "
            "CHARACTER_MAXIMUM_LENGTH, NUMERIC_PRECISION, NUMERIC_SCALE, "
            "CHARACTER_SET_NAME, COLLATION_NAME "
            "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s "
            "ORDER BY ORDINAL_POSITION", (database, table))
        column_rows = cursor.fetchall()
        columns = []
        for row in column_rows:
            columns.append({
                "name": row[0], "ordinal": int(row[1]) - 1, "type": row[2],
                "nullable": row[3] == "YES", "length": row[4],
                "precision": row[5], "scale": row[6], "charset": row[7],
                "collation": row[8],
            })
        cursor.execute(
            "SELECT COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE "
            "WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s AND CONSTRAINT_NAME='PRIMARY' "
            "ORDER BY ORDINAL_POSITION", (database, table))
        primary_key = [row[0] for row in cursor.fetchall()]
        schema = {"columns": columns, "primary_key": primary_key}
        column_names = [column["name"] for column in columns]
        cursor.execute(f"SELECT {', '.join(_quote_identifier(name) for name in column_names)} FROM {db}.{tbl}")
        rows = [dict(zip(column_names, row)) for row in cursor.fetchall()]
        return build_ground_truth(database, table, schema, rows, mysql_version)
    finally:
        cursor.close()
