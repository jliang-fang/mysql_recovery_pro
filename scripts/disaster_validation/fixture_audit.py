#!/usr/bin/env python3
"""Static validation of fixture suites before any server is started.

The integration harness is the real gate, but it costs a container per run.  A
round trip into MySQL is the only way to catch an *encoding* mistake, while
everything here — key uniqueness, declared widths, representable values — is
decidable from the generated literals alone.  Running these first means a broken
fixture fails in milliseconds instead of after a database bootstrap.
"""

from __future__ import annotations

import argparse
import re
import sys
from decimal import Decimal, InvalidOperation
from pathlib import Path

sys.path.insert(0, "/app/mysql_recovery_pro")

from scripts.disaster_validation import fixtures as fx

VERSIONS = ("5.6", "5.7", "8.0", "8.4", "9.0")
SAMPLE_ROWS = 400
CHAR_CAPACITY = re.compile(r"(?i)^(VAR)?CHAR\((\d+)\)$")
DECIMAL_CAPACITY = re.compile(r"(?i)^DECIMAL\((\d+),(\d+)\)$")
# TEXT/BLOB capacities are byte counts, not character counts.
LOB_CAPACITY = {
    "TINYTEXT": 255, "TEXT": 65535, "MEDIUMTEXT": 16777215, "LONGTEXT": 4294967295,
    "TINYBLOB": 255, "BLOB": 65535, "MEDIUMBLOB": 16777215, "LONGBLOB": 4294967295,
}


class Issue:
    def __init__(self, suite: str, column: str, row: int, message: str) -> None:
        self.suite, self.column, self.row, self.message = suite, column, row, message

    def __str__(self) -> str:
        return f"{self.suite}.{self.column} row {self.row}: {self.message}"


def _unescaped_body(literal: str) -> str:
    text = literal.strip()
    if text.startswith("'") and text.endswith("'") and len(text) >= 2:
        return text[1:-1]
    return ""


def _decoded(text: str) -> str:
    """Undo the SQL string escaping so byte budgets are measured on real bytes."""
    out: list[str] = []
    index = 0
    while index < len(text):
        char = text[index]
        if char == "\\" and index + 1 < len(text):
            nxt = text[index + 1]
            out.append({"0": "\0", "n": "\n", "r": "\r", "t": "\t", "Z": "\x1a",
                        "\\": "\\", "'": "'", '"': '"'}.get(nxt, nxt))
            index += 2
            continue
        if text[index:index + 2] == "''":
            out.append("'")
            index += 2
            continue
        out.append(char)
        index += 1
    return "".join(out)


