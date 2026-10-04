"""Canonical literal builders shared by the Phase A and Phase B fixtures.

Both harnesses must generate byte-identical value syntax: a literal that one
path accepts and the other rejects silently turns a valid matrix cell into a
SETUP_ERROR instead of a recovery measurement.
"""

from __future__ import annotations

import re

_HEX_LITERAL = re.compile(r"^X'([0-9a-f]+)'$")


def hex_literal(value: int) -> str:
    """Render an InnoDB binary literal with an even number of hex digits.

    MySQL rejects `X'...'` with an odd digit count (ERROR 1064). The previous
    `f"X'{value:04x}'"` crossed into five digits exactly at id 65536, so every
    100k/1m profile aborted at row 65537 with a syntax error. Padding to the
    next even width keeps the value intact for any id range and never emits an
    odd-length literal.
    """
    if value < 0:
        raise ValueError(f"hex literal requires a non-negative id: {value}")
    digits = f"{value:x}"
    if len(digits) % 2:
        digits = "0" + digits
    literal = f"X'{digits}'"
    match = _HEX_LITERAL.fullmatch(literal)
    if match is None or len(match.group(1)) % 2 or int(match.group(1), 16) != value:
        raise AssertionError(f"generated an unusable hex literal: {literal}")
    return literal
