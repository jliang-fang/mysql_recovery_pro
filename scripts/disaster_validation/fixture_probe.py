#!/usr/bin/env python3
"""Show the generated literal for one suite column/row, with its byte width."""

import argparse
import sys

sys.path.insert(0, "/app/mysql_recovery_pro")

from scripts.disaster_validation import fixtures as fx


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("suite")
    parser.add_argument("column")
    parser.add_argument("--version", default="8.0")
    parser.add_argument("--rows", type=int, default=4)
    args = parser.parse_args()

    spec = fx.build_suite(args.suite, args.version)
    column = spec.column(args.column)
    print(f"ddl_type={column.ddl_type} parser_type={column.parser_type} "
          f"binary={column.binary} charset={column.charset_id}")
    for row in range(args.rows):
        literal = column.literal(row)
        if literal.upper().startswith("X'"):
            width = len(literal[2:-1]) // 2
            shape = "hex"
        else:
            body = literal[1:-1] if literal.startswith("'") else literal
            # Undo the escape doubling to recover the stored byte count.
            decoded = (body.replace("\\\\", "\\").replace("\\0", "\0")
                       .replace("\\n", "\n").replace("\\t", "\t").replace("''", "'"))
            width = len(decoded.encode("utf-8"))
            shape = "text"
        print(f"row {row}: {shape} bytes={width} literal_head={literal[:80]!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
