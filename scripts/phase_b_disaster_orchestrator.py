#!/usr/bin/env python3
"""Phase B declarative disaster-validation orchestration entry point."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import replace
from pathlib import Path
from typing import Any, Iterable

from scripts.disaster_validation.adapters import default_adapters
from scripts.disaster_validation.orchestrator import (
    DisasterOrchestrator,
    OrchestrationConfig,
    OrchestrationError,
)

CONFIG_VERSION = 1
_REQUIRED_FIELDS = frozenset({
    "orchestration_version", "run_id", "evidence_root", "versions", "filesystems",
    "scenarios", "dataset_profiles",
})
_ALLOWED_FIELDS = _REQUIRED_FIELDS | frozenset({"page_sizes", "row_formats"})


def load_config(path: Path) -> OrchestrationConfig:
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise OrchestrationError(f"cannot load configuration: {path}") from error
    if not isinstance(document, dict):
        raise OrchestrationError("configuration must be a JSON object")
    unknown = set(document) - _ALLOWED_FIELDS
    missing = _REQUIRED_FIELDS - set(document)
    if unknown:
        raise OrchestrationError(f"unknown configuration fields: {', '.join(sorted(unknown))}")
    if missing:
        raise OrchestrationError(f"missing configuration fields: {', '.join(sorted(missing))}")
    if document["orchestration_version"] != CONFIG_VERSION:
        raise OrchestrationError("unsupported orchestration configuration version")
    return OrchestrationConfig(
        run_id=_scalar(document, "run_id"),
        evidence_root=Path(_scalar(document, "evidence_root")),
        versions=_axis(document, "versions", str),
        filesystems=_axis(document, "filesystems", str),
        scenarios=_axis(document, "scenarios", str),
        dataset_profiles=_axis(document, "dataset_profiles", str),
        page_sizes=_axis(document, "page_sizes", int, [16384]),
        row_formats=_axis(document, "row_formats", str, ["DYNAMIC"]),
    )


def apply_overrides(config: OrchestrationConfig, args: argparse.Namespace) -> OrchestrationConfig:
    updates: dict[str, Any] = {
        "dry_run": not args.execute,
        "resume": args.resume,
        "retry_case_ids": tuple(args.retry_case or ()),
        "allow_real_media": args.allow_real_media,
        "real_media_action": args.real_media_action,
    }
    for name in ("versions", "filesystems", "scenarios", "dataset_profiles", "page_sizes", "row_formats"):
        value = getattr(args, name)
        if value:
            updates[name] = tuple(value)
    if args.run_id:
        updates["run_id"] = args.run_id
    if args.evidence_root:
        updates["evidence_root"] = Path(args.evidence_root)
    return replace(config, **updates)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plan or execute fail-closed disaster validation. Default mode is non-mutating dry-run.",
    )
    parser.add_argument("--config", type=Path, required=True, help="versioned orchestration JSON configuration")
    parser.add_argument("--execute", action="store_true", help="dispatch adapters after capability and safety checks")
    parser.add_argument("--run-id")
    parser.add_argument("--evidence-root")
    parser.add_argument("--versions", nargs="+")
    parser.add_argument("--filesystems", nargs="+")
    parser.add_argument("--scenarios", nargs="+")
    parser.add_argument("--dataset-profiles", nargs="+")
    parser.add_argument("--page-sizes", nargs="+", type=int)
    parser.add_argument("--row-formats", nargs="+")
    parser.add_argument("--resume", action="store_true", help="reuse only sealed matching case evidence")
    parser.add_argument("--retry-case", action="append", help="retry an eligible case in a new workspace")
    parser.add_argument("--allow-real-media", action="store_true")
    parser.add_argument(
        "--real-media-action",
        choices=("mysql80-ext4-drop",),
        help="required with --allow-real-media for the one audited real-media case",
    )
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        config = apply_overrides(load_config(args.config), args)
        result = DisasterOrchestrator(default_adapters()).run(config)
    except OrchestrationError as error:
        print(f"orchestration_rejected: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result.document(), sort_keys=True))
    return 0


def _scalar(document: dict[str, Any], field: str) -> str:
    value = document[field]
    if not isinstance(value, str) or not value:
        raise OrchestrationError(f"{field} must be a non-empty string")
    return value


def _axis(document: dict[str, Any], field: str, element_type: type,
          default: tuple[Any, ...] | None = None) -> tuple[Any, ...]:
    value = document.get(field, default)
    if not isinstance(value, list) or not value or any(type(item) is not element_type for item in value):
        raise OrchestrationError(f"{field} must be a non-empty array of {element_type.__name__} values")
    return tuple(value)


if __name__ == "__main__":
    raise SystemExit(main())