def check_suite(spec: fx.TableSpec, rows: int) -> list[Issue]:
    issues: list[Issue] = []
    key_names = set(spec.primary_key)

    # -- primary key uniqueness -------------------------------------------
    if spec.comparable:
        seen: dict[tuple, int] = {}
        for row in range(rows):
            values = tuple(spec.column(name).literal(row) for name in spec.primary_key)
            if values in seen:
                issues.append(Issue(spec.name, ",".join(spec.primary_key), row,
                                    f"duplicate primary key, first seen at row {seen[values]}"))
            seen.setdefault(values, row)

    # -- per-column representability --------------------------------------
    for column in spec.columns:
        if column.virtual_generated:
            continue
        char_capacity = CHAR_CAPACITY.match(column.ddl_type)
        decimal_capacity = DECIMAL_CAPACITY.match(column.ddl_type)
        low_high = _numeric_bounds(column)

        for row in range(rows):
            literal = column.literal(row)
            if literal == "NULL":
                if not column.nullable:
                    issues.append(Issue(spec.name, column.name, row,
                                        "NULL in a NOT NULL column"))
                continue

            if char_capacity and literal.startswith("'"):
                capacity = int(char_capacity.group(2))
                payload = _decoded(_unescaped_body(literal))
                # Enforce the declared capacity as an encoded byte budget: the MySQL
                # client negotiates a single-byte charset by default, and the
                # server then counts bytes, not characters.
                width = len(payload.encode("utf-8"))
                if width > capacity:
                    issues.append(Issue(spec.name, column.name, row,
                                        f"{width} bytes exceeds declared {capacity}"))

            lob_capacity = LOB_CAPACITY.get(column.ddl_type.upper())
            if lob_capacity is not None:
                if literal.startswith("'") and not column.binary:
                    payload = _decoded(_unescaped_body(literal))
                    width = len(payload.encode("utf-8"))
                    if width > lob_capacity:
                        issues.append(Issue(
                            spec.name, column.name, row,
                            f"{width} bytes exceeds {column.ddl_type} capacity {lob_capacity}"))
                elif literal.upper().startswith("X'"):
                    payload = literal[2:-1]
                    if len(payload) // 2 > lob_capacity:
                        issues.append(Issue(
                            spec.name, column.name, row,
                            f"{len(payload) // 2} bytes exceeds {column.ddl_type} "
                            f"capacity {lob_capacity}"))

            if decimal_capacity:
                precision, scale = (int(decimal_capacity.group(1)),
                                    int(decimal_capacity.group(2)))
                try:
                    value = Decimal(literal.strip("'"))
                except InvalidOperation:
                    issues.append(Issue(spec.name, column.name, row,
                                        f"not a decimal literal: {literal!r}"))
                    continue
                digits = value.as_tuple()
                integer_digits = max(0, len(digits.digits) + digits.exponent)
                if integer_digits > precision - scale:
                    issues.append(Issue(
                        spec.name, column.name, row,
                        f"DECIMAL({precision},{scale}) overflow: {integer_digits} "
                        f"integer digits allowed {precision - scale}"))

            if low_high and literal.lstrip("-").isdigit():
                low, high = low_high
                value = int(literal)
                if not low <= value <= high:
                    issues.append(Issue(spec.name, column.name, row,
                                        f"value {value} outside [{low}, {high}]"))

            if column.parser_type in (fx.PARSER_ENUM, fx.PARSER_SET) and literal.startswith("'"):
                payload = _decoded(_unescaped_body(literal))
                members = payload.split(",") if payload else []
                if not set(members) <= set(column.elements):
                    issues.append(Issue(spec.name, column.name, row,
                                        f"members {members} not in {column.elements}"))

    # -- composite key members must be NOT NULL ---------------------------
    for column in spec.columns:
        if column.name in key_names and column.nullable:
            issues.append(Issue(spec.name, column.name, 0,
                                "primary-key column declared nullable"))
    return issues


def _numeric_bounds(column: fx.Column) -> tuple[int, int] | None:
    widths = {fx.PARSER_TINY: 8, fx.PARSER_SHORT: 16, fx.PARSER_INT24: 24,
              fx.PARSER_LONG: 32, fx.PARSER_LONGLONG: 64}
    if column.parser_type not in widths:
        return None
    bits = widths[column.parser_type]
    if column.unsigned:
        return 0, (1 << bits) - 1
    return -(1 << (bits - 1)), (1 << (bits - 1)) - 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Static fixture validation")
    parser.add_argument("--versions", nargs="+", default=None)
    parser.add_argument("--rows", type=int, default=SAMPLE_ROWS)
    args = parser.parse_args()

    versions = args.versions or list(VERSIONS)
    all_issues: list[Issue] = []
    checked = 0
    for version in versions:
        for name in fx.suite_names():
            supported, _ = fx.suite_supports(name, version)
            if not supported:
                continue
            spec = fx.build_suite(name, version)
            issues = check_suite(spec, args.rows)
            checked += 1
            for issue in issues:
                print(f"{version} {issue}")
            all_issues.extend(issues)
    print(f"checked {checked} suite/version combinations, {len(all_issues)} issues")
    return 1 if all_issues else 0


if __name__ == "__main__":
    raise SystemExit(main())
