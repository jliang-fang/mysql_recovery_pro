#!/usr/bin/env python3
"""Deterministic, capability-aware fixtures shared by every validation phase.

Before this module each phase carried its own inline table builder, so the
column set exercised by Phase A, Phase B and the corruption matrix could drift
apart silently — a type validated in one path was never validated in another.
The fixtures here are the single source of truth for:

  * the MySQL DDL a case creates,
  * the deterministic row payload inserted into it, and
  * the parser-domain schema JSON the recovery binary is given.

Values are derived from a hash of (table, column, row) rather than from
sequential RNG draws, so a table can be generated in any batch size and still
produce byte-identical rows.  Boundary and special values are pinned to fixed
row indices so a failure is always reproducible from the row number alone.
"""

from __future__ import annotations

import base64
import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence

# ---------------------------------------------------------------------------
# InnoDB parser-domain type IDs (include/dbrecovery/innodb/page_defs.hpp)
# ---------------------------------------------------------------------------

PARSER_TINY = 2
PARSER_SHORT = 3
PARSER_LONG = 4
PARSER_FLOAT = 5
PARSER_DOUBLE = 6
PARSER_LONGLONG = 9
PARSER_INT24 = 10
PARSER_YEAR = 14
PARSER_NEWDATE = 15
PARSER_VARCHAR = 16
PARSER_BIT = 17
PARSER_TIMESTAMP2 = 18
PARSER_DATETIME2 = 19
PARSER_TIME2 = 20
PARSER_NEWDECIMAL = 21
PARSER_ENUM = 22
PARSER_SET = 23
PARSER_TINY_BLOB = 24
PARSER_MEDIUM_BLOB = 25
PARSER_LONG_BLOB = 26
PARSER_BLOB = 27
PARSER_STRING = 29
PARSER_GEOMETRY = 30
PARSER_JSON = 31

# Character-set / collation IDs as stored in the InnoDB data dictionary.
# 63 is the binary pseudo-charset shared by BINARY/VARBINARY/BLOB.
COLLATION_BINARY = 63
COLLATION_UTF8MB4 = 255
COLLATION_LATIN1 = 8
COLLATION_ASCII = 11

Literal = Callable[[int], str]


# ---------------------------------------------------------------------------
# deterministic helpers
# ---------------------------------------------------------------------------

def _entropy(table: str, column: str, row: int) -> int:
    """Stable 64-bit value for one cell, independent of generation order."""
    digest = hashlib.sha256(f"{table}\x1f{column}\x1f{row}".encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big")


def _pick(table: str, column: str, row: int, options: Sequence[Any]) -> Any:
    return options[_entropy(table, column, row) % len(options)]


def sql_string(text: str) -> str:
    """Render a Python string as a MySQL single-quoted literal.

    Backslash escapes are used (MySQL's default), so backslash itself must be
    escaped first; a raw newline/tab embedded in the literal would otherwise be
    indistinguishable from the statement separator in a captured log.
    """
    escaped = (text
               .replace("\\", "\\\\")
               .replace("'", "''")
               .replace("\0", "\\0")
               .replace("\n", "\\n")
               .replace("\r", "\\r")
               .replace("\t", "\\t")
               .replace("\x1a", "\\Z"))
    return "'" + escaped + "'"


def sql_bytes(payload: bytes) -> str:
    return "X'" + payload.hex() + "'"


# ---------------------------------------------------------------------------
# column specification
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Column:
    """One fixture column, carrying both server DDL and parser-domain metadata."""

    name: str
    ddl_type: str
    parser_type: int
    literal: Literal
    nullable: bool = True
    key: bool = False            # part of PRIMARY KEY
    key_order: int = 0           # position within the primary key, 0-based
    unsigned: bool = False
    length: int = 0
    precision: int = 0
    scale: int = 0
    datetime_precision: int = 0
    charset_id: int = 0
    collation_id: int = 0
    elements: tuple[str, ...] = ()
    binary: bool = False
    virtual_generated: bool = False

    @property
    def is_text(self) -> bool:
        return self.parser_type in (PARSER_VARCHAR, PARSER_STRING, PARSER_BLOB,
                                    PARSER_TINY_BLOB, PARSER_MEDIUM_BLOB, PARSER_LONG_BLOB)

    @property
    def charset_name(self) -> str:
        """SQL character-set name for this column, or "" when not applicable."""
        return {COLLATION_UTF8MB4: "utf8mb4", COLLATION_LATIN1: "latin1",
                COLLATION_ASCII: "ascii"}.get(self.charset_id, "")

    def ddl(self) -> str:
        column = f"`{self.name}` {self.ddl_type}"
        # The charset is stated explicitly rather than inherited: a MySQL 5.6/5.7
        # server defaults to latin1, and a utf8mb4 value inserted into an
        # inherited-latin1 column is rejected with ERROR 1366 well before
        # recovery ever sees the table.
        if self.charset_name and not self.binary:
            column += f" CHARACTER SET {self.charset_name}"
        if self.unsigned:
            column += " UNSIGNED"
        if not self.nullable:
            column += " NOT NULL"
        return column

    def char_length(self) -> int:
        """Physical byte length used by the parser-domain schema.

        ``column_length`` is not a generic fallback width: for fixed numeric and
        temporal fields it must match the on-page byte width.  Emitting 255 for
        an INT/UNSIGNED/DATE field shifts every following field and can make a
        raw-carving result look structured while every later value is wrong.
        """
        fixed_lengths = {
            PARSER_TINY: 1, PARSER_SHORT: 2, PARSER_INT24: 3,
            PARSER_LONG: 4, PARSER_FLOAT: 4, PARSER_DOUBLE: 8,
            PARSER_LONGLONG: 8, PARSER_YEAR: 1, PARSER_NEWDATE: 3,
            PARSER_TIMESTAMP2: 4, PARSER_DATETIME2: 5, PARSER_TIME2: 3,
        }
        if self.parser_type in fixed_lengths:
            return fixed_lengths[self.parser_type]
        if self.parser_type == PARSER_NEWDECIMAL:
            integer_digits = max(0, self.precision - self.scale)
            fractional_digits = max(0, self.scale)
            def decimal_group_bytes(digits: int) -> int:
                remainder = digits % 9
                return (digits // 9) * 4 + (0 if remainder == 0 else
                        1 if remainder <= 2 else 2 if remainder <= 4 else
                        3 if remainder <= 6 else 4)
            return decimal_group_bytes(integer_digits) + decimal_group_bytes(fractional_digits)
        if self.parser_type == PARSER_BIT:
            # IndexColumnDef::fixed_size receives bit count, not bytes.
            return self.length
        if self.length:
            return self.length
        if self.parser_type == PARSER_TINY_BLOB:
            return 255
        if self.parser_type == PARSER_BLOB:
            return 65535
        if self.parser_type == PARSER_MEDIUM_BLOB:
            return 16777215
        if self.parser_type in (PARSER_LONG_BLOB, PARSER_JSON):
            return 4294967295
        return 255


# Rows 0..PINNED_ROWS are reserved for boundary probes (empty, maximum, zero,
# negative, unicode, special characters).  NULL coverage starts after them, so a
# pinned probe is never silently replaced by NULL.
PINNED_ROWS = 6


def _null_every(inner: Literal, every: int, nullable: bool = True) -> Literal:
    """Return NULL on a periodic row so NULL semantics are always covered.

    Numbering starts past the pinned boundary rows: ``row % every`` would put
    NULL on row 0 and hide the empty/maximum probes that make a failure
    reproducible from a row number alone.  A NOT NULL column — every primary-key
    member, for instance — is returned untouched.
    """
    if not nullable:
        return inner

    def literal(row: int) -> str:
        if row < PINNED_ROWS or every <= 1:
            return inner(row)
        return "NULL" if (row - PINNED_ROWS) % every == 0 else inner(row)
    return literal


# ---------------------------------------------------------------------------
# numeric columns
# ---------------------------------------------------------------------------

def _int_literal(table: str, column: str, low: int, high: int,
                 pinned: Sequence[int] = ()) -> Literal:
    span = high - low
    def literal(row: int) -> str:
        if row < len(pinned):
            return str(pinned[row])
        return str(low + _entropy(table, column, row) % (span + 1))
    return literal


def int_column(name: str, bit_width: int, *, unsigned: bool = False,
               key: bool = False, key_order: int = 0, nullable: bool = True,
               table: str = "") -> Column:
    signed_bounds = {8: (-128, 127), 16: (-32768, 32767), 24: (-8388608, 8388607),
                     32: (-2147483648, 2147483647),
                     64: (-9223372036854775808, 9223372036854775807)}
    unsigned_bounds = {8: (0, 255), 16: (0, 65535), 24: (0, 16777215),
                       32: (0, 4294967295), 64: (0, 18446744073709551615)}
    ddl = {8: "TINYINT", 16: "SMALLINT", 24: "MEDIUMINT", 32: "INT", 64: "BIGINT"}[bit_width]
    parser = {8: PARSER_TINY, 16: PARSER_SHORT, 24: PARSER_INT24,
              32: PARSER_LONG, 64: PARSER_LONGLONG}[bit_width]
    low, high = (unsigned_bounds if unsigned else signed_bounds)[bit_width]
    # Rows 0..3 pin the extremes, zero and one so boundary decoding is
    # deterministic.  The values must stay distinct: these columns carry UNIQUE
    # indexes, and a repeated pinned value would be rejected by the server long
    # before recovery ever sees it.
    pinned = [0, 1, 2, high] if unsigned else [0, low, high, -1]
    return Column(
        name=name, ddl_type=ddl, parser_type=parser,
        literal=_int_literal(table or name, name, low, high, pinned),
        nullable=nullable, key=key, key_order=key_order, unsigned=unsigned,
    )


def decimal_column(name: str, precision: int, scale: int, *, table: str = "",
                   nullable: bool = True, key: bool = False,
                   key_order: int = 0) -> Column:
    """DECIMAL(p,s) column.

    Every literal is rendered with an explicit decimal point.  A bare
    ``9``*``precision`` integer literal is *not* the maximum of DECIMAL(p,s) —
    it is an integer with ``precision`` integer digits, which overflows the
    ``precision - scale`` integer digits a scaled DECIMAL actually allows.
    """
    integer_digits = max(1, precision - scale)
    max_integer = int("9" * integer_digits)
    max_fraction = int("9" * scale) if scale else 0
    peak = f"{max_integer}." + (f"{max_fraction:0{scale}d}" if scale else "")
    peak = peak.rstrip(".") if not scale else peak
    pinned = [
        "0",
        peak,
        "-" + peak,
        "0." + (f"1{'0' * (scale - 1)}" if scale else ""),
    ]
    if not scale:
        pinned = ["0", str(max_integer), str(-max_integer), "1"]

    def literal(row: int) -> str:
        if row < len(pinned):
            return pinned[row]
        magnitude = _entropy(table or name, name, row) % (max_integer + 1)
        sign = "-" if (_entropy(table or name, name, row) >> 63) & 1 else ""
        if scale:
            fraction = _entropy(table or name, name, row) // 7 % (10 ** scale)
            return f"{sign}{magnitude}.{fraction:0{scale}d}"
        return f"{sign}{magnitude}"

    return Column(name=name, ddl_type=f"DECIMAL({precision},{scale})",
                  parser_type=PARSER_NEWDECIMAL, literal=literal, nullable=nullable,
                  key=key, key_order=key_order, precision=precision, scale=scale)


def float_column(name: str, double: bool, *, table: str = "", nullable: bool = True) -> Column:
    pinned = ["0", "-0", "3.4028234663852886E38" if not double else "1.7976931348623157E308",
              "1.5"]
    def literal(row: int) -> str:
        if row < len(pinned):
            return pinned[row]
        value = _entropy(table or name, name, row) % 10_000_000
        return repr(value / 1024.0)
    return Column(name=name, ddl_type="DOUBLE" if double else "FLOAT",
                  parser_type=PARSER_DOUBLE if double else PARSER_FLOAT,
                  literal=literal, nullable=nullable)


def bit_column(name: str, width: int, *, table: str = "", nullable: bool = True) -> Column:
    def literal(row: int) -> str:
        if row == 0:
            value = 0
        elif row == 1:
            value = (1 << width) - 1
        else:
            value = _entropy(table or name, name, row) % (1 << width)
        return f"b'{value:0{width}b}'"
    # IndexColumnDef::fixed_size expects the number of *bits* for type 17 and
    # computes (col_length + 7) / 8 internally.  Passing a byte count makes
    # BIT(64) look like one byte, shifts YEAR and every following field, and
    # produces convincing but wrong rows during raw recovery.
    return Column(name=name, ddl_type=f"BIT({width})", parser_type=PARSER_BIT,
                  literal=literal, nullable=nullable, length=width)


def year_column(name: str, *, table: str = "", nullable: bool = True) -> Column:
    pinned = ["1901", "2155", "2026", "0"]
    def literal(row: int) -> str:
        if row < len(pinned):
            return pinned[row]
        return str(1901 + _entropy(table or name, name, row) % 154)
    return Column(name=name, ddl_type="YEAR", parser_type=PARSER_YEAR,
                  literal=literal, nullable=nullable, length=1)


# ---------------------------------------------------------------------------
# temporal columns
# ---------------------------------------------------------------------------

_DATE_PINNED = ("'1000-01-01'", "'9999-12-31'", "'2024-02-29'", "'2026-01-31'")
_DATETIME_PINNED = ("'1970-01-01 00:00:01'", "'2024-02-29 23:59:59'",
                    "'2026-12-31 12:00:00'", "'2038-01-19 03:14:07'")
_TIME_PINNED = ("'-838:59:59'", "'838:59:59'", "'00:00:00'", "'-00:00:01'")
# MySQL permits fractional seconds on TIME(1..6) only within +/-838:59:59 exactly;
# appending a fraction to the extreme template yields ERROR 1292, so the
# fractional column pins the widest representable fractional values instead.
_TIME_PINNED_FSP = ("'-838:59:58.999999'", "'838:59:58.999999'",
                    "'00:00:00.000001'", "'-00:00:00.000001'")


def date_column(name: str, *, table: str = "", nullable: bool = True) -> Column:
    def literal(row: int) -> str:
        if row < len(_DATE_PINNED):
            return _DATE_PINNED[row]
        year = 1970 + _entropy(table or name, name, row) % 56
        month = 1 + _entropy(table or name, name, row) // 13 % 12
        day = 1 + _entropy(table or name, name, row) // 29 % 28
        return f"'{year:04d}-{month:02d}-{day:02d}'"
    return Column(name=name, ddl_type="DATE", parser_type=PARSER_NEWDATE,
                  literal=literal, nullable=nullable, length=3)


def datetime_column(name: str, fsp: int, temporal: str, *,
                    table: str = "", nullable: bool = True) -> Column:
    parser = {"DATETIME": PARSER_DATETIME2, "TIMESTAMP": PARSER_TIMESTAMP2,
              "TIME": PARSER_TIME2}[temporal]
    if temporal == "TIME" and fsp:
        pinned = _TIME_PINNED_FSP
    else:
        pinned = {"DATETIME": _DATETIME_PINNED, "TIMESTAMP": _DATETIME_PINNED,
                  "TIME": _TIME_PINNED}[temporal]

    def literal(row: int) -> str:
        # Boundary pairs are fixed templates; the fractional-second suffix is
        # only appended to the fractional column. Appending it to the fsp=0
        # sibling produces an invalid literal such as '-838:59:59.891036'.
        boundary = row < len(pinned)
        if boundary:
            base = pinned[row]
        else:
            year = 1970 + _entropy(table or name, name, row) % 56
            month = 1 + _entropy(table or name, name, row) // 13 % 12
            day = 1 + _entropy(table or name, name, row) // 29 % 28
            hour = _entropy(table or name, name, row) // 17 % 24
            minute = _entropy(table or name, name, row) // 19 % 60
            second = _entropy(table or name, name, row) // 23 % 60
            if temporal == "TIME":
                base = f"'{hour:02d}:{minute:02d}:{second:02d}'"
            else:
                base = f"'{year:04d}-{month:02d}-{day:02d} {hour:02d}:{minute:02d}:{second:02d}'"
        if fsp and not (boundary and "." in base):
            micros = 999999 if row == 1 else _entropy(table or name, name, row) % 1000000
            return base[:-1] + f".{micros:0{fsp}d}'"
        return base

    ddl = temporal if not fsp else f"{temporal}({fsp})"
    return Column(name=name, ddl_type=ddl, parser_type=parser, literal=literal,
                  nullable=nullable, datetime_precision=fsp,
                  length={PARSER_DATETIME2: 5, PARSER_TIMESTAMP2: 4, PARSER_TIME2: 3}[parser])


# ---------------------------------------------------------------------------
# string and binary columns
# ---------------------------------------------------------------------------

_ASCII = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
_CJK = "数据库恢复验证测试日本語のテスト한국어테스트"
_EMOJI = "🙂🚀🔥🧪🌏"
# Quotes, backslashes, newlines and tabs must survive both CSV and SQL export.
_SPECIAL = "\"'\\\n\t,;--/*%_\\x00".replace("\\x00", "\0")


def _unicode_sample(table: str, column: str, row: int, length: int, charset: str) -> str:
    """Deterministic multi-script string that fits the column's byte budget.

    ``length`` is the column's declared capacity.  It is enforced as an **encoded
    byte** budget rather than a character budget: a client that negotiates a
    single-byte connection charset (latin1 is the MySQL client default) reports
    the byte length to the server, so a string of ``length`` emoji would be
    stored as up to ``4 * length`` characters and rejected with ERROR 1406.

    The generic branch embeds the row number, so two rows can never collide in a
    UNIQUE or primary-key text column — an entropy-only sample collides in
    practice, and SQL's default collation ignores trailing spaces, which shrinks
    the effective keyspace further.
    """
    if charset == "ascii":
        pool = _ASCII
    elif charset == "latin1":
        pool = _ASCII + "äöüßÄÖÜéàçñøåæ"
    elif row == 2:
        pool = _SPECIAL * 4
    elif row == 3:
        pool = _CJK * 3
    elif row == 4:
        pool = _EMOJI * 3 + _CJK
    elif row == 5:
        pool = "ȩ́" * 8              # combining characters
    else:
        alphabet = _ASCII + _CJK + _EMOJI
        size = 1 + _entropy(table, column, row) % max(1, min(length, 48))
        body = "".join(alphabet[(_entropy(table, column, row) >> (index % 61)) % len(alphabet)]
                       for index in range(size))
        # Prefix the row number so identity is guaranteed, not merely likely.
        pool = f"r{row}_{body}"
    return _truncate_bytes(pool, length, charset)


def _truncate_bytes(text: str, budget: int, charset: str) -> str:
    """Longest prefix of ``text`` whose encoded size stays within ``budget``."""
    encoding = "ascii" if charset == "ascii" else ("latin-1" if charset == "latin1" else "utf-8")
    out: list[str] = []
    used = 0
    for char in text:
        try:
            width = len(char.encode(encoding))
        except UnicodeEncodeError:
            continue                      # not representable in this charset
        if used + width > budget:
            break
        out.append(char)
        used += width
    return "".join(out)


def _char_literal(table: str, column: str, length: int, charset: str) -> Literal:
    def literal(row: int) -> str:
        if row == 0:
            # Probe the declared maximum width using ASCII only, so the pad can
            # never push the value past the column's byte capacity under a
            # single-byte client charset.
            sample = _ascii_pad(length)
        else:
            sample = _unicode_sample(table, column, row, length, charset)
        return sql_string(sample)
    return literal


def _ascii_pad(length: int) -> str:
    """A deterministic ASCII string of exactly ``length`` characters."""
    unit = "_pad0123456789"
    repeats = (length // len(unit)) + 1
    return (unit * repeats)[:length]


def char_column(name: str, length: int, *, table: str = "", nullable: bool = True,
                charset: str = "utf8mb4", fixed: bool = False,
                key: bool = False, key_order: int = 0) -> Column:
    collation = {"utf8mb4": COLLATION_UTF8MB4, "latin1": COLLATION_LATIN1,
                 "ascii": COLLATION_ASCII}[charset]
    ddl = f"{'CHAR' if fixed else 'VARCHAR'}({length})"
    return Column(
        name=name, ddl_type=ddl,
        parser_type=PARSER_STRING if fixed else PARSER_VARCHAR,
        literal=_null_every(_char_literal(table or name, name, length, charset), 7, nullable),
        nullable=nullable, length=length, key=key, key_order=key_order,
        charset_id=collation, collation_id=collation,
    )


def binary_column(name: str, length: int, *, table: str = "", nullable: bool = True,
                  fixed: bool = False) -> Column:
    def literal(row: int) -> str:
        if row == 0:
            payload = b""
        elif row == 1:
            payload = bytes(range(256))[:length] if length >= 256 else b"\xff" * length
        else:
            size = _entropy(table or name, name, row) % (length + 1)
            payload = bytes((_entropy(table or name, name, row) >> (index % 48)) & 0xFF
                            for index in range(size))
        if fixed and len(payload) < length:
            payload = payload + b"\0" * (length - len(payload))
        return sql_bytes(payload)
    return Column(name=name, ddl_type=f"{'BINARY' if fixed else 'VARBINARY'}({length})",
                  parser_type=PARSER_STRING if fixed else PARSER_VARCHAR,
                  literal=_null_every(literal, 7, nullable), nullable=nullable, length=length,
                  charset_id=COLLATION_BINARY, collation_id=COLLATION_BINARY, binary=True)


_BLOB_PARSER = {"TINYTEXT": PARSER_TINY_BLOB, "TEXT": PARSER_BLOB,
                "MEDIUMTEXT": PARSER_MEDIUM_BLOB, "LONGTEXT": PARSER_LONG_BLOB}
_BLOB_BYTES = {"TINYTEXT": 255, "TEXT": 65535, "MEDIUMTEXT": 16777215, "LONGTEXT": 4294967295}


def text_column(name: str, family: str, *, table: str = "", nullable: bool = True,
                charset: str = "utf8mb4") -> Column:
    """TEXT FAMILY (character) or BLOB family (binary) column.

    TEXT and BLOB share an identical InnoDB record encoding; only the charset
    (63 = binary) distinguishes them, which is exactly what the decoder keys on.
    """
    is_blob = family.endswith("BLOB")
    parser = ({f"TINY{'BLOB'}": PARSER_TINY_BLOB, "BLOB": PARSER_BLOB,
               "MEDIUMBLOB": PARSER_MEDIUM_BLOB, "LONGBLOB": PARSER_LONG_BLOB}[family]
              if is_blob else _BLOB_PARSER[family])
    collation = COLLATION_BINARY if is_blob else {
        "utf8mb4": COLLATION_UTF8MB4, "latin1": COLLATION_LATIN1,
        "ascii": COLLATION_ASCII}[charset]
    cap = _BLOB_BYTES[family.replace("BLOB", "TEXT")] if is_blob else _BLOB_BYTES[family]

    def literal(row: int) -> str:
        # Row 1 is deliberately larger than a page-friendly inline payload so the
        # overflow / external-storage path is always exercised.  Every branch is
        # clamped to the family's byte capacity: TINYTEXT/TINYBLOB hold 255 bytes,
        # and a uniform 512-byte sample would overflow them on most rows.
        if row == 1:
            size = min(cap, 4096 if is_blob else 3000)
        elif row == 0:
            size = 0
        else:
            size = min(cap, _entropy(table or name, name, row) % 512)
        if is_blob:
            return sql_bytes(bytes((_entropy(table or name, name, row) >> (i % 48)) & 0xFF
                                   for i in range(size)))
        # TEXT capacity is a byte count too: a repeated multi-byte sample would
        # overflow TINYTEXT well before the character budget is reached.
        sample = _unicode_sample(table, name, row, 64, charset)
        repeats = (size // max(1, len(sample))) + 1
        return sql_string(_truncate_bytes(sample * repeats, cap, charset))

    ddl = family if not is_blob else family
    return Column(name=name, ddl_type=ddl, parser_type=parser,
                  literal=_null_every(literal, 11, nullable), nullable=nullable,
                  length=0, charset_id=collation, collation_id=collation,
                  binary=is_blob)


def enum_column(name: str, members: Sequence[str], *, table: str = "",
                nullable: bool = True) -> Column:
    rendered = ",".join(sql_string(member) for member in members)
    def literal(row: int) -> str:
        return sql_string(members[_entropy(table or name, name, row) % len(members)])
    return Column(name=name, ddl_type=f"ENUM({rendered})", parser_type=PARSER_ENUM,
                  literal=_null_every(literal, 13, nullable), nullable=nullable, length=1,
                  charset_id=COLLATION_UTF8MB4, collation_id=COLLATION_UTF8MB4,
                  elements=tuple(members))


def set_column(name: str, members: Sequence[str], *, table: str = "",
               nullable: bool = True) -> Column:
    rendered = ",".join(sql_string(member) for member in members)
    def literal(row: int) -> str:
        if row == 0:
            return sql_string("")                       # empty set is legal
        if row == 1:
            return sql_string(",".join(members))        # the full set
        mask = _entropy(table or name, name, row) % (1 << len(members))
        chosen = [member for index, member in enumerate(members) if mask >> index & 1]
        return sql_string(",".join(chosen))
    return Column(name=name, ddl_type=f"SET({rendered})", parser_type=PARSER_SET,
                  literal=_null_every(literal, 13, nullable), nullable=nullable, length=8,
                  charset_id=COLLATION_UTF8MB4, collation_id=COLLATION_UTF8MB4,
                  elements=tuple(members))


def json_column(name: str, *, table: str = "", nullable: bool = True) -> Column:
    def literal(row: int) -> str:
        if row == 0:
            document: Any = {}
        elif row == 1:
            document = {"unicode": _CJK + _EMOJI, "nested": {"list": [1, 2, {"deep": None}]},
                        "escapes": "quote\" backslash\\ newline\n tab\t"}
        elif row == 2:
            document = []
        elif row == 3:
            document = [{"i": index, "f": index / 3.0} for index in range(5)]
        else:
            document = {"k": _entropy(table or name, name, row) % 100000,
                        "s": _unicode_sample(table or name, name, row, 24, "utf8mb4")}
        return sql_string(json.dumps(document, ensure_ascii=False,
                                     separators=(",", ":")))
    return Column(name=name, ddl_type="JSON", parser_type=PARSER_JSON,
                  literal=_null_every(literal, 17, nullable), nullable=nullable,
                  charset_id=COLLATION_BINARY, collation_id=COLLATION_BINARY)


def geometry_column(name: str, *, table: str = "", nullable: bool = True) -> Column:
    """GEOMETRY stored as WKB; only expressible through the JSON schema file."""
    def literal(row: int) -> str:
        base = _entropy(table or name, name, row)
        if row == 0:
            return "ST_GeomFromText('POINT(0 0)')"
        if row == 1:
            return "ST_GeomFromText('LINESTRING(0 0,1 1,2 2)')"
        if row == 2:
            return "ST_GeomFromText('POLYGON((0 0,4 0,4 4,0 4,0 0))')"
        x, y = base % 10000, base // 10000 % 10000
        return f"ST_GeomFromText('POINT({x} {y})')"
    return Column(name=name, ddl_type="GEOMETRY", parser_type=PARSER_GEOMETRY,
                  literal=_null_every(literal, 19, nullable), nullable=nullable,
                  charset_id=COLLATION_BINARY, collation_id=COLLATION_BINARY, binary=True)


# ---------------------------------------------------------------------------
# table specification
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class TableSpec:
    name: str
    columns: tuple[Column, ...]
    secondary_indexes: tuple[tuple[str, ...], ...] = ()
    unique_indexes: tuple[tuple[str, ...], ...] = ()
    prefix_indexes: tuple[tuple[str, int], ...] = ()
    row_format: str = ""
    min_version: str = "5.6"
    requires: tuple[str, ...] = ()
    # A table with no user-visible unique key cannot be compared row by row;
    # it is carried so the hidden row-id path is exercised, and scored as
    # evidence-only rather than being dropped from the matrix.
    comparable: bool = True

    @property
    def primary_key(self) -> tuple[str, ...]:
        return tuple(column.name for column in sorted(
            (c for c in self.columns if c.key), key=lambda c: c.key_order))

    def column(self, name: str) -> Column:
        for entry in self.columns:
            if entry.name == name:
                return entry
        raise KeyError(name)

    def ddl(self, database: str, *, row_format: str = "", include_indexes: bool = True) -> str:
        parts = [column.ddl() for column in self.columns
                 if not column.virtual_generated]
        parts.extend(f"`{column.name}` {column.ddl_type} GENERATED ALWAYS AS "
                     f"(NULL) VIRTUAL" for column in self.columns if column.virtual_generated)
        key = self.primary_key
        if key:
            parts.append("PRIMARY KEY (" + ", ".join(f"`{name}`" for name in key) + ")")
        if include_indexes:
            for index, columns in enumerate(self.secondary_indexes):
                parts.append(f"KEY `idx_{self.name}_{index}` ("
                             + ", ".join(f"`{name}`" for name in columns) + ")")
            for index, columns in enumerate(self.unique_indexes):
                parts.append(f"UNIQUE KEY `uniq_{self.name}_{index}` ("
                             + ", ".join(f"`{name}`" for name in columns) + ")")
            for index, (name, width) in enumerate(self.prefix_indexes):
                parts.append(f"KEY `pfx_{self.name}_{index}` (`{name}`({width}))")
        # The caller's row format wins; otherwise the spec's own, so a matrix
        # cell that only sets `TableSpec.row_format` still creates the format
        # it claims to test rather than the server default.
        effective = row_format or self.row_format
        suffix = f" ROW_FORMAT={effective}" if effective else ""
        return (f"CREATE TABLE `{database}`.`{self.name}` ("
                + ", ".join(parts) + f") ENGINE=InnoDB{suffix};")

    def insert_sql(self, database: str, first: int, last: int) -> str:
        columns = [column for column in self.columns if not column.virtual_generated]
        rendered = []
        for row in range(first, last + 1):
            rendered.append("(" + ", ".join(column.literal(row) for column in columns) + ")")
        names = ", ".join(f"`{column.name}`" for column in columns)
        return (f"INSERT INTO `{database}`.`{self.name}` ({names}) VALUES "
                + ", ".join(rendered) + ";")

    def schema_json(self) -> dict[str, Any]:
        """Parser-domain schema document for `--schema-file`.

        physical_pos follows InnoDB's actual clustered-index layout: the key
        columns first (in key order), then every remaining column in table
        declaration order.  Declaring key columns first keeps this identical to
        declaration order, but computing it explicitly means a table whose key
        is declared mid-way still lands in the right physical order.
        """
        key = self.primary_key
        ordered = list(key) + [column.name for column in self.columns
                               if column.name not in key and not column.virtual_generated]
        position = {name: index for index, name in enumerate(ordered)}
        columns = []
        for column in self.columns:
            if column.virtual_generated:
                continue
            entry: dict[str, Any] = {
                "name": column.name,
                "type": column.parser_type,
                "is_nullable": bool(column.nullable) and not column.key,
                # Schema consumers historically understand is_unsigned; use that
                # canonical spelling instead of is_signed so unsigned integer
                # columns cannot silently be decoded as signed in a raw-carving
                # recovery path.
                "is_unsigned": column.unsigned,
                "char_length": column.char_length(),
                "physical_pos": position[column.name],
                "ordinal_position": position[column.name],
            }
            if column.charset_id:
                entry["charset_id"] = column.charset_id
            if column.collation_id:
                entry["collation_id"] = column.collation_id
            if column.parser_type == PARSER_NEWDECIMAL:
                entry["numeric_precision"] = column.precision
                entry["numeric_scale"] = column.scale
            if column.parser_type in (PARSER_DATETIME2, PARSER_TIMESTAMP2, PARSER_TIME2):
                entry["datetime_precision"] = column.datetime_precision
            if column.elements:
                entry["elements"] = [
                    {"name": base64.b64encode(member.encode("utf-8")).decode("ascii")}
                    for member in column.elements
                ]
            columns.append(entry)
        document: dict[str, Any] = {
            "table_name": self.name,
            "columns": columns,
        }
        if key:
            document["indexes"] = [{
                "name": "PRIMARY",
                "is_primary": True,
                "is_unique": True,
                "columns": list(key),
            }]
        return document


def write_schema_file(spec: TableSpec, path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(f"refusing to overwrite schema evidence: {path}")
    path.write_text(json.dumps(spec.schema_json(), indent=1, ensure_ascii=False) + "\n",
                    encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# table suites
# ---------------------------------------------------------------------------

def _numeric_table(version: str) -> TableSpec:
    table = "t_numeric"
    columns: list[Column] = [
        int_column("id", 64, key=True, key_order=0, nullable=False, table=table),
        int_column("c_tiny", 8, table=table),
        int_column("c_tiny_u", 8, unsigned=True, table=table),
        int_column("c_small", 16, table=table),
        int_column("c_small_u", 16, unsigned=True, table=table),
        int_column("c_medium", 24, table=table),
        int_column("c_medium_u", 24, unsigned=True, table=table),
        int_column("c_int", 32, table=table),
        int_column("c_int_u", 32, unsigned=True, table=table),
        int_column("c_big", 64, table=table),
        int_column("c_big_u", 64, unsigned=True, table=table),
        float_column("c_float", False, table=table),
        float_column("c_double", True, table=table),
        decimal_column("c_dec_max", 65, 30, table=table),
        decimal_column("c_dec_zero_scale", 18, 0, table=table),
        decimal_column("c_dec_small", 5, 2, table=table),
        bit_column("c_bit1", 1, table=table),
        bit_column("c_bit8", 8, table=table),
        bit_column("c_bit64", 64, table=table),
        int_column("c_bool", 8, table=table),
        year_column("c_year", table=table),
        char_column("c_varchar", 191, table=table),
        char_column("c_varchar_max", 4000, table=table),
    ]
    return TableSpec(name=table, columns=tuple(columns),
                     secondary_indexes=(("c_int",),),
                     unique_indexes=(("c_big_u",),),
                     prefix_indexes=(("c_varchar", 32),))


def _temporal_table(version: str) -> TableSpec:
    table = "t_temporal"
    columns = [
        int_column("id", 64, key=True, key_order=0, nullable=False, table=table),
        date_column("c_date", table=table),
        datetime_column("c_time", 0, "TIME", table=table),
        datetime_column("c_time6", 6, "TIME", table=table),
        datetime_column("c_datetime", 0, "DATETIME", table=table),
        datetime_column("c_datetime6", 6, "DATETIME", table=table),
        datetime_column("c_timestamp", 0, "TIMESTAMP", table=table),
        datetime_column("c_timestamp6", 6, "TIMESTAMP", table=table),
        char_column("c_zone", 64, table=table),
    ]
    return TableSpec(name=table, columns=tuple(columns),
                     secondary_indexes=(("c_datetime",),))


def _text_table(version: str) -> TableSpec:
    table = "t_text"
    columns = [
        int_column("id", 64, key=True, key_order=0, nullable=False, table=table),
        char_column("c_char", 16, fixed=True, table=table),
        char_column("c_varchar", 255, table=table),
        char_column("c_varchar_latin1", 128, charset="latin1", table=table),
        char_column("c_varchar_ascii", 128, charset="ascii", table=table),
        binary_column("c_binary", 16, fixed=True, table=table),
        binary_column("c_varbinary", 255, table=table),
        binary_column("c_varbinary_big", 1024, table=table),
        text_column("c_tinytext", "TINYTEXT", table=table),
        text_column("c_text", "TEXT", table=table),
        text_column("c_mediumtext", "MEDIUMTEXT", table=table),
        text_column("c_longtext", "LONGTEXT", table=table),
        text_column("c_tinyblob", "TINYBLOB", table=table),
        text_column("c_blob", "BLOB", table=table),
        text_column("c_mediumblob", "MEDIUMBLOB", table=table),
        text_column("c_longblob", "LONGBLOB", table=table),
    ]
    # Under Antelope (MySQL 5.6 default table format) the maximum secondary-index
    # prefix is 767 bytes; a 255-character utf8mb4 prefix is up to 1020 bytes
    # and fails with ERROR 1709.  Prefixing the index to 191 characters keeps it
    # valid on both Antelope and Barracuda without losing index coverage.
    return TableSpec(name=table, columns=tuple(columns),
                     prefix_indexes=(("c_varchar", 191),))


def _structured_table(version: str) -> TableSpec:
    table = "t_structured"
    columns = [
        int_column("id", 64, key=True, key_order=0, nullable=False, table=table),
        enum_column("c_enum", ("alpha", "beta", "数据库", "emoji🙂"), table=table),
        set_column("c_set", ("read", "write", "执行", "admin"), table=table),
        json_column("c_json", table=table),
    ]
    requires = ["json"]
    return TableSpec(name=table, columns=tuple(columns), requires=tuple(requires),
                     min_version="5.7")


def _composite_table(version: str) -> TableSpec:
    table = "t_composite"
    columns = [
        int_column("id_a", 32, key=True, key_order=0, nullable=False, table=table),
        char_column("id_b", 32, table=table, nullable=False, key=True, key_order=1),
        int_column("c_value", 32, table=table),
        char_column("c_note", 128, table=table, nullable=True),
    ]
    return TableSpec(name=table, columns=tuple(columns),
                     secondary_indexes=(("c_value",),),
                     unique_indexes=(("c_note",),))


def _nokey_table(version: str) -> TableSpec:
    """No primary key: InnoDB falls back to the hidden DB_ROW_ID clustered index."""
    table = "t_nokey"
    columns = [
        int_column("c_value", 32, table=table),
        char_column("c_text", 64, table=table),
    ]
    return TableSpec(name=table, columns=tuple(columns), comparable=False)


def _geometry_table(version: str) -> TableSpec:
    table = "t_geometry"
    columns = [
        int_column("id", 64, key=True, key_order=0, nullable=False, table=table),
        geometry_column("c_point", table=table),
    ]
    return TableSpec(name=table, columns=tuple(columns), min_version="5.6")


#   name                          factory             min_version  requires
_SUITES: tuple[tuple[str, Callable[[str], TableSpec], str, tuple[str, ...]], ...] = (
    ("numeric", _numeric_table, "5.6", ()),
    ("temporal", _temporal_table, "5.6", ()),
    ("text", _text_table, "5.6", ()),
    ("structured", _structured_table, "5.7", ("json",)),
    ("composite", _composite_table, "5.6", ()),
    ("nokey", _nokey_table, "5.6", ()),
    ("geometry", _geometry_table, "5.6", ()),
)

_VERSION_ORDER = {"5.6": 0, "5.7": 1, "8.0": 2, "8.4": 3, "9.0": 4}


def suite_names() -> tuple[str, ...]:
    return tuple(name for name, _, _, _ in _SUITES)


def build_suite(name: str, version: str, *, row_format: str = "") -> TableSpec:
    """Materialize one named table suite for a specific server version."""
    for candidate, factory, minimum, requires in _SUITES:
        if candidate != name:
            continue
        if _VERSION_ORDER.get(version, 99) < _VERSION_ORDER.get(minimum, 99):
            raise KeyError(f"suite {name} requires MySQL {minimum}+, not {version}")
        spec = factory(version)
        unsupported = [token for token in requires
                       if token == "json" and _VERSION_ORDER.get(version, 99) < 1]
        if unsupported:
            raise KeyError(f"suite {name} requires {unsupported} on {version}")
        return TableSpec(**{**spec.__dict__, "row_format": row_format})
    raise KeyError(f"unknown table suite: {name}")


def build_suites(version: str, names: Iterable[str] | None = None,
                 *, row_format: str = "") -> list[TableSpec]:
    selected = list(names) if names is not None else list(suite_names())
    return [build_suite(name, version, row_format=row_format) for name in selected]


def suite_supports(name: str, version: str) -> tuple[bool, str]:
    try:
        build_suite(name, version)
    except KeyError as error:
        return False, str(error).strip("'")
    return True, ""


def row_count_batches(total: int, batch_size: int = 500) -> Iterable[tuple[int, int]]:
    """Yield inclusive (first, last) id ranges covering ``total`` rows."""
    if total <= 0:
        return
    first = 0
    while first < total:
        last = min(first + batch_size, total) - 1
        yield first, last
        first = last + 1
